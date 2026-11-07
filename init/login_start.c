/* Starting and keeping the accounts service and login (login_start.h).
   Both come from the nova-session store path, whose name init finds by
   listing /store, as store.c finds its test program, and the supervisor
   starts each again when it ends (platform_keep). Each start is a new
   process in a new job, given what it holds afresh: the service
   /data/accounts and /users and a control channel, login the store, a
   store control channel of its own and a job for its sessions with
   CREATE_PROCESS alone (accounts.md A1, A7).

   init keeps its end of the service's control channel and of login's
   bootstrap channel, and after each start of either joins the two with
   a new login channel, one end to each (accounts_protocol.h,
   session.h); after each start of login and of the console it gives
   login a new console session. A restart of one therefore reaches the
   other without a restart of its own. */

#include "login_start.h"
#include "accounts_protocol.h"
#include "console.h"
#include "console_protocol.h"
#include "directory_protocol.h"
#include "path.h"
#include "services.h"
#include "session.h"
#include "store_image.h"

#define SESSION_NAME "nova-session"
#define ACCOUNTS_FILE "/bin/accounts.exe"
#define LOGIN_FILE "/bin/login.exe"
#define SHELL_FILE "/bin/shell.exe"
#define STORE_DIRECTORY "/store/"
#define STORE_PATH "/store"
#define RECORDS_PATH "/data/accounts"
#define USERS_PATH "/users"

/* A program's own process: MAP, to map sections into itself. */
#define SELF_RIGHTS (PROCESS_RIGHT_MAP | RIGHT_TRANSFER)

/* The rights /data/accounts and /users are passed with. */
#define TREE_RIGHTS (IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST)

/* Login's store: to read programs, list, and map them executable. */
#define LOGIN_STORE_RIGHTS (IO_RIGHT_READ | IO_RIGHT_EXECUTE | IO_RIGHT_LIST)

/* The sessions' job, as login holds it: to create processes, no more. */
#define SESSIONS_JOB_RIGHTS JOB_RIGHT_CREATE_PROCESS

/* The service's bootstrap channel queues its one message; its control
   channel the login channels init has handed it and it has yet to take,
   one per start of login. A login channel queues one request and its
   reply. */
#define BOOTSTRAP_CAPACITY 1
#define CONTROL_CAPACITY 4
#define LOGIN_QUEUE_CAPACITY 1

/* The jobs take no interrupt vectors: neither program drives a device. */
static const JOB_LIMITS no_vectors = {0};

/* The service's bootstrap message has no bytes, but still names a
   buffer in user memory, which channel_write checks whatever the count
   (syscalls.md S6). */
static const UINT8 no_bytes[1];

static VIEW names;
static UINT32 debug_log = HANDLE_INVALID, root_job = HANDLE_INVALID;
static UINT32 store_control = HANDLE_INVALID;
static UINT8 session_base[STORE_BASE_NAME_MAX];
static UINT32 session_base_length;
static char path[sizeof STORE_DIRECTORY + STORE_BASE_NAME_MAX +
                 sizeof ACCOUNTS_FILE];

/* init's ends of the running service's control channel and of the
   running login's bootstrap channel, or HANDLE_INVALID. */
static UINT32 accounts_control = HANDLE_INVALID;
static UINT32 login_setups = HANDLE_INVALID;

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

static void say_status(const char *what, UINT32 status)
{
    say("nova-init: ");
    say(what);
    say(", status ");
    say_number(status);
    say("\r\n");
}

static void close_open(UINT32 *handle)
{
    if (*handle != HANDLE_INVALID)
        handle_close(*handle);
    *handle = HANDLE_INVALID;
}

static void close_transfers(HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        close_open(&handles[i].handle);
}

/* The path of file, one of the _FILE names, in the session's store path,
   at path. */
static const char *session_file(const char *file)
{
    UINT32 at = length_of(STORE_DIRECTORY);

    copy((UINT8 *)path, (const UINT8 *)STORE_DIRECTORY, at);
    copy((UINT8 *)path + at, session_base, session_base_length);
    at += session_base_length;
    copy((UINT8 *)path + at, (const UINT8 *)file, length_of(file) + 1);
    return path;
}

/* Note the store path whose name is SESSION_NAME. */
static void find_session(void *context, const UINT8 *name, UINT32 length,
                         UINT32 kind)
{
    UINT32 name_length = length_of(SESSION_NAME);

    (void)context;
    if (kind == IO_KIND_DIRECTORY &&
        length == STORE_HASH_CHARS + 1 + name_length &&
        same(name + STORE_HASH_CHARS + 1, (const UINT8 *)SESSION_NAME,
             name_length)) {
        copy(session_base, name, length);
        session_base_length = length;
    }
}

