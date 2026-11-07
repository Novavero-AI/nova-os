/* The shell, version 0 (docs/spec/accounts.md A8): a prompt on the
   console, name@nova and the current directory, and the commands help
   lists. It holds only its session's handles (session.h): the console,
   the launcher, its home directory as /users/<name>, the store's root
   as /store, read only, and in an administrator's session the accounts
   service's administrator channel. A path outside those two prefixes
   reaches nothing, since no handle in the shell's view covers it, and
   user create is refused in a session without the administrator
   channel. */

#include "accounts_protocol.h"
#include "console.h"
#include "directory_protocol.h"
#include "log.h"
#include "native.h"
#include "path.h"
#include "session.h"
#include "wipe.h"

/* The shell's exit codes: logged out, or its session's handles were
   not what login sends. */
#define EXIT_LOGGED_OUT 0
#define EXIT_FAILED 1

/* A command line: the most a console line holds (console.md C4). */
#define LINE_MAX CONSOLE_LINE_MAX
#define WORDS_MAX 32
#define READ_PIECE 4096

/* What ls writes before the console sees it: entries are collected
   while the listing runs, since its callback may call no path function
   (path.h), and written after it. A longer listing is cut off. */
#define LISTING_MAX 16384

/* A line read for a name or a password holds one byte more than either
   may have, so that a line too long is told from one that fits. */
#define NAME_LINE (ACCOUNT_NAME_MAX + 1)
#define PASSWORD_LINE (ACCOUNT_PASSWORD_MAX + 1)

#define DECIMAL_DIGITS_MAX 20

typedef struct {
    UINT8 bytes[PASSWORD_LINE];
    UINT32 length;
} SECRET;

typedef struct {
    UINT32 at;
    int cut;
} LISTING;

static UINT32 console = HANDLE_INVALID, launcher = HANDLE_INVALID;
static UINT32 administrator = HANDLE_INVALID;
static VIEW view;
static ACCOUNTS_USER user;
static char home[sizeof SESSION_USERS_PREFIX + ACCOUNT_NAME_MAX];
static char cwd[IO_PATH_MAX + 1];
static char resolved[IO_PATH_MAX + 1];
static char joined[2 * IO_PATH_MAX + 2];
static char line[LINE_MAX + 1];
static char *words[WORDS_MAX];
static UINT32 word_count;
static UINT8 listing[LISTING_MAX];
static UINT8 piece[READ_PIECE];
static UINT8 message[CHANNEL_MAX_BYTES];
static RECEIVED_HANDLE received[CHANNEL_MAX_HANDLES];
static SECRET password, again;
static UINT8 new_name[NAME_LINE];
static UINT32 new_name_length;

/* The names of syscalls.md S7's status codes, by value. */
static const char *const status_names[] = {
    "OK",
    "ERR_NOT_SUPPORTED",
    "ERR_INVALID_ARGS",
    "ERR_BAD_ADDRESS",
    "ERR_BAD_HANDLE",
    "ERR_WRONG_TYPE",
    "ERR_ACCESS_DENIED",
    "ERR_NO_MEMORY",
    "ERR_OUT_OF_RANGE",
    "ERR_SHOULD_WAIT",
    "ERR_TIMED_OUT",
    "ERR_CANCELED",
    "ERR_PEER_CLOSED",
    "ERR_BUFFER_TOO_SMALL",
    "ERR_BAD_STATE",
    "ERR_NO_RESOURCES",
    "ERR_NOT_FOUND",
    "ERR_ALREADY_EXISTS",
    "ERR_DATA_INTEGRITY",
};

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

static int is(const char *word, const char *text)
{
    UINT32 n = length_of(text);

    return length_of(word) == n && same((const UINT8 *)word,
                                        (const UINT8 *)text, n);
}

static void forget(SECRET *secret)
{
    wipe(secret, sizeof *secret);
}

/* ------------------------------------------------------------------------
   Output. */

static void show(const char *text)
{
    if (console_say(console, text) != OK)
        process_exit(EXIT_FAILED);
}

static void show_bytes(const UINT8 *bytes, UINT32 length)
{
    if (console_write(console, bytes, length) != OK)
        process_exit(EXIT_FAILED);
}

