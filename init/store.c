/* init as the launcher of /store's first programs (docs/spec/storage.md
   F16). It reads the store image nova-boot loaded, which the kernel
   passed as a section init may only read, with the store service's own
   reader, finds the store service in the path the image's header names,
   checks its blob and starts it with the image, a control channel and
   the store's root directory. It opens the namespace server's image in
   that path through the store service and starts it, and publishes the
   store's root in the namespace as /store, its only name. Through the
   namespace it finds the test program by path, asks the store service
   for the program's closure directory, and starts the program with a
   view of that directory under /store and nothing else (F5), and with
   one argument, a store path outside the closure. It reads the
   program's exit code, closes its own channels to both services, which
   end once no channel to them is left, and waits for them. While the
   namespace server runs, the data disk's /data and /users are published
   beside /store and checked (data.c). */

#include "init.h"
#include "data.h"
#include "directory_protocol.h"
#include "hello.h"
#include "launch.h"
#include "loader.h"
#include "path.h"
#include "reader.h"
#include "services.h"

#define STORE_PROGRAM "bin/store.exe"
#define NAMESPACE_PROGRAM "bin/namespace.exe"
#define STORE_NAME "store"
#define STORE_DIRECTORY "/store/"

/* The requests a service's channel from init queues before the service
   reads them: init sends one at a time, the namespace server one open
   per client open it passes on. */
#define SERVICE_CHANNEL_CAPACITY 8

/* The process's own handle each program gets: MAP, to map sections into
   itself, and not CREATE_THREAD, which none of these programs uses. */
#define SELF_RIGHTS (PROCESS_RIGHT_MAP | RIGHT_TRANSFER)
#define IMAGE_RIGHTS (SECTION_RIGHT_READ | RIGHT_TRANSFER)

/* What the program's view grants: read and list its closure, and map
   its files executable, which a launcher does for it. */
#define CLOSURE_RIGHTS (IO_RIGHT_READ | IO_RIGHT_EXECUTE | IO_RIGHT_LIST)

typedef struct {
    UINT32 process;
    const char *name;
} SERVICE;

static UINT8 message[CHANNEL_MAX_BYTES];

/* The view init reaches the store service's root by, and then the
   namespace's root by, each larger than a stack frame may grow without
   probing its pages. */
static VIEW store_view, namespace_view;
static char hello_path[LAUNCH_PREFIX_MAX + STORE_BASE_NAME_MAX];
static char program_path[LAUNCH_PREFIX_MAX + STORE_BASE_NAME_MAX];
static UINT8 hello_base[STORE_BASE_NAME_MAX];
static UINT32 hello_base_length;

static UINT32 length_of(const char *text)
{
    UINT32 n = 0;

    while (text[n])
        n++;
    return n;
}

