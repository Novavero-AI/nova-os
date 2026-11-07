/* init and the data disk (data.h; docs/spec/storage.md F18). The data
   disk is the second virtio block function in the order the platform
   driver walks PCI, whose first is the boot disk (platform.c). Its
   driver, \nova\virtblk.exe started with VIRTIO_BLK_SERVE, runs in a job
   of its own with the bootstrap message the platform driver gives a
   driver, resources covering only its function, and reports with the
   block protocol's client endpoint (virtio_blk.h); the file system
   service, \nova\fs.exe, takes that endpoint and the server end of its
   volume's root (services.h).
   init keeps the root, through which it creates /data and /users at
   first boot (F11, item 1) and publishes them in the namespace (F4).

   Neither the driver nor the service is restarted when it ends, and the
   function is not stopped then: its pins stay in quarantine, so no page
   the device may still reach is reused (ipc.md I8). */

#include "data.h"
#include "block_protocol.h"
#include "directory_protocol.h"
#include "fstest.h"
#include "init.h"
#include "launch.h"
#include "path.h"
#include "pci.h"
#include "services.h"
#include "virtio_blk.h"

#define BLOCK_FILE "virtblk.exe"
#define FS_FILE "fs.exe"

/* The process's own handle each program gets: MAP, to map sections into
   itself. */
#define SELF_RIGHTS (PROCESS_RIGHT_MAP | RIGHT_TRANSFER)

/* The driver's bootstrap channel carries its bootstrap message one way
   and its report the other. */
#define BOOTSTRAP_CAPACITY 1

/* A service's channel from init queues the requests init sends it, one
   at a time, and the namespace server's opens passed on through a
   publication. */
#define SERVICE_CHANNEL_CAPACITY 8

/* The names published, and the rights the directories behind them carry:
   read, write and list, which every program granted them narrows. */
#define DATA_NAME "data"
#define USERS_NAME "users"
#define DIRECTORY_RIGHTS (IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST)
#define READ_ONLY_RIGHTS (IO_RIGHT_READ | IO_RIGHT_LIST)
#define CREATE_DIRECTORY (IO_OPEN_CREATE | IO_OPEN_DIRECTORY)

/* init's end of the volume's root, or HANDLE_INVALID with no data disk. */
static UINT32 fs_root = HANDLE_INVALID;
static VIEW namespace_view;
static UINT8 message[CHANNEL_MAX_BYTES];

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

/* ------------------------------------------------------------------------
   The driver and the service. */

/* Close *handle if it holds one, and leave HANDLE_INVALID there. */
static void close_open(UINT32 *handle)
{
    if (*handle != HANDLE_INVALID)
        handle_close(*handle);
    *handle = HANDLE_INVALID;
}

/* Write count handles and byte_count bytes as program's bootstrap message
   and start it with argument. Every handle moves, into the process or
   into a message destroyed with its channel; each is HANDLE_INVALID
   after. */
static UINT32 launch(PROGRAM *program, const void *bytes, UINT32 byte_count,
                     HANDLE_TRANSFER *handles, UINT32 count, UINT64 argument,
                     UINT32 *mine)
{
    UINT32 theirs = HANDLE_INVALID, i, status;

    *mine = HANDLE_INVALID;
    status = channel_create(BOOTSTRAP_CAPACITY, mine, &theirs);
    if (status == OK)
        status = channel_write(*mine, bytes, byte_count, handles, count);
    if (status == OK)
        for (i = 0; i < count; i++)
            handles[i].handle = HANDLE_INVALID;
    if (status == OK)
        status = process_start(program->process, program->thread,
                               program->entry, program->stack, theirs,
                               argument);
    if (status == OK)
        theirs = HANDLE_INVALID;
    close_open(&theirs);
    close_open(&program->thread);
    close_transfers(handles, count);
    for (i = 0; i < count; i++)
        handles[i].handle = HANDLE_INVALID;
    if (status != OK)
        close_open(mine);
    return status;
}

/* Start the driver for the function device describes, serving, and
   return the endpoint it reports, or HANDLE_INVALID, logged. */
