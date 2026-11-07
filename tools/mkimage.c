/* mkimage: writes nova-os's boot disk twice, as a raw disk image and as a
   VHDX file. The disk carries a protective MBR and a GPT with one EFI
   System Partition, formatted FAT32, which holds \EFI\BOOT\BOOTX64.EFI
   (the file firmware boots when no boot option names one) and
   \NOVA\KERNEL.EXE. Every GUID, serial number, timestamp and sequence
   number is a constant, and the disk is assembled in a zeroed buffer in a
   fixed order, so the same inputs give the same bytes.

   usage: mkimage DISK.IMG DISK.VHDX BOOTX64.EFI KERNEL.EXE

   Formats: UEFI 2.10 chapter 5 (MBR and GPT), section 13.3 (the EFI
   System Partition's file system) and appendix A (GUIDs); Microsoft's
   FAT32 File System Specification 1.03 ("fatgen103"); [MS-VHDX]
   v20240423; [MS-DTYP] v20241119 section 2.3.4.2 (GUIDs in VHDX). */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KIB 1024u
#define MIB (1024u * KIB)

/* ------------------------------------------------------------------------
   The disk, in 512-byte sectors. FAT32 needs at least 65,525 clusters,
   32 MiB of 512-byte clusters, so 64 MiB holds it with room to grow. */

#define SECTOR_SIZE 512u
#define DISK_SIZE (64u * MIB)
#define DISK_SECTORS (DISK_SIZE / SECTOR_SIZE)
#define SECTORS(n) ((size_t)(n) * SECTOR_SIZE)

/* The two files on the disk, in the order of the command line. */
enum { INPUT_LOADER, INPUT_KERNEL, INPUT_COUNT };

struct input {
    const char *path;
    uint8_t *data;
    size_t size;
};

static int fail(const char *what)
{
    fprintf(stderr, "mkimage: %s: %s\n", what, strerror(errno));
    return -1;
}

/* ------------------------------------------------------------------------
   Little-endian fields, which every structure here uses. A GUID keeps its
   text form's fields; on disk the first three are little-endian and the
   last eight bytes follow the text order (UEFI 2.10 table A.2). VHDX
   stores GUIDs "in binary form" without defining it ([MS-VHDX] 2.2.3.2);
   Microsoft's binary form, [MS-DTYP] 2.3.4.2, is the same layout. */

struct guid {
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t data4[8];
};

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFFu);
    put16(p + 2, v >> 16);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void put_guid(uint8_t *p, const struct guid *g)
{
    put32(p, g->data1);
    put16(p + 4, g->data2);
    put16(p + 6, g->data3);
    memcpy(p + 8, g->data4, sizeof g->data4);
}

/* ASCII as UTF-16LE; the buffer's zeros supply the terminator. */
static void put_utf16(uint8_t *p, const char *ascii)
{
    for (; *ascii != '\0'; ascii++, p += 2)
        put16(p, (uint8_t)*ascii);
}

/* ------------------------------------------------------------------------
   Checksums: GPT uses CRC-32 with polynomial 0x04C11DB7 (UEFI 2.10
   section 4.2), VHDX uses CRC-32C with polynomial 0x1EDC6F41 ([MS-VHDX]
   section 2). Both take each byte least significant bit first, so the
   register shifts right against the bit-reversed polynomial; it starts as
   all ones and the result is inverted. */

#define CRC32 0x04C11DB7u
#define CRC32C 0x1EDC6F41u

static uint32_t reverse_bits(uint32_t v)
{
    uint32_t r = 0;
    int i;

    for (i = 0; i < 32; i++, v >>= 1)
        r = r << 1 | (v & 1);
    return r;
}

static uint32_t crc(uint32_t polynomial, const uint8_t *p, size_t n)
{
    uint32_t reversed = reverse_bits(polynomial), r = 0xFFFFFFFFu;
    int bit;

    for (; n > 0; n--) {
        r ^= *p++;
        for (bit = 0; bit < 8; bit++)
            r = (r & 1) ? (r >> 1) ^ reversed : r >> 1;
    }
    return ~r;
}

/* ------------------------------------------------------------------------
   The partition table (UEFI 2.10 chapter 5): a protective MBR in LBA 0,
   the primary GPT header in LBA 1 with its 128-entry array after it, and
   the backup array and header in the disk's last sectors. The one
   partition, the EFI System Partition, starts at 1 MiB, the alignment
   section 5.3.1 suggests, and runs to the last usable LBA. */

