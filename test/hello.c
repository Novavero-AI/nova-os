/* A test program run from /store (docs/spec/storage.md F16). Its
   launcher starts it with a view of its own closure under /store and
   nothing else (F5, launch.h). It lists and reads what the view holds,
   follows the links its store path holds, takes a section of its own
   image, and checks that nothing outside the view answers: a store path
   outside its closure, which its launcher names as its one argument, a
   path that climbs with "..", and a write. It logs what it finds and
   exits with HELLO_EXIT_PASSED once every check passes, or
   HELLO_EXIT_FAILED at the first that does not.

   GREETING_PATH, the path of its greeting in /store, is defined when it
   is built, so the program records the path, which makes the greeting's
   store path a reference of its own (storage.md, fact 5). */

#include "hello.h"
#include "launch.h"
#include "log.h"
#include "native.h"
#include "path.h"
#include "store_image.h"

/* Where the section of its own image is mapped. */
#define SECTION_VIEW 0x0000000100000000ULL

/* The links of its store path, each to the greeting: absolute; relative,
   climbing out of the path; and as a Windows-built link names it. */
#define LINK_ABSOLUTE "share/greeting"
#define LINK_RELATIVE "share/greeting-relative"
#define LINK_WINDOWS "share/greeting-windows"

#define GREETING_MAX 256
#define PATH_BUFFER (LAUNCH_PREFIX_MAX + STORE_BASE_NAME_MAX + 64)
#define STORE_DIRECTORY "/store/"

static const char greeting_path[] = GREETING_PATH;
static UINT8 launch[CHANNEL_MAX_BYTES];
static VIEW view;
static const char *outside;
static UINT32 self = HANDLE_INVALID;

/* The other path /store lists beside the greeting: its own. */
static char own[STORE_BASE_NAME_MAX + 1];
static UINT32 store_entries;
static int greeting_listed;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    say("nova-hello: check failed: ");
    say(what);
    say("\r\n");
    process_exit(HELLO_EXIT_FAILED);
}

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

/* Join the NUL-terminated pieces into out, of PATH_BUFFER bytes; the
   first may be out itself. */
static void join(char *out, const char *a, const char *b, const char *c)
{
    const char *parts[3];
    UINT32 at = 0, i, j, n;

    parts[0] = a;
    parts[1] = b;
    parts[2] = c;
    for (i = 0; i < 3; i++) {
        n = length_of(parts[i]);
        check(n < PATH_BUFFER - at, "a path fits its buffer");
        for (j = 0; j < n; j++)
            out[at + j] = parts[i][j];
        at += n;
    }
    out[at] = 0;
}

/* The launch message: the debug log, the process itself, the view, and
   the one argument. */
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
        process_exit(HELLO_EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[LAUNCH_DEBUG_LOG].handle);
    self = handles[LAUNCH_SELF].handle;
    check(bytes >= sizeof *header && header->view_count <= LAUNCH_VIEW_MAX &&
              count == LAUNCH_FIRST_DIRECTORY + header->view_count &&
              header->argument_bytes <= LAUNCH_ARGUMENTS_MAX &&
              bytes == sizeof *header +
                           header->view_count * sizeof *entries +
                           header->argument_bytes,
          "the launch message holds a view and arguments");
    for (i = 0; i < header->view_count; i++) {
        check(entries[i].length <= LAUNCH_PREFIX_MAX,
              "a view prefix fits its entry");
        for (j = 0; j < entries[i].length; j++)
            prefix[j] = (char)entries[i].prefix[j];
        prefix[j] = 0;
        check(view_add(&view, prefix,
                       handles[LAUNCH_FIRST_DIRECTORY + i].handle) == OK,
              "a view entry is a prefix");
    }
    outside = (const char *)entries + header->view_count * sizeof *entries;
    check(header->argument_bytes > 0 &&
              outside[header->argument_bytes - 1] == 0,
          "the one argument is a string");
}

/* ------------------------------------------------------------------------
   Listings. */

static void say_entry(void *context, const UINT8 *name, UINT32 length,
                      UINT32 kind)
{
    (void)context;
    say(" ");
    say_printable(name, length);
    say(kind == IO_KIND_DIRECTORY ? "/" : kind == IO_KIND_LINK ? "@" : "");
}

static void count_root(void *context, const UINT8 *name, UINT32 length,
                       UINT32 kind)
{
    UINT32 *count = context;

    say_entry(0, name, length, kind);
    check(length == length_of("store") &&
              same(name, (const UINT8 *)"store", length) &&
              kind == IO_KIND_DIRECTORY,
          "/ holds only the view's store");
    (*count)++;
}

/* The greeting's base name is GREETING_PATH's after /store/; the other
   entry is its own path. */
static void note_store(void *context, const UINT8 *name, UINT32 length,
                       UINT32 kind)
{
    const char *greeting = greeting_path + length_of(STORE_DIRECTORY);
    UINT32 i;

    (void)context;
    say_entry(0, name, length, kind);
    store_entries++;
    if (length == length_of(greeting) &&
        same(name, (const UINT8 *)greeting, length)) {
        greeting_listed = 1;
        return;
    }
    check(length <= STORE_BASE_NAME_MAX && length > STORE_HASH_CHARS + 1 &&
              length - STORE_HASH_CHARS - 1 == length_of(HELLO_NAME) &&
              same(name + STORE_HASH_CHARS + 1, (const UINT8 *)HELLO_NAME,
                   length_of(HELLO_NAME)),
          "/store lists the greeting and its own path only");
    for (i = 0; i < length; i++)
        own[i] = (char)name[i];
    own[length] = 0;
}

