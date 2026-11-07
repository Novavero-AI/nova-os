/* The accounts service (docs/spec/accounts.md): it keeps each user's
   record in /data/accounts, creates each user's home under /users, and
   checks passwords with Argon2id (RFC 9106) over memory of its own. It
   serves login channels, which the first process hands it on its
   control channel, one for each start of login, and administrator
   channels (accounts_protocol.h). A login that succeeds is answered with the
   session's handles: the user's home, and for an administrator a new
   administrator channel, so the authority to create accounts exists
   only in an administrator's session (A7).

   It reads every record from the disk when it needs it and keeps none,
   so what it answers is what the volume holds.

   At start it checks its BLAKE2b against RFC 7693's test values and its
   Argon2id against RFC 9106's test vector, and ends with
   SERVICE_EXIT_FAILED if either differs. One thread serves every
   channel; it ends once none is left open, which the control channel,
   held by the first process while it supervises the service, keeps
   from happening. */

#include "accounts_protocol.h"
#include "argon2.h"
#include "blake2b.h"
#include "directory_protocol.h"
#include "log.h"
#include "native.h"
#include "path.h"
#include "record.h"
#include "services.h"
#include "wipe.h"

/* The Argon2id parameters the service writes (A4): RFC 9106's second
   recommended option (section 4), for machines with much less memory
   than its first's 2 GiB. */
#define HASH_LANES 4
#define HASH_MEMORY_KIB 65536
#define HASH_PASSES 3

/* A failed login's reply waits this long (A5), so that guessing online
   costs seconds per guess on top of the hash. The channel waits; the
   service serves its other channels meanwhile. */
#define FAILURE_DELAY_SECONDS 3
#define NS_PER_SECOND 1000000000ULL
#define FAILURE_DELAY_NS (FAILURE_DELAY_SECONDS * NS_PER_SECOND)

/* Where the service maps the memory Argon2id fills: RECORD_MEMORY_MAX_KIB,
   in sections of SECTION_MAX_SIZE, mapped end to end. */
#define ARENA_BASE 0x0000000100000000ULL
#define ARENA_SIZE ((UINT64)RECORD_MEMORY_MAX_KIB * ARGON2_BLOCK_SIZE)
#define ARENA_BLOCKS ((UINT64)RECORD_MEMORY_MAX_KIB)

/* The channels served at once: login's, and the administrator channels
   of the sessions open, each waited on in one call. */
#define CHANNELS_MAX 8

/* A record's temporary name while it is written: '.', the name, ".new".
   No account's name starts with '.', so a scan never takes it for a
   record. */
#define TEMPORARY_PREFIX "."
#define TEMPORARY_SUFFIX ".new"
#define TEMPORARY_EXTRA 5

/* What a session's home carries: read, write and list; the store, not
   the home, holds programs. */
#define HOME_RIGHTS (IO_RIGHT_READ | IO_RIGHT_WRITE | IO_RIGHT_LIST)

#define RECORDS_PREFIX "/data/accounts"
#define USERS_PREFIX "/users"

/* The longest path the service opens: a prefix, '/' and a record's
   temporary name. */
#define SERVICE_PATH_MAX                                                  \
    (sizeof RECORDS_PREFIX + ACCOUNT_NAME_MAX + TEMPORARY_EXTRA + 1)

/* What opening a record's temporary name and a home do: create a file
   that is not there, and a directory that may be. */
#define CREATE_RECORD (IO_OPEN_CREATE | IO_OPEN_EXCLUSIVE)
#define CREATE_HOME (IO_OPEN_CREATE | IO_OPEN_DIRECTORY)

/* The rights the service writes a record's temporary name with. */
#define RECORD_WRITE_RIGHTS (IO_RIGHT_READ | IO_RIGHT_WRITE)

/* The largest id, after which no account can be created. */
#define ID_MAX 0xffffffffu

/* The administrator channel's queue: one request and its reply. */
#define ADMINISTRATOR_CAPACITY 1

/* The largest request: the header, credentials, a name and a
   password. */
#define REQUEST_MAX                                                       \
    (sizeof(IO_REQUEST) + sizeof(ACCOUNTS_CREDENTIALS) + ACCOUNT_NAME_MAX + \
     ACCOUNT_PASSWORD_MAX)

typedef enum {
    CHANNEL_CONTROL = 1,
    CHANNEL_LOGIN,
    CHANNEL_ADMINISTRATOR
} CHANNEL_KIND;