#define GPT_HEADER_SIZE 92u
#define GPT_REVISION 0x00010000u
#define GPT_ENTRY_SIZE 128u
#define GPT_ENTRY_COUNT 128u
#define GPT_ARRAY_SIZE (GPT_ENTRY_SIZE * GPT_ENTRY_COUNT)
#define GPT_ARRAY_SECTORS (GPT_ARRAY_SIZE / SECTOR_SIZE)

#define PRIMARY_HEADER_LBA 1u
#define PRIMARY_ARRAY_LBA 2u
#define FIRST_USABLE_LBA (PRIMARY_ARRAY_LBA + GPT_ARRAY_SECTORS)
#define BACKUP_HEADER_LBA (DISK_SECTORS - 1)
#define BACKUP_ARRAY_LBA (BACKUP_HEADER_LBA - GPT_ARRAY_SECTORS)
#define LAST_USABLE_LBA (BACKUP_ARRAY_LBA - 1)

#define ALIGN_SECTORS (MIB / SECTOR_SIZE)
#define ESP_FIRST_LBA ALIGN_SECTORS
#define ESP_LAST_LBA LAST_USABLE_LBA
#define ESP_SECTORS (ESP_LAST_LBA - ESP_FIRST_LBA + 1)
#define ESP_NAME "EFI System Partition"

#define MBR_PARTITION_RECORDS 446
#define MBR_TYPE_GPT_PROTECTIVE 0xEEu
#define BOOT_SIGNATURE_OFFSET 510
#define BOOT_SIGNATURE 0xAA55u

/* Table 5.4's starting CHS address is LBA 1's. The disk has no CHS
   geometry, so the ending address takes the value the table gives for one
   it cannot represent; UEFI firmware reads only the LBA fields (table
   5.2). */
#define CHS_LBA_1 0x000200u
#define CHS_UNREPRESENTABLE 0xFFFFFFu

/* UEFI 2.10 table 5.7. */
static const struct guid esp_type = {
    0xC12A7328, 0xF81F, 0x11D2,
    {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};

/* Made once with a random generator and kept, so every build writes the
   same disk. */
static const struct guid disk_guid = {
    0x06F91F74, 0xCA25, 0x46CB,
    {0x9E, 0xAE, 0x68, 0x78, 0x87, 0x71, 0xA2, 0x26}};
static const struct guid esp_guid = {
    0x4134B647, 0x852F, 0x49AA,
    {0xB6, 0x08, 0x33, 0xB3, 0x48, 0x62, 0x32, 0x7D}};

/* A 24-bit CHS address is stored low byte first. */
static void put24(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFFu);
    p[2] = (uint8_t)(v >> 16);
}

/* Table 5.4: one record covering every sector after LBA 0. */
static void protective_mbr(uint8_t *mbr)
{
    uint8_t *record = mbr + MBR_PARTITION_RECORDS;

    put24(record + 1, CHS_LBA_1);
    record[4] = MBR_TYPE_GPT_PROTECTIVE;
    put24(record + 5, CHS_UNREPRESENTABLE);
    put32(record + 8, PRIMARY_HEADER_LBA);
    put32(record + 12, DISK_SECTORS - 1);
    put16(mbr + BOOT_SIGNATURE_OFFSET, BOOT_SIGNATURE);
}

/* Table 5.5. The header's CRC covers its 92 bytes with the CRC field
   zero, and it covers the array's CRC, so it is computed last. */
static void gpt_header(uint8_t *h, uint32_t my_lba, uint32_t alternate_lba,
                       uint32_t array_lba, uint32_t array_crc)
{
    memcpy(h, "EFI PART", 8);
    put32(h + 8, GPT_REVISION);
    put32(h + 12, GPT_HEADER_SIZE);
    put64(h + 24, my_lba);
    put64(h + 32, alternate_lba);
    put64(h + 40, FIRST_USABLE_LBA);
    put64(h + 48, LAST_USABLE_LBA);
    put_guid(h + 56, &disk_guid);
    put64(h + 72, array_lba);
    put32(h + 80, GPT_ENTRY_COUNT);
    put32(h + 84, GPT_ENTRY_SIZE);
    put32(h + 88, array_crc);
    put32(h + 16, crc(CRC32, h, GPT_HEADER_SIZE));
}

