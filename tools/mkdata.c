/* mkdata: writes an empty nova-os data disk, a raw disk image of the size
   given in MiB. The disk carries a protective MBR and a GPT with one
   partition, the pool, whose type GUID is the pool format's
   (include/pool_format.h); the pool holds one data volume whose root
   directory is empty, committed as transaction group 1 with one
   superblock slot written in each ring (docs/spec/storage.md F11,
   docs/spec/pool-format.md). Every GUID is a constant and the disk is
   assembled in a zeroed buffer in a fixed order, so the same size gives
   the same bytes.

   usage: mkdata DATA.IMG MIB

   Formats: UEFI 2.11 chapter 5 (MBR and GPT) and appendix A (GUIDs). */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"

#define MIB (1024u * 1024u)
#define SECTOR_SIZE 512u
#define SECTORS(n) ((size_t)(n) * SECTOR_SIZE)

/* The size argument's bounds: room for the GPT, its 1 MiB alignment and a
   pool of POOL_BLOCKS_MIN blocks; and the most the file system service
   opens (fs/fs.c, POOL_BLOCKS_MAX). */
#define SIZE_MIN_MIB 2u
#define SIZE_MAX_MIB 4096u

/* The partition table (UEFI 2.11 chapter 5), as tools/mkimage.c writes
   the boot disk's: a protective MBR in LBA 0, the primary GPT header in
   LBA 1 with its 128-entry array after it, and the backup array and
   header in the disk's last sectors. The pool starts at 1 MiB, the
   alignment section 5.3.1 suggests, and ends where its last whole pool
   block does. */
#define GPT_HEADER_SIZE 92u
#define GPT_REVISION 0x00010000u
#define GPT_ENTRY_SIZE 128u
#define GPT_ENTRY_COUNT 128u
#define GPT_ARRAY_SIZE (GPT_ENTRY_SIZE * GPT_ENTRY_COUNT)
#define GPT_ARRAY_SECTORS (GPT_ARRAY_SIZE / SECTOR_SIZE)
#define PRIMARY_HEADER_LBA 1u
#define PRIMARY_ARRAY_LBA 2u
#define FIRST_USABLE_LBA (PRIMARY_ARRAY_LBA + GPT_ARRAY_SECTORS)
#define POOL_FIRST_LBA (MIB / SECTOR_SIZE)
#define POOL_NAME "nova-os pool"

#define MBR_PARTITION_RECORDS 446
#define MBR_TYPE_GPT_PROTECTIVE 0xEEu
#define MBR_SIZE_UNREPRESENTABLE 0xFFFFFFFFu
#define BOOT_SIGNATURE_OFFSET 510
#define BOOT_SIGNATURE 0xAA55u
#define CHS_LBA_1 0x000200u
#define CHS_UNREPRESENTABLE 0xFFFFFFu


/* Made once with a random generator and kept, so every build writes the
   same disk: the disk's GUID and the pool partition's unique GUID, in
   GPT's byte order. */
static const uint8_t disk_guid[POOL_GUID_SIZE] = {
    0x70, 0x41, 0x06, 0xa7, 0x5a, 0x1c, 0x54, 0x49,
    0xbd, 0x75, 0x0c, 0x97, 0xa2, 0x81, 0x59, 0x16};
static const uint8_t pool_guid[POOL_GUID_SIZE] = {
    0x2f, 0x65, 0x4e, 0xb7, 0x30, 0xf8, 0x2d, 0x46,
    0xb2, 0x54, 0xbd, 0xa2, 0x03, 0xa9, 0x91, 0xac};
static const uint8_t pool_type[POOL_GUID_SIZE] = POOL_PARTITION_TYPE;

/* The empty volume's transaction group, and the root directory's
   generation, the first a live node takes. */
#define FIRST_TXG 1u
#define ROOT_GENERATION 1u

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* A 24-bit CHS address is stored low byte first. */
static void put24(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFFu);
    p[2] = (uint8_t)(v >> 16);
}