/* A channel served, and a failure's reply waiting for its time. */
typedef struct {
    UINT32 handle;
    CHANNEL_KIND kind;
    int waiting;
    UINT64 due;
    IO_REPLY reply;
} CHANNEL;

typedef struct {
    UINT8 bytes[ACCOUNT_NAME_MAX];
    UINT32 length;
} NAME;

/* A scan of /data/accounts. */
typedef struct {
    UINT32 count;
    int overflow;
} SCAN;

static CHANNEL channels[CHANNELS_MAX];
static UINT32 channel_count;
static UINT32 records = HANDLE_INVALID, users = HANDLE_INVALID;
static UINT32 self = HANDLE_INVALID;
static VIEW records_view, users_view;
static char path[SERVICE_PATH_MAX];
static ARGON2_BLOCK *arena;

static UINT8 message[REQUEST_MAX];
static UINT8 reply[sizeof(IO_REPLY) + sizeof(ACCOUNTS_USER)];
static RECEIVED_HANDLE received[CHANNEL_MAX_HANDLES];
static NAME names[ACCOUNTS_MAX];
static UINT8 record_bytes[RECORD_SIZE];

/* A message with no handles still names an array in user memory, which
   channel_write checks whatever the count (syscalls.md S6). */
static const HANDLE_TRANSFER no_handles[1];

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

/* Whether the count bytes at a and b are equal, in a time that depends
   on count alone: every byte is read and no branch depends on one. */
static int equal_in_constant_time(const UINT8 *a, const UINT8 *b,
                                  UINT32 count)
{
    const volatile UINT8 *x = a, *y = b;
    UINT32 difference = 0, i;

    for (i = 0; i < count; i++)
        difference |= (UINT32)(x[i] ^ y[i]);
    return difference == 0;
}

/* ------------------------------------------------------------------------
   Records. */

static void take_name(void *context, const UINT8 *name, UINT32 length,
                      UINT32 kind)
{
    SCAN *scan = context;

    if (kind != IO_KIND_FILE || !account_name_valid(name, length))
        return;
    if (scan->count == ACCOUNTS_MAX) {
        scan->overflow = 1;
        return;
    }
    copy(names[scan->count].bytes, name, length);
    names[scan->count].length = length;
    scan->count++;
}

/* The names of the records in /data/accounts, into names, and how many
   at *count. A file whose name is no account's, a record's temporary
   name among them, is not a record and is left out. */
static UINT32 scan_records(UINT32 *count)
{
    SCAN scan;
    UINT32 status;

    scan.count = 0;
    scan.overflow = 0;
    status = view_list(&records_view, RECORDS_PREFIX, take_name, &scan);
    if (status == OK && scan.overflow)
        status = ERR_OUT_OF_RANGE;
    *count = scan.count;
    return status;
}

/* The record of the account named by the length bytes at name. */
static UINT32 read_account(const UINT8 *name, UINT32 length, ACCOUNT *account)
{
    IO_ATTRIBUTES attributes;
    UINT32 channel, got, status;

    status = io_open(records, IO_OPEN, name, length, IO_RIGHT_READ, &channel,
                     &attributes);
    if (status != OK)
        return status;
    if (attributes.kind != IO_KIND_FILE || attributes.size != RECORD_SIZE)
        status = ERR_DATA_INTEGRITY;
    if (status == OK)
        status = io_read(channel, 0, record_bytes, RECORD_SIZE, &got);
    handle_close(channel);
    if (status == OK &&
        !record_decode(record_bytes, got, name, length, account))
        status = ERR_DATA_INTEGRITY;
    if (status == ERR_DATA_INTEGRITY) {
        say("nova-accounts: the record of ");
        say_printable(name, length);
        say(" does not read as a record\r\n");
    }
    return status;
}

/* prefix, '/' and the length bytes at name, NUL-terminated, at path. */
static const char *path_of(const char *prefix, const UINT8 *name,
                           UINT32 length)
{
    UINT32 at = 0;

    while (prefix[at]) {
        path[at] = prefix[at];
        at++;
    }
    path[at++] = '/';
    copy((UINT8 *)path + at, name, length);
    path[at + length] = 0;
    return path;
}