static void build_gpt(uint8_t *disk)
{
    uint8_t *array = disk + SECTORS(PRIMARY_ARRAY_LBA);
    uint32_t array_crc;

    protective_mbr(disk);
    put_guid(array, &esp_type); /* table 5.6 */
    put_guid(array + 16, &esp_guid);
    put64(array + 32, ESP_FIRST_LBA);
    put64(array + 40, ESP_LAST_LBA);
    put_utf16(array + 56, ESP_NAME);
    memcpy(disk + SECTORS(BACKUP_ARRAY_LBA), array, GPT_ARRAY_SIZE);
    array_crc = crc(CRC32, array, GPT_ARRAY_SIZE);
    gpt_header(disk + SECTORS(PRIMARY_HEADER_LBA), PRIMARY_HEADER_LBA,
               BACKUP_HEADER_LBA, PRIMARY_ARRAY_LBA, array_crc);
    gpt_header(disk + SECTORS(BACKUP_HEADER_LBA), BACKUP_HEADER_LBA,
               PRIMARY_HEADER_LBA, BACKUP_ARRAY_LBA, array_crc);
}

/* ------------------------------------------------------------------------
   The FAT32 volume in the EFI System Partition (fatgen103; UEFI 2.10
   section 13.3.1.1). Names are 8.3 short names only, which FAT stores in
   upper case and matches ignoring case (fatgen103, "Name Matching"), so
   nova-boot's \nova\kernel.exe opens \NOVA\KERNEL.EXE. */

#define FAT_SECTORS_PER_CLUSTER 1u /* fatgen103's FAT32 table, to 260 MB */
#define CLUSTER_SIZE (FAT_SECTORS_PER_CLUSTER * SECTOR_SIZE)
#define FAT_COUNT 2u
#define FAT_MIN_RESERVED 32u
#define FAT_ENTRY_SIZE 4u
#define FAT32_MIN_CLUSTERS 65525u
#define FAT_TYPE_MARGIN 16u /* fatgen103 keeps this far from the cutover */
#define FAT_FIRST_CLUSTER 2u
#define FAT_MEDIA_FIXED 0xF8u
#define FAT_ENTRY_0 (0x0FFFFF00u | FAT_MEDIA_FIXED)
#define FAT_END_OF_CHAIN 0x0FFFFFFFu /* in FAT[1], also "clean, no errors" */

#define FAT_OEM_NAME "MSWIN4.1" /* the value fatgen103 recommends */
#define FAT_NO_LABEL "NO NAME    "
#define FAT_TYPE_NAME "FAT32   "
#define FAT_FSINFO_SECTOR 1u
#define FAT_BACKUP_BOOT_SECTOR 6u
#define FAT_BOOT_RECORD_SECTORS 2u /* the boot sector and FSInfo */
#define FAT_DRIVE_NUMBER 0x80u
#define FAT_EXTENDED_BOOT_SIGNATURE 0x29u
#define FAT_VOLUME_ID 0x37523EEEu /* made once at random, like the GUIDs */

/* BS_jmpBoot jumps to where boot code would follow BS_FilSysType. There is
   none: firmware reads the volume through its own FAT driver. */
#define X86_JMP_SHORT 0xEBu
#define X86_NOP 0x90u
#define FAT32_BOOT_CODE 90u

#define FSI_LEAD_SIGNATURE 0x41615252u
#define FSI_STRUCT_SIGNATURE 0x61417272u
#define FSI_TRAIL_SIGNATURE 0xAA550000u
#define FSI_NO_HINT 0xFFFFFFFFu

#define DIR_ENTRY_SIZE 32u
#define FAT_NAME_SIZE 11u
#define ATTR_DIRECTORY 0x10u
#define ATTR_ARCHIVE 0x20u
#define FAT_DATE(year, month, day) \
    (((year) - 1980u) << 9 | (month) << 5 | (day))
#define FILE_DATE FAT_DATE(1980u, 1u, 1u) /* the earliest FAT date */
#define FILE_TIME 0u

/* The tree, in the order clusters are allocated and directory entries
   written. A parent comes before its children, and each directory is one
   cluster, room for 16 entries. */
enum {
    NODE_ROOT,
    NODE_EFI,
    NODE_BOOT,
    NODE_LOADER,
    NODE_NOVA,
    NODE_KERNEL,
    NODE_COUNT
};

#define DIRECTORY (-1)
#define NO_PARENT (-1)