/* ASCII as UTF-16LE; the buffer's zeros supply the terminator. */
static void put_utf16(uint8_t *p, const char *ascii)
{
    for (; *ascii != '\0'; ascii++, p += 2)
        put16(p, (uint8_t)*ascii);
}

/* Table 5.4: one record covering every sector after LBA 0. */
static void protective_mbr(uint8_t *mbr, uint64_t sectors)
{
    uint8_t *record = mbr + MBR_PARTITION_RECORDS;

    put24(record + 1, CHS_LBA_1);
    record[4] = MBR_TYPE_GPT_PROTECTIVE;
    put24(record + 5, CHS_UNREPRESENTABLE);
    pool_put32(record + 8, PRIMARY_HEADER_LBA);
    pool_put32(record + 12, sectors - 1 > MBR_SIZE_UNREPRESENTABLE
                                ? MBR_SIZE_UNREPRESENTABLE
                                : (uint32_t)(sectors - 1));
    put16(mbr + BOOT_SIGNATURE_OFFSET, BOOT_SIGNATURE);
}

/* Table 5.5. The header's CRC covers its 92 bytes with the CRC field
   zero, and it covers the array's CRC, so it is computed last. */
static void gpt_header(uint8_t *h, uint64_t sectors, uint64_t my_lba,
                       uint64_t alternate_lba, uint64_t array_lba,
                       uint32_t array_crc)
{
    memcpy(h, "EFI PART", 8);
    pool_put32(h + 8, GPT_REVISION);
    pool_put32(h + 12, GPT_HEADER_SIZE);
    pool_put64(h + 24, my_lba);
    pool_put64(h + 32, alternate_lba);
    pool_put64(h + 40, FIRST_USABLE_LBA);
    pool_put64(h + 48, sectors - 1 - GPT_ARRAY_SECTORS - 1);
    memcpy(h + 56, disk_guid, POOL_GUID_SIZE);
    pool_put64(h + 72, array_lba);
    pool_put32(h + 80, GPT_ENTRY_COUNT);
    pool_put32(h + 84, GPT_ENTRY_SIZE);
    pool_put32(h + 88, array_crc);
    pool_put32(h + 16, pool_crc32(0, h, GPT_HEADER_SIZE));
}

/* The GPT, with the pool from POOL_FIRST_LBA for pool_blocks blocks. */
static void build_gpt(uint8_t *disk, uint64_t sectors, uint64_t pool_blocks)
{
    uint64_t backup_header = sectors - 1;
    uint64_t backup_array = backup_header - GPT_ARRAY_SECTORS;
    uint8_t *array = disk + SECTORS(PRIMARY_ARRAY_LBA);
    uint32_t array_crc;

    protective_mbr(disk, sectors);
    memcpy(array, pool_type, POOL_GUID_SIZE); /* table 5.6 */
    memcpy(array + 16, pool_guid, POOL_GUID_SIZE);
    pool_put64(array + 32, POOL_FIRST_LBA);
    pool_put64(array + 40,
               POOL_FIRST_LBA + pool_blocks * POOL_SECTORS_PER_BLOCK - 1);
    put_utf16(array + 56, POOL_NAME);
    memcpy(disk + SECTORS(backup_array), array, GPT_ARRAY_SIZE);
    array_crc = pool_crc32(0, array, GPT_ARRAY_SIZE);
    gpt_header(disk + SECTORS(PRIMARY_HEADER_LBA), sectors,
               PRIMARY_HEADER_LBA, backup_header, PRIMARY_ARRAY_LBA,
               array_crc);
    gpt_header(disk + SECTORS(backup_header), sectors, backup_header,
               PRIMARY_HEADER_LBA, backup_array, array_crc);
}

/* ------------------------------------------------------------------------
   The pool. Metadata goes in two copies, the first at the data area's
   start and the second at the pool's middle, as the service places
   them (docs/spec/pool-format.md): the node table's one block, then the
   root block. */

static uint8_t *block_at(uint8_t *pool, uint64_t block)
{
    return pool + block * POOL_BLOCK_SIZE;
}