/* Write account's record under a temporary name, rename it to the
   account's name in one request, then flush the volume. The rename is
   sent only once the write is answered, so any commit that holds the
   rename holds the whole record, and a crash keeps each commit whole or
   none of it (storage.md F6): the account's name holds a whole record
   or none, and a crash before this returns leaves at most a temporary
   name, no account's. Once it returns OK the account, and its home,
   created before, are durable. */
static UINT32 write_account(const ACCOUNT *account)
{
    UINT8 temporary[ACCOUNT_NAME_MAX + TEMPORARY_EXTRA];
    IO_ATTRIBUTES attributes;
    UINT32 length, channel, status, written = 0;

    copy(temporary, (const UINT8 *)TEMPORARY_PREFIX, 1);
    length = 1;
    copy(temporary + length, account->name, account->name_length);
    length += account->name_length;
    copy(temporary + length, (const UINT8 *)TEMPORARY_SUFFIX, 4);
    length += 4;
    status = io_remove(records, temporary, length);
    if (status != OK && status != ERR_NOT_FOUND)
        return status;
    record_encode(account, record_bytes);
    status = view_create(&records_view,
                         path_of(RECORDS_PREFIX, temporary, length),
                         RECORD_WRITE_RIGHTS, CREATE_RECORD, &channel,
                         &attributes);
    if (status != OK)
        return status;
    status = io_write(channel, 0, record_bytes, RECORD_SIZE, &written);
    if (status == OK && written != RECORD_SIZE)
        status = ERR_BAD_STATE;
    handle_close(channel);
    if (status == OK)
        status = io_rename(records, temporary, length, account->name,
                           account->name_length, 0);
    if (status == OK)
        status = io_flush(records);
    return status;
}

/* ------------------------------------------------------------------------
   Passwords. */

static void hash_input(const ACCOUNT *account, const UINT8 *password,
                       UINT32 password_length, ARGON2_INPUT *input)
{
    input->password = password;
    input->password_length = password_length;
    input->salt = account->salt;
    input->salt_length = RECORD_SALT_SIZE;
    input->secret = 0;
    input->secret_length = 0;
    input->data = 0;
    input->data_length = 0;
    input->lanes = account->lanes;
    input->memory_kib = account->memory_kib;
    input->passes = account->passes;
    input->tag_length = RECORD_TAG_SIZE;
}

/* Whether password is account's: its tag, recomputed with the record's
   parameters and salt, equals the record's. */
static int password_matches(const ACCOUNT *account, const UINT8 *password,
                            UINT32 password_length)
{
    UINT8 tag[RECORD_TAG_SIZE];
    ARGON2_INPUT input;
    int matches;

    hash_input(account, password, password_length, &input);
    matches = argon2id(&input, arena, ARENA_BLOCKS, tag) &&
              equal_in_constant_time(tag, account->tag, RECORD_TAG_SIZE);
    wipe(tag, sizeof tag);
    return matches;
}

/* The account a name with no record is checked against, so that a
   login for it does the work a wrong password does. Its tag matches
   nothing that matters, since the result is discarded. */
static void stand_in_account(ACCOUNT *account)
{
    UINT32 i;

    account->id = 0;
    account->flags = 0;
    account->name_length = 0;
    account->lanes = HASH_LANES;
    account->memory_kib = HASH_MEMORY_KIB;
    account->passes = HASH_PASSES;
    for (i = 0; i < RECORD_SALT_SIZE; i++)
        account->salt[i] = 0;
    for (i = 0; i < RECORD_TAG_SIZE; i++)
        account->tag[i] = 0;
}

/* ------------------------------------------------------------------------
   Creating accounts. */

/* Create an account named by the length bytes at name with password and
   flags, and its home. The record is written last, so an account exists
   only once its home does. Returns ERR_ALREADY_EXISTS if the name has a
   record; ERR_NO_RESOURCES if ACCOUNTS_MAX accounts exist, since a scan
   would then fail and with it every login; ERR_DATA_INTEGRITY if a
   record does not read, since its id could then be given twice;
   ERR_BAD_STATE if the kernel has no random bytes for the salt. */