static const struct node {
    char name[FAT_NAME_SIZE + 1];
    int parent;
    int input; /* an INPUT_ index, or DIRECTORY */
} tree[NODE_COUNT] = {
    [NODE_ROOT] = {"", NO_PARENT, DIRECTORY},
    [NODE_EFI] = {"EFI        ", NODE_ROOT, DIRECTORY},
    [NODE_BOOT] = {"BOOT       ", NODE_EFI, DIRECTORY},
    [NODE_LOADER] = {"BOOTX64 EFI", NODE_BOOT, INPUT_LOADER},
    [NODE_NOVA] = {"NOVA       ", NODE_ROOT, DIRECTORY},
    [NODE_KERNEL] = {"KERNEL  EXE", NODE_NOVA, INPUT_KERNEL},
};

struct fat_layout {
    uint32_t reserved;    /* BPB_RsvdSecCnt */
    uint32_t fat_sectors; /* BPB_FATSz32 */
    uint32_t clusters;    /* CountofClusters */
};

/* BPB_FATSz32 comes from fatgen103's "FAT Volume Initialization"
   arithmetic. The reserved region then grows from the usual 32 sectors
   until the data region starts on a 1 MiB boundary: UEFI 2.10 section
   13.3.1.1 asks for the device's physical block and transfer granularity,
   and section 5.3.1 suggests 1 MiB to cover any of them. That leaves the
   FAT a few sectors larger than needed, which fatgen103 allows. */
static struct fat_layout plan_fat(void)
{
    struct fat_layout l;
    uint32_t per_fat_sector = (256 * FAT_SECTORS_PER_CLUSTER + FAT_COUNT) / 2;
    uint32_t fats, data_lba;

    l.fat_sectors = (ESP_SECTORS - FAT_MIN_RESERVED + per_fat_sector - 1) /
                    per_fat_sector;
    fats = FAT_COUNT * l.fat_sectors;
    data_lba = (ESP_FIRST_LBA + FAT_MIN_RESERVED + fats + ALIGN_SECTORS - 1) /
               ALIGN_SECTORS * ALIGN_SECTORS;
    l.reserved = data_lba - ESP_FIRST_LBA - fats;
    l.clusters = (ESP_SECTORS - l.reserved - fats) / FAT_SECTORS_PER_CLUSTER;
    return l;
}

/* Fields not set stay zero: FAT32 requires it of BPB_RootEntCnt,
   BPB_TotSec16 and BPB_FATSz16; BPB_ExtFlags 0 mirrors the two FATs;
   BPB_FSVer is 0.0; BPB_SecPerTrk and BPB_NumHeads matter only to BIOS
   INT 13h, which never reads this disk. */
static void boot_sector(uint8_t *s, const struct fat_layout *l)
{
    s[0] = X86_JMP_SHORT;
    s[1] = FAT32_BOOT_CODE - 2; /* counted from the end of the jump */
    s[2] = X86_NOP;
    memcpy(s + 3, FAT_OEM_NAME, 8);
    put16(s + 11, SECTOR_SIZE);
    s[13] = FAT_SECTORS_PER_CLUSTER;
    put16(s + 14, l->reserved);
    s[16] = FAT_COUNT;
    s[21] = FAT_MEDIA_FIXED;
    put32(s + 28, ESP_FIRST_LBA); /* BPB_HiddSec: sectors before the volume */
    put32(s + 32, ESP_SECTORS);
    put32(s + 36, l->fat_sectors);
    put32(s + 44, FAT_FIRST_CLUSTER); /* BPB_RootClus */
    put16(s + 48, FAT_FSINFO_SECTOR);
    put16(s + 50, FAT_BACKUP_BOOT_SECTOR);
    s[64] = FAT_DRIVE_NUMBER;
    s[66] = FAT_EXTENDED_BOOT_SIGNATURE;
    put32(s + 67, FAT_VOLUME_ID);
    memcpy(s + 71, FAT_NO_LABEL, FAT_NAME_SIZE);
    memcpy(s + 82, FAT_TYPE_NAME, 8);
    put16(s + BOOT_SIGNATURE_OFFSET, BOOT_SIGNATURE);
}

static void fsinfo(uint8_t *s, uint32_t free_clusters, uint32_t next_free)
{
    put32(s, FSI_LEAD_SIGNATURE);
    put32(s + 484, FSI_STRUCT_SIGNATURE);
    put32(s + 488, free_clusters);
    put32(s + 492, next_free);
    put32(s + 508, FSI_TRAIL_SIGNATURE);
}