static void copy(UINT8 *to, const UINT8 *from, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

static int same(const UINT8 *a, const UINT8 *b, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* The NUL-terminated text of the pieces, into out of size bytes. */
static int join(char *out, UINT32 size, const char *a, const UINT8 *b,
                UINT32 b_length, const char *c)
{
    UINT32 a_length = length_of(a), c_length = length_of(c);

    if (a_length + b_length + c_length >= size)
        return 0;
    copy((UINT8 *)out, (const UINT8 *)a, a_length);
    copy((UINT8 *)out + a_length, b, b_length);
    copy((UINT8 *)out + a_length + b_length, (const UINT8 *)c, c_length);
    out[a_length + b_length + c_length] = 0;
    return 1;
}

static void say_status(const char *what, UINT32 status)
{
    say("nova-init: ");
    say(what);
    say(", status ");
    say_number(status);
    say("\r\n");
}

static void close_transfers(const HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (handles[i].handle != HANDLE_INVALID)
            handle_close(handles[i].handle);
}

/* Start program with a bootstrap message of byte_count bytes at bytes
   and the count handles, which move into it. Returns its process's
   handle, or HANDLE_INVALID with the handles closed: by init if the
   message was not written, or with the message, whose handles close as
   it is destroyed unread (ipc.md I2), if the program did not start. */
static UINT32 start(PROGRAM *program, const void *bytes, UINT32 byte_count,
                    const HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 mine, theirs, status = channel_create(1, &mine, &theirs);
    int written = 0;

    if (status == OK) {
        status = channel_write(mine, bytes, byte_count, handles, count);
        written = status == OK;
        handle_close(mine);
        if (status == OK)
            status = loader_start(program, theirs);
        if (status != OK)
            handle_close(theirs);
    }
    if (status != OK) {
        if (!written)
            close_transfers(handles, count);
        say_status("a program from /store did not start", status);
        if (program->thread != HANDLE_INVALID)
            handle_close(program->thread);
        handle_close(program->process);
        return HANDLE_INVALID;
    }
    return program->process;
}

/* A sub-resource for the debug log, and program's own process with
   MAP, as the first two handles of its bootstrap message. Returns 0,
   with both HANDLE_INVALID, if either call fails. */
static int common_handles(UINT32 debug_log, const PROGRAM *program,
                          HANDLE_TRANSFER *handles)
{
    handles[0].rights = RIGHTS_SAME;
    handles[1].rights = RIGHTS_SAME;
    handles[1].handle = HANDLE_INVALID;
    if (resource_create(debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG, 1,
                        &handles[0].handle) != OK)
        return 0;
    if (handle_duplicate(program->process, SELF_RIGHTS, &handles[1].handle) !=
        OK) {
        handle_close(handles[0].handle);
        handles[0].handle = HANDLE_INVALID;
        return 0;
    }
    return 1;
}

/* Wait for process to end and report its exit code, 0 if it is not
   seen to end. */
static UINT32 end_of(UINT32 process, const char *name)
{
    UINT32 observed, state = 0, exit_code = 0;

    if (object_wait_one(process, PROCESS_TERMINATED, DEADLINE_INFINITE,
                        &observed) != OK ||
        process_info(process, &state, &exit_code) != OK ||
        state != PROCESS_STATE_TERMINATED)
        exit_code = 0;
    handle_close(process);
    say("nova-init: ");
    say(name);
    say(" ended, exit code ");
    say_number(exit_code);
    say("\r\n");
    return exit_code;
}

/* ------------------------------------------------------------------------
   The store service, from the image itself. */

/* The file at path, '/'-separated, below the path the image's header
   names, its blob checked whole. */
static UINT32 boot_program(const STORE_IMAGE *image, const char *path,
                           STORE_BLOB *blob)
{
    STORE_REGISTRATION registration;
    STORE_NODE node;
    const UINT8 *child;
    UINT32 index, at = 0, n, status;

    if (!store_registration_find(image, image->boot_name,
                                 image->boot_name_length, &index))
        return ERR_NOT_FOUND;
    store_registration_get(image, index, &registration);
    status = store_node_get(image, registration.root, &node);
    while (status == OK && path[at]) {
        for (n = 0; path[at + n] && path[at + n] != '/'; n++)
            ;
        if (node.kind != STORE_NODE_DIRECTORY ||
            !store_entry_find(&node, (const UINT8 *)path + at, n, &child))
            return ERR_NOT_FOUND;
        status = store_node_get(image, child, &node);
        at += path[at + n] ? n + 1 : n;
    }
    if (status == OK && node.kind != STORE_NODE_FILE)
        status = ERR_NOT_FOUND;
    if (status == OK)
        status = store_blob_get(image, &node, blob);
    if (status == OK)
        status = store_blob_check(blob);
    return status;
}

/* Start the store service over the image in file, opened at *image.
   Returns its process, with init's ends of its control channel and root
   directory at *control and *root; or HANDLE_INVALID with nothing
   open. */
static UINT32 start_store(UINT32 job, UINT32 debug_log,
                          const BOOT_FILE_VIEW *file, STORE_IMAGE *image,
                          UINT32 *control, UINT32 *root)
{
    HANDLE_TRANSFER handles[STORE_BOOTSTRAP_HANDLES];
    STORE_BOOTSTRAP boot;
    STORE_BLOB blob;
    PROGRAM program;
    UINT32 i, process, status = store_image_open(image, file->data, file->size);

    *control = HANDLE_INVALID;
    *root = HANDLE_INVALID;
    if (status != OK) {
        say_status("the store image does not open", status);
        return HANDLE_INVALID;
    }
    say("nova-init: store image of ");
    say_number(file->size);
    say(" bytes, ");
    say_number(image->registration_count);
    say(" paths; the store service is ");
    say(STORE_DIRECTORY);
    say_printable(image->boot_name, image->boot_name_length);
    say("/" STORE_PROGRAM "\r\n");
    status = boot_program(image, STORE_PROGRAM, &blob);
    if (status == OK)
        status = loader_load_image(job, blob.data, blob.length, &program);
    if (status != OK) {
        say_status("the store service does not load", status);
        return HANDLE_INVALID;
    }
    for (i = 0; i < STORE_BOOTSTRAP_HANDLES; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    if (!common_handles(debug_log, &program, handles) ||
        handle_duplicate(file->section, IMAGE_RIGHTS,
                         &handles[STORE_BOOTSTRAP_IMAGE].handle) != OK ||
        channel_create(SERVICE_CHANNEL_CAPACITY, control,
                       &handles[STORE_BOOTSTRAP_CONTROL].handle) != OK ||
        channel_create(SERVICE_CHANNEL_CAPACITY, root,
                       &handles[STORE_BOOTSTRAP_ROOT].handle) != OK) {
        close_transfers(handles, STORE_BOOTSTRAP_HANDLES);
        handle_close(program.thread);
        handle_close(program.process);
        process = HANDLE_INVALID;
    } else {
        boot.image_size = file->size;
        process = start(&program, &boot, sizeof boot, handles,
                        STORE_BOOTSTRAP_HANDLES);
    }
    if (process == HANDLE_INVALID) {
        if (*control != HANDLE_INVALID)
            handle_close(*control);
        if (*root != HANDLE_INVALID)
            handle_close(*root);
        *control = HANDLE_INVALID;
        *root = HANDLE_INVALID;
    }
    return process;
}

/* ------------------------------------------------------------------------
   Programs from /store, through the directory protocol. */

/* Load the program at path in view: its file's section, which init maps
   to read until it is laid out in a new process. */
static UINT32 load_from_store(UINT32 job, const VIEW *view, const char *path,
                              PROGRAM *program)
{
    IO_ATTRIBUTES attributes;
    RECEIVED_HANDLE section;
    UINT32 channel, status, unmapped;
    UINT64 size = 0;
    const UINT8 *mapped = 0;

    status = view_open(view, path, IO_RIGHT_READ, &channel, &attributes);
    if (status != OK)
        return status;
    status = io_get_section(channel, IO_RIGHT_READ, &section, &size);
    handle_close(channel);
    if (status != OK)
        return status;
    if (size)
        mapped = map_into_init(section.handle, size, MAP_READ);
    handle_close(section.handle);
    if (!mapped)
        return ERR_NO_MEMORY;
    status = loader_load_image(job, mapped, size, program);
    unmapped = unmap_from_init(mapped, size);
    if (status == OK && unmapped != OK) {
        handle_close(program->thread);
        handle_close(program->process);
        status = unmapped;
    }
    return status;
}

static UINT32 start_namespace(UINT32 job, UINT32 debug_log, UINT32 store_root,
                              const char *path, UINT32 *root)
{
    HANDLE_TRANSFER handles[NAMESPACE_BOOTSTRAP_HANDLES];
    PROGRAM program;
    UINT32 process, status;

    *root = HANDLE_INVALID;
    store_view.count = 0;
    status = view_add(&store_view, "/store", store_root);
    if (status == OK)
        status = load_from_store(job, &store_view, path, &program);
    if (status != OK) {
        say_status("the namespace server does not load", status);
        return HANDLE_INVALID;
    }
    handles[NAMESPACE_BOOTSTRAP_DEBUG_LOG].handle = HANDLE_INVALID;
    handles[NAMESPACE_BOOTSTRAP_ROOT].handle = HANDLE_INVALID;
    handles[NAMESPACE_BOOTSTRAP_ROOT].rights = RIGHTS_SAME;
    handles[NAMESPACE_BOOTSTRAP_DEBUG_LOG].rights = RIGHTS_SAME;
    if (resource_create(debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG, 1,
                        &handles[NAMESPACE_BOOTSTRAP_DEBUG_LOG].handle) !=
            OK ||
        channel_create(SERVICE_CHANNEL_CAPACITY, root,
                       &handles[NAMESPACE_BOOTSTRAP_ROOT].handle) != OK) {
        close_transfers(handles, NAMESPACE_BOOTSTRAP_HANDLES);
        handle_close(program.thread);
        handle_close(program.process);
        process = HANDLE_INVALID;
    } else {
        process = start(&program, message, 0, handles,
                        NAMESPACE_BOOTSTRAP_HANDLES);
    }
    if (process == HANDLE_INVALID && *root != HANDLE_INVALID) {
        handle_close(*root);
        *root = HANDLE_INVALID;
    }
    return process;
}

/* Publish *directory under name. Once the request is sent the handle
   is the namespace server's, and *directory HANDLE_INVALID. The reply is
   read over the request, so the request is kept apart to check the
   reply's transaction against. */
static UINT32 publish(UINT32 root, const char *name, UINT32 *directory)
{
    IO_REQUEST *request = (IO_REQUEST *)message, sent;
    HANDLE_TRANSFER transfer;
    UINT32 length = length_of(name), reply_length, status;

    request->operation = IO_PUBLISH;
    request->offset = 0;
    request->length = length;
    request->rights = 0;
    request->flags = 0;
    request->reserved = 0;
    copy(message + sizeof *request, (const UINT8 *)name, length);
    transfer.handle = *directory;
    transfer.rights = RIGHTS_SAME;
    status = io_send(root, request, sizeof *request + length, &transfer, 1);
    if (status != OK)
        return status;
    *directory = HANDLE_INVALID;
    sent = *request;
    return io_receive(root, &sent, message, sizeof message, &reply_length,
                      0);
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

/* Note the test program's path, the one whose name is HELLO_NAME. */
static void find_hello(void *context, const UINT8 *name, UINT32 length,
                       UINT32 kind)
{
    UINT32 name_length = length_of(HELLO_NAME);

    say_entry(context, name, length, kind);
    if (length == STORE_HASH_CHARS + 1 + name_length &&
        same(name + STORE_HASH_CHARS + 1, (const UINT8 *)HELLO_NAME,
             name_length)) {
        copy(hello_base, name, length);
        hello_base_length = length;
    }
}

static UINT32 list(const VIEW *view, const char *path,
                   void (*each)(void *, const UINT8 *, UINT32, UINT32))
{
    UINT32 status;

    say("nova-init: ");
    say(path);
    say(" holds");
    status = view_list(view, path, each, 0);
    say("\r\n");
    return status;
}

/* ------------------------------------------------------------------------
   The test program. */

/* Start the test program, its file at path in view, with its closure,
   whose directory moves into it, as its view's /store, and outside as
   its argument. */
static UINT32 start_hello(UINT32 job, UINT32 debug_log, const VIEW *view,
                          const char *path, UINT32 closure,
                          const char *outside)
{
    HANDLE_TRANSFER handles[LAUNCH_FIRST_DIRECTORY + 1];
    LAUNCH_HEADER *header = (LAUNCH_HEADER *)message;
    LAUNCH_VIEW_ENTRY *entry =
        (LAUNCH_VIEW_ENTRY *)(message + sizeof *header);
    UINT8 *argument = message + sizeof *header + sizeof *entry;
    UINT32 i, status, outside_length = length_of(outside) + 1;
    PROGRAM program;

    status = load_from_store(job, view, path, &program);
    if (status != OK) {
        handle_close(closure);
        say_status("the test program does not load", status);
        return HANDLE_INVALID;
    }
    if (!common_handles(debug_log, &program, handles)) {
        handle_close(closure);
        handle_close(program.thread);
        handle_close(program.process);
        return HANDLE_INVALID;
    }
    handles[LAUNCH_FIRST_DIRECTORY].handle = closure;
    handles[LAUNCH_FIRST_DIRECTORY].rights = RIGHTS_SAME;
    header->view_count = 1;
    header->argument_bytes = outside_length;
    entry->length = length_of("/store");
    for (i = 0; i < LAUNCH_PREFIX_MAX; i++)
        entry->prefix[i] = 0;
    copy(entry->prefix, (const UINT8 *)"/store", entry->length);
    copy(argument, (const UINT8 *)outside, outside_length);
    return start(&program, message,
                 sizeof *header + sizeof *entry + outside_length, handles,
                 LAUNCH_FIRST_DIRECTORY + 1);
}

int store_run(UINT32 job, UINT32 debug_log, const BOOT_FILE_VIEW *image)
{
    STORE_IMAGE store_image;
    SERVICE store, names;
    IO_ATTRIBUTES attributes;
    char outside[LAUNCH_PREFIX_MAX + STORE_BASE_NAME_MAX];
    UINT32 control, store_root, root = HANDLE_INVALID, closure, hello;
    UINT32 status = OK, store_exit, names_exit = 0, hello_exit = 0;

    store.name = "the store service";
    names.name = "the namespace server";
    names.process = HANDLE_INVALID;
    namespace_view.count = 0;
    store.process =
        start_store(job, debug_log, image, &store_image, &control, &store_root);
    if (store.process == HANDLE_INVALID)
        return 0;
    if (!join(program_path, sizeof program_path, STORE_DIRECTORY,
              store_image.boot_name, store_image.boot_name_length,
              "/" NAMESPACE_PROGRAM) ||
        !join(outside, sizeof outside, STORE_DIRECTORY, store_image.boot_name,
              store_image.boot_name_length, ""))
        status = ERR_OUT_OF_RANGE;
    if (status == OK) {
        names.process =
            start_namespace(job, debug_log, store_root, program_path, &root);
        if (names.process == HANDLE_INVALID)
            status = ERR_BAD_STATE;
    }
    if (status == OK)
        status = publish(root, STORE_NAME, &store_root);
    if (status == OK)
        data_mount(job, debug_log, root);
    if (status == OK)
        status = view_add(&namespace_view, "/", root);
    if (status == OK)
        status = list(&namespace_view, "/", say_entry);
    if (status == OK)
        status = list(&namespace_view, "/store", find_hello);
    if (status == OK && !hello_base_length)
        status = ERR_NOT_FOUND;
    if (status == OK)
        status = io_open(control, STORE_OPEN_CLOSURE, hello_base,
                         hello_base_length, CLOSURE_RIGHTS, &closure,
                         &attributes);
    if (status == OK &&
        !join(hello_path, sizeof hello_path, STORE_DIRECTORY, hello_base,
              hello_base_length, "/" HELLO_FILE)) {
        handle_close(closure);
        status = ERR_OUT_OF_RANGE;
    }
    if (status == OK) {
        hello = start_hello(job, debug_log, &namespace_view, hello_path,
                            closure, outside);
        if (hello != HANDLE_INVALID) {
            say("nova-init: started ");
            say(hello_path);
            say(" with a view of its closure under /store\r\n");
            hello_exit = end_of(hello, hello_path);
        }
    } else {
        say_status("the test program did not start from /store", status);
    }
    if (store_root != HANDLE_INVALID)
        handle_close(store_root);
    if (root != HANDLE_INVALID)
        handle_close(root);
    handle_close(control);
    if (names.process != HANDLE_INVALID)
        names_exit = end_of(names.process, names.name);
    store_exit = end_of(store.process, store.name);
    return hello_exit == HELLO_EXIT_PASSED && names_exit == SERVICE_EXIT_DONE &&
           store_exit == SERVICE_EXIT_DONE;
}