static UINT32 create_account(const UINT8 *name, UINT32 length,
                             const UINT8 *password, UINT32 password_length,
                             UINT32 flags, ACCOUNT *account)
{
    ARGON2_INPUT input;
    ACCOUNT existing;
    IO_ATTRIBUTES attributes;
    UINT32 count, i, home, status, top = 0;

    if (!account_name_valid(name, length) ||
        password_length < ACCOUNT_PASSWORD_MIN)
        return ERR_INVALID_ARGS;
    status = scan_records(&count);
    if (status == OK && count == ACCOUNTS_MAX)
        status = ERR_NO_RESOURCES;
    for (i = 0; status == OK && i < count; i++) {
        if (names[i].length == length && same(names[i].bytes, name, length))
            status = ERR_ALREADY_EXISTS;
        else
            status = read_account(names[i].bytes, names[i].length, &existing);
        if (status == OK && existing.id > top)
            top = existing.id;
    }
    if (status == OK && top == ID_MAX)
        status = ERR_OUT_OF_RANGE;
    if (status != OK)
        return status;
    account->id = top < ACCOUNT_FIRST_ID ? ACCOUNT_FIRST_ID : top + 1;
    account->flags = flags;
    account->name_length = length;
    for (i = 0; i < ACCOUNT_NAME_MAX; i++)
        account->name[i] = i < length ? name[i] : 0;
    account->lanes = HASH_LANES;
    account->memory_kib = HASH_MEMORY_KIB;
    account->passes = HASH_PASSES;
    status = random_read(account->salt, RECORD_SALT_SIZE);
    if (status != OK)
        return status;
    hash_input(account, password, password_length, &input);
    if (!argon2id(&input, arena, ARENA_BLOCKS, account->tag))
        return ERR_BAD_STATE;
    status = view_create(&users_view, path_of(USERS_PREFIX, name, length),
                         HOME_RIGHTS, CREATE_HOME, &home, &attributes);
    if (status != OK)
        return status;
    handle_close(home);
    status = write_account(account);
    if (status == OK) {
        say("nova-accounts: created ");
        say_printable(name, length);
        say(", id ");
        say_number(account->id);
        say(account->flags & ACCOUNT_ADMINISTRATOR ? ", an administrator\r\n"
                                                   : "\r\n");
    }
    return status;
}

/* ------------------------------------------------------------------------
   Replies. */

/* Reply on channel with data bytes of reply's after its header, and
   handle_count handles, which close if the reply is not written. */
static void send(UINT32 channel, UINT32 transaction, UINT32 status,
                 UINT64 information, UINT32 data,
                 const HANDLE_TRANSFER *handles, UINT32 handle_count)
{
    IO_REPLY *header = (IO_REPLY *)reply;
    UINT32 i;

    header->transaction = transaction;
    header->status = status;
    header->information = information;
    if (channel_write(channel, reply, sizeof(IO_REPLY) + data,
                      handle_count ? handles : no_handles,
                      handle_count) == OK)
        return;
    for (i = 0; i < handle_count; i++)
        handle_close(handles[i].handle);
}

/* A failure's reply, sent once FAILURE_DELAY_NS has passed. */
static void answer_later(CHANNEL *c, UINT32 transaction, UINT32 status)
{
    UINT64 now = 0;

    clock_monotonic(&now);
    c->waiting = 1;
    c->due = now + FAILURE_DELAY_NS;
    c->reply.transaction = transaction;
    c->reply.status = status;
    c->reply.information = 0;
}

static void user_of(const ACCOUNT *account)
{
    ACCOUNTS_USER *user = (ACCOUNTS_USER *)(reply + sizeof(IO_REPLY));
    UINT32 i;

    user->id = account->id;
    user->flags = account->flags;
    user->name_length = account->name_length;
    for (i = 0; i < ACCOUNT_NAME_MAX; i++)
        user->name[i] = account->name[i];
}

/* Serve handle as a channel of kind. */
static UINT32 add_channel(UINT32 handle, CHANNEL_KIND kind)
{
    CHANNEL *c;

    if (channel_count == CHANNELS_MAX)
        return ERR_NO_RESOURCES;
    c = &channels[channel_count++];
    c->handle = handle;
    c->kind = kind;
    c->waiting = 0;
    return OK;
}

/* Answer a login of account on c with its session's handles: its home,
   and for an administrator a new administrator channel. */