/* The creation and access stamps are optional and stay zero, as fatgen103
   asks of a writer that does not keep them. */
static void dir_entry(uint8_t *e, const char *name, uint32_t attributes,
                      uint32_t cluster, uint32_t size)
{
    memcpy(e, name, FAT_NAME_SIZE);
    e[11] = (uint8_t)attributes;
    put16(e + 20, cluster >> 16);
    put16(e + 22, FILE_TIME);
    put16(e + 24, FILE_DATE);
    put16(e + 26, cluster & 0xFFFFu);
    put32(e + 28, size);
}

/* The data region starts with cluster 2. */
static uint8_t *cluster_at(uint8_t *data, uint32_t cluster)
{
    return data + (size_t)(cluster - FAT_FIRST_CLUSTER) * CLUSTER_SIZE;
}

/* Links COUNT clusters from FIRST into one chain. */
static void chain(uint8_t *fat, uint32_t first, uint32_t count)
{
    uint32_t k;

    for (k = 0; k < count; k++)
        put32(fat + (size_t)(first + k) * FAT_ENTRY_SIZE,
              k + 1 < count ? first + k + 1 : FAT_END_OF_CHAIN);
}

/* A new directory's "." and ".." entries; ".." holds 0 when the parent is
   the root, even on FAT32. */
static void dot_entries(uint8_t *at, uint32_t self, uint32_t parent)
{
    dir_entry(at, ".          ", ATTR_DIRECTORY, self, 0);
    dir_entry(at + DIR_ENTRY_SIZE, "..         ", ATTR_DIRECTORY, parent, 0);
}

static int build_fat(uint8_t *disk, const struct input *inputs)
{
    const struct fat_layout l = plan_fat();
    uint8_t *volume = disk + SECTORS(ESP_FIRST_LBA);
    uint8_t *fat = volume + SECTORS(l.reserved);
    uint8_t *data = fat + SECTORS(FAT_COUNT * l.fat_sectors);
    uint32_t first[NODE_COUNT], entries[NODE_COUNT];
    uint32_t next = FAT_FIRST_CLUSTER, end = FAT_FIRST_CLUSTER + l.clusters;
    int i;

    if (l.clusters < FAT32_MIN_CLUSTERS + FAT_TYPE_MARGIN) {
        fprintf(stderr, "mkimage: %u clusters are too few for FAT32\n",
                (unsigned)l.clusters);
        return -1;
    }
    put32(fat, FAT_ENTRY_0);
    put32(fat + FAT_ENTRY_SIZE, FAT_END_OF_CHAIN);
    for (i = 0; i < NODE_COUNT; i++) {
        const struct node *n = &tree[i];
        const struct input *in =
            n->input == DIRECTORY ? NULL : &inputs[n->input];
        size_t size = in == NULL ? CLUSTER_SIZE : in->size;
        uint8_t *at = cluster_at(data, next);
        uint32_t count;

        if (size > (size_t)(end - next) * CLUSTER_SIZE) {
            fprintf(stderr, "mkimage: no room on the volume for %s\n",
                    in == NULL ? "the directories" : in->path);
            return -1;
        }
        count = (uint32_t)((size + CLUSTER_SIZE - 1) / CLUSTER_SIZE);
        first[i] = count > 0 ? next : 0;
        chain(fat, next, count);
        next += count;
        entries[i] = 0;
        if (in != NULL)
            memcpy(at, in->data, in->size);
        else if (i != NODE_ROOT) {
            dot_entries(at, first[i],
                        n->parent == NODE_ROOT ? 0 : first[n->parent]);
            entries[i] = 2;
        }
        if (i != NODE_ROOT)
            dir_entry(cluster_at(data, first[n->parent]) +
                          entries[n->parent]++ * DIR_ENTRY_SIZE,
                      n->name, in == NULL ? ATTR_DIRECTORY : ATTR_ARCHIVE,
                      first[i], in == NULL ? 0 : (uint32_t)in->size);
    }
    memcpy(fat + SECTORS(l.fat_sectors), fat, SECTORS(l.fat_sectors));
    boot_sector(volume, &l);
    fsinfo(volume + SECTORS(FAT_FSINFO_SECTOR), end - next,
           next < end ? next : FSI_NO_HINT);
    memcpy(volume + SECTORS(FAT_BACKUP_BOOT_SECTOR), volume,
           SECTORS(FAT_BOOT_RECORD_SECTORS));
    return 0;
}