/* Write block at both its copies and point pointer at them. */
static void place(uint8_t *pool, const uint8_t *block, uint64_t first,
                  uint64_t second, POOL_POINTER *pointer)
{
    memcpy(block_at(pool, first), block, POOL_BLOCK_SIZE);
    memcpy(block_at(pool, second), block, POOL_BLOCK_SIZE);
    pointer->address[0] = first;
    pointer->address[1] = second;
    pointer->birth = FIRST_TXG;
    pool_digest(block, POOL_BLOCK_SIZE, pointer->digest);
}

static void build_pool(uint8_t *pool, uint64_t blocks)
{
    uint8_t block[POOL_BLOCK_SIZE];
    uint64_t middle = blocks / 2;
    uint32_t slot = FIRST_TXG % POOL_RING_SLOTS;
    POOL_RECORD directory;
    POOL_ROOT root;
    POOL_POINTER root_pointer;

    memset(block, 0, sizeof block);
    directory.kind = POOL_KIND_DIRECTORY;
    directory.flags = 0;
    directory.levels = 0;
    directory.generation = ROOT_GENERATION;
    directory.size = 0;
    pool_hole(&directory.root);
    pool_record_write(block + POOL_ROOT_SLOT * POOL_RECORD_SIZE, &directory);
    root.txg = FIRST_TXG;
    root.slots = POOL_RECORDS_PER_BLOCK;
    root.levels = 1;
    place(pool, block, POOL_RING_SLOTS, middle, &root.table);
    pool_root_write(block, &root);
    place(pool, block, POOL_RING_SLOTS + 1, middle + 1, &root_pointer);
    pool_superblock_write(block_at(pool, slot), blocks, FIRST_TXG,
                          &root_pointer);
    memcpy(block_at(pool, blocks - POOL_RING_SLOTS + slot),
           block_at(pool, slot), POOL_BLOCK_SIZE);
}

/* ------------------------------------------------------------------------
   The file. */

static int fail(const char *what)
{
    fprintf(stderr, "mkdata: %s: %s\n", what, strerror(errno));
    return EXIT_FAILURE;
}

/* The size argument, a decimal number of MiB within its bounds, or 0. */
static uint32_t parse_size(const char *text)
{
    uint32_t mib = 0;

    for (; *text >= '0' && *text <= '9'; text++) {
        mib = mib * 10 + (uint32_t)(*text - '0');
        if (mib > SIZE_MAX_MIB)
            return 0;
    }
    return *text == '\0' && mib >= SIZE_MIN_MIB ? mib : 0;
}

int main(int argc, char **argv)
{
    uint32_t mib;
    uint64_t sectors, last_usable, pool_blocks;
    uint8_t *disk;
    FILE *f;
    int status = EXIT_FAILURE;

    if (argc != 3 || !(mib = parse_size(argv[2]))) {
        fprintf(stderr, "usage: mkdata DATA.IMG MIB, MIB from %u to %u\n",
                SIZE_MIN_MIB, SIZE_MAX_MIB);
        return EXIT_FAILURE;
    }
    sectors = (uint64_t)mib * MIB / SECTOR_SIZE;
    last_usable = sectors - 1 - GPT_ARRAY_SECTORS - 1;
    pool_blocks = (last_usable - POOL_FIRST_LBA + 1) / POOL_SECTORS_PER_BLOCK;
    disk = calloc(1, (size_t)mib * MIB);
    if (disk == NULL)
        return fail("allocating the disk");
    build_gpt(disk, sectors, pool_blocks);
    build_pool(disk + SECTORS(POOL_FIRST_LBA), pool_blocks);
    f = fopen(argv[1], "wb");
    if (f == NULL) {
        fail(argv[1]);
    } else if (fwrite(disk, 1, (size_t)mib * MIB, f) != (size_t)mib * MIB) {
        fail(argv[1]);
        fclose(f);
        remove(argv[1]);
    } else if (fclose(f) != 0) {
        fail(argv[1]);
        remove(argv[1]);
    } else {
        status = EXIT_SUCCESS;
    }
    free(disk);
    return status;
}
