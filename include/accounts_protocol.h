/** @file
    The accounts service's protocol (docs/spec/accounts.md A6): requests
    on a channel to the service, framed as the directory protocol frames
    them (directory_protocol.h): an IO_REQUEST with the operation and
    length the bytes that follow it, answered by one IO_REPLY carrying its
    transaction and a status of syscalls.md S7. Every field is
    little-endian.

    A channel to the service is a login channel or an administrator
    channel, which the service records when it takes the channel; the
    kernel knows neither. A login channel, which only the first process
    gives, on the service's control channel and to login, carries
    ACCOUNTS_COUNT, ACCOUNTS_CREATE_FIRST and ACCOUNTS_LOGIN. An
    administrator channel, which the service gives only to an
    administrator's session, carries ACCOUNTS_CREATE. Any other request
    is answered with ERR_ACCESS_DENIED, and every handle a request carries
    is closed.

    The control channel, from the first process, carries ACCOUNTS_SETUPs
    of ACCOUNTS_SETUP_SIZE bytes, a transaction ID of 0 and kind
    ACCOUNTS_SETUP_LOGIN, each with one handle, the service's end of a new
    login channel, with ACCOUNTS_LOGIN_END_RIGHTS. There is no reply; a
    setup the service cannot take closes its handle, which login sees as
    its channel's peer closing.

    Count: ACCOUNTS_COUNT with no bytes. The reply's information is the
    number of accounts, every record in /data/accounts (A3).

    Credentials: ACCOUNTS_CREATE_FIRST, ACCOUNTS_LOGIN and ACCOUNTS_CREATE
    carry an ACCOUNTS_CREDENTIALS, then the name's bytes, then the
    password's, and length is exactly their sum.

    Log in: ACCOUNTS_LOGIN. On OK the reply's bytes are an ACCOUNTS_USER
    and it carries the session's handles: the home directory, and for an
    administrator an administrator channel after it. A name with no
    account and a wrong password give the same status, ERR_ACCESS_DENIED,
    after the same hash and the same delay (A5).

    Create the first account: ACCOUNTS_CREATE_FIRST, only while there is
    no account (ERR_ALREADY_EXISTS once there is one). The account is an
    administrator's, and the reply is as a login's.

    Create an account: ACCOUNTS_CREATE, on an administrator channel. The
    account is not an administrator's. The reply carries an ACCOUNTS_USER
    and no handle. Once ACCOUNTS_MAX accounts exist, a creation fails with
    ERR_NO_RESOURCES (A3). */

#ifndef NOVA_ACCOUNTS_PROTOCOL_H
#define NOVA_ACCOUNTS_PROTOCOL_H

#include "native_abi.h"

/** @name Bootstrap handles
    The service's bootstrap message, from the first process: these
    handles in this order, and no bytes. The two directories carry
    IO_RIGHT_READ, _WRITE and _LIST.
    @{ */
#define ACCOUNTS_BOOTSTRAP_DEBUG_LOG 0 /**< the debug log's resource */
#define ACCOUNTS_BOOTSTRAP_SELF 1      /**< its process, with MAP */
#define ACCOUNTS_BOOTSTRAP_RECORDS 2   /**< /data/accounts */
#define ACCOUNTS_BOOTSTRAP_USERS 3     /**< /users */
#define ACCOUNTS_BOOTSTRAP_CONTROL 4   /**< its end of the control channel */
#define ACCOUNTS_BOOTSTRAP_HANDLES 5   /**< how many handles it carries */
/** @} */

/** @name The control channel
    @{ */
/** a setup's kind: it carries the service's end of a new login
    channel */
#define ACCOUNTS_SETUP_LOGIN 1
#define ACCOUNTS_SETUP_SIZE 8 /**< the bytes of a setup */
/** @} */

/** A message on the control channel. */
typedef struct {
    UINT32 transaction; /**< 0 */
    UINT32 kind;        /**< ACCOUNTS_SETUP_LOGIN */
} ACCOUNTS_SETUP;

/** The rights each end of a login channel travels with: requests and
    replies, waited for, and passed on nowhere. */
#define ACCOUNTS_LOGIN_END_RIGHTS                                         \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)

/** @name Operations
    @{ */
#define ACCOUNTS_COUNT 1        /**< count the accounts */
#define ACCOUNTS_CREATE_FIRST 2 /**< create the first account */
#define ACCOUNTS_LOGIN 3        /**< log in */
#define ACCOUNTS_CREATE 4       /**< create an account */
/** @} */

/** @name Names and passwords
    A name: 1 to ACCOUNT_NAME_MAX bytes, a lowercase ASCII letter, then
    lowercase letters, digits and '-' (A2). A password: ACCOUNT_PASSWORD_MIN
    to ACCOUNT_PASSWORD_MAX bytes of any value.
    @{ */
#define ACCOUNT_NAME_MAX 32      /**< the longest name, in bytes */
#define ACCOUNT_PASSWORD_MIN 8   /**< the shortest password, in bytes */
#define ACCOUNT_PASSWORD_MAX 128 /**< the longest password, in bytes */
/** @} */

/** The most accounts: more than one machine's users, and few enough that
    the service's scan of their names fits a static table. A directory
    with more fails the scan, so the service creates no account past it. */
#define ACCOUNTS_MAX 256

/** An account's flags. */
#define ACCOUNT_ADMINISTRATOR 0x1

/** @name Reply handles
    The handles a login's reply carries, in this order.
    @{ */
#define ACCOUNTS_REPLY_HOME 0 /**< the user's home directory */
/** an administrator channel, for an administrator */
#define ACCOUNTS_REPLY_ADMINISTRATOR 1
/** @} */

/** What a request that names an account starts with; the name's bytes
    and then the password's follow it. */
typedef struct {
    UINT32 name_length;     /**< the name's bytes */
    UINT32 password_length; /**< the password's bytes */
} ACCOUNTS_CREDENTIALS;

/** An account, as a reply reports it. */
typedef struct {
    UINT32 id;                    /**< its id, 1 or more, given once (A2) */
    UINT32 flags;                 /**< ACCOUNT_ flags */
    UINT32 name_length;           /**< the name's bytes */
    UINT8 name[ACCOUNT_NAME_MAX]; /**< the name */
} ACCOUNTS_USER;

_Static_assert(sizeof(ACCOUNTS_SETUP) == ACCOUNTS_SETUP_SIZE,
               "one layout everywhere");
_Static_assert(sizeof(ACCOUNTS_CREDENTIALS) == 8, "one layout everywhere");
_Static_assert(sizeof(ACCOUNTS_USER) == 44, "one layout everywhere");

#endif