/* ------------------------------------------------------------------------
   The VHDX container ([MS-VHDX]). The file is fixed: every payload block
   is present and the payload is the raw disk verbatim, so every virtual
   sector reads from the file, never from a block state that lets a reader
   return arbitrary data, and the two outputs can be compared byte for
   byte. It costs the file the disk's full size.

   Layout, every structure on a 1 MiB boundary (section 2.1): the header
   section (identifier, two headers, two region tables), an empty log, the
   BAT, the metadata region, then the payload. */

#define VHDX_HEADER_1 (64u * KIB)
#define VHDX_HEADER_2 (128u * KIB)
#define VHDX_HEADER_SIZE (4u * KIB)
#define VHDX_REGION_TABLE_1 (192u * KIB)
#define VHDX_REGION_TABLE_2 (256u * KIB)
#define VHDX_REGION_TABLE_SIZE (64u * KIB)
#define VHDX_LOG_OFFSET (1u * MIB)
#define VHDX_LOG_SIZE MIB
#define VHDX_BAT_OFFSET (2u * MIB)
#define VHDX_BAT_SIZE MIB
#define VHDX_METADATA_OFFSET (3u * MIB)
#define VHDX_METADATA_SIZE MIB
#define VHDX_PAYLOAD_OFFSET (4u * MIB)
#define VHDX_FILE_SIZE (VHDX_PAYLOAD_OFFSET + DISK_SIZE)

#define VHDX_VERSION 1u
#define VHDX_CREATOR "nova-os mkimage"
#define VHDX_REGION_COUNT 2u
#define VHDX_REQUIRED 1u

/* A header is current when its sequence number is the greater of two
   valid headers' (section 2.2.2), so the two must differ. */
#define VHDX_SEQUENCE_1 1u
#define VHDX_SEQUENCE_2 2u

/* Section 2.6.2.1: payload blocks of 1 MiB, the smallest allowed. A sector
   bitmap block covers 2^23 sectors, so a chunk is that many sectors'
   worth of blocks (section 2.4), and the BAT puts one bitmap entry after
   each chunk's payload entries (section 2.5). */
#define VHDX_BLOCK_SIZE MIB
#define VHDX_BLOCK_COUNT (DISK_SIZE / VHDX_BLOCK_SIZE)
#define VHDX_BITMAP_SECTORS (1u << 23)
#define VHDX_BLOCK_SECTORS (VHDX_BLOCK_SIZE / SECTOR_SIZE)
#define VHDX_CHUNK_RATIO (VHDX_BITMAP_SECTORS / VHDX_BLOCK_SECTORS)
#define VHDX_BAT_ENTRY_SIZE 8u
#define VHDX_FILE_OFFSET_SHIFT 20 /* FileOffsetMB, bits 20 to 63 */
#define PAYLOAD_BLOCK_FULLY_PRESENT 6u

#define VHDX_METADATA_HEADER_SIZE 32u
#define VHDX_METADATA_ENTRY_SIZE 32u
#define VHDX_METADATA_ITEMS (64u * KIB)
#define METADATA_IS_VIRTUAL_DISK 2u
#define METADATA_IS_REQUIRED 4u
#define LEAVE_BLOCK_ALLOCATED 1u

/* Sections 2.2.3.2 and 2.6.2. */
static const struct guid bat_region = {
    0x2DC27766, 0xF623, 0x4200,
    {0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08}};
static const struct guid metadata_region = {
    0x8B7CA206, 0x4790, 0x4B9A,
    {0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E}};
static const struct guid file_parameters_item = {
    0xCAA16737, 0xFA36, 0x4D43,
    {0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B}};
static const struct guid disk_size_item = {
    0x2FA54224, 0xCD1B, 0x4876,
    {0xB2, 0x11, 0x5D, 0xBE, 0xD8, 0x3B, 0xF4, 0xB8}};
static const struct guid disk_id_item = {
    0xBECA12AB, 0xB2E6, 0x4523,
    {0x93, 0xEF, 0xC3, 0x09, 0xE0, 0x00, 0xC7, 0x46}};
static const struct guid logical_sector_item = {
    0x8141BF1D, 0xA96F, 0x4709,
    {0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F}};
static const struct guid physical_sector_item = {
    0xCDA348C7, 0x445D, 0x4471,
    {0x9C, 0xC9, 0xE9, 0x88, 0x52, 0x51, 0xC5, 0x56}};