static UINT32 start_driver(const PLATFORM *platform,
                           const PCI_DEVICE_BOOTSTRAP *device)
{
    HANDLE_TRANSFER handles[PCI_HANDLES_MAX];
    RECEIVED_HANDLE endpoint;
    PROGRAM program;
    UINT32 job = HANDLE_INVALID, initiator = HANDLE_INVALID, count = 0;
    UINT32 mine = HANDLE_INVALID, transaction = 0, bytes = 0;
    UINT32 handle_count = 0, observed, status;

    program.process = HANDLE_INVALID;
    program.thread = HANDLE_INVALID;
    endpoint.handle = HANDLE_INVALID;
    job = platform_driver_job(platform);
    status = job == HANDLE_INVALID ? ERR_NO_MEMORY : OK;
    if (status == OK)
        status = initiator_create(platform->dma, device->requester,
                                  &initiator);
    if (status == OK)
        status = load_boot_file(job, boot_file(BLOCK_FILE), &program);
    if (status == OK) {
        count = platform_driver_handles(platform, device, program.process,
                                        initiator, handles);
        status = count ? OK : ERR_NO_MEMORY;
    }
    if (status == OK)
        status = launch(&program, device, sizeof *device, handles, count,
                        VIRTIO_BLK_SERVE, &mine);
    if (status == OK)
        status = object_wait_one(mine, CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = channel_read(mine, &transaction, sizeof transaction,
                              &endpoint, 1, &bytes, &handle_count);
    if (status == OK &&
        (bytes != VIRTIO_BLK_REPORT_SIZE || transaction != 0 ||
         handle_count != 1 || endpoint.type != TYPE_CHANNEL ||
         endpoint.rights != BLOCK_CLIENT_RIGHTS)) {
        if (handle_count)
            close_open(&endpoint.handle);
        status = ERR_BAD_STATE;
    }
    close_open(&mine);
    close_open(&program.thread);
    close_open(&program.process);
    close_open(&initiator);
    close_open(&job);
    if (status != OK) {
        say_status("the data disk's driver did not report", status);
        return HANDLE_INVALID;
    }
    return endpoint.handle;
}

/* Start the file system service over block, which moves into it, and
   keep init's end of its root. */
static void start_fs(const PLATFORM *platform, UINT32 job, UINT32 block)
{
    HANDLE_TRANSFER handles[FS_BOOTSTRAP_HANDLES];
    PROGRAM program;
    UINT32 i, mine = HANDLE_INVALID, status;

    for (i = 0; i < FS_BOOTSTRAP_HANDLES; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    handles[FS_BOOTSTRAP_BLOCK].handle = block;
    status = load_boot_file(job, boot_file(FS_FILE), &program);
    if (status == OK)
        status = resource_create(platform->debug_log,
                                 SYSTEM_PRIVILEGE_DEBUG_LOG, 1,
                                 &handles[FS_BOOTSTRAP_DEBUG_LOG].handle);
    if (status == OK)
        status = handle_duplicate(program.process, SELF_RIGHTS,
                                  &handles[FS_BOOTSTRAP_SELF].handle);
    if (status == OK)
        status = channel_create(SERVICE_CHANNEL_CAPACITY, &fs_root,
                                &handles[FS_BOOTSTRAP_ROOT].handle);
    if (status == OK)
        status = launch(&program, message, 0, handles, FS_BOOTSTRAP_HANDLES,
                        0, &mine);
    close_transfers(handles, FS_BOOTSTRAP_HANDLES);
    close_open(&mine);
    close_open(&program.thread);
    close_open(&program.process);
    if (status != OK) {
        close_open(&fs_root);
        say_status("the file system service did not start", status);
    }
}

void data_start(const PLATFORM *platform, UINT32 job)
{
    PCI_DEVICE_BOOTSTRAP device;
    UINT32 block;

    if (!platform_data_disk(platform, &device)) {
        say("nova-init: no data disk: no second virtio block function\r\n");
        return;
    }
    if (!boot_file(BLOCK_FILE) || !boot_file(FS_FILE)) {
        say("nova-init: no data disk: \\nova\\ lacks " BLOCK_FILE " or "
            FS_FILE "\r\n");
        return;
    }
    say("nova-init: the data disk is ");
    pci_say_function(device.requester);
    say("; starting its driver and the file system service\r\n");
    block = start_driver(platform, &device);
    if (block != HANDLE_INVALID)
        start_fs(platform, job, block);
}

/* ------------------------------------------------------------------------
   /data and /users. */

/* Send request, of length bytes in message with the count handles at
   handles, on channel, and read its reply on answer into message. Once
   sent, the handles are the receiver's, and HANDLE_INVALID at handles.
   The request is kept apart to check the reply against, since the reply
   overwrites it. */
static UINT32 call(UINT32 channel, UINT32 answer, UINT32 length,
                   HANDLE_TRANSFER *handles, UINT32 count,
                   UINT32 *reply_length)
{
    IO_REQUEST *request = (IO_REQUEST *)message, sent;
    UINT32 i, status = io_send(channel, request, length, handles, count);

    if (status != OK)
        return status;
    for (i = 0; i < count; i++)
        handles[i].handle = HANDLE_INVALID;
    sent = *request;
    return io_receive(answer, &sent, message, sizeof message, reply_length,
                      0);
}

static void request_for(UINT32 operation, const char *name, UINT32 rights,
                        UINT32 flags)
{
    IO_REQUEST *request = (IO_REQUEST *)message;

    request->operation = operation;
    request->offset = 0;
    request->length = length_of(name);
    request->rights = rights;
    request->flags = flags;
    request->reserved = 0;
    copy(message + sizeof *request, (const UINT8 *)name, request->length);
}

/* Open name in the volume's root, a directory created if missing, and
   publish it in the namespace. Its endpoint queues
   SERVICE_CHANNEL_CAPACITY messages, since the namespace server passes
   every open below the name on through it. */
static UINT32 mount(UINT32 namespace_root, const char *name)
{
    HANDLE_TRANSFER transfer;
    UINT32 mine = HANDLE_INVALID, reply_length, status;
    UINT32 length = sizeof(IO_REQUEST) + length_of(name);

    transfer.handle = HANDLE_INVALID;
    transfer.rights = RIGHTS_SAME;
    status = channel_create(SERVICE_CHANNEL_CAPACITY, &mine,
                            &transfer.handle);
    if (status == OK) {
        request_for(IO_OPEN, name, DIRECTORY_RIGHTS, CREATE_DIRECTORY);
        status = call(fs_root, mine, length, &transfer, 1, &reply_length);
    }
    if (status == OK &&
        (reply_length != sizeof(IO_REPLY) + sizeof(IO_ATTRIBUTES) ||
         ((const IO_ATTRIBUTES *)(message + sizeof(IO_REPLY)))->kind !=
             IO_KIND_DIRECTORY))
        status = ERR_BAD_STATE;
    if (status == OK) {
        request_for(IO_PUBLISH, name, 0, 0);
        transfer.handle = mine;
        mine = HANDLE_INVALID;
        status = call(namespace_root, namespace_root, length, &transfer, 1,
                      &reply_length);
    }
    close_open(&transfer.handle);
    close_open(&mine);
    return status;
}

/* Run the check program with /data, reached through the namespace,
   read-write and read-only, and its mode. */
static UINT32 run_check(UINT32 job, UINT32 debug_log)
{
    HANDLE_TRANSFER handles[LAUNCH_FIRST_DIRECTORY + FSTEST_VIEW_ENTRIES];
    LAUNCH_HEADER *header = (LAUNCH_HEADER *)message;
    LAUNCH_VIEW_ENTRY *entries =
        (LAUNCH_VIEW_ENTRY *)(message + sizeof *header);
    static const char *const prefixes[FSTEST_VIEW_ENTRIES] = {
        FSTEST_DATA, FSTEST_READ_ONLY, FSTEST_NAMESPACE};
    static const char *const paths[FSTEST_VIEW_ENTRIES] = {"/" DATA_NAME,
                                                           "/" DATA_NAME, "/"};
    static const UINT32 rights[FSTEST_VIEW_ENTRIES] = {
        DIRECTORY_RIGHTS, READ_ONLY_RIGHTS, READ_ONLY_RIGHTS};
    const char *mode = boot_file(FSTEST_LOOP_MARKER) ? FSTEST_LOOP
                                                     : FSTEST_ONCE;
    IO_ATTRIBUTES attributes;
    PROGRAM program;
    UINT32 i, j, mine = HANDLE_INVALID, observed, state, exit_code = 0;
    UINT32 status, mode_bytes = length_of(mode) + 1;

    for (i = 0; i < LAUNCH_FIRST_DIRECTORY + FSTEST_VIEW_ENTRIES; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    status = load_boot_file(job, boot_file(FSTEST_FILE), &program);
    if (status != OK)
        return status;
    if (resource_create(debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG, 1,
                        &handles[LAUNCH_DEBUG_LOG].handle) != OK ||
        handle_duplicate(program.process, SELF_RIGHTS,
                         &handles[LAUNCH_SELF].handle) != OK)
        status = ERR_NO_MEMORY;
    for (i = 0; status == OK && i < FSTEST_VIEW_ENTRIES; i++)
        status = view_open(&namespace_view, paths[i], rights[i],
                           &handles[LAUNCH_FIRST_DIRECTORY + i].handle,
                           &attributes);
    header->view_count = FSTEST_VIEW_ENTRIES;
    header->argument_bytes = mode_bytes;
    for (i = 0; i < FSTEST_VIEW_ENTRIES; i++) {
        for (j = 0; j < LAUNCH_PREFIX_MAX; j++)
            entries[i].prefix[j] = 0;
        entries[i].length = length_of(prefixes[i]);
        copy(entries[i].prefix, (const UINT8 *)prefixes[i],
             entries[i].length);
    }
    copy((UINT8 *)&entries[FSTEST_VIEW_ENTRIES], (const UINT8 *)mode,
         mode_bytes);
    if (status == OK)
        status = launch(&program, message,
                        sizeof *header + FSTEST_VIEW_ENTRIES * sizeof *entries +
                            mode_bytes,
                        handles, LAUNCH_FIRST_DIRECTORY + FSTEST_VIEW_ENTRIES,
                        0, &mine);
    close_transfers(handles, LAUNCH_FIRST_DIRECTORY + FSTEST_VIEW_ENTRIES);
    close_open(&mine);
    close_open(&program.thread);
    if (status != OK) {
        close_open(&program.process);
        return status;
    }
    say("nova-init: started \\nova\\" FSTEST_FILE " ");
    say(mode);
    say(" with a view of /data, read-write and read-only, and of the "
        "namespace without WRITE\r\n");
    if (object_wait_one(program.process, PROCESS_TERMINATED,
                        DEADLINE_INFINITE, &observed) != OK ||
        process_info(program.process, &state, &exit_code) != OK)
        exit_code = 0;
    handle_close(program.process);
    say("nova-init: \\nova\\" FSTEST_FILE " ended, exit code ");
    say_number(exit_code);
    say("\r\n");
    return exit_code == FSTEST_EXIT_PASSED ? OK : ERR_BAD_STATE;
}

void data_mount(UINT32 job, UINT32 debug_log, UINT32 namespace_root)
{
    UINT32 status;

    if (fs_root == HANDLE_INVALID)
        return;
    namespace_view.count = 0;
    status = view_add(&namespace_view, "/", namespace_root);
    if (status == OK)
        status = mount(namespace_root, DATA_NAME);
    if (status == OK)
        status = mount(namespace_root, USERS_NAME);
    if (status == OK)
        status = io_flush(fs_root);
    if (status != OK) {
        say_status("/data and /users are not published", status);
        return;
    }
    say("nova-init: /data and /users published from the data disk\r\n");
    if (!boot_file(FSTEST_FILE))
        return;
    status = run_check(job, debug_log);
    if (status != OK)
        say_status("the data disk's check did not pass", status);
}