static void open_session(const CHANNEL *c, UINT32 transaction,
                         const ACCOUNT *account)
{
    HANDLE_TRANSFER handles[ACCOUNTS_REPLY_ADMINISTRATOR + 1];
    IO_ATTRIBUTES attributes;
    UINT32 count = 1, served, status;

    status = io_open(users, IO_OPEN, account->name, account->name_length,
                     HOME_RIGHTS, &handles[ACCOUNTS_REPLY_HOME].handle,
                     &attributes);
    if (status == OK && attributes.kind != IO_KIND_DIRECTORY) {
        handle_close(handles[ACCOUNTS_REPLY_HOME].handle);
        status = ERR_NOT_SUPPORTED;
    }
    handles[ACCOUNTS_REPLY_HOME].rights = RIGHTS_SAME;
    if (status == OK && (account->flags & ACCOUNT_ADMINISTRATOR)) {
        handles[ACCOUNTS_REPLY_ADMINISTRATOR].rights = RIGHTS_SAME;
        status = channel_create(ADMINISTRATOR_CAPACITY, &served,
                                &handles[ACCOUNTS_REPLY_ADMINISTRATOR].handle);
        if (status == OK) {
            status = add_channel(served, CHANNEL_ADMINISTRATOR);
            if (status != OK) {
                handle_close(served);
                handle_close(handles[ACCOUNTS_REPLY_ADMINISTRATOR].handle);
            }
        }
        if (status != OK)
            handle_close(handles[ACCOUNTS_REPLY_HOME].handle);
        count = 2;
    }
    if (status != OK) {
        send(c->handle, transaction, status, 0, 0, 0, 0);
        return;
    }
    user_of(account);
    send(c->handle, transaction, OK, 0, sizeof(ACCOUNTS_USER), handles,
         count);
    say("nova-accounts: ");
    say_printable(account->name, account->name_length);
    say(" logged in\r\n");
}

/* ------------------------------------------------------------------------
   Requests. */

/* The credentials a request carries, checked against its length. */
static int credentials(const IO_REQUEST *request, UINT32 bytes,
                       const UINT8 **name, UINT32 *name_length,
                       const UINT8 **password, UINT32 *password_length)
{
    const ACCOUNTS_CREDENTIALS *c =
        (const ACCOUNTS_CREDENTIALS *)(message + sizeof *request);

    if (bytes < sizeof *request + sizeof *c ||
        request->length != bytes - sizeof *request)
        return 0;
    if (c->name_length > ACCOUNT_NAME_MAX ||
        c->password_length > ACCOUNT_PASSWORD_MAX ||
        request->length != sizeof *c + c->name_length + c->password_length)
        return 0;
    *name = message + sizeof *request + sizeof *c;
    *name_length = c->name_length;
    *password = *name + c->name_length;
    *password_length = c->password_length;
    return 1;
}

static void login(CHANNEL *c, UINT32 transaction, const UINT8 *name,
                  UINT32 name_length, const UINT8 *password,
                  UINT32 password_length)
{
    ACCOUNT account;
    int known = account_name_valid(name, name_length) &&
                read_account(name, name_length, &account) == OK;

    if (!known)
        stand_in_account(&account);
    if (password_matches(&account, password, password_length) && known) {
        open_session(c, transaction, &account);
        return;
    }
    say("nova-accounts: a login was refused\r\n");
    answer_later(c, transaction, ERR_ACCESS_DENIED);
}

static void create_first(CHANNEL *c, UINT32 transaction, const UINT8 *name,
                         UINT32 name_length, const UINT8 *password,
                         UINT32 password_length)
{
    ACCOUNT account;
    UINT32 count, status = scan_records(&count);

    if (status == OK && count)
        status = ERR_ALREADY_EXISTS;
    if (status == OK)
        status = create_account(name, name_length, password, password_length,
                                ACCOUNT_ADMINISTRATOR, &account);
    if (status == OK)
        open_session(c, transaction, &account);
    else
        send(c->handle, transaction, status, 0, 0, 0, 0);
}

static void create(CHANNEL *c, UINT32 transaction, const UINT8 *name,
                   UINT32 name_length, const UINT8 *password,
                   UINT32 password_length)
{
    ACCOUNT account;
    UINT32 status = create_account(name, name_length, password,
                                   password_length, 0, &account);

    if (status == OK)
        user_of(&account);
    send(c->handle, transaction, status, 0,
         status == OK ? sizeof(ACCOUNTS_USER) : 0, 0, 0);
}