/* Made once with a random generator and kept, like the GPT's. The log is
   empty, so the header's LogGuid stays zero. */
static const struct guid file_write_guid = {
    0x25E15FA3, 0x4C4E, 0x440D,
    {0xA0, 0x05, 0x5A, 0x9B, 0xA1, 0xAD, 0xF9, 0x47}};
static const struct guid data_write_guid = {
    0x653B0F01, 0x2C97, 0x4D8D,
    {0x95, 0x88, 0xF9, 0x86, 0xD2, 0x03, 0xA1, 0xC8}};
static const struct guid virtual_disk_id = {
    0x518D0982, 0x394E, 0x4089,
    {0xB9, 0x2C, 0x84, 0x96, 0x82, 0xE8, 0xAB, 0xBE}};

/* Section 2.2.2. The specification writes each signature as a number,
   some reading the string's bytes in order ("vhdxfile", "head", "regi")
   and some reversed ("metadata"), but every one names an ASCII string and
   section 2 stores strings byte by byte, so each goes on disk as its
   string. */
static void vhdx_header(uint8_t *h, uint64_t sequence)
{
    memcpy(h, "head", 4);
    put64(h + 8, sequence);
    put_guid(h + 16, &file_write_guid);
    put_guid(h + 32, &data_write_guid);
    put16(h + 66, VHDX_VERSION);
    put32(h + 68, VHDX_LOG_SIZE);
    put64(h + 72, VHDX_LOG_OFFSET);
    put32(h + 4, crc(CRC32C, h, VHDX_HEADER_SIZE));
}

static void region_entry(uint8_t *e, const struct guid *id, uint64_t offset,
                         uint32_t length)
{
    put_guid(e, id);
    put64(e + 16, offset);
    put32(e + 24, length);
    put32(e + 28, VHDX_REQUIRED);
}

/* Section 2.2.3; the checksum covers the whole 64 KiB. */
static void vhdx_region_table(uint8_t *t)
{
    memcpy(t, "regi", 4);
    put32(t + 8, VHDX_REGION_COUNT);
    region_entry(t + 16, &bat_region, VHDX_BAT_OFFSET, VHDX_BAT_SIZE);
    region_entry(t + 48, &metadata_region, VHDX_METADATA_OFFSET,
                 VHDX_METADATA_SIZE);
    put32(t + 4, crc(CRC32C, t, VHDX_REGION_TABLE_SIZE));
}

struct metadata_table {
    uint8_t *region;
    uint32_t count;
    uint32_t next; /* where the next item goes */
};

/* Section 2.6.1: a table entry for an item, packed after the last one in
   the item area; returns where the item's value goes. */
static uint8_t *metadata_item(struct metadata_table *m, const struct guid *id,
                              uint32_t length, uint32_t flags)
{
    uint8_t *e = m->region + VHDX_METADATA_HEADER_SIZE +
                 m->count * VHDX_METADATA_ENTRY_SIZE;
    uint8_t *item = m->region + m->next;

    put_guid(e, id);
    put32(e + 16, m->next);
    put32(e + 20, length);
    put32(e + 24, flags);
    m->count++;
    m->next += length;
    return item;
}

/* Section 2.6.2: the five items a disk without a parent needs, flagged as
   the known-items table gives them. */
static void vhdx_metadata(uint8_t *region)
{
    struct metadata_table m = {region, 0, VHDX_METADATA_ITEMS};
    const uint32_t disk_item =
        METADATA_IS_VIRTUAL_DISK | METADATA_IS_REQUIRED;
    uint8_t *item;

    item = metadata_item(&m, &file_parameters_item, 8, METADATA_IS_REQUIRED);
    put32(item, VHDX_BLOCK_SIZE);
    put32(item + 4, LEAVE_BLOCK_ALLOCATED);
    item = metadata_item(&m, &disk_size_item, 8, disk_item);
    put64(item, DISK_SIZE);
    item = metadata_item(&m, &disk_id_item, 16, disk_item);
    put_guid(item, &virtual_disk_id);
    item = metadata_item(&m, &logical_sector_item, 4, disk_item);
    put32(item, SECTOR_SIZE);
    item = metadata_item(&m, &physical_sector_item, 4, disk_item);
    put32(item, SECTOR_SIZE);
    memcpy(region, "metadata", 8);
    put16(region + 10, m.count);
}

