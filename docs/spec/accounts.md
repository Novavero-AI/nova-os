# Accounts

Users of nova-os: their accounts and where they are kept, how a password
is checked, the login program on the console, the session a login opens,
the shell that runs in it, and the first boot, which has no account.
[objects.md](objects.md) decides that authority comes only from handles,
[storage.md](storage.md) the directory protocol and per-process views,
and [console.md](console.md) the console's sessions; this document
builds on all three.

Each item gives the design, why, what it costs, and the main alternative
not taken with that alternative's cost. RFC 9106 (Argon2) and RFC 7693
(BLAKE2) are cited by section at the versions pinned in
[README.md](README.md#cryptography).

## A1. The parts

**Decided.**

| Part | What it is | What it holds |
|---|---|---|
| The accounts service, `accounts.exe` | A user-mode service that keeps the accounts, checks passwords and creates accounts and homes | `/data/accounts` and `/users`, each to read, write and list; a control channel from the first process; its own process with `MAP`, for the memory Argon2id fills; the debug log |
| Login, `login.exe` | The program on the console that asks for a name and a password, opens a session, and starts the shell in it | A console session and a login channel to the accounts service, each handed over by the first process ([A10](#a10-supervision)); `/store` to read, list and map executable; a store control channel of its own; a job to create the sessions' processes in; its own process with `MAP`; the debug log |
| The shell, `shell.exe` | The session's command line | The session's handles ([A7](#a7-the-session)) |

The first process starts the service and login, each in a job of its
own, from the `nova-session` store path, gives each the handles in its
row, and keeps both running ([A10](#a10-supervision),
`init/login_start.c`). The protocols are `include/accounts_protocol.h`
and `include/session.h`. With no data disk, and so no `/data` and
`/users`, neither starts.

- **Why:** the authority to read every account and open every home is
  the authority to log anyone in, so it sits in the one process that
  checks passwords, not in login, which talks to whoever is at the
  console, nor in a shell. Login holds what starting a session needs,
  and a session holds what its user may use (DESIGN.md, principle 7).
- **Cost:** three programs and two protocols where one program could
  check a password and start a shell; the service is trusted with every
  user's files.
- **Not taken:** login holding `/users` and the records, as Unix's
  login reads `/etc/shadow` as root. Its cost is that the program that
  parses what a stranger types holds every user's files.

## A2. Accounts

**Decided.** An account has a name, a numeric id and an administrator
flag.

1. **A name** is 1 to 32 bytes: a lowercase ASCII letter, then
   lowercase letters, digits and `-`. It is the account's record's file
   name and its home's directory name.
2. **An id** is a 32-bit number from 1, given once: an account created
   gets one more than the largest id of the accounts there are. No
   account is removed in this version, so no id is given twice.
3. **The administrator flag.** An administrator's login gets an
   administrator channel to the accounts service, which creates accounts
   ([A6](#a6-the-accounts-service)). The flag does nothing else: the
   kernel and the file systems know no users (objects.md O2: no access
   tokens or security descriptors). The first account is an
   administrator's; one an administrator creates is not.
4. **A password** is 8 to 128 bytes of any value.

- **Why these names:** a name becomes a path component in two volumes
  and a word in prompts and logs. ASCII lowercase leaves no two names
  that look alike or differ only in case, which a person reading a
  prompt cannot tell apart and a case-insensitive file system cannot
  either; a leading letter keeps `.`, `..` and `-`-led words out.
- **Why an id:** a name is what a person types, and is a path; the id
  names the account in logs and records independently of it
  (DESIGN.md, principle 8).
- **Why a flag and a channel:** the power to create accounts is then a
  handle a session holds or does not, which the shell cannot forge and
  a program run from it is not given ([A7](#a7-the-session)).
- **Why 8 to 128 bytes:** guessing costs least against short passwords,
  whatever the hash costs per guess; 128 bounds a request, though
  Argon2 takes up to 2^32 - 1 bytes (RFC 9106 3.1).
- **Cost:** no names outside ASCII; one administrator, the first, since
  no request makes another; ids that only grow.
- **Not taken:** a superuser id, 0 in POSIX, that every check compares
  with, or NT's administrators group in a token checked against each
  object's security descriptor. Their cost is a second authority beside
  handles, which objects.md O2 and O5 rule out.

## A3. Records

**Decided.** Each account is a file, `/data/accounts/<name>`, of 116
bytes, every integer little-endian (`accounts/record.h`):

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `NOVAACCT` |
| 8 | 4 | Version, 1 |
| 12 | 4 | Id, at least 1 |
| 16 | 4 | Flags: bit 0 the administrator; the others 0 |
| 20 | 4 | The name's length |
| 24 | 32 | The name, zeros after it |
| 56 | 4 | Argon2id's lanes, p |
| 60 | 4 | Its memory in KiB, m |
| 64 | 4 | Its passes, t |
| 68 | 16 | The salt |
| 84 | 32 | The tag |

1. A record is taken only whole and exact: 116 bytes, the magic and
   version, an id of at least 1, no undefined flag, a name that follows
   A2 and equals the file's, zeros after it, and parameters within the
   service's bounds: 1 to 16 lanes, 8 KiB per lane (RFC 9106 3.1) to
   64 MiB of memory, and 1 to 10 passes. A file that fails any check is
   no account: a login for its name fails, and while it is there no
   account is created, since its id could be given twice.
2. A record is written under a name no account has, `.<name>.new`,
   renamed to the account's name in one request once the write is
   answered ([storage.md F1](storage.md#f1-the-directory-protocol)), and
   the volume flushed. Any commit that holds the rename holds the whole
   record, and a crash keeps each commit whole or none of it
   ([F6](storage.md#f6-pools-volumes-and-the-commit)), so the name holds
   a whole record or none. A temporary name is no account's, so a crash
   before the rename commits leaves no account and no half of one.
3. An account's home is `/users/<name>`, created before its record, so
   an account never lacks a home; a home left by a creation a crash cut
   short is opened again by the next one for the name.
4. A file in `/data/accounts` whose name is no account's is ignored.
   No account is cached: the service reads the records when it needs
   them, so what it answers is what the volume holds.
5. There are at most 256 accounts (`ACCOUNTS_MAX`), the most names the
   service's scan holds; once there are that many, creating one fails
   with `ERR_NO_RESOURCES`, since one more would fail every scan and
   with it every login.

- **Why a file per account:** creating an account adds a name and never
  rewrites another account's bytes, and a damaged file costs one
  account. A fixed layout read field by field leaves the parser no
  lengths to follow and no text to scan, and the parameters in each
  record let the service change them for new accounts without breaking
  old ones.
- **Cost:** counting accounts lists the directory; creating one reads
  every record, for the largest id; a person cannot read a record as
  text. A home outlives the loss of its record: if `/data/accounts` is
  lost and `/users` kept, the account next given a former user's name
  gets that user's files.
- **Not taken:** one file of every account, as Unix's `/etc/passwd`
  and `/etc/shadow`. Its cost is a rewrite of every account to change
  one, with a crash in the middle able to lose them all unless the
  rewrite is itself a rename; and a text format's parser.

## A4. Hashing passwords

**Decided.** A password's tag is Argon2id's (RFC 9106 3), version 0x13,
with a 16-byte salt from the kernel's generator (`random_read`,
[syscalls.md S8](syscalls.md#s8-the-first-calls)), no secret and no
associated data, and a 32-byte tag, with RFC 9106's second recommended
option (4): 3 passes, 4 lanes and 64 MiB of memory. BLAKE2b, Argon2's H
(RFC 9106 3.2), is RFC 7693's, unkeyed. Both are C in `accounts/`. When
the service starts, it checks BLAKE2b against RFC 7693's
BLAKE2b-512("abc") (Appendix A) and its self-test's hash of keyed and
unkeyed hashes of six lengths and four sizes (Appendix E), and
Argon2id against RFC 9106's test vector (5.3), and ends without serving
if either differs. It maps the memory once, at start, and wipes it after
each hash, with Argon2's working blocks and the stack below it, the
memory-wiping option of RFC 9106 4, step 9.

- **Why Argon2id:** RFC 9106 4 names it for settings where an attacker
  may run code on the same machine or read its memory, and for anyone
  who is unsure; a login service is such a setting. Memory per guess is
  what makes a guess dear on hardware built to guess.
- **Why these parameters:** RFC 9106 4 offers its second option for
  machines with "much less memory" than its first's 2 GiB, and the
  smallest machines nova-os runs on, a virtual machine of 256 MiB among
  them, are such machines; 64 MiB is a quarter of that one. The service
  computes the lanes one after another on one thread, so 4 lanes cost
  it what 1 would, and keep the option as the RFC gives it. The
  parameters are stored in each record (A3), so a later choice applies
  to new passwords without breaking old ones. The salt is the RFC's
  recommended 128 bits, unique per password, so no two accounts share a
  tag and no table computed before the salt was drawn applies.
- **Why the checks at start:** Argon2 compiled wrong still gives a tag,
  and every tag it gives would match itself, so nothing else would show
  the error; the kernel checks its own SHA-256 and ChaCha20 at boot the
  same way.
- **Cost:** 64 MiB held by the service for its life, and the time of
  196,608 compressions per login and per account created: 9 to 13
  seconds under QEMU's emulation of x86-64 on an Apple M2, against 0.8
  seconds for the same C built unoptimized and run natively there; on
  x86-64 hardware it is not yet measured. Login shows `Checking...`
  while a login is checked and `Creating the account...` while one is
  created, and the shell the latter for `user create`, so no one faces
  a screen that seems to have stopped. One login is checked at a time,
  so a second waits for the first.
- **Not taken:** RFC 9106's first option, 2 GiB, which no small machine
  has to spare; a cheaper hash, such as one SHA-256 of salt and
  password, which costs a guesser almost nothing; Argon2i, whose
  data-independent memory access RFC 9106 recommends over Argon2id only
  where side channels are the whole threat.

## A5. Checking a password

**Decided.**

1. The service recomputes the tag with the record's parameters and salt
   and compares it with the record's in time that depends only on the
   tag's length: every byte is read and combined, and no branch depends
   on one.
2. A name with no record, or that A2 refuses, is checked against a
   stand-in account with the service's own parameters, so a failed
   login does the same hash whether or not the name exists.
3. A failed login is answered `FAILURE_DELAY_SECONDS` (3 seconds) after
   it is checked. The answer waits on its channel; the service serves
   its other channels meanwhile, so one user's failures delay no one
   else.
4. Login ends an attempt after three refused logins in a row, says so,
   and starts the next by asking for a name again.
5. A password is wiped from every buffer it passes through once it is
   used: login's lines and its request, the shell's, the service's
   request buffer after each request, and the console client's reply
   buffer after each read.

- **Why:** a comparison that stops at the first difference tells how
  much of a tag matched; a reply that comes sooner for a name with no
  account tells which names exist. The delay sits in the service, which
  checks every password, so no client can skip it, and its length makes
  guessing online cost seconds per guess on top of the hash while a
  person who mistypes waits a moment. Three refusals bound how long one
  attempt holds the console.
- **Cost:** each failure costs its user three seconds; whoever holds a
  login channel can make the service hash without end, since only the
  first process gives one, to login. The hash is the same, the reads
  before it are not: a name with a record costs an open and a read of
  it, one with none an open that fails, and one A2 refuses no read at
  all, a difference of a few messages to the file system beside seconds
  of hashing. Copies of a password in the kernel's channel queues, in
  the console's line buffer and in registers TinyCC spilled are not
  this design's to wipe.
- **Not taken:** locking an account after some number of failures. Its
  cost is that anyone who knows a name can lock its owner out.

## A6. The accounts service

**Decided.** The service serves two kinds of channel
(`include/accounts_protocol.h`), which it records when it takes one:

| Channel | From | Requests |
|---|---|---|
| Login | The first process, on the service's control channel, one for each start of login or of the service, whose other end it hands login ([A10](#a10-supervision)) | `ACCOUNTS_COUNT`, `ACCOUNTS_CREATE_FIRST`, `ACCOUNTS_LOGIN` |
| Administrator | The service, in an administrator's login reply | `ACCOUNTS_CREATE` |

A login that succeeds is answered with the session's handles: the
user's home, opened from `/users` with read, write and list, and for an
administrator a new administrator channel. Any other request on a
channel is refused with `ERR_ACCESS_DENIED`. The service serves at most
8 channels: the control channel, login's, and one for each
administrator's session open.

- **Why:** a login's result is then the authority itself, handles, not
  a ticket some other check must trust, as the store service answers a
  closure request with the closure's directory
  ([storage.md F5](storage.md#f5-per-process-views)). An administrator
  channel lives as long as the session that holds it: when the shell
  ends, its handles close, and the service drops the channel.
- **Cost:** a session that crashes loses its handles, so a new login is
  the only way back in; login sees the home's channel and could keep
  it, and is trusted not to.
- **Not taken:** a token naming the user, which servers check on each
  request, as NT's access tokens. objects.md O2 rules tokens out.

## A7. The session

**Decided.** The shell login starts for a user holds exactly these
handles (`include/session.h`), and its view is the two directories:

| Handle | In the view as | Rights |
|---|---|---|
| The user's home | `/users/<name>` | Read, write, list |
| The store's root | `/store` | Read, list |
| A console session, which login relays | | The console's session protocol |
| A launcher channel, to login | | `LAUNCHER_RUN` |
| The administrator channel, for an administrator | | `ACCOUNTS_CREATE` |
| The debug log | | Writing lines to the kernel's log |

It holds no other user's home, no `/data`, no root of the namespace, no
job and no process, its own included. A path outside its two prefixes
names nothing in its view, so another user's home is not found; a
session without the administrator channel cannot create accounts.

`run` asks login, the session's launcher, to start a program from
`/store`. Login starts it, as it starts the shell, in the sessions'
job, which the first process makes apart from login's own, with a view
of its closure under `/store` and nothing else, its own process with `MAP`
and the debug log, as the first process starts its test program
([storage.md F16](storage.md#f16-the-first-milestone)), and answers
with the program's process, with `INSPECT` and `WAIT`, by which the
shell waits for its end and reads its exit code. A program run so gets
none of the session's handles.

The shell's console session is a channel login serves: login shows
each write on its own session, reads each line there and answers the
shell with it, and ends the shell's session for any message the
console would end one for ([console.md C4](console.md#c4-sessions)).

- **Why these handles:** a user's shell reads and writes the user's
  files, reads what the store holds, talks to the console, and runs
  programs; each is one handle, and the view is the user's tree. The
  whole store, not a closure, since a shell exists to name programs to
  run, and the store holds no user's data.
- **Why login launches:** the shell then holds no job and no store
  control channel, the authority to create processes and to open the
  closure of any path, which it would hold only to do what login does
  for it; login already holds both to start the shell.
- **Why a program run gets its closure alone:** a program's authority
  is what its derivation declares (DESIGN.md, principle 7), and no
  launcher reads declarations yet; until one does, a program gets what
  every program needs and nothing of its user's.
- **Why login relays the console:** the first process gives console
  sessions and serves no requests after startup (console.md C2), so
  login keeps the one it has; a relay lets it outlive the shell's, and
  keeps whatever the shell sends from ending login's.
- **Cost:** a program run from the shell cannot write to the console or
  reach the user's files yet; every console request of the shell's
  costs a second message and a switch to login; login stays in every
  session, and while the shell waits for a line, login waits with it.
  Programs a shell started run on after it ends, since the kernel has
  no call yet to end a job's processes.
- **Not taken:** a child that inherits its parent's handles, as NT
  handle inheritance and Unix file descriptors do. Its cost is a
  program holding handles nobody chose for it (objects.md O6).

## A8. The shell

**Decided.** Version 0 prompts with `name@nova` and its current
directory, `alice@nova /users/alice>`, reads a line, splits it at
spaces and runs the command:

| Command | Effect |
|---|---|
| `help` | Lists the commands |
| `whoami` | The user's name and id, and whether an administrator |
| `ls [path]` | Lists a directory, the current one if none is given |
| `cat path` | Shows a file |
| `cd [path]` | Changes the current directory, to the home if none is given |
| `run path [args]` | Runs a program from `/store` (A7) and shows its exit code |
| `user create` | Asks for a name and a password twice, without echo, and creates the account; refused in a session without the administrator channel |
| `logout` | Ends the session |

A path is absolute or relative to the current directory, and resolved
as the path functions resolve one (storage.md F5): `..` never climbs
above `/`, and every lookup goes through the view. A refusal shows the
command, the path and the status, by its name in
[syscalls.md S7](syscalls.md#s7-error-reporting).

- **Why:** these are the commands a person needs to see who they are,
  look around their files and the store, run a program and, for an
  administrator, add a user, and each shows what the session can and
  cannot reach.
- **Cost:** no writing, quoting, pipes or scripts. The console has no
  key that only login answers, so a file the shell shows can draw what
  looks like login's prompt, and a person who
  takes it for one types a password into the shell; the shell logs the
  name of a command it knows, a refusal's status and the store path of a
  program it started, never other text typed, so such a password
  reaches no log.
- **Not taken:** a POSIX shell. Its cost is a POSIX layer under it,
  which DESIGN.md, principle 2, makes a subsystem for later.

## A9. The first boot

**Decided.** While `/data/accounts` holds no record, login asks for a
name and a password twice, without echo, and the service creates the
account, an administrator's, and its home, and answers as for a login,
so the first session starts at once. The service refuses
`ACCOUNTS_CREATE_FIRST` once a record exists, checking when it serves
the request. The first process creates `/data/accounts` if it is
missing.

- **Why:** someone must hold the first administrator channel, and on a
  new machine the person at its console is its owner. Creating the
  account where it is used puts no password in the image, which every
  machine built from it would share (DESIGN.md, principle 1: the image
  is the same everywhere).
- **Cost:** whoever reaches the console of a machine with no account
  becomes its administrator; a machine whose only record is damaged
  counts one account, so it offers no first boot and no login until the
  record is repaired outside it.
- **Not taken:** an account and password built into the image. Its
  cost is a credential every machine shares and its owner must know to
  change.

## A10. Supervision

**Decided.** The first process keeps the accounts service and login
running through its supervisor, with the restart policy it gives its
drivers ([ipc.md I10](ipc.md#i10-driver-failure-and-restart)): one that
ends is started again, unless it was restarted within the last 60
seconds, when it is given up. Each start is a new process in a new job
with its handles opened afresh, a store control channel opened through
the first process's own among them (`services.h`).

The first process keeps its end of the service's control channel and
of login's bootstrap channel. After each start of either it makes a new
login channel and hands one end to each, as setups
(`accounts_protocol.h`, `session.h`); after each start of login, and of
the console after the display's release, it gives login a new console
session. Login uses the newest of each, and when one's peer closes
takes the next, so:

| What ends | What follows |
|---|---|
| Login | A new login with a new session and login channel; the service drops the old channel. A shell login ran loses its relayed console and launcher and ends at its next request |
| The service | A new service with a new login channel, which login takes when its old one fails: login says the service restarted and asks again. An administrator's session loses its administrator channel |
| The console | The console's restart, and a new session for login, which carries on, a session in progress included |

- **Why:** a crashed login or service must come back, or the console
  sits with no one to log in. Handing channels over, as the first
  process hands the console a keyboard's endpoint after each start of
  its driver (input.md K4), lets each restart without the other, so a
  restart costs only the requests in flight.
- **Cost:** the first process holds a channel to each and carries a
  hand-over for each pairing; a program that fails twice within a
  minute is given up as a driver is, which leaves the console without a
  client until the next boot. A restart opens its program's file and
  directories through the store service, the namespace server and the
  file system service, waiting on each with no deadline, so one of them
  that hangs stops the supervisor with it; none of the three is itself
  supervised.
- **Not taken:** restarting login and the service together as one
  unit. Its cost is a second service writing `/data/accounts` beside
  the first, which runs on until the last channel to it closes, since
  the kernel has no call that ends a process from outside.

## Changes elsewhere

- **Kernel:** `random_read` (call 42), for the salt
  ([syscalls.md S8](syscalls.md#s8-the-first-calls)).
- **The first process:** starts and supervises the service and login
  (`init/login_start.c`, A10) after the console and the writable file
  system, which give it console sessions and `/data` and `/users`. The
  store service and the namespace server run on after its startup, and
  it keeps its channels to them, through which login reaches `/store`.
  Login replaces the console's demo client
  ([console.md C2](console.md#c2-starting-the-console)).
- **The store service:** a control channel opens another through an
  open of the empty path, so that each start of login gets its own.
