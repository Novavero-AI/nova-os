/* The data disk's check (fstest.h, init/data.c; docs/spec/storage.md
   F18). Its launcher starts it with a view of the data volume's /data,
   reached through the namespace server: read-write at FSTEST_DATA and
   read-only at FSTEST_READ_ONLY; and of the namespace's root without
   WRITE at FSTEST_NAMESPACE. Under /data/fstest it checks:

   - that a file written, flushed and read back at an earlier boot reads
     back byte for byte, or writes it, flushes and reads it back if this
     is the disk's first boot, and counts the disk's boots; with a block
     of it failing its check, that a write into each block in part fails
     as the block's read does;
   - that a file extended by truncate alone reads zeros, at every boot;
   - that what the read-only directory or the namespace without WRITE
     reaches cannot be written, created, truncated, flushed, renamed or
     removed, nor a file opened for reading alone be written;
   - create, exclusive create, write past the end, truncate, rename with
     and without replace, and remove, with a channel that keeps reaching
     its node across a rename and answers ERR_NOT_FOUND once it is
     removed; and a directory filled until it refuses a create and a
     rename into it;
   - a crash state: files whose contents name one generation, each
     generation committed by one flush, which it checks is whole before
     it commits the next. With FSTEST_LOOP it then commits generations
     until the machine stops, logging each as it starts, as it flushes
     and once its flush has answered, so a crash test that kills the
     machine finds at the next boot the generation last logged committed
     or the one after.

   It logs what it finds and exits with FSTEST_EXIT_PASSED once every
   check passes, or FSTEST_EXIT_FAILED once all have run and one has
   not. */

#include "fstest.h"
#include "launch.h"
#include "log.h"
#include "native.h"
#include "path.h"

#define PATH_BUFFER 512

/* The persisted file: PERSIST_SIZE bytes, enough that its block tree has
   two levels of indirect blocks, from the generator seeded with
   PERSIST_SEED. */
#define PERSIST_SIZE 300000
#define PERSIST_SEED 0x6e6f76612d667331ULL
#define BLOCK_SIZE 4096
#define PERSIST_BLOCKS ((PERSIST_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE)

/* A file extended by truncate alone, far past any block it holds. */
#define SPARSE_SIZE 200000
#define SPARSE_TAIL 16

/* Names of the most bytes a name may have, NAME_MAX_BYTES, which fill a
   directory with the fewest entries: a prefix, then the entry's number in
   FULL_DIGITS decimal digits, so each sorts after the last. */
#define FULL_NAME_LENGTH 255
#define FULL_DIGITS 4
#define DECIMAL 10

/* A crash-state file holds its generation, GENERATION_SIZE bytes, then
   CRASH_UNIT bytes times one more than the generation modulo
   CRASH_UNITS from the generator seeded with the generation, so its
   size and its blocks change from one generation to the next. */
#define GENERATION_SIZE 8
#define CRASH_UNIT 4500
#define CRASH_UNITS 5
#define CRASH_SIZE_MAX (GENERATION_SIZE + CRASH_UNIT * CRASH_UNITS)

/* xorshift64 (Marsaglia, 2003): a byte stream reproducible from its seed
   alone, which is all the checks need of it. */
#define XORSHIFT_A 13
#define XORSHIFT_B 7
#define XORSHIFT_C 17
#define SEED_NONZERO 0x9e3779b97f4a7c15ULL

#define BITS_PER_BYTE 8

static UINT8 launch[CHANNEL_MAX_BYTES];
static UINT8 buffer[IO_TRANSFER_MAX];
static UINT8 expected[IO_TRANSFER_MAX];
static char path[PATH_BUFFER];
static VIEW view;
static const char *mode;
static int failed;

static UINT32 length_of(const char *text)
{
    UINT32 n = 0;

    while (text[n])
        n++;
    return n;
}