/* Section 2.5: payload block b's entry sits after b / VHDX_CHUNK_RATIO
   sector bitmap entries, which stay zero, SB_BLOCK_NOT_PRESENT. */
static void vhdx_bat(uint8_t *table)
{
    uint64_t b;

    for (b = 0; b < VHDX_BLOCK_COUNT; b++) {
        uint64_t offset = VHDX_PAYLOAD_OFFSET + b * VHDX_BLOCK_SIZE;

        put64(table + (b + b / VHDX_CHUNK_RATIO) * VHDX_BAT_ENTRY_SIZE,
              offset / MIB << VHDX_FILE_OFFSET_SHIFT |
                  PAYLOAD_BLOCK_FULLY_PRESENT);
    }
}

/* Everything before the payload; the log stays zero. */
static void build_vhdx(uint8_t *file)
{
    memcpy(file, "vhdxfile", 8);
    put_utf16(file + 8, VHDX_CREATOR);
    vhdx_header(file + VHDX_HEADER_1, VHDX_SEQUENCE_1);
    vhdx_header(file + VHDX_HEADER_2, VHDX_SEQUENCE_2);
    vhdx_region_table(file + VHDX_REGION_TABLE_1);
    memcpy(file + VHDX_REGION_TABLE_2, file + VHDX_REGION_TABLE_1,
           VHDX_REGION_TABLE_SIZE);
    vhdx_bat(file + VHDX_BAT_OFFSET);
    vhdx_metadata(file + VHDX_METADATA_OFFSET);
}

/* ------------------------------------------------------------------------
   Files. */

#define READ_CHUNK (64u * KIB)

/* An input larger than the disk cannot fit on it, so reading stops there,
   which also keeps the doubling buffer far from overflowing size_t. */
static int read_input(struct input *in)
{
    FILE *f = fopen(in->path, "rb");
    size_t capacity = 0, n;

    if (f == NULL)
        return fail(in->path);
    do {
        if (in->size == capacity) {
            uint8_t *grown;

            capacity = capacity == 0 ? READ_CHUNK : 2 * capacity;
            grown = realloc(in->data, capacity);
            if (grown == NULL) {
                fail(in->path);
                fclose(f);
                return -1;
            }
            in->data = grown;
        }
        n = fread(in->data + in->size, 1, capacity - in->size, f);
        in->size += n;
        if (in->size > DISK_SIZE) {
            fprintf(stderr, "mkimage: %s is larger than the disk\n",
                    in->path);
            fclose(f);
            return -1;
        }
    } while (n > 0);
    if (ferror(f)) {
        fail(in->path);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return fail(in->path);
    return 0;
}

/* A failed write removes the file, so no partial image is left behind. */
static int write_output(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL)
        return fail(path);
    if (fwrite(data, 1, size, f) != size) {
        fail(path);
        fclose(f);
        remove(path);
        return -1;
    }
    if (fclose(f) != 0) {
        fail(path);
        remove(path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct input inputs[INPUT_COUNT] = {{NULL, NULL, 0}, {NULL, NULL, 0}};
    uint8_t *file = NULL, *disk;
    int status = EXIT_FAILURE, i;

    if (argc != 5) {
        fprintf(stderr,
                "usage: mkimage DISK.IMG DISK.VHDX BOOTX64.EFI KERNEL.EXE\n");
        return EXIT_FAILURE;
    }
    inputs[INPUT_LOADER].path = argv[3];
    inputs[INPUT_KERNEL].path = argv[4];
    for (i = 0; i < INPUT_COUNT; i++)
        if (read_input(&inputs[i]) != 0)
            goto done;
    file = calloc(1, VHDX_FILE_SIZE);
    if (file == NULL) {
        fail("allocating the image");
        goto done;
    }
    /* The disk is assembled where the VHDX payload goes, so the raw image
       is that slice of the VHDX file. */
    disk = file + VHDX_PAYLOAD_OFFSET;
    build_gpt(disk);
    if (build_fat(disk, inputs) != 0)
        goto done;
    build_vhdx(file);
    if (write_output(argv[1], disk, DISK_SIZE) == 0 &&
        write_output(argv[2], file, VHDX_FILE_SIZE) == 0)
        status = EXIT_SUCCESS;
done:
    free(file);
    for (i = 0; i < INPUT_COUNT; i++)
        free(inputs[i].data);
    return status;
}