static void list_view(void)
{
    UINT32 count = 0;

    say("nova-hello: / holds");
    check(view_list(&view, "/", count_root, &count) == OK && count == 1,
          "/ lists");
    say("\r\nnova-hello: /store holds");
    check(view_list(&view, "/store", note_store, 0) == OK, "/store lists");
    say("\r\n");
    check(store_entries == 2 && greeting_listed && own[0],
          "/store lists the greeting and its own path only");
}

/* ------------------------------------------------------------------------
   Reads. */

/* The file at path, whole, into out, at most GREETING_MAX bytes. */
static UINT32 read_whole(const char *path, UINT8 *out, UINT32 *length)
{
    IO_ATTRIBUTES attributes;
    UINT32 channel, status;

    status = view_open(&view, path, IO_RIGHT_READ, &channel, &attributes);
    if (status != OK)
        return status;
    status = attributes.kind == IO_KIND_FILE &&
                     attributes.size <= GREETING_MAX
                 ? io_read(channel, 0, out, GREETING_MAX, length)
                 : ERR_NOT_SUPPORTED;
    if (status == OK && *length != attributes.size)
        status = ERR_BAD_STATE;
    handle_close(channel);
    return status;
}

static void read_greeting(void)
{
    static const char *const links[] = {LINK_ABSOLUTE, LINK_RELATIVE,
                                        LINK_WINDOWS};
    UINT8 greeting[GREETING_MAX], through[GREETING_MAX];
    char path[PATH_BUFFER];
    UINT32 length, through_length, i;

    check(read_whole(greeting_path, greeting, &length) == OK && length > 0,
          "the greeting reads");
    say("nova-hello: ");
    say(greeting_path);
    say(" says: ");
    say_printable(greeting, length - (greeting[length - 1] == '\n'));
    say("\r\n");
    for (i = 0; i < sizeof links / sizeof links[0]; i++) {
        join(path, STORE_DIRECTORY, own, "/");
        join(path, path, links[i], "");
        check(read_whole(path, through, &through_length) == OK &&
                  through_length == length && same(through, greeting, length),
              "a link in its path reaches the greeting");
        say("nova-hello: ");
        say(path);
        say(" leads to the greeting, resolved in its own view\r\n");
    }
}

/* Its own image as a section: one it may read and map, never write. */
static void take_section(void)
{
    IO_ATTRIBUTES attributes;
    RECEIVED_HANDLE section;
    char path[PATH_BUFFER];
    UINT32 channel, status;
    UINT64 size;
    const UINT8 *mapped;

    join(path, STORE_DIRECTORY, own, "/" HELLO_FILE);
    check(view_open(&view, path, IO_RIGHT_READ | IO_RIGHT_EXECUTE, &channel,
                    &attributes) == OK &&
              attributes.kind == IO_KIND_FILE &&
              (attributes.flags & IO_EXECUTABLE),
          "its own image opens, executable");
    check(io_get_section(channel, IO_RIGHT_READ | IO_RIGHT_WRITE, &section,
                         &size) == ERR_INVALID_ARGS,
          "a section is not given with WRITE");
    status = io_get_section(channel, IO_RIGHT_READ, &section, &size);
    check(status == OK && size == attributes.size &&
              section.rights == (SECTION_RIGHT_READ | RIGHT_TRANSFER),
          "its image's section comes with READ alone");
    check(process_map(self, section.handle, 0, SECTION_VIEW, PAGE_SIZE,
                      MAP_READ | MAP_WRITE) == ERR_ACCESS_DENIED,
          "its image's section does not map writable");
    check(process_map(self, section.handle, 0, SECTION_VIEW, PAGE_SIZE,
                      MAP_READ) == OK,
          "its image's section maps readable");
    mapped = (const UINT8 *)SECTION_VIEW;
    check(mapped[0] == 'M' && mapped[1] == 'Z', "its image starts with MZ");
    handle_close(section.handle);
    handle_close(channel);
    check(view_open(&view, path, IO_RIGHT_WRITE, &channel, &attributes) ==
              ERR_ACCESS_DENIED,
          "its image does not open for writing");
    say("nova-hello: ");
    say(path);
    say(" maps as a section it can read and cannot write\r\n");
}

/* Nothing outside the view answers. */
static void check_outside(void)
{
    IO_ATTRIBUTES attributes;
    char path[PATH_BUFFER];
    const char *base = outside + length_of(STORE_DIRECTORY);
    UINT32 channel, status;

    status = view_open(&view, outside, IO_RIGHT_READ, &channel, &attributes);
    say("nova-hello: ");
    say(outside);
    say(", outside its closure, gives status ");
    say_number(status);
    say("\r\n");
    check(status == ERR_NOT_FOUND, "a path outside its closure is not found");
    join(path, STORE_DIRECTORY, own, "/bin/../../../../store/");
    join(path, path, base, "");
    status = view_open(&view, path, IO_RIGHT_READ, &channel, &attributes);
    say("nova-hello: ");
    say(path);
    say(" gives status ");
    say_number(status);
    say("\r\n");
    check(status == ERR_NOT_FOUND, ".. reaches no path outside its closure");
}

_Noreturn void hello_main(UINT32 bootstrap, UINT64 argument)
{
    (void)argument;
    read_launch(bootstrap);
    say("nova-hello: running from /store, with a view of its closure\r\n");
    list_view();
    read_greeting();
    take_section();
    check_outside();
    say("nova-hello: checks passed\r\n");
    process_exit(HELLO_EXIT_PASSED);
}
