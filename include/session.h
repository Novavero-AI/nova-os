/* A login session (docs/spec/accounts.md A7): the bootstrap message the
   first process starts login with, the one login sends the shell it
   starts for a user, and the launcher protocol the shell asks login to
   run programs by. Every field is little-endian.

   Login's bootstrap message carries the handles LOGIN_ names, in that
   order. Its bytes are the absolute path of the shell's file in /store,
   NUL-terminated. The first process keeps the channel open and sends on
   it, after each start of the console or of the accounts service and
   after login's own start, a LOGIN_SETUP of LOGIN_SETUP_SIZE bytes, a
   transaction ID of 0 and a kind, with one handle: for
   LOGIN_SETUP_CONSOLE a console session's client end, for
   LOGIN_SETUP_ACCOUNTS a login channel to the accounts service. Login
   uses the newest of each, and waits for the next when one's peer
   closes, so a restart of either leaves login running.

   The shell's bootstrap message carries these handles in this order:
   the debug log's resource, the console, the launcher channel, the
   user's home directory, the store's root directory, and for an
   administrator the accounts service's administrator channel last. Its
   bytes are an ACCOUNTS_USER (accounts_protocol.h), the user it runs
   for.

   Run: on the launcher channel, an IO_REQUEST (directory_protocol.h)
   with operation LAUNCHER_RUN and length the bytes that follow it: a
   LAUNCHER_RUN_REQUEST, the path's bytes, then argument_bytes of
   arguments, each NUL-terminated. The path is absolute, below /store,
   and names a program's file. Login starts it as a launcher starts a
   program from /store (launch.h): with a view of its closure under
   /store and nothing else. The reply is an IO_REPLY; on OK it carries
   one handle, the program's process with PROCESS_RIGHT_INSPECT and
   RIGHT_WAIT, by which the shell waits for it and reads its exit
   code. */

#ifndef NOVA_SESSION_H
#define NOVA_SESSION_H

#include "native_abi.h"

#define LOGIN_DEBUG_LOG 0     /* a resource covering the debug log */
#define LOGIN_SELF 1          /* login's process, with MAP */
#define LOGIN_JOB 2           /* sessions' job, with CREATE_PROCESS */
#define LOGIN_STORE 3         /* the store's root: READ, EXECUTE, LIST */
#define LOGIN_STORE_CONTROL 4 /* the store service, with CLOSURE */
#define LOGIN_HANDLES 5

#define LOGIN_SETUP_CONSOLE 1
#define LOGIN_SETUP_ACCOUNTS 2
#define LOGIN_SETUP_SIZE 8

/* Login's bootstrap channel queues the bootstrap message and the setups
   login has yet to take: a session and a login channel from its start,
   and one more of each from a restart of the console or the service
   before login takes them. */
#define LOGIN_BOOTSTRAP_CAPACITY 5

typedef struct {
    UINT32 transaction; /* 0 */
    UINT32 kind;        /* LOGIN_SETUP_CONSOLE or _ACCOUNTS */
} LOGIN_SETUP;

#define SESSION_DEBUG_LOG 0
#define SESSION_CONSOLE 1
#define SESSION_LAUNCHER 2
#define SESSION_HOME 3
#define SESSION_STORE 4
#define SESSION_ADMINISTRATOR 5
#define SESSION_HANDLES_MAX 6

/* Where the session's two directories are in the shell's view: the
   store at STORE_PREFIX, the home at USERS_PREFIX and the user's name. */
#define SESSION_STORE_PREFIX "/store"
#define SESSION_USERS_PREFIX "/users/"

#define LAUNCHER_RUN 1

/* The rights of the process handle a run's reply carries. */
#define LAUNCHER_PROCESS_RIGHTS (PROCESS_RIGHT_INSPECT | RIGHT_WAIT)

typedef struct {
    UINT32 path_length;
    UINT32 argument_bytes;
} LAUNCHER_RUN_REQUEST;

_Static_assert(sizeof(LAUNCHER_RUN_REQUEST) == 8, "one layout everywhere");
_Static_assert(sizeof(LOGIN_SETUP) == LOGIN_SETUP_SIZE,
               "one layout everywhere");

#endif