static int same(const UINT8 *a, const UINT8 *b, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* A check that fails is logged and remembered; the program runs on. */
static int check(int passed, const char *what)
{
    if (!passed) {
        say("nova-fstest: check failed: ");
        say(what);
        say("\r\n");
        failed = 1;
    }
    return passed;
}

/* Stop at once: a check whose failure leaves nothing after it to run. */
_Noreturn static void give_up(const char *what, UINT32 status)
{
    say("nova-fstest: check failed: ");
    say(what);
    say(", status ");
    say_number(status);
    say("\r\n");
    process_exit(FSTEST_EXIT_FAILED);
}

/* prefix and rest joined into path. */
static const char *at(const char *prefix, const char *rest)
{
    UINT32 a = length_of(prefix), b = length_of(rest), i;

    if (a + b >= PATH_BUFFER)
        give_up("a path fits its buffer", ERR_OUT_OF_RANGE);
    for (i = 0; i < a; i++)
        path[i] = prefix[i];
    for (i = 0; i < b; i++)
        path[a + i] = rest[i];
    path[a + b] = 0;
    return path;
}

static UINT64 next_random(UINT64 *state)
{
    UINT64 x = *state;

    x ^= x << XORSHIFT_A;
    x ^= x >> XORSHIFT_B;
    x ^= x << XORSHIFT_C;
    *state = x;
    return x;
}

/* count bytes of the stream seeded with seed into out, going on from
   *state, which starts as the seed with no zero bits lost. */
static void fill(UINT64 *state, UINT8 *out, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        out[i] = (UINT8)next_random(state);
}

static UINT64 start_state(UINT64 seed)
{
    return seed ^ SEED_NONZERO;
}

static void put64(UINT8 *p, UINT64 value)
{
    UINT32 i;

    for (i = 0; i < GENERATION_SIZE; i++)
        p[i] = (UINT8)(value >> (BITS_PER_BYTE * i));
}

static UINT64 get64(const UINT8 *p)
{
    UINT64 value = 0;
    UINT32 i;

    for (i = GENERATION_SIZE; i-- > 0;)
        value = value << BITS_PER_BYTE | p[i];
    return value;
}

static void read_launch(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[LAUNCH_FIRST_DIRECTORY + LAUNCH_VIEW_MAX];
    const LAUNCH_HEADER *header = (const LAUNCH_HEADER *)launch;
    const LAUNCH_VIEW_ENTRY *entries =
        (const LAUNCH_VIEW_ENTRY *)(launch + sizeof *header);
    char prefix[LAUNCH_PREFIX_MAX + 1];
    UINT32 bytes, count, i, j;

    if (channel_read(bootstrap, launch, sizeof launch, handles,
                     LAUNCH_FIRST_DIRECTORY + LAUNCH_VIEW_MAX, &bytes,
                     &count) != OK ||
        count < LAUNCH_FIRST_DIRECTORY ||
        handles[LAUNCH_DEBUG_LOG].type != TYPE_RESOURCE)
        process_exit(FSTEST_EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[LAUNCH_DEBUG_LOG].handle);
    handle_close(handles[LAUNCH_SELF].handle);
    if (bytes < sizeof *header ||
        header->view_count != FSTEST_VIEW_ENTRIES ||
        count != LAUNCH_FIRST_DIRECTORY + FSTEST_VIEW_ENTRIES ||
        header->argument_bytes == 0 ||
        header->argument_bytes > LAUNCH_ARGUMENTS_MAX ||
        bytes != sizeof *header + FSTEST_VIEW_ENTRIES * sizeof *entries +
                     header->argument_bytes)
        give_up("the launch message holds its view and a mode",
                ERR_INVALID_ARGS);
    for (i = 0; i < FSTEST_VIEW_ENTRIES; i++) {
        if (entries[i].length > LAUNCH_PREFIX_MAX)
            give_up("a view prefix fits its entry", ERR_INVALID_ARGS);
        for (j = 0; j < entries[i].length; j++)
            prefix[j] = (char)entries[i].prefix[j];
        prefix[j] = 0;
        if (view_add(&view, prefix,
                     handles[LAUNCH_FIRST_DIRECTORY + i].handle) != OK)
            give_up("a view entry is a prefix", ERR_INVALID_ARGS);
    }
    mode = (const char *)&entries[FSTEST_VIEW_ENTRIES];
    if (mode[header->argument_bytes - 1] != 0)
        give_up("the mode is a string", ERR_INVALID_ARGS);
}

/* ------------------------------------------------------------------------
   Files. */

/* Open the file at path with rights and IO_OPEN_ flags; a node that is
   not a file fails with ERR_BAD_STATE. */
static UINT32 open_file(const char *file, UINT32 rights, UINT32 flags,
                        UINT32 *channel, UINT64 *size)
{
    IO_ATTRIBUTES attributes;
    UINT32 status = view_create(&view, file, rights, flags, channel,
                                &attributes);

    *size = status == OK ? attributes.size : 0;
    if (status == OK && attributes.kind != IO_KIND_FILE) {
        handle_close(*channel);
        *channel = HANDLE_INVALID;
        status = ERR_BAD_STATE;
    }
    return status;
}

/* Write count bytes at offset, whole. */
static UINT32 write_all(UINT32 channel, UINT64 offset, const UINT8 *bytes,
                        UINT32 count)
{
    UINT32 written, status = io_write(channel, offset, bytes, count,
                                      &written);

    return status == OK && written != count ? ERR_BAD_STATE : status;
}

/* Read count bytes at offset into buffer, whole. */
static UINT32 read_all(UINT32 channel, UINT64 offset, UINT32 count)
{
    UINT32 got, status = io_read(channel, offset, buffer, count, &got);

    return status == OK && got != count ? ERR_BAD_STATE : status;
}

/* Whether the file holds size bytes of the stream from seed. */
static UINT32 matches(UINT32 channel, UINT64 seed, UINT64 offset,
                      UINT64 size)
{
    UINT64 state = start_state(seed);
    UINT32 n, status = OK;

    while (status == OK && size) {
        n = size < IO_TRANSFER_MAX ? (UINT32)size : IO_TRANSFER_MAX;
        fill(&state, expected, n);
        status = read_all(channel, offset, n);
        if (status == OK && !same(buffer, expected, n))
            status = ERR_BAD_STATE;
        offset += n;
        size -= n;
    }
    return status;
}

/* Write size bytes of the stream from seed at offset. */
static UINT32 write_stream(UINT32 channel, UINT64 seed, UINT64 offset,
                           UINT64 size)
{
    UINT64 state = start_state(seed);
    UINT32 n, status = OK;

    while (status == OK && size) {
        n = size < IO_TRANSFER_MAX ? (UINT32)size : IO_TRANSFER_MAX;
        fill(&state, expected, n);
        status = write_all(channel, offset, expected, n);
        offset += n;
        size -= n;
    }
    return status;
}

/* The directory at dir, opened with rights. */
static UINT32 open_directory(const char *dir, UINT32 rights, UINT32 *channel)
{
    IO_ATTRIBUTES attributes;

    return view_create(&view, dir, rights, IO_OPEN_CREATE | IO_OPEN_DIRECTORY,
                       channel, &attributes);
}

static UINT32 remove_name(UINT32 directory, const char *name)
{
    return io_remove(directory, (const UINT8 *)name, length_of(name));
}

static UINT32 rename_name(UINT32 directory, const char *from, const char *to,
                          UINT32 flags)
{
    return io_rename(directory, (const UINT8 *)from, length_of(from),
                     (const UINT8 *)to, length_of(to), flags);
}

/* ------------------------------------------------------------------------
   Persistence. */

/* With a block of the persisted file failing its check: a byte read from
   each block, and written back to it, a write into a block in part, so
   the service reads the block first. A block that fails its read fails
   the write the same way, and the others take it unchanged. */
static void write_around_damage(UINT32 channel)
{
    UINT32 i, read_status, write_status, damaged = 0;
    UINT8 byte;

    for (i = 0; i < PERSIST_BLOCKS; i++) {
        read_status = read_all(channel, (UINT64)i * BLOCK_SIZE, 1);
        byte = read_status == OK ? buffer[0] : 0;
        write_status = write_all(channel, (UINT64)i * BLOCK_SIZE, &byte, 1);
        damaged += read_status == ERR_DATA_INTEGRITY;
        check(read_status == write_status &&
                  (read_status == OK || read_status == ERR_DATA_INTEGRITY),
              "a write into a block in part fails as its read does");
    }
    say("nova-fstest: ");
    say_number(damaged);
    say(" blocks of /data/fstest/persist fail their check; a write into "
        "one in part fails the same way, and the others take writes\r\n");
}

static void count_boots(void)
{
    UINT32 channel, status;
    UINT64 size, boots = 0;

    status = open_file(at(FSTEST_DATA, "/fstest/boots"),
                       IO_RIGHT_READ | IO_RIGHT_WRITE, IO_OPEN_CREATE,
                       &channel, &size);
    if (status != OK)
        give_up("the boot count opens", status);
    if (size == GENERATION_SIZE && read_all(channel, 0, GENERATION_SIZE) == OK)
        boots = get64(buffer);
    check(size == 0 || size == GENERATION_SIZE, "the boot count is whole");
    put64(expected, boots + 1);
    check(write_all(channel, 0, expected, GENERATION_SIZE) == OK,
          "the boot count is written");
    handle_close(channel);
    say("nova-fstest: boot ");
    say_number(boots + 1);
    say(" of this data disk\r\n");
}

static void persist(void)
{
    UINT32 channel, status;
    UINT64 size;

    status = open_file(at(FSTEST_DATA, "/fstest/persist"),
                       IO_RIGHT_READ | IO_RIGHT_WRITE, IO_OPEN_CREATE,
                       &channel, &size);
    if (status != OK)
        give_up("the persisted file opens", status);
    if (size == 0) {
        status = write_stream(channel, PERSIST_SEED, 0, PERSIST_SIZE);
        if (status == OK)
            status = io_flush(channel);
        if (status == OK)
            status = matches(channel, PERSIST_SEED, 0, PERSIST_SIZE);
        say("nova-fstest: /data/fstest/persist written, ");
        say_number(PERSIST_SIZE);
        say(" bytes, flushed and read back, status ");
        say_number(status);
        say("\r\n");
        check(status == OK, "a file written and flushed reads back");
    } else {
        status = size == PERSIST_SIZE
                     ? matches(channel, PERSIST_SEED, 0, PERSIST_SIZE)
                     : ERR_BAD_STATE;
        say("nova-fstest: /data/fstest/persist from an earlier boot, ");
        say_number(size);
        say(" bytes, ");
        if (status == OK)
            say("reads back byte for byte\r\n");
        else if (status == ERR_DATA_INTEGRITY) {
            say("reports an integrity error, status 18\r\n");
            write_around_damage(channel);
        }
        else {
            say("does not read back, status ");
            say_number(status);
            say("\r\n");
        }
        check(status == OK, "a file from an earlier boot reads back");
    }
    handle_close(channel);
}

/* ------------------------------------------------------------------------
   Rights. */

/* A file extended by truncate past every block it holds reads zeros
   there, and the pool holding it opens at the next boot. */
static void sparse(void)
{
    UINT32 channel, status, i, zero = 1;
    UINT64 size;

    status = open_file(at(FSTEST_DATA, "/fstest/sparse"),
                       IO_RIGHT_READ | IO_RIGHT_WRITE, IO_OPEN_CREATE,
                       &channel, &size);
    if (status != OK)
        give_up("the sparse file opens", status);
    if (size == 0) {
        check(io_truncate(channel, SPARSE_SIZE) == OK,
              "an empty file is extended by truncate");
        size = SPARSE_SIZE;
    }
    check(size == SPARSE_SIZE, "the sparse file keeps its size");
    status = read_all(channel, SPARSE_SIZE - SPARSE_TAIL, SPARSE_TAIL);
    for (i = 0; status == OK && i < SPARSE_TAIL; i++)
        zero &= buffer[i] == 0;
    check(status == OK && zero, "a file extended by truncate reads zeros");
    handle_close(channel);
    say("nova-fstest: /data/fstest/sparse, ");
    say_number(SPARSE_SIZE);
    say(" bytes from truncate alone, reads zeros\r\n");
}

/* The name of entry n of the full directory, NUL-terminated, into out. */
static void full_name(char *out, UINT32 n)
{
    UINT32 i;

    for (i = 0; i < FULL_NAME_LENGTH - FULL_DIGITS; i++)
        out[i] = 'f';
    for (i = FULL_NAME_LENGTH; i-- > FULL_NAME_LENGTH - FULL_DIGITS;) {
        out[i] = (char)('0' + n % DECIMAL);
        n /= DECIMAL;
    }
    out[FULL_NAME_LENGTH] = 0;
}

/* A directory filled until a create is refused for want of room refuses
   a rename into it too, both before anything changes, and the service
   goes on serving: every entry is then removed. */
static void directory_full(void)
{
    static const char below[] = "full/";
    char name[FULL_NAME_LENGTH + 1], into[sizeof below + FULL_NAME_LENGTH];
    UINT32 full, parent, channel, status, count = 0, i;
    UINT64 size;

    if (open_directory(at(FSTEST_DATA, "/fstest/full"),
                       IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST,
                       &full) != OK ||
        open_directory(at(FSTEST_DATA, "/fstest"),
                       IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST,
                       &parent) != OK)
        give_up("the directory to fill opens", ERR_BAD_STATE);
    do {
        full_name(name, count);
        status = open_file(at(at(FSTEST_DATA, "/fstest/full/"), name),
                           IO_RIGHT_WRITE, IO_OPEN_CREATE | IO_OPEN_EXCLUSIVE,
                           &channel, &size);
        if (status == OK) {
            handle_close(channel);
            count++;
        }
    } while (status == OK);
    check(status == ERR_NO_RESOURCES && count > 0,
          "a full directory refuses a create for want of room");
    full_name(name, count);
    for (i = 0; i < sizeof below - 1; i++)
        into[i] = below[i];
    for (i = 0; i <= FULL_NAME_LENGTH; i++)
        into[sizeof below - 1 + i] = name[i];
    check(open_file(at(FSTEST_DATA, "/fstest/moving"), IO_RIGHT_WRITE,
                    IO_OPEN_CREATE, &channel, &size) == OK,
          "a file to move is created");
    handle_close(channel);
    check(rename_name(parent, "moving", into, 0) == ERR_NO_RESOURCES &&
              remove_name(parent, "moving") == OK,
          "a full directory refuses a rename into it, which leaves the file");
    for (i = count; i-- > 0;) {
        full_name(name, i);
        check(remove_name(full, name) == OK, "a full directory's entry goes");
    }
    handle_close(full);
    check(remove_name(parent, "full") == OK, "the emptied directory goes");
    handle_close(parent);
    say("nova-fstest: a directory full at ");
    say_number(count);
    say(" entries refuses a create and a rename into it, and empties\r\n");
}

static void rights(void)
{
    IO_ATTRIBUTES attributes;
    UINT32 channel, directory, status;
    UINT64 size;

    check(open_file(at(FSTEST_READ_ONLY, "/fstest/persist"),
                    IO_RIGHT_READ | IO_RIGHT_WRITE, 0, &channel,
                    &size) == ERR_ACCESS_DENIED,
          "the read-only view opens nothing for writing");
    check(open_file(at(FSTEST_READ_ONLY, "/fstest/denied"), IO_RIGHT_READ,
                    IO_OPEN_CREATE, &channel, &size) == ERR_ACCESS_DENIED,
          "the read-only view creates nothing");
    status = open_file(at(FSTEST_READ_ONLY, "/fstest/persist"),
                       IO_RIGHT_READ, 0, &channel, &size);
    if (check(status == OK, "the read-only view opens a file to read")) {
        check(write_all(channel, 0, expected, 1) == ERR_ACCESS_DENIED &&
                  io_truncate(channel, 0) == ERR_ACCESS_DENIED &&
                  io_flush(channel) == ERR_ACCESS_DENIED,
              "a file opened read-only is not written, truncated or flushed");
        handle_close(channel);
    }
    status = view_open(&view, at(FSTEST_READ_ONLY, "/fstest"),
                       IO_RIGHT_READ | IO_RIGHT_LIST, &directory,
                       &attributes);
    if (check(status == OK, "the read-only view opens its directory")) {
        check(remove_name(directory, "persist") == ERR_ACCESS_DENIED &&
                  rename_name(directory, "persist", "moved", 0) ==
                      ERR_ACCESS_DENIED,
              "the read-only directory removes and renames nothing");
        handle_close(directory);
    }
    status = open_file(at(FSTEST_DATA, "/fstest/persist"), IO_RIGHT_READ, 0,
                       &channel, &size);
    if (check(status == OK, "the read-write view opens a file to read")) {
        check(write_all(channel, 0, expected, 1) == ERR_ACCESS_DENIED,
              "a file opened to read alone is not written");
        handle_close(channel);
    }
    check(open_file(at(FSTEST_NAMESPACE, "/data/fstest/denied"),
                    IO_RIGHT_READ, IO_OPEN_CREATE, &channel,
                    &size) == ERR_ACCESS_DENIED &&
              open_file(at(FSTEST_NAMESPACE, "/data/fstest/persist"),
                        IO_RIGHT_READ | IO_RIGHT_WRITE, 0, &channel,
                        &size) == ERR_ACCESS_DENIED,
          "the namespace without WRITE creates and writes nothing below a "
          "publication");
    status = open_file(at(FSTEST_NAMESPACE, "/data/fstest/persist"),
                       IO_RIGHT_READ, 0, &channel, &size);
    if (check(status == OK, "the namespace opens a published file to read"))
        handle_close(channel);
    check(view_create(&view, FSTEST_NAMESPACE, IO_RIGHT_READ,
                      IO_OPEN_DIRECTORY, &channel,
                      &attributes) == ERR_INVALID_ARGS,
          "the directory flag without the create flag is refused");
    say("nova-fstest: what is opened without WRITE is not written, created, "
        "truncated, flushed, renamed or removed\r\n");
}

/* ------------------------------------------------------------------------
   Changes. */

static void changes(void)
{
    static const UINT8 first[] = "first";
    UINT32 directory, file, kept, inner, status;
    UINT64 size;

    if (open_directory(at(FSTEST_DATA, "/fstest"),
                       IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST,
                       &directory) != OK)
        give_up("/data/fstest opens", ERR_BAD_STATE);
    remove_name(directory, "scratch");
    remove_name(directory, "moved");
    remove_name(directory, "sub/inner");
    remove_name(directory, "sub");
    remove_name(directory, "a");
    remove_name(directory, "b");
    status = open_file(at(FSTEST_DATA, "/fstest/scratch"),
                       IO_RIGHT_READ | IO_RIGHT_WRITE,
                       IO_OPEN_CREATE | IO_OPEN_EXCLUSIVE, &kept, &size);
    if (status != OK)
        give_up("a file is created", status);
    check(write_all(kept, 0, first, sizeof first - 1) == OK,
          "a new file is written");
    check(open_file(at(FSTEST_DATA, "/fstest/scratch"), IO_RIGHT_READ,
                    IO_OPEN_CREATE | IO_OPEN_EXCLUSIVE, &file,
                    &size) == ERR_ALREADY_EXISTS &&
              open_directory(at(FSTEST_DATA, "/fstest/scratch"),
                             IO_RIGHT_READ, &file) == ERR_ALREADY_EXISTS,
          "an exclusive create, or a directory's, finds the file there");
    check(rename_name(directory, "scratch", "moved", 0) == OK &&
              open_file(at(FSTEST_DATA, "/fstest/scratch"), IO_RIGHT_READ, 0,
                        &file, &size) == ERR_NOT_FOUND,
          "a rename moves the name");
    check(read_all(kept, 0, sizeof first - 1) == OK &&
              same(buffer, first, sizeof first - 1),
          "a channel reaches its node across a rename");
    check(io_truncate(kept, 2) == OK && write_all(kept, 10, first, 1) == OK,
          "a file is truncated and written past its end");
    status = open_file(at(FSTEST_DATA, "/fstest/moved"), IO_RIGHT_READ, 0,
                       &file, &size);
    if (check(status == OK && size == 11, "the moved file has its new size")) {
        check(read_all(file, 0, 11) == OK && buffer[0] == 'f' &&
                  buffer[1] == 'i' && buffer[2] == 0 && buffer[9] == 0 &&
                  buffer[10] == 'f',
              "truncated bytes read back as zeros");
        handle_close(file);
    }
    status = open_directory(at(FSTEST_DATA, "/fstest/sub"),
                            IO_RIGHT_READ | IO_RIGHT_WRITE, &inner);
    if (check(status == OK, "a directory is created")) {
        handle_close(inner);
        check(open_file(at(FSTEST_DATA, "/fstest/sub/inner"), IO_RIGHT_READ,
                        IO_OPEN_CREATE, &file, &size) == OK,
              "a file is created in it");
        handle_close(file);
        check(remove_name(directory, "sub") == ERR_BAD_STATE,
              "a directory that is not empty is not removed");
        check(rename_name(directory, "sub", "sub/deeper", 0) ==
                  ERR_INVALID_ARGS,
              "a directory does not move into itself");
        check(remove_name(directory, "sub/inner") == OK &&
                  remove_name(directory, "sub") == OK,
              "a file and then its empty directory are removed");
    }
    check(open_file(at(FSTEST_DATA, "/fstest/a"), IO_RIGHT_WRITE,
                    IO_OPEN_CREATE, &file, &size) == OK &&
              write_all(file, 0, first, 1) == OK,
          "a file is created");
    handle_close(file);
    check(open_file(at(FSTEST_DATA, "/fstest/b"), IO_RIGHT_WRITE,
                    IO_OPEN_CREATE, &file, &size) == OK,
          "a second file is created");
    handle_close(file);
    check(rename_name(directory, "a", "b", 0) == ERR_ALREADY_EXISTS &&
              rename_name(directory, "a", "b", IO_RENAME_REPLACE) == OK,
          "a rename replaces only with IO_RENAME_REPLACE");
    status = open_file(at(FSTEST_DATA, "/fstest/b"), IO_RIGHT_READ, 0, &file,
                       &size);
    if (check(status == OK && size == 1, "the replacing file is there")) {
        handle_close(file);
    }
    check(remove_name(directory, "b") == OK &&
              remove_name(directory, "moved") == OK,
          "files are removed");
    check(read_all(kept, 0, 1) == ERR_NOT_FOUND,
          "a channel to a removed file answers ERR_NOT_FOUND");
    handle_close(kept);
    check(io_flush(directory) == OK, "the directory flushes");
    handle_close(directory);
    say("nova-fstest: creates, writes, truncates, renames and removes "
        "behave\r\n");
}

/* ------------------------------------------------------------------------
   The crash state. Generation g is the files a and b, each g and its
   stream; current, g alone; and odd, present when g is odd. Each
   generation's change ends in one flush. */

#define CRASH "/fstest/crash"

static UINT64 crash_size(UINT64 generation)
{
    return CRASH_UNIT * (generation % CRASH_UNITS + 1);
}

/* Whether the file at name holds generation's contents. */
static UINT32 crash_file_holds(const char *name, UINT64 generation)
{
    UINT32 channel, status;
    UINT64 size;

    status = open_file(at(FSTEST_DATA, name), IO_RIGHT_READ, 0, &channel,
                       &size);
    if (status != OK)
        return status;
    if (size != GENERATION_SIZE + crash_size(generation))
        status = ERR_BAD_STATE;
    if (status == OK)
        status = read_all(channel, 0, GENERATION_SIZE);
    if (status == OK && get64(buffer) != generation)
        status = ERR_BAD_STATE;
    if (status == OK)
        status = matches(channel, generation, GENERATION_SIZE,
                         crash_size(generation));
    handle_close(channel);
    return status;
}

/* Write generation's a, b, odd and current, the last by a rename over
   it, and flush, logging the flush before it is sent. */
static UINT32 crash_commit(UINT32 directory, UINT64 generation)
{
    static const char *const files[] = {CRASH "/a", CRASH "/b"};
    UINT32 channel, i, status = OK;
    UINT64 size;

    for (i = 0; status == OK && i < sizeof files / sizeof files[0]; i++) {
        status = open_file(at(FSTEST_DATA, files[i]),
                           IO_RIGHT_READ | IO_RIGHT_WRITE, IO_OPEN_CREATE,
                           &channel, &size);
        if (status != OK)
            break;
        put64(expected, generation);
        status = io_truncate(channel, 0);
        if (status == OK)
            status = write_all(channel, 0, expected, GENERATION_SIZE);
        if (status == OK)
            status = write_stream(channel, generation, GENERATION_SIZE,
                                  crash_size(generation));
        handle_close(channel);
    }
    if (status == OK && generation % 2) {
        status = open_file(at(FSTEST_DATA, CRASH "/odd"), IO_RIGHT_READ,
                           IO_OPEN_CREATE, &channel, &size);
        if (status == OK)
            handle_close(channel);
    } else if (status == OK && generation) {
        status = remove_name(directory, "odd");
    }
    if (status == OK)
        status = open_file(at(FSTEST_DATA, CRASH "/next"),
                           IO_RIGHT_READ | IO_RIGHT_WRITE,
                           IO_OPEN_CREATE | IO_OPEN_EXCLUSIVE, &channel,
                           &size);
    if (status == OK) {
        put64(expected, generation);
        status = write_all(channel, 0, expected, GENERATION_SIZE);
        handle_close(channel);
    }
    if (status == OK)
        status = rename_name(directory, "next", "current", IO_RENAME_REPLACE);
    if (status == OK) {
        say("nova-fstest: generation ");
        say_number(generation);
        say(" flushing\r\n");
        status = io_flush(directory);
    }
    return status;
}

/* The generation the crash state holds, checked whole: a, b and current
   agree, odd matches it, and next, which no flush leaves, is absent. A
   disk without the state gets generation 0. */
static UINT64 crash_check(UINT32 directory)
{
    UINT32 channel, status, odd;
    UINT64 size, generation;

    status = open_file(at(FSTEST_DATA, CRASH "/current"), IO_RIGHT_READ, 0,
                       &channel, &size);
    if (status == ERR_NOT_FOUND) {
        status = crash_commit(directory, 0);
        if (status != OK)
            give_up("the crash state is made", status);
        say("nova-fstest: crash state made at generation 0\r\n");
        return 0;
    }
    if (status == OK && size != GENERATION_SIZE)
        status = ERR_BAD_STATE;
    if (status == OK)
        status = read_all(channel, 0, GENERATION_SIZE);
    if (channel != HANDLE_INVALID)
        handle_close(channel);
    if (status != OK)
        give_up("the crash state's generation reads", status);
    generation = get64(buffer);
    status = crash_file_holds(CRASH "/a", generation);
    if (status == OK)
        status = crash_file_holds(CRASH "/b", generation);
    odd = open_file(at(FSTEST_DATA, CRASH "/odd"), IO_RIGHT_READ, 0, &channel,
                    &size);
    if (odd == OK)
        handle_close(channel);
    if (status == OK && (odd == OK) != (generation % 2 == 1))
        status = ERR_BAD_STATE;
    if (status == OK &&
        open_file(at(FSTEST_DATA, CRASH "/next"), IO_RIGHT_READ, 0, &channel,
                  &size) != ERR_NOT_FOUND)
        status = ERR_BAD_STATE;
    say("nova-fstest: crash state at generation ");
    say_number(generation);
    say(status == OK ? ", whole\r\n" : ", MIXED\r\n");
    if (status != OK)
        give_up("the crash state is one generation's", status);
    return generation;
}

static void crash_state(void)
{
    UINT32 directory, status;
    UINT64 generation;

    if (open_directory(at(FSTEST_DATA, CRASH),
                       IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST,
                       &directory) != OK)
        give_up("the crash state's directory opens", ERR_BAD_STATE);
    generation = crash_check(directory);
    do {
        generation++;
        say("nova-fstest: generation ");
        say_number(generation);
        say(" writing\r\n");
        status = crash_commit(directory, generation);
        if (status != OK)
            give_up("a generation commits", status);
        say("nova-fstest: generation ");
        say_number(generation);
        say(" committed\r\n");
    } while (same((const UINT8 *)mode, (const UINT8 *)FSTEST_LOOP,
                  sizeof FSTEST_LOOP));
    handle_close(directory);
}

_Noreturn void fstest_main(UINT32 bootstrap, UINT64 argument)
{
    UINT32 directory;

    (void)argument;
    read_launch(bootstrap);
    say("nova-fstest: running with /data, read-write and read-only, ");
    say(mode);
    say("\r\n");
    if (open_directory(at(FSTEST_DATA, "/fstest"),
                       IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST,
                       &directory) != OK)
        give_up("/data/fstest is made", ERR_BAD_STATE);
    handle_close(directory);
    count_boots();
    persist();
    sparse();
    rights();
    changes();
    directory_full();
    crash_state();
    if (failed)
        process_exit(FSTEST_EXIT_FAILED);
    say("nova-fstest: checks passed\r\n");
    process_exit(FSTEST_EXIT_PASSED);
}