/* ------------------------------------------------------------------------
   Handing over channels. */

/* Write a setup of size bytes at setup on channel, carrying handle,
   which moves; closed if the write fails, as it does once the program
   at the other end has ended. */
static void hand_over(UINT32 channel, const void *setup, UINT32 size,
                      UINT32 handle, UINT32 rights)
{
    HANDLE_TRANSFER transfer;
    UINT32 status;

    transfer.handle = handle;
    transfer.rights = rights;
    status = channel_write(channel, setup, size, &transfer, 1);
    if (status != OK) {
        handle_close(handle);
        say_status("a setup reached no program", status);
    }
}

/* With both running, join the service and login with a new login
   channel, one end to each. */
static void connect(void)
{
    ACCOUNTS_SETUP to_service;
    LOGIN_SETUP to_login;
    UINT32 service_end, login_end;

    if (accounts_control == HANDLE_INVALID ||
        login_setups == HANDLE_INVALID ||
        channel_create(LOGIN_QUEUE_CAPACITY, &service_end,
                       &login_end) != OK)
        return;
    to_service.transaction = 0;
    to_service.kind = ACCOUNTS_SETUP_LOGIN;
    to_login.transaction = 0;
    to_login.kind = LOGIN_SETUP_ACCOUNTS;
    hand_over(accounts_control, &to_service, sizeof to_service, service_end,
              ACCOUNTS_LOGIN_END_RIGHTS);
    hand_over(login_setups, &to_login, sizeof to_login, login_end,
              ACCOUNTS_LOGIN_END_RIGHTS);
}

void login_console_started(void)
{
    LOGIN_SETUP setup;
    UINT32 session;

    if (login_setups == HANDLE_INVALID)
        return;
    session = console_session();
    if (session == HANDLE_INVALID)
        return;
    setup.transaction = 0;
    setup.kind = LOGIN_SETUP_CONSOLE;
    hand_over(login_setups, &setup, sizeof setup, session, RIGHTS_SAME);
}

/* ------------------------------------------------------------------------
   Laying the two out. */

/* Load the program at the session path's file into a new process in a
   new job of its own, whose handle the process holds; on failure
   nothing is left open. */
static UINT32 load_image(const char *file, PROGRAM *program)
{
    IO_ATTRIBUTES attributes;
    RECEIVED_HANDLE section;
    UINT32 channel, job = HANDLE_INVALID, status, unmapped;
    UINT64 size = 0;
    const UINT8 *mapped = 0;

    program->process = HANDLE_INVALID;
    program->thread = HANDLE_INVALID;
    status = view_open(&names, session_file(file), IO_RIGHT_READ, &channel,
                       &attributes);
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
    status = job_create(root_job, &no_vectors, &job);
    if (status == OK)
        status = program_load(init_layout(), job, mapped, size, program);
    unmapped = unmap_from_init(mapped, size);
    if (status == OK && unmapped != OK) {
        close_open(&program->thread);
        close_open(&program->process);
        status = unmapped;
    }
    close_open(&job);
    return status;
}

/* The debug log's sub-resource and program's own process with MAP, the
   first two handles of a bootstrap message. */
static UINT32 common_handles(const PROGRAM *program, HANDLE_TRANSFER *handles)
{
    UINT32 status = resource_create(debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG, 1,
                                    &handles[0].handle);

    if (status == OK)
        status = handle_duplicate(program->process, SELF_RIGHTS,
                                  &handles[1].handle);
    return status;
}

/* Write a bootstrap message of byte_count bytes and count handles, which
   move, on a new channel of capacity, keeping init's end at *mine and
   the program's at *theirs. On failure the handles are closed. */
static UINT32 write_bootstrap(UINT32 capacity, const void *bytes,
                              UINT32 byte_count, HANDLE_TRANSFER *handles,
                              UINT32 count, UINT32 *mine, UINT32 *theirs)
{
    UINT32 status = channel_create(capacity, mine, theirs);

    if (status == OK)
        status = channel_write(*mine, bytes, byte_count, handles, count);
    if (status != OK) {
        close_transfers(handles, count);
        close_open(mine);
        close_open(theirs);
    }
    return status;
}