static void show_number(UINT64 n)
{
    char digits[DECIMAL_DIGITS_MAX + 1];
    UINT32 i = DECIMAL_DIGITS_MAX;

    digits[i] = 0;
    do {
        digits[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    show(digits + i);
}

static void show_status(UINT32 status)
{
    if (status < sizeof status_names / sizeof status_names[0])
        show(status_names[status]);
    else
        show_number(status);
}

/* "command: what: STATUS" on the console; the debug log gets the
   command and the status alone, since what may be a path typed, and so
   the rest of a password typed at a prompt a file drew. */
static void refuse(const char *command, const char *what, UINT32 status)
{
    show(command);
    show(": ");
    show(what);
    show(": ");
    show_status(status);
    show("\n");
    say("nova-shell: ");
    say_printable(user.name, user.name_length);
    say(": ");
    say(command);
    say(" gives status ");
    say_number(status);
    say("\r\n");
}

/* ------------------------------------------------------------------------
   Paths. */

/* The normal form of the absolute path text, as path.c resolves one:
   no empty, "." or ".." component, never above "/", into out of
   capacity bytes. Returns 0 if it does not fit. */
static int normalize(const char *text, char *out, UINT32 capacity)
{
    UINT32 length = length_of(text), at = 0, i = 0, start;

    while (i < length) {
        while (i < length && text[i] == '/')
            i++;
        start = i;
        while (i < length && text[i] != '/')
            i++;
        if (i == start || (i - start == 1 && text[start] == '.'))
            continue;
        if (i - start == 2 && text[start] == '.' && text[start + 1] == '.') {
            while (at > 0 && out[at - 1] != '/')
                at--;
            if (at > 0)
                at--;
            continue;
        }
        if (i - start + 2 > capacity - at)
            return 0;
        out[at++] = '/';
        copy((UINT8 *)out + at, (const UINT8 *)text + start, i - start);
        at += i - start;
    }
    if (at == 0)
        out[at++] = '/';
    out[at] = 0;
    return 1;
}

/* path, absolute or relative to the current directory, in normal form
   at resolved. Returns 0, having said why, if it is too long. */
static int resolve(const char *command, const char *path)
{
    UINT32 n = length_of(path), c = length_of(cwd);

    if (path[0] == '/') {
        copy((UINT8 *)joined, (const UINT8 *)path, n + 1);
    } else {
        copy((UINT8 *)joined, (const UINT8 *)cwd, c);
        joined[c] = '/';
        copy((UINT8 *)joined + c + 1, (const UINT8 *)path, n + 1);
    }
    if (n > IO_PATH_MAX ||
        !normalize(joined, resolved, sizeof resolved)) {
        refuse(command, path, ERR_INVALID_ARGS);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------
   Commands. */

static void help(void)
{
    show("help              this list\n"
         "whoami            the user this session is for\n"
         "ls [path]         list a directory\n"
         "cat path          show a file\n"
         "cd [path]         change directory; home if no path\n"
         "run path [args]   run a program from /store\n"
         "user create       create an account (administrators only)\n"
         "logout            end the session\n");
}

/* The user, on the console and in the debug log, which shows that the
   shell ran the command. */
static void whoami(void)
{
    const char *role =
        user.flags & ACCOUNT_ADMINISTRATOR ? ", an administrator" : "";

    show_bytes(user.name, user.name_length);
    show(", id ");
    show_number(user.id);
    show(role);
    show("\n");
    say("nova-shell: whoami: ");
    say_printable(user.name, user.name_length);
    say(", id ");
    say_number(user.id);
    say(role);
    say("\r\n");
}

static void collect(void *context, const UINT8 *name, UINT32 length,
                    UINT32 kind)
{
    LISTING *l = context;
    const char *mark = kind == IO_KIND_DIRECTORY ? "/"
                       : kind == IO_KIND_LINK    ? "@"
                                                 : "";
    UINT32 mark_length = length_of(mark);

    if (l->cut || LISTING_MAX - l->at < length + mark_length + 1) {
        l->cut = 1;
        return;
    }
    copy(listing + l->at, name, length);
    l->at += length;
    copy(listing + l->at, (const UINT8 *)mark, mark_length);
    l->at += mark_length;
    listing[l->at++] = '\n';
}

static void list(void)
{
    LISTING l;
    UINT32 status;

    if (!resolve("ls", word_count > 1 ? words[1] : cwd))
        return;
    l.at = 0;
    l.cut = 0;
    status = view_list(&view, resolved, collect, &l);
    if (status != OK) {
        refuse("ls", resolved, status);
        return;
    }
    show_bytes(listing, l.at);
    if (l.cut)
        show("(cut off)\n");
}

static void cat(void)
{
    IO_ATTRIBUTES attributes;
    UINT32 channel, count, status;
    UINT64 at = 0;

    if (word_count != 2) {
        show("cat path\n");
        return;
    }
    if (!resolve("cat", words[1]))
        return;
    status = view_open(&view, resolved, IO_RIGHT_READ, &channel, &attributes);
    if (status == OK && attributes.kind != IO_KIND_FILE) {
        handle_close(channel);
        status = ERR_NOT_SUPPORTED;
    }
    if (status != OK) {
        refuse("cat", resolved, status);
        return;
    }
    do {
        status = io_read(channel, at, piece, sizeof piece, &count);
        if (status == OK)
            show_bytes(piece, count);
        at += count;
    } while (status == OK && count > 0);
    handle_close(channel);
    if (status != OK)
        refuse("cat", resolved, status);
}

static void count_entry(void *context, const UINT8 *name, UINT32 length,
                        UINT32 kind)
{
    (void)context;
    (void)name;
    (void)length;
    (void)kind;
}

/* A directory is one the view lists: a directory a handle reaches, or a
   prefix's component, such as /users above the home. */
static void change_directory(void)
{
    UINT32 status;

    if (!resolve("cd", word_count > 1 ? words[1] : home))
        return;
    status = view_list(&view, resolved, count_entry, 0);
    if (status != OK) {
        refuse("cd", resolved, status);
        return;
    }
    copy((UINT8 *)cwd, (const UINT8 *)resolved, length_of(resolved) + 1);
}

/* Ask the launcher to run the program at resolved with the words after
   it as arguments, and wait for it to end. */
static void run(void)
{
    IO_REQUEST *request = (IO_REQUEST *)message;
    LAUNCHER_RUN_REQUEST *r =
        (LAUNCHER_RUN_REQUEST *)(message + sizeof *request);
    UINT8 *data = message + sizeof *request + sizeof *r;
    UINT32 path_length, at, n, i, status, reply_length = 0, count = 0;
    UINT32 observed, transaction, state = 0, exit_code = 0;

    if (word_count < 2) {
        show("run path [args]\n");
        return;
    }
    if (!resolve("run", words[1]))
        return;
    path_length = length_of(resolved);
    copy(data, (const UINT8 *)resolved, path_length);
    at = path_length;
    for (i = 2; i < word_count; i++) {
        n = length_of(words[i]) + 1;
        copy(data + at, (const UINT8 *)words[i], n);
        at += n;
    }
    request->operation = LAUNCHER_RUN;
    request->offset = 0;
    request->length = sizeof *r + at;
    request->rights = 0;
    request->flags = 0;
    request->reserved = 0;
    r->path_length = path_length;
    r->argument_bytes = at - path_length;
    status = io_send(launcher, request, sizeof *request + request->length,
                     0, 0);
    transaction = request->transaction;
    if (status == OK)
        status = object_wait_one(launcher,
                                 CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = channel_read(launcher, message, sizeof message, received,
                              CHANNEL_MAX_HANDLES, &reply_length, &count);
    if (status == OK)
        status = reply_length < sizeof(IO_REPLY) ||
                         ((const IO_REPLY *)message)->transaction !=
                             transaction
                     ? ERR_BAD_STATE
                     : ((const IO_REPLY *)message)->status;
    if (status == OK &&
        (count != 1 || received[0].type != TYPE_PROCESS))
        status = ERR_BAD_STATE;
    if (status != OK) {
        for (i = 0; i < count; i++)
            handle_close(received[i].handle);
        refuse("run", resolved, status);
        return;
    }
    status = object_wait_one(received[0].handle, PROCESS_TERMINATED,
                             DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = process_info(received[0].handle, &state, &exit_code);
    handle_close(received[0].handle);
    if (status != OK) {
        refuse("run", resolved, status);
        return;
    }
    show(resolved);
    show(" ended with exit code ");
    show_number(exit_code);
    show("\n");
    say("nova-shell: ");
    say(resolved);
    say(" ended with exit code ");
    say_number(exit_code);
    say("\r\n");
}

/* Read a line into secret without echo. */
static UINT32 ask_secret(const char *prompt, SECRET *secret)
{
    UINT32 status;

    show(prompt);
    status = console_read_line(console, CONSOLE_NO_ECHO, secret->bytes,
                               sizeof secret->bytes, &secret->length);
    show("\n");
    return status;
}

/* Send the accounts service a request to create the account named
   new_name with password, then wipe the request, which holds it. */
static UINT32 create_account(ACCOUNTS_USER *created)
{
    IO_REQUEST *request = (IO_REQUEST *)message;
    ACCOUNTS_CREDENTIALS *c =
        (ACCOUNTS_CREDENTIALS *)(message + sizeof *request);
    UINT8 *data = message + sizeof *request + sizeof *c;
    IO_REQUEST sent;
    UINT32 status, reply_length = 0, i;

    request->operation = ACCOUNTS_CREATE;
    request->offset = 0;
    request->rights = 0;
    request->flags = 0;
    request->reserved = 0;
    c->name_length = new_name_length;
    c->password_length = password.length;
    copy(data, new_name, new_name_length);
    copy(data + new_name_length, password.bytes, password.length);
    request->length = sizeof *c + new_name_length + password.length;
    status = io_send(administrator, request,
                     sizeof *request + request->length, 0, 0);
    sent = *request;
    wipe(message, sizeof *request + sent.length);
    if (status == OK)
        status = io_receive(administrator, &sent, message, sizeof message,
                            &reply_length, 0);
    if (status == OK) {
        if (reply_length != sizeof(IO_REPLY) + sizeof *created)
            return ERR_BAD_STATE;
        for (i = 0; i < sizeof *created; i++)
            ((UINT8 *)created)[i] = message[sizeof(IO_REPLY) + i];
    }
    return status;
}

static void user_create(void)
{
    ACCOUNTS_USER created;
    UINT32 status;

    if (word_count != 2 || !is(words[1], "create")) {
        show("user create\n");
        return;
    }
    if (administrator == HANDLE_INVALID) {
        refuse("user create", "this session holds no administrator handle",
               ERR_ACCESS_DENIED);
        return;
    }
    show("new user's name: ");
    status = console_read_line(console, CONSOLE_ECHO, new_name,
                               sizeof new_name, &new_name_length);
    if (status == OK && new_name_length > ACCOUNT_NAME_MAX)
        status = ERR_INVALID_ARGS;
    if (status == OK)
        status = ask_secret("password: ", &password);
    if (status == OK)
        status = ask_secret("password again: ", &again);
    if (status == OK && password.length > ACCOUNT_PASSWORD_MAX)
        status = ERR_INVALID_ARGS;
    if (status == OK &&
        (password.length != again.length ||
         !same(password.bytes, again.bytes, password.length))) {
        show("The passwords differ.\n");
        forget(&password);
        forget(&again);
        return;
    }
    forget(&again);
    if (status == OK) {
        show("Creating the account...\n");
        status = create_account(&created);
    }
    forget(&password);
    if (status != OK) {
        refuse("user create", "the account was not created", status);
        if (status == ERR_INVALID_ARGS)
            show("A name is 1 to 32 lowercase letters, digits and '-', "
                 "starting with a letter; a password has 8 to 128 "
                 "characters.\n");
        return;
    }
    show("Created ");
    show_bytes(created.name, created.name_length < ACCOUNT_NAME_MAX
                                 ? created.name_length
                                 : ACCOUNT_NAME_MAX);
    show(", id ");
    show_number(created.id);
    show(".\n");
    say("nova-shell: ");
    say_printable(user.name, user.name_length);
    say(" created an account\r\n");
}

static void logout(void)
{
    show("Goodbye.\n");
    process_exit(EXIT_LOGGED_OUT);
}

/* ------------------------------------------------------------------------
   The loop. */

typedef struct {
    const char *name;
    void (*run)(void);
} COMMAND;

static const COMMAND commands[] = {
    {"help", help},
    {"whoami", whoami},
    {"ls", list},
    {"cat", cat},
    {"cd", change_directory},
    {"run", run},
    {"user", user_create},
    {"logout", logout},
};

/* The command word names, or 0. */
static const COMMAND *command_named(const char *word)
{
    UINT32 i;

    for (i = 0; i < sizeof commands / sizeof commands[0]; i++)
        if (is(word, commands[i].name))
            return &commands[i];
    return 0;
}

/* Split line into words at spaces, each NUL-terminated in place. */
static void split(UINT32 length)
{
    UINT32 i = 0;

    word_count = 0;
    while (i < length && word_count < WORDS_MAX) {
        while (i < length && line[i] == ' ')
            i++;
        if (i == length)
            break;
        words[word_count++] = &line[i];
        while (i < length && line[i] != ' ')
            i++;
        line[i++] = 0;
    }
}

static void prompt(void)
{
    show_bytes(user.name, user.name_length);
    show("@nova ");
    show(cwd);
    show("> ");
}

static void read_bootstrap(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[SESSION_HANDLES_MAX];
    UINT32 bytes, count, i;

    if (channel_read(bootstrap, &user, sizeof user, handles,
                     SESSION_HANDLES_MAX, &bytes, &count) != OK ||
        bytes != sizeof user || count < SESSION_ADMINISTRATOR ||
        handles[SESSION_DEBUG_LOG].type != TYPE_RESOURCE ||
        user.name_length == 0 || user.name_length > ACCOUNT_NAME_MAX)
        process_exit(EXIT_FAILED);
    for (i = SESSION_CONSOLE; i < count; i++)
        if (handles[i].type != TYPE_CHANNEL)
            process_exit(EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[SESSION_DEBUG_LOG].handle);
    console = handles[SESSION_CONSOLE].handle;
    launcher = handles[SESSION_LAUNCHER].handle;
    if (count > SESSION_ADMINISTRATOR)
        administrator = handles[SESSION_ADMINISTRATOR].handle;
    copy((UINT8 *)home, (const UINT8 *)SESSION_USERS_PREFIX,
         length_of(SESSION_USERS_PREFIX));
    copy((UINT8 *)home + length_of(SESSION_USERS_PREFIX), user.name,
         user.name_length);
    home[length_of(SESSION_USERS_PREFIX) + user.name_length] = 0;
    if (view_add(&view, SESSION_STORE_PREFIX, handles[SESSION_STORE].handle) !=
            OK ||
        view_add(&view, home, handles[SESSION_HOME].handle) != OK)
        process_exit(EXIT_FAILED);
    copy((UINT8 *)cwd, (const UINT8 *)home, length_of(home) + 1);
}

_Noreturn void shell_main(UINT32 bootstrap, UINT64 argument)
{
    const COMMAND *command;
    UINT32 length, status;

    (void)argument;
    read_bootstrap(bootstrap);
    say("nova-shell: a session for ");
    say_printable(user.name, user.name_length);
    say(administrator != HANDLE_INVALID
            ? ", with the administrator handle\r\n"
            : ", without the administrator handle\r\n");
    show("Type help for the commands.\n");
    for (;;) {
        prompt();
        status = console_read_line(console, CONSOLE_ECHO, (UINT8 *)line,
                                   LINE_MAX, &length);
        if (status != OK)
            process_exit(EXIT_FAILED);
        line[length] = 0;
        split(length);
        if (word_count == 0)
            continue;
        command = command_named(words[0]);
        /* A known command's name and nothing typed: a line is whatever
           was typed, and a person who took a prompt a file drew for
           login's may type a password there. */
        say("nova-shell: ");
        say_printable(user.name, user.name_length);
        say(" runs ");
        say(command ? command->name : "no command");
        say("\r\n");
        if (command) {
            command->run();
        } else {
            show(words[0]);
            show(": no such command; help lists them\n");
        }
    }
}