/* Serve the request in message, of bytes bytes, on channel index. */
static void serve_request(UINT32 index, UINT32 bytes)
{
    const IO_REQUEST *request = (const IO_REQUEST *)message;
    CHANNEL *c = &channels[index];
    const UINT8 *name, *password;
    UINT32 name_length, password_length, count, status;
    int login_channel = c->kind == CHANNEL_LOGIN;

    switch (request->operation) {
    case ACCOUNTS_COUNT:
        if (!login_channel)
            break;
        status = scan_records(&count);
        send(c->handle, request->transaction, status,
             status == OK ? count : 0, 0, 0, 0);
        return;
    case ACCOUNTS_CREATE_FIRST:
    case ACCOUNTS_LOGIN:
        if (!login_channel)
            break;
        if (!credentials(request, bytes, &name, &name_length, &password,
                         &password_length)) {
            send(c->handle, request->transaction, ERR_INVALID_ARGS, 0, 0, 0,
                 0);
            return;
        }
        if (request->operation == ACCOUNTS_LOGIN)
            login(c, request->transaction, name, name_length, password,
                  password_length);
        else
            create_first(c, request->transaction, name, name_length,
                         password, password_length);
        return;
    case ACCOUNTS_CREATE:
        if (login_channel)
            break;
        if (!credentials(request, bytes, &name, &name_length, &password,
                         &password_length)) {
            send(c->handle, request->transaction, ERR_INVALID_ARGS, 0, 0, 0,
                 0);
            return;
        }
        create(c, request->transaction, name, name_length, password,
               password_length);
        return;
    }
    send(c->handle, request->transaction, ERR_ACCESS_DENIED, 0, 0, 0, 0);
}

/* Take the login channel a setup on the control channel at index
   carries. A setup that is not one is logged and its handles closed.
   Returns 0 if the control channel ends: a message that cannot be read,
   which would stay queued. */
static int serve_setup(UINT32 index)
{
    ACCOUNTS_SETUP setup;
    UINT32 bytes = 0, count = 0, i, status;

    status = channel_read(channels[index].handle, &setup, sizeof setup,
                          received, CHANNEL_MAX_HANDLES, &bytes, &count);
    if (status != OK)
        return 0;
    if (bytes == sizeof setup && setup.transaction == 0 &&
        setup.kind == ACCOUNTS_SETUP_LOGIN && count == 1 &&
        received[0].type == TYPE_CHANNEL &&
        (received[0].rights & ACCOUNTS_LOGIN_END_RIGHTS) ==
            ACCOUNTS_LOGIN_END_RIGHTS) {
        if (add_channel(received[0].handle, CHANNEL_LOGIN) == OK)
            return 1;
        say("nova-accounts: no room for a login channel\r\n");
    } else {
        say("nova-accounts: a malformed setup on the control channel\r\n");
    }
    for (i = 0; i < count; i++)
        handle_close(received[i].handle);
    return 1;
}

/* Serve one message on channel index. Returns 0 if the channel ends: a
   message that cannot be read, which would stay queued, or one too
   short to be a request. Every handle a request carries is closed, and
   the message, which may hold a password, is wiped. */
static int serve(UINT32 index)
{
    UINT32 bytes = 0, count = 0, i, status;

    if (channels[index].kind == CHANNEL_CONTROL)
        return serve_setup(index);

    status = channel_read(channels[index].handle, message, sizeof message,
                          received, CHANNEL_MAX_HANDLES, &bytes, &count);
    /* A read that fails as it moves the handles has copied the bytes
       already, so the message is wiped whatever the status. */
    if (status == OK) {
        for (i = 0; i < count; i++)
            handle_close(received[i].handle);
        if (bytes >= sizeof(IO_REQUEST))
            serve_request(index, bytes);
    }
    wipe(message, sizeof message);
    return status == OK && bytes >= sizeof(IO_REQUEST);
}

/* ------------------------------------------------------------------------
   The service. */

static void remove_channel(UINT32 index)
{
    handle_close(channels[index].handle);
    channels[index] = channels[--channel_count];
}

/* Map ARENA_SIZE bytes of fresh sections at ARENA_BASE. */
static int map_arena(void)
{
    UINT64 at;
    UINT32 section;

    for (at = 0; at < ARENA_SIZE; at += SECTION_MAX_SIZE) {
        if (section_create(SECTION_MAX_SIZE, 0, &section) != OK)
            return 0;
        if (process_map(self, section, 0, ARENA_BASE + at, SECTION_MAX_SIZE,
                        MAP_READ | MAP_WRITE) != OK) {
            handle_close(section);
            return 0;
        }
        handle_close(section);
    }
    arena = (ARGON2_BLOCK *)ARENA_BASE;
    return 1;
}