static UINT32 load_accounts(const PLATFORM *platform, PROGRAM *program,
                            UINT32 *bootstrap)
{
    HANDLE_TRANSFER handles[ACCOUNTS_BOOTSTRAP_HANDLES];
    IO_ATTRIBUTES attributes;
    UINT32 control = HANDLE_INVALID, mine = HANDLE_INVALID, i, status;

    (void)platform;
    for (i = 0; i < ACCOUNTS_BOOTSTRAP_HANDLES; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    status = view_create(&names, RECORDS_PATH, TREE_RIGHTS,
                         IO_OPEN_CREATE | IO_OPEN_DIRECTORY,
                         &handles[ACCOUNTS_BOOTSTRAP_RECORDS].handle,
                         &attributes);
    if (status == OK)
        status = view_open(&names, USERS_PATH, TREE_RIGHTS,
                           &handles[ACCOUNTS_BOOTSTRAP_USERS].handle,
                           &attributes);
    if (status == OK)
        status = channel_create(CONTROL_CAPACITY, &control,
                                &handles[ACCOUNTS_BOOTSTRAP_CONTROL].handle);
    if (status == OK)
        status = load_image(ACCOUNTS_FILE, program);
    if (status == OK) {
        status = common_handles(program, handles);
        if (status != OK) {
            close_open(&program->thread);
            close_open(&program->process);
        }
    }
    if (status != OK) {
        close_transfers(handles, ACCOUNTS_BOOTSTRAP_HANDLES);
        close_open(&control);
        return status;
    }
    status = write_bootstrap(BOOTSTRAP_CAPACITY, no_bytes, 0, handles,
                             ACCOUNTS_BOOTSTRAP_HANDLES, &mine, bootstrap);
    close_open(&mine);
    if (status != OK) {
        close_open(&control);
        close_open(&program->thread);
        close_open(&program->process);
        return status;
    }
    close_open(&accounts_control);
    accounts_control = control;
    connect();
    return OK;
}

static UINT32 load_login(const PLATFORM *platform, PROGRAM *program,
                         UINT32 *bootstrap)
{
    static char shell[sizeof path];
    HANDLE_TRANSFER handles[LOGIN_HANDLES];
    IO_ATTRIBUTES attributes;
    UINT32 mine = HANDLE_INVALID, i, status;

    (void)platform;
    for (i = 0; i < LOGIN_HANDLES; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    handles[LOGIN_JOB].rights = SESSIONS_JOB_RIGHTS;
    copy((UINT8 *)shell, (const UINT8 *)session_file(SHELL_FILE),
         length_of(path) + 1);
    status = view_open(&names, STORE_PATH, LOGIN_STORE_RIGHTS,
                       &handles[LOGIN_STORE].handle, &attributes);
    if (status == OK)
        status = io_open(store_control, IO_OPEN, 0, 0, STORE_RIGHT_CLOSURE,
                         &handles[LOGIN_STORE_CONTROL].handle, &attributes);
    if (status == OK)
        status = job_create(root_job, &no_vectors, &handles[LOGIN_JOB].handle);
    if (status == OK)
        status = load_image(LOGIN_FILE, program);
    if (status == OK) {
        status = common_handles(program, handles);
        if (status != OK) {
            close_open(&program->thread);
            close_open(&program->process);
        }
    }
    if (status != OK) {
        close_transfers(handles, LOGIN_HANDLES);
        return status;
    }
    status = write_bootstrap(LOGIN_BOOTSTRAP_CAPACITY, shell,
                             length_of(shell) + 1, handles, LOGIN_HANDLES,
                             &mine, bootstrap);
    if (status != OK) {
        close_open(&program->thread);
        close_open(&program->process);
        return status;
    }
    close_open(&login_setups);
    login_setups = mine;
    login_console_started();
    connect();
    return OK;
}

/* ------------------------------------------------------------------------
   Starting. */

int login_start(const PLATFORM *platform, const STORE_SERVICES *services)
{
    IO_ATTRIBUTES attributes;
    UINT32 users, status;

    debug_log = platform->debug_log;
    root_job = platform->job;
    store_control = services->control;
    names.count = 0;
    session_base_length = 0;
    status = view_add(&names, "/", services->names);
    if (status == OK)
        status = view_list(&names, STORE_PATH, find_session, 0);
    if (status == OK && !session_base_length)
        status = ERR_NOT_FOUND;
    if (status != OK) {
        say_status("no " SESSION_NAME " path in /store; the accounts service "
                   "and login do not start",
                   status);
        return 0;
    }
    status = view_open(&names, USERS_PATH, IO_RIGHT_LIST, &users, &attributes);
    if (status != OK) {
        say_status("no /data and /users; the accounts service and login do "
                   "not start",
                   status);
        return 0;
    }
    handle_close(users);
    return platform_keep(platform, "accounts service", load_accounts) &&
           platform_keep(platform, "login", load_login);
}