static void read_bootstrap(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[ACCOUNTS_BOOTSTRAP_HANDLES];
    UINT32 bytes, count;

    if (channel_read(bootstrap, message, sizeof message, handles,
                     ACCOUNTS_BOOTSTRAP_HANDLES, &bytes, &count) != OK ||
        count != ACCOUNTS_BOOTSTRAP_HANDLES ||
        handles[ACCOUNTS_BOOTSTRAP_DEBUG_LOG].type != TYPE_RESOURCE ||
        handles[ACCOUNTS_BOOTSTRAP_SELF].type != TYPE_PROCESS ||
        handles[ACCOUNTS_BOOTSTRAP_RECORDS].type != TYPE_CHANNEL ||
        handles[ACCOUNTS_BOOTSTRAP_USERS].type != TYPE_CHANNEL ||
        handles[ACCOUNTS_BOOTSTRAP_CONTROL].type != TYPE_CHANNEL)
        process_exit(SERVICE_EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[ACCOUNTS_BOOTSTRAP_DEBUG_LOG].handle);
    self = handles[ACCOUNTS_BOOTSTRAP_SELF].handle;
    records = handles[ACCOUNTS_BOOTSTRAP_RECORDS].handle;
    users = handles[ACCOUNTS_BOOTSTRAP_USERS].handle;
    add_channel(handles[ACCOUNTS_BOOTSTRAP_CONTROL].handle, CHANNEL_CONTROL);
    if (view_add(&records_view, RECORDS_PREFIX, records) != OK ||
        view_add(&users_view, USERS_PREFIX, users) != OK)
        process_exit(SERVICE_EXIT_FAILED);
}

static void start(UINT32 bootstrap)
{
    UINT32 count;

    read_bootstrap(bootstrap);
    if (!map_arena()) {
        say("nova-accounts: no memory for Argon2id\r\n");
        process_exit(SERVICE_EXIT_FAILED);
    }
    if (!blake2b_self_test() || !argon2_self_test(arena, ARENA_BLOCKS)) {
        say("nova-accounts: BLAKE2b or Argon2id differs from its RFC's "
            "test values\r\n");
        process_exit(SERVICE_EXIT_FAILED);
    }
    say("nova-accounts: BLAKE2b gives RFC 7693's values (Appendices A and "
        "E) and Argon2id RFC 9106's tag (5.3)\r\n");
    if (scan_records(&count) != OK) {
        say("nova-accounts: /data/accounts does not list\r\n");
        process_exit(SERVICE_EXIT_FAILED);
    }
    say("nova-accounts: ");
    say_number(count);
    say(count == 1 ? " account\r\n" : " accounts\r\n");
}

/* The loop waits on every channel: for a request on one with no reply
   waiting, for its close on one with a reply waiting, and until the
   earliest waiting reply is due. Channels are served from the last, so
   that removing one, whose place the last takes, or adding one leaves
   each index below it naming the channel it was waited on as. */
_Noreturn void accounts_main(UINT32 bootstrap, UINT64 argument)
{
    WAIT_ENTRY items[CHANNELS_MAX];
    UINT64 now, deadline;
    UINT32 waited, i, status;
    int keep;

    (void)argument;
    start(bootstrap);
    while (channel_count) {
        now = 0;
        clock_monotonic(&now);
        deadline = DEADLINE_INFINITE;
        for (i = 0; i < channel_count; i++) {
            CHANNEL *c = &channels[i];

            if (c->waiting && c->due <= now) {
                send(c->handle, c->reply.transaction, c->reply.status, 0, 0,
                     0, 0);
                c->waiting = 0;
            }
            if (c->waiting && c->due < deadline)
                deadline = c->due;
            items[i].handle = c->handle;
            items[i].signals = c->waiting
                                   ? CHANNEL_PEER_CLOSED
                                   : CHANNEL_READABLE | CHANNEL_PEER_CLOSED;
        }
        waited = channel_count;
        status = object_wait_many(items, waited, deadline);
        if (status == ERR_TIMED_OUT)
            continue;
        if (status != OK)
            break;
        for (i = waited; i-- > 0;) {
            if ((items[i].observed & CHANNEL_READABLE) && !channels[i].waiting)
                keep = serve(i);
            else
                keep = !(items[i].observed & CHANNEL_PEER_CLOSED);
            if (!keep)
                remove_channel(i);
        }
    }
    say("nova-accounts: no channel left open, ending\r\n");
    process_exit(SERVICE_EXIT_DONE);
}
