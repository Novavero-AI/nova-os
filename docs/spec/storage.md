# Storage

How nova-os names and keeps files: the namespace server's directory
protocol and the per-process views built on it, the native file system,
the content-addressed store at `/store`, how builds write into the store
and how it is collected, which disk holds what, how storage meets record
and replay, and the first milestone.
[objects.md O8](objects.md#o8-the-namespace) decides the namespace's
shape, [ipc.md I9](ipc.md#i9-asynchronous-io-requests) the I/O request
and [replay.md](replay.md) what a recording captures; this document
builds on all three.

Each item gives the design, why, what it costs, and the main alternative
not taken with that alternative's cost. [Given](#given) lists the layout
the design builds on, which [DESIGN.md](../../DESIGN.md) records.

Specifications are cited by the versions in [README.md](README.md): "UEFI
5.3.3" is section 5.3.3 of UEFI 2.11, "virtio 5.2.6.2" a section of VIRTIO
1.4, "NVMe 7.2" a section of the NVM Express Base Specification 2.4, and
"FIPS 180-4" the Secure Hash Standard. Papers and reference documents are
pinned under [Storage design references](README.md#storage-design-references)
and cited by these names, with the page numbers each pinned copy prints:

| Name | Document |
|---|---|
| Dolstra 2006 | The Purely Functional Software Deployment Model, by section and page |
| Rosenblum 1991 | The Design and Implementation of a Log-Structured File System, the preprint, by section and page |
| Bonwick 2003 | The Zettabyte File System, by section and page |
| ZFS on-disk | ZFS On-Disk Specification, Draft, by chapter and section |
| Hitz 1994 | File System Design for an NFS File Server Appliance, TR 3002, by section and page |
| APFS | Apple File System Reference, by page |

Web documentation (Microsoft Learn, fuchsia.dev, OpenZFS, Btrfs, OSTree,
composefs and NILFS) is linked under [References](#references) with the
date each page shows. nova-nix, the Nix implementation that builds
nova-os, is cited by module and function at commit `570dec3`, and
nova-cache, which holds its NAR code, at release 0.12.0.1.

## Given

The layout, as [DESIGN.md](../../DESIGN.md) records it:

| Name | Holds |
|---|---|
| `/store` | Every program, library and the OS itself, read-only and content-addressed |
| `/system` | The running system: a pointer into `/store`, switched atomically |
| `/users/<name>` | A user's files and profile |
| `/data` | Services' state |

- No drive letters, and no `/dev`: a device is an object reached by a
  handle.
- Each process sees only its own view: its closure in `/store` plus what
  it was granted (DESIGN.md, principle 7).
- The Windows compatibility layer maps old-style paths such as
  `C:\nix\store\...` onto `/store` for programs from the Windows seed
  chain.
- A change to the system is a new version, with rollback. User data
  changes in place, and the file system can snapshot it.
- FAT is used only for the EFI System Partition, because the FAT
  specification's licence covers products used in the boot process
  ([README.md](README.md#storage-and-file-systems), clause 1(e)).

## What the store holds

These facts about store paths, checked in the sources cited, constrain
everything below.

1. **A store path is `<store directory>/<hash>-<name>`,** and its hash part
   is a SHA-256 folded to 160 bits by XOR, over a string that names the
   store directory: `type ":sha256:" inner-hash ":" store-directory ":" name`
   (Dolstra 2006, 5.2.2, p. 94; nova-nix `Nix.Hash.makeStorePath`). "Nix
   stores in different locations yield different hash parts" (Dolstra
   2006, p. 94).
2. **nova-nix hashes `/nix/store` on every host.** Every store path it
   computes passes `defaultStoreDir`, `/nix/store`, to `makeStorePath`
   (`makeTextPath`, `makeFixedOutputPath` and `makeOutputPath` in
   `Nix.Hash`), on Windows too. Where the store's own directory differs,
   as `C:\nix\store` does, the builder replaces `/nix/store/` with that
   directory in the builder's arguments and environment when it spawns it
   (`Nix.Builder.onStore`, `storeRewrite`). A Windows-hosted build
   therefore sees its inputs and outputs under `C:\nix\store`, and an
   output that records a store path records it in that spelling.
   Rewriting the spelling inside the seed chain's outputs would change
   their NAR hashes, so on nova-os they keep their names and their bytes.

**Decided:** native builds on nova-os hash `/nix/store` too, so every host
names a derivation's outputs alike, as principle 6's byte-for-byte
comparison with the Windows-hosted build needs. The store's files live at
`/store`, and a builder sees its paths rewritten to `/store` the way a
Windows-hosted builder sees `C:\nix\store`. The cost is a hash that names a
directory the path is never found in.
3. **An input-addressed path's name does not commit to its contents:**
   "the output path of a derivation is computed from the input derivation
   ..., not from the actual contents produced by the builder" (Dolstra
   2006, 6.3, p. 141). Only a content-addressed path, a source or a
   fixed-output derivation's output, has a name computed from its contents
   or its declared hash (Dolstra 2006, 5.3, p. 97, and 5.4.1, p. 106). The
   NAR hash recorded beside a path is
   there "to detect accidental tampering with store objects, which are
   supposed to be read-only" (Dolstra 2006, 5.2.3, p. 96).
4. **A NAR has three kinds of node:** a regular file with an executable
   flag and its bytes, a symbolic link with its target, and a directory
   whose entries are sorted bytewise by name (nova-cache `NovaCache.NAR`;
   Dolstra 2006, pp. 91 to 93). Names and targets are bytes in no stated
   encoding; a name may not be empty, `.` or `..`, or hold `/` or NUL.
   The model keeps no "meta-information about files other than the
   executable bit; notably, time stamps are discarded" (Dolstra 2006,
   5.2.1, p. 91). The NAR hash is the SHA-256 of the serialization.
5. **References are found by scanning** an output for the hash parts of
   the store paths in its inputs' closure (Dolstra 2006, 5.5.1, p. 113),
   and "The set of valid paths is closed under the references relation"
   (Dolstra 2006, Invariant 3, p. 96).
6. **On Windows two NAR fields have their own form.** The executable flag
   is an NTFS alternate data stream, `nova.exec`, whose presence is the
   flag (nova-nix `Nix.Store.ExecBit`). A link's target goes into the NAR
   with `/` for `\`, so an absolute link into the store reads
   `C:/nix/store/...` there (nova-cache `symlinkTargetBytes`).

By fact 2, the Windows spelling the Windows layer maps ([Given](#given))
is in the seed chain's bytes, not in its hashes.

## The namespace

### F1. The directory protocol

**Decided.**

O8 makes a directory a channel to a server that speaks the directory
protocol, and every lookup relative to one. The protocol's requests, each
answered by one reply that carries the request's transaction ID, as in
I9:

| Request | Effect | Right |
|---|---|---|
| `open(path, rights, flags, endpoint)` | Connects `endpoint` to the node at `path` (O8, rule 1). The first message on `endpoint` is the result: a status and the node's attributes ([F2](#f2-attributes)), so the opener learns in one round trip what it opened and which protocol to speak on it. With the create flag, a missing file or directory is created | `READ` to read what is opened, `WRITE` to write or create, `EXECUTE` to map it executable; what is opened gets at most the directory's rights |
| `read_dir(cookie, count)` | Up to `count` entries, each a name and a kind, after `cookie`, in bytewise order of their names, and the cookie to continue from | `LIST` |
| `get_attributes()` | The directory's own attributes | none |
| `remove(path)` | Removes a file, a link or an empty directory | `WRITE` |
| `rename(from, to)` | Moves a node; both paths are resolved against this directory, so a holder moves only within what it holds | `WRITE` |
| `publish(name, directory)`, `replace(name, directory)`, `unpublish(name)` | Only on the namespace server's own directories ([F4](#f4-publishing-file-systems)) | `PUBLISH` |

A file is a channel that speaks I9's requests (read, write, control,
flush, cancel) and two more: `get_attributes`, and `get_section`, which
returns a section holding the file's contents where the server offers one
([F9](#f9-reading-and-verifying-the-store)). A node has one name: there
are no hard links.

- **Why:** these are the requests a shell, a compiler and a launcher
  need: reach a node, list a directory, learn what a node is, change a
  mutable tree. Bytewise order is NAR order, so a store directory lists
  the same everywhere; for a mutable directory it makes the listing
  independent of the directory's history, so a build that lists a
  directory sees the same order on every machine holding the same files
  (DESIGN.md, principle 1). Rights narrow at every open, as handle rights
  narrow at every transfer (O5), so a directory handle bounds everything
  opened through it. One name per node keeps every volume a tree, so
  space accounting and snapshots need no link counts.
- **Cost:** a mutable directory keeps its entries sorted, which a B-tree
  keyed by name does at some cost per insert; programs that make hard
  links fail; every POSIX or Win32 call the protocol lacks is the
  subsystem's to emulate.
- **Not taken:** listing in the order entries were stored, as most file
  systems do. Its cost is an order that differs between two machines
  holding the same files, so a build that lists a directory can differ
  between them.

### F2. Attributes

**Decided.**

| Attribute | Store node | Data-volume node |
|---|---|---|
| Kind: directory, file, symbolic link, service | yes | yes; a service appears only in the namespace server's directories |
| Size in bytes | files | files |
| Executable flag | the NAR's flag | kept, so a tree copied into the store keeps it |
| Content digest | the blob's or node's digest ([F8](#f8-the-store-volume)) | none |
| Modification time | none | set by the file system at each write, from the system's wall-clock service, which this document does not specify |
| Node ID, 64 bits, unique within the volume | none | yes |
| Link target | links | links |

There are no owners, groups, permission bits or access control lists.
Authority comes only from handles (objects.md O2: no access tokens or
security descriptors), so a file records nothing about who may open it.

- **Why:** each attribute has a reader. The kind tells a client which
  protocol to speak; size and modification time serve tools that compare
  them; the executable flag is part of a store path's NAR; the digest
  serves verification and replay ([F15](#f15-store-reads-and-replay)); the
  node ID tells a copy tool whether two paths, through two handles, reach
  one node. A store node has no time and no node ID because its NAR has
  neither, and two copies of one store path must not differ.
- **Cost:** the POSIX and Win32 subsystems invent owners, modes and
  times; for a store node they report one fixed time, as Nix sets store
  files' times "to 0 seconds in the Unix epoch" (Dolstra 2006, 5.5.1,
  p. 112). A program that relies on ownership or an access control list
  to protect a file is protected instead by who holds handles to it.
- **Not taken:** NT's security descriptor on every file, checked at open
  against the caller's token. O2 rules out tokens and descriptors; its
  cost would be a second authority beside handles.

### F3. Symbolic links

**Decided.** A server never follows a symbolic link. An open whose path
reaches a link stops there and replies with the link's target and the
part of the path not yet resolved. The client library resolves the target
against its own view ([F5](#f5-per-process-views)), a relative target
against the link's directory and an absolute one against the view's root,
appends the rest and opens again, following at most `SYMLINK_MAX_HOPS`
links before it fails. A target that starts with a drive letter, as a
Windows-built link into the store does (`C:/nix/store/...`, [fact
6](#what-the-store-holds)), is mapped as the Windows layer maps an
old-style path ([F5](#f5-per-process-views)).

- **Why:** the store holds any NAR exactly, and NARs hold links. Resolved
  by the client, a link is only text: it reaches nothing the reader's view
  does not already hold, so a link inside a store path cannot widen a
  process's authority, and no server needs to know any client's view. A
  relative target that climbs (`../lib`) is resolved textually, as O8
  rule 2 resolves `..`.
- **Cost:** each link on a path costs a round trip, and every client
  library carries the loop.
- **Not taken:** links followed by the server during lookup. A server's
  root is not its client's, so an absolute target would resolve against
  the wrong tree, or the server would have to be told every client's view.

### F4. Publishing file systems

**Decided.** Mounting is O8 rule 3: a file system's root directory
published under a name.

1. `publish(name, directory)` on a namespace-server directory, with
   `PUBLISH`, binds one new name to a directory handle. The name must not
   exist: a publication never hides entries already there, and no name is
   a union of two trees.
2. `replace(name, directory)` swaps the handle behind an existing
   publication in one step, so no open finds the name missing.
3. The supervisor that starts a file system server
   ([ipc.md I10](ipc.md#i10-driver-failure-and-restart)) holds the
   server's root handle and publishes it, and replaces it after a
   restart.
4. `unpublish(name)` removes the name. Channels already open below it keep
   working until their server closes them.
5. Which volume is published under which name is declared in the system
   recipe ([F14](#f14-disks-and-the-system-recipe)).

- **Why:** once every lookup is relative to a handle, binding a name to a
  handle is all a mount needs. One server per name keeps a name's meaning
  local: what a name shows is what one server serves.
- **Cost:** combining two trees under one name needs a server that merges
  them.
- **Not taken:** union and overlay mounts, where a later mount hides or
  merges with what is beneath it. Their cost is a path whose contents
  depend on the order of mounting.

### F5. Per-process views

**Decided.** A process's view is a table in its own memory that maps
path prefixes to directory handles. Its launcher builds the table and
sends it, with the handles, in the bootstrap message
([objects.md O6](objects.md#o6-no-ambient-authority)). The native layer's
path functions resolve a path by its longest matching prefix and send the
rest of it to that prefix's handle; the namespace server takes no part.
Fuchsia hands a new process the same structure, "a table that maps one or
more namespace path prefixes to object handles" ([Fuchsia
Namespaces][fx-ns], "Namespace Transfer").

| Prefix | Handle | From |
|---|---|---|
| `/store` | A closure directory: the store service serves it, and its only entries are the store paths in the program's closure | The store service, given the program's store path |
| Each name the derivation is granted, such as `/users/alice/Documents` or `/data/mail` | A directory handle, narrowed to the rights granted | The launcher's own view |
| `/system` | A read-only directory on the running system's store path; the path's closure joins the `/store` closure directory | The store service, only where the derivation declares it |

1. The store service computes the closure from the references it recorded
   at registration ([F8](#f8-the-store-volume)); the launcher names only
   the program's store path.
2. A launcher grants only what its own view holds, with the same or fewer
   rights.
3. A grant the derivation declares that the launcher cannot give fails the
   launch, naming what is missing. Nothing is left out silently, as O5
   reports a right a caller lacks rather than dropping it.
4. Listing `/` gives the table's first path components, and a listing of
   a directory includes the next component of any prefix nested in it;
   listing `/store` gives the closure only; opening any other store path
   fails as a missing name does.

**Old-style store paths.** The Windows layer turns a path that starts with
`C:\nix\store` into `/store` and the rest, taking `\` and `/` alike as
separators (nova-nix's rewrite writes `C:\nix\store/` before the base
name), and resolves it through the same table. Case-insensitive matching,
which Win32 programs may expect, is the layer's concern and is not
specified here.

- **Why:** principle 7 makes a process's authority exactly the handles its
  creator passes, and a table of prefixes and handles is that authority
  with names on it. Every lookup goes straight to the server that holds
  the name, so the namespace server is not on the path of a running
  program's opens, and its failure (O8's cost) does not stop them. A
  closure directory makes "its closure in `/store`" one handle: a closure
  of hundreds of store paths as a handle each would take several
  bootstrap messages, since a message carries at most 64 handles (ipc.md
  I1), and the store service knows the closure already.
- **Cost:** a view has no single root handle, so giving a child a whole
  view sends every handle again; one path names different objects in
  different processes, by design; the store service keeps one closure set
  per open closure directory. When the server behind a view entry
  restarts, the entry's handle is dead, and the process cannot reopen it
  by name as I10's clients do, since it holds no handle above the entry:
  its launcher sends a new handle, or the process ends.
- **Not taken:** a view root served by the namespace server: one handle
  per process, whose server composes the granted subtrees and forwards
  opens below them. Its cost is per-process state in the namespace
  server, which then sits on every open in the system, so its failure
  stops all of them.

## The native file system

### F6. Pools, volumes and the commit

**Decided.** The native file system keeps its data in pools. A pool is
one GPT partition with a nova-os partition type GUID (disk-layout.md,
"The system partition"). It holds volumes: one store volume per system
([F8](#f8-the-store-volume)) and any number of data volumes
([F10](#f10-data-volumes-and-snapshots)). One file system server serves a
pool.

Every write is copy-on-write: no block that a committed tree references
is overwritten. Changes collect in memory into a transaction group, and a
commit:

1. writes every new block to free space;
2. flushes the device's volatile cache (a `VIRTIO_BLK_T_FLUSH` request,
   virtio 5.2.6.2; the Flush command, NVMe 7.2);
3. writes the next superblock slot: the group's number, the root block's
   pointer and checksum, and the slot's own checksum;
4. flushes again.

Superblock slots form two rings, one at each end of the partition, as GPT
keeps a primary and a backup header. Opening a pool takes the valid slot
with the highest group number. A block that a commit releases is freed
unless a snapshot still holds it ([F10](#f10-data-volumes-and-snapshots)),
and a freed block is reused only once that commit is durable, so the tree
it replaces stays intact until the new one is.

A volume's owner, such as the store service for the store volume, groups
its updates into a transaction: the server applies a transaction whole,
within one transaction group, or not at all. A registration
([F12](#f12-builds-and-substitution)), a collection and a profile switch
([F13](#f13-profiles-system-and-garbage-collection)) are each one
transaction, so a crash never leaves half of one on the disk.

- **Why:** the disk holds a consistent tree at every instant, so recovery
  after a power cut, or after the file system server is killed and
  restarted (DESIGN.md, principle 2; ipc.md I10), is reading the newest
  valid superblock, with no log to replay and no repair pass. A parent
  block is written after its children, so it can carry their checksums
  ([F7](#f7-checksums)). A snapshot is a kept root
  ([F10](#f10-data-volumes-and-snapshots)), and switching the system is
  one commit ([F13](#f13-profiles-system-and-garbage-collection)). ZFS
  commits by rewriting the root, "in effect switching atomically from the
  old tree of blocks to the new tree of blocks" (Bonwick 2003, 3.3, p. 8),
  as WAFL (Hitz 1994, 3.6, p. 14) and APFS (APFS, pp. 8 and 26) do in
  their own forms.
- **Cost:**
  - A file its program overwrites in place, as a database does, moves on
    every commit and fragments: "Rewriting a file scatters its new blocks
    wherever there was free space" ([OpenZFS Copy-on-Write][zfs-cow]),
    and Btrfs's need for defragmentation "stems from the COW design that
    BTRFS is built on and is inherent" ([Btrfs
    Defragmentation][btrfs-defrag]).
  - Every commit rewrites the path from each changed block to the root;
    grouping changes amortizes it.
  - Deleting needs free space, since freeing blocks writes new metadata:
    "Even deleting data requires allocating" ([OpenZFS
    Copy-on-Write][zfs-cow]), and copy-on-write "may cause nonintuitive
    out-of-space errors when the disk is nearly full" (Bonwick 2003, 5,
    p. 11). The pool keeps a reserve.
  - A `flush` of one file costs a whole commit. ZFS's intent log makes
    such a flush cheaper by logging the change and replaying it after a
    crash (ZFS on-disk, chapter 7); it is left out of the first
    version.
  - No design survives a device that reports writes durable before they
    are; virtio requires a device backed by persistent storage to commit
    them first, since "Failure to do so can cause data loss in case of a
    crash" (virtio 5.2.6.2).
- **Not taken:**
  - **Journaling,** as NTFS does: a log of metadata changes, replayed
    after a crash, with blocks updated in place. NTFS "Records the
    metadata operations of a transaction in a log file", and "guarantees
    user data only if the program used to create the data uses the
    FILE_FLAG_WRITE_THROUGH Win32 flag"; after a failure a file shows
    "either the previous data, the new data, or zeros" ([How NTFS
    Works][ntfs-works]). Its cost here: data overwritten in place cannot
    keep a checksum consistent with itself through a crash unless the
    data is journaled too, and so written twice, and snapshots need a
    mechanism of their own. ReFS shows the trade: it checksums file data
    only on request, and then "all write operations become
    allocate-on-write operations" ([ReFS integrity
    streams][refs-integrity]). Fuchsia's Fxfs keeps a journal that "does
    not include data", and checksums data written copy-on-write so that
    it "can detect a torn COW write" ([RFC-0136][fx-fxfs], "Journal").
  - **A log-structured file system** (Rosenblum 1991; NILFS): every write
    appends to a log, which makes small writes fast (Sprite LFS was
    "almost ten times as fast as SunOS for the create and delete phases"
    of a small-file benchmark, Rosenblum 1991, 5.1, p. 11) and snapshots
    natural: NILFS "generates checkpoints every few seconds", and "A
    snapshot is the checkpoint marked not to be deleted by GC" ([What is
    NILFS?][nilfs]). Its cost is a cleaner that copies live data out of
    partly empty segments. Cleaning segments at utilization u gives a
    write cost of 2/(1-u), and they "must have a utilization of less than
    .8" for LFS to beat the Unix file system of its day (Rosenblum 1991,
    3.4, p. 6); a file written randomly and read sequentially needs seeks
    (5.1, p. 12). Collecting `/store` frees store paths scattered through
    the log, which the cleaner would then have to compact.

### F7. Checksums

**Decided.** Every block pointer carries the SHA-256 (FIPS 180-4) of the
block it points to, and every superblock slot its own. A block read from
the disk is checked against its pointer before it is used; a mismatch is
an error, never data. A store blob's blocks are checked by the blob's hash
tree instead ([F9](#f9-reading-and-verifying-the-store)), so they are not
hashed twice. The pool's own metadata (pointer blocks and tables) is
written in two copies at different places, file data in one.

A failed check on a store blob is repaired by fetching the store path
again or rebuilding it ([F9](#f9-reading-and-verifying-the-store)). On
user data it is reported; only a mirror, which this design leaves for
later, could repair it.

- **Why:**
  - A checksum kept in the parent detects what one kept with its block
    cannot: a write sent to the wrong place, or one the device dropped,
    leaves a block that agrees with its own checksum. ZFS keeps "A
    block's checksum ... in its parent indirect block", which "makes the
    checksums self-validating" (Bonwick 2003, 3.2.1, p. 7), and names
    "misdirected reads, misdirected writes, and phantom writes" among the
    errors disks make (2.6, p. 4). Btrfs keeps in each parent the
    generation it expects of the child, which "allows Btrfs to detect
    phantom or misplaced writes on the media" ([Btrfs design][btrfs-design]).
  - One algorithm serves the pool, the store's content identity and
    nova-nix's NAR hashes, and the kernel already implements it. A store
    blob needs a cryptographic digest in any case, since sharing one blob
    between store paths is safe only if no two contents share a digest
    ([F8](#f8-the-store-volume)).
  - Two copies of metadata keep one bad block from cutting off the tree
    beneath it.
- **Cost:** SHA-256 for every block written and every block read from the
  disk (not from memory). The server's SHA-256 is C compiled by TinyCC,
  which does not optimize, and its throughput against a disk's is
  unmeasured; it is measured against a disk before the pool format is
  fixed. A 32-byte checksum in every pointer; metadata written twice.
- **Not taken:** a fast non-cryptographic checksum for pool blocks, with a
  cryptographic hash only where content identity needs one. ZFS's default
  is "fletcher4 for non-deduped datasets and sha256 for deduped datasets"
  ([OpenZFS Checksums][zfs-checksums]); Btrfs checksums "Data and
  metadata ... by default" with CRC32C as the default ([Btrfs
  Checksumming][btrfs-csum]); APFS checksums each object with Fletcher 64
  in the object's own header, not its parent's (APFS, pp. 10 to 11), and
  describes no checksum of ordinary file data. Its cost is two algorithms,
  and pool checksums that cannot serve as content identity, so the store
  would hash its data a second time anyway.

### F8. The store volume

**Decided.** The store volume holds three kinds of record:

1. **Blob:** a file's bytes, named by its digest, the root of a SHA-256
   hash tree over its 4,096-byte blocks, bound to the blob's length. One
   blob holds each distinct content in the whole store.
2. **Node:** a NAR node without file contents: a directory's entries in
   bytewise order, each a name and a node digest; a file's executable
   flag, length and blob digest; or a link's target. A node is named by
   the SHA-256 of its canonical encoding, so a store path's root node
   digest commits to its whole tree, as an OSTree commit does through its
   "reference to a dirtree/dirmeta pair of checksums" ([Anatomy of an
   OSTree repository][ostree-repo], "Commit objects").
3. **Registration,** keyed by base name: the root node digest, the NAR
   hash and size, the references (base names), the deriver, and for a
   content-addressed path its content address. There is no registration
   time.

Profiles are records in the same volume
([F13](#f13-profiles-system-and-garbage-collection)). Registrations, nodes
and blobs are B-trees keyed by base name and by digest. Nodes and blobs
carry reference counts: a node's is the number of registrations and
directory entries that name it, a blob's the number of file nodes that
name it. Removing a registration decrements its root node, and a node
whose count reaches zero is removed and decrements everything it names,
within one transaction ([F6](#f6-pools-volumes-and-the-commit)), so a node
or blob that a live path shares keeps a count above zero. The format
specification fixes the digests' exact layout, including how each tree
level and the length are bound in, as Fuchsia's Merkle roots bind each
block's offset, level and length ([Fuchsia merkle roots][fx-merkle]).

A NAR with a name that is not UTF-8 is refused at registration: nova-os
names are UTF-8 (O8), and keeping such a name as bytes would put names in
the store that nothing else in the namespace can spell. The cost is that
such a NAR, which upstream Nix accepts, cannot enter the store. The store
compares names case-sensitively, so no store path needs the case-hack
suffix nova-nix adds on folding file systems (`Nix.Store.CaseSensitive`).

Deduplication follows from the naming: a blob already present is not
written again, and identical subtrees share nodes. The executable flag is
in the node, not the blob, so an executable file and a plain one with the
same bytes share a blob.

- **Why:**
  - A blob never changes, so sharing it costs none of what deduplicating
    mutable data costs: no table consulted on every write, and no
    copy-on-write bookkeeping when one sharer writes. OpenZFS warns
    "Unless you have a specific reason, do not enable dedup"
    ([OpenZFS Deduplication][zfs-dedup]), and each cached entry of its
    table "uses slightly more than 320 bytes of memory" ([OpenZFS
    Workload Tuning][zfs-tuning], "Deduplication").
  - The digests give verification its anchor
    ([F9](#f9-reading-and-verifying-the-store)) and replay an identity
    for store content ([F15](#f15-store-reads-and-replay)).
  - OSTree, "a git-like content-addressed object store" ([OSTree
    Overview][ostree-intro]), and composefs, where "shared files only need
    to be stored once" ([composefs README][composefs]), split content from
    metadata the same way; Fuchsia's blobfs names a blob by its Merkle
    root ([BlobFS][fx-blobfs]).
  - Without a registration time, the volume's content is a function of
    the paths and registrations it holds ([F11](#f11-reproducible-images)).
- **Cost:**
  - A SHA-256 pass over every file at registration, and reference counts
    updated for every node of a path registered or removed.
  - A small file takes a whole block unless the format packs small blobs,
    as composefs inlines files of 64 bytes or less into its image
    ([mkcomposefs][composefs-mk]).
  - The hash tree takes 32 bytes per 4,096-byte block plus its upper
    levels, about 0.8% of a blob; Fuchsia's 8 KiB blocks ([Fuchsia
    merkle roots][fx-merkle]) would halve that, at the price of checking
    two pages to read one.
  - The NAR hash cannot be computed from node digests, since SHA-256 does
    not compose, so checking it reads the whole path
    ([F9](#f9-reading-and-verifying-the-store)).
- **Not taken:** store paths as ordinary directories on a general file
  system, as Nix keeps `/nix/store`, its components "marked as read-only
  in the file system" (Dolstra 2006, 2.1, p. 21). Its cost is
  immutability that rests on write access alone, no identity to check a
  read against, sharing only by a later pass, and a collector that walks
  directories.

### F9. Reading and verifying the store

**Decided.** Three checks, at three times:

1. **On read.** When the store service reads a block of a blob from the
   disk, it checks the block against the blob's hash tree, anchored at
   the blob's digest, which its node names, whose digest the
   registration's root node digest commits to. A mismatch fails the read
   with an integrity error and marks the path damaged. The store service
   repairs it by fetching the path again from a substituter or rebuilding
   it, and replaces the blob only if the result has the registered
   digests. Fuchsia's Fxfs, in the design that replaces blobfs, does the
   same: "Any time a section of a file is read from disk, Fxfs verifies
   the contents against the merkle tree for the file" ([RFC-0225][fx-fxblob]).
2. **At registration.** The NAR hash is computed by streaming the path's
   NAR as it is written, and compared with the expected one, the
   narinfo's for a substituted path. A content-addressed path is also
   checked against the hash its name was computed from: the NAR hash for
   a recursive SHA-256 path, the file's own hash for a flat one.
3. **By scrub.** The store service rebuilds each path's NAR from its nodes
   and blobs, hashes it, and compares the result with the registration.

What each check proves:

- For an input-addressed path, which is every derivation output that is
  not fixed-output, nothing about the name can check the contents
  ([fact 3](#what-the-store-holds)). The anchor is the registration,
  written when the path was built or substituted: a check proves the
  bytes are the ones registered, not that they are what the derivation
  would build.
- For a content-addressed path, the name is computed from the contents,
  so the path can be checked against its own name, trusting nothing
  recorded.
- Two machines whose registrations give one input-addressed path
  different root node digests hold a build that is not reproducible
  (DESIGN.md, principle 1). Comparing digests checks reproducibility
  without moving any contents.

`get_section` on a store file returns a section holding the whole
verified file, with `READ`, with `EXECUTE` if the opener holds it, and
never with `WRITE`. The store service fills a blob's section by copying
from its own I/O buffers, never by a device writing into it, checks the
copy against the blob's digest, and then gives up its own write access
([F15](#f15-store-reads-and-replay)). It keeps a bounded cache of such
sections and gives every process a narrowed handle to one, so processes
that map the same file share its pages, as composefs's "data files are
shared in the page cache" ([composefs README][composefs]). A PE image
whose file alignment is smaller than its section alignment, as the
kernel's is (512 and 4,096 bytes, [boot-protocol.md](boot-protocol.md)),
cannot be mapped as its file: its loader copies it into a section of its
own, so executables share pages only when linked with equal alignments.

- **Why:** checking each block as it is read stops corruption at the read
  that would use it. Checking the NAR hash ties the store to nova-nix's
  record of a path, which block digests alone cannot. The scrub finds
  damage in paths nobody has read lately. APFS seals a system volume in a
  similar way: a hashed B-tree over its file system, which Apple likens
  to a Merkle tree, on a volume that cannot be modified (APFS, pp. 69 and
  150); the reference does not say when the hash is checked.
- **Cost:** SHA-256 for every block read from the disk. A section is
  filled whole before it is mapped, since a section is zero-filled memory
  or device memory (objects.md O2) and there is no pager, so mapping a
  large file reads all of it first. A section evicted from the cache
  lives on while another process maps it, and since sections are not
  waitable (objects.md O9) the store service cannot learn when the last
  mapping goes, so a file opened again after eviction can be in memory
  twice. A scrub reads the whole store.
- **Not taken:** checking only the NAR hash, when a path is opened. Its
  cost is reading a whole store path to use one file of it.

### F10. Data volumes and snapshots

**Decided.** A data volume holds a mutable tree: nodes keyed by node ID,
directories as B-trees keyed by name, file contents as extents. Each user
has one, published under `/users/<name>`, and each service one under
`/data/<service>`, as the recipe declares.

A snapshot is a kept root of one volume: read-only, made by one commit,
and reached through the volume's control channel rather than through a
name in the tree.

| Request | Effect |
|---|---|
| `snapshot_create(label)` | Commits, and keeps the volume's root |
| `snapshot_list()` | Each snapshot's label, transaction group and time |
| `snapshot_open(snapshot)` | A read-only directory handle on the snapshot's root |
| `snapshot_destroy(snapshot)` | Frees the blocks only that snapshot holds |

Every block pointer records the transaction group that wrote the block,
as a ZFS block pointer records the group in which it was allocated (ZFS
on-disk, 2.11). When the live tree releases a block written after the
newest snapshot, the block is freed. Otherwise a snapshot still holds it,
and it goes on the volume's dead list, as a ZFS dataset keeps a list of
the blocks released since its last snapshot (ZFS on-disk, 4.3). Taking a
snapshot hands the volume's dead list to the snapshot and starts an empty
one. Destroying a snapshot frees the blocks on the next snapshot's dead
list (the volume's, for the newest) that were written after the previous
snapshot, since only the destroyed one held them, and moves its own dead
list to the next. A snapshot service holds the control channels, and
takes and expires snapshots on the schedule the recipe declares.

- **Why:** copy-on-write makes a snapshot free when it is made: WAFL
  "simply duplicates the root inode", and "a brand new Snapshot consumes
  no disk space except for the Snapshot inode itself" (Hitz 1994, 3.4,
  p. 10). Reaching snapshots through a control channel keeps them under
  principle 7: a file a user deleted stays readable only to holders of
  that channel, not to every program granted the directory it was in.
- **Cost:** blocks a snapshot holds stay allocated, so deleting files
  frees nothing while a snapshot keeps them; WAFL found that "keeping
  Snapshots for one week consumes 10 to 20 percent of disk space" (Hitz
  1994, 2.2, p. 7). Destroying a snapshot walks the next one's dead list.
  A user finds an old version through a tool, not by browsing.
- **Not taken:** snapshots as a directory inside the tree they capture.
  In WAFL "Every directory in the file system contains a hidden
  sub-directory named .snapshot" (Hitz 1994, 2.1, p. 6), and ZFS
  snapshots "can be accessed under the .zfs/snapshot directory in the
  root of the file system" ([zfsconcepts(7)][zfs-concepts]). Its cost is
  that every program granted a tree can read its history. WAFL's block
  map, "a 32-bit entry for each 4 KB disk block" with a bit per snapshot
  (Hitz 1994, 4.1, p. 16), costs a fixed limit on snapshots and bits to
  clear in every entry when one is destroyed.

### F11. Reproducible images

**Decided.** The image tool writes the pool partition the way
`tools/mkimage.c` writes the EFI System Partition, "assembled in a zeroed
buffer in a fixed order", every identifier a constant (its header
comment):

1. The image holds the store volume only, with one system's closure and
   the system profile at its first generation. Data volumes are created
   at first boot, as the recipe declares them.
2. Registrations are written in bytewise order of base name, each path's
   nodes in NAR order, and each blob's blocks where the blob first occurs
   in that order.
3. Everything is one transaction group, numbered 1, with one superblock
   slot written in each ring.
4. Nothing records a time ([F8](#f8-the-store-volume)).
5. The pool's and the partition's GUIDs follow whichever rule
   disk-layout.md's undecided "Identifiers" takes.

The volume's content (registrations, nodes, blobs) is a function of the
closure's paths and their registrations as nova-nix exports them, and the
image's bytes a function of those and the image tool. A booted pool
diverges at its first commit, as any written disk does; what is
reproducible is the image as built, and the store's content, which
machines compare by digest at any time
([F9](#f9-reading-and-verifying-the-store)).

- **Why:** principle 1 asks for the same bytes from the same inputs, and
  the EFI System Partition already meets it.
- **Cost:** the image tool writes the pool format itself, in a fixed
  order, rather than through the server's allocator, so two writers of
  one format must agree; the server opening and scrubbing every image the
  tool writes tests that.
- **Not taken:** running the file system server, in a VM, to fill the
  image. Its cost is the server's allocation order and timing in the
  image's bytes.

## The store

### F12. Builds and substitution

**Decided.** Only the store service holds a write handle to the store
volume, and every path enters through it, by a build or a substitution.
Its clients hold handles to it whose rights belong to its protocol:

| Right | Allows |
|---|---|
| `BUILD` | Registering derivations and asking for their outputs |
| `IMPORT` | Streaming a substituted path in |
| `CLOSURE` | Opening the closure directory of any valid path, as a launcher does |
| `COLLECT` | Running a collection |

A profile is reached through a handle of its own, whose `SWITCH` right
changes its current generation
([F13](#f13-profiles-system-and-garbage-collection)).

**Build.** nova-nix stays the evaluator and the scheduler: the client's
nova-nix decides what to build and in what order, and asks. The building
happens behind the store service.

1. The client registers the derivation, a text store path; the service
   checks its name against its contents and references.
2. The client asks for its outputs. The service checks that every input
   is valid and starts a build runner in a job of its own: nova-nix's
   builder code, from the running system's closure. The runner computes
   each output's name from the derivation and gets from the service an
   empty staging directory under each name, outside `/store`. It builds
   the builder's environment as nova-nix does and starts the builder with
   a view of three things:
   - as `/store` (or `C:\nix\store`, through the Windows layer), a
     closure directory of the inputs in which each output's name opens
     its staging directory;
   - a scratch directory;
   - only for a fixed-output derivation, whose result is checked against
     the hash it declares, a network handle.

   Nothing else reaches the builder: no `/users`, `/data` or `/system`.
   The client receives the build log and the result, never a staging
   handle.
3. When the builder exits, the service closes the staging channels, which
   cuts off the builder and the runner
   ([objects.md O10](objects.md#o10-object-lifetime): access a holder
   might lose "is given through a server that can cut it off"). For each
   output it refuses names that are not UTF-8, writes the blobs it lacks
   and the nodes, computes the NAR hash and the root node digest, scans
   for references among the inputs' closure and the outputs' own names
   (Dolstra 2006, 5.5.1, p. 113), and checks a fixed-output derivation's
   declared hash. All outputs are registered in one transaction
   ([F6](#f6-pools-volumes-and-the-commit)).
4. If the service stops before that transaction commits, nothing is
   registered, and the restarted service discards the staging
   directories.

**Substitution.** A substituter process, which holds a network handle and
a store-service handle with `IMPORT`, fetches a narinfo and its NAR and
streams the NAR into the service with the narinfo's NAR hash, references
and signatures. The service writes it as in step 3 and registers it only
if the NAR hash matches, every reference is valid, and a signature
verifies under a key the system recipe trusts.

- **Why:** with one writer, the store's invariants are kept in one
  process: paths never change, and every reference of a valid path is
  valid (Dolstra 2006, Invariant 3). No client holds a staging handle or
  chooses an output's name, so one user's client cannot put bytes under a
  name another user's derivation gives; the thesis proposes the same
  split for a shared store, where "only this server process and its
  children have write access to the store and the Nix database" (Dolstra
  2006, 6.1, p. 138). A builder's view is its inputs and its outputs, so
  the confinement is the view, not a sandbox added around the builder:
  principle 7's "declaration that confines the build is the one that
  bounds the running program".
- **Cost:** nova-nix runs in two roles, as each client's evaluator and as
  the runner the service trusts with every build's names and staging;
  nova-nix needs a store backend that speaks the service's protocol in
  place of writing its store directory and database itself; every output
  is read back once after its build to be hashed.
- **Not taken:**
  - Clients that run their builders and write staging themselves, as
    nova-nix writes its single-user store. Its cost is that any client
    can register whatever bytes it likes under the names its derivations
    give, so a shared store trusts every user's client.
  - The store service naming outputs and building environments itself,
    in C. Its cost is a second implementation of nova-nix's derivation
    hashing and builder environment, which must agree with nova-nix's in
    every case.

### F13. Profiles, /system and garbage collection

**Decided.**

**Profiles.** A profile is a record in the store volume: a name (`system`,
or `users/<name>`), its generations, each a number and a store path, and
which one is current. Switching generations, rolling back included, is
one transaction ([F6](#f6-pools-volumes-and-the-commit)). Generations are
removed by a policy the recipe declares (how many, for how long) or on
request. This is Nix's model: user environments "reside in the Nix
store", each change makes a new generation, and "Upgrades are atomic"
(Dolstra 2006, 2.3, pp. 36 to 37); a record takes the place of a chain
of symbolic links.

A user's profile is not a name in the user's volume, which another server
serves: the launcher puts the current generation's store path, as a
read-only directory, in the views of that user's programs under the
prefix `/users/<name>/profile` ([F5](#f5-per-process-views)), and the
volume keeps the name `profile` free at its root, so the prefix hides
nothing.

**`/system`** is a read-only directory on the running generation's store
path, which the store service serves and the namespace server publishes.
A switch commits the system profile's new current generation, which the
next boot starts; a switch applied without a reboot also replaces
`/system` in one step ([F4](#f4-publishing-file-systems)). Which running
services move to the new generation without a reboot is outside this
design.

**Boot files.** Booting a generation needs its kernel, first process and
boot image on the EFI System Partition
([F14](#f14-disks-and-the-system-recipe)). A switch writes the new
generation's files there, then commits the profile, then writes the boot
menu's default. Per-generation files and a menu are additions to the
boot protocol, which reads one kernel and one first process
(`\nova\kernel.exe`, `\nova\init.exe`) and has no menu; writing the ESP
needs a FAT writer in nova-os, used for that partition alone. FAT has no
crash consistency of its own, so a crash while the ESP is being written
can damage it, and no ordering prevents that; the order keeps the
profile from naming a generation whose files were never written. At
each boot the store service compares the menu's default and the ESP's
entries with the system profile: it rewrites a default a crash left
behind, so an interrupted switch takes effect at the following boot, and
removes the entries of generations the profile no longer holds, whose
paths the collector may already have freed. OSTree's switch leaves
"either the old system, or the new one" after a crash ([OSTree Atomic
Upgrades][ostree-upgrades]); on FAT this design promises less, that
the profile and the ESP agree again after the next boot.

**Roots.** The collector keeps:

1. every generation of every profile;
2. the inputs and staging of every build and substitution in progress;
3. the closure behind every channel the store service serves: an open
   closure directory, and every directory and file opened through one,
   keep their closure live until they close.

The third makes running programs exact roots: every use of `/store` goes
through a channel the store service serves, so it knows which paths are
in use. Nix lacks this: in the thesis "it is possible that a currently
executing program is garbage collected" (Dolstra 2006, 5.6.1, p. 126),
and nova-nix finds no runtime roots, so "a program running out of an
unrooted store path is not protected" (`Nix.Store.GC`).

**Collection.** The store service marks every path reachable from the
roots along the recorded references. Every unmarked registration is
removed, and the counts it held are decremented as
[F8](#f8-the-store-volume) describes, in one transaction; nodes and blobs
whose counts reach zero are freed in it.

- **Why:** roots kept as records in the store volume mean the collector
  never reads a user's volume, and no root dangles or is planted by
  whoever can write a directory. With one writer, collection and
  registration are ordered within one process, so the thesis's per-path
  lock files, per-process temporary-root files and global lock, under
  which "the creation of new temporary or permanent roots is blocked"
  while a collection runs (Dolstra 2006, 5.6.4, pp. 131 to 134), have no
  counterpart. A transaction makes each collection atomic, where the
  thesis orders deletion so that "a path is deleted before its references
  are deleted" (p. 130).
- **Cost:** a profile changes only through the store service's protocol,
  not by editing a link; reference counts are kept for every node and
  blob; the collector holds the service's write side while it marks; the
  boot protocol gains generations and a menu, and nova-os a FAT writer.
- **Not taken:** Nix's roots, "symlinks to store paths in the directory
  /nix/var/nix/gcroots" (Dolstra 2006, 5.6.1, p. 125), with profiles as
  chains of links. Their cost is roots that dangle, a scan to find them,
  and running programs found by inspecting processes or not at all.

### F14. Disks and the system recipe

**Decided.** The system recipe declares:

- each pool, by the unique partition GUID of its partition (UEFI 5.3.3);
- which pool holds the store volume, one per system;
- each data volume: its pool, the name it is published under, and its
  snapshot schedule;
- the EFI System Partition nova-boot is loaded from.

System versions live where the store does: they are store paths and
profile records. A user's or a service's volume may be on any pool. A
pool the recipe declares and the machine lacks is not opened, its volumes
are not published, and a program granted one of them fails to launch
([F5](#f5-per-process-views), rule 3).

The EFI System Partition holds nova-boot and, for each kept system
generation, its kernel, its first process
([boot-protocol.md](boot-protocol.md), "First process image") and its
boot image: a store image
([F16](#f16-the-first-milestone)) holding the closure of what runs before
the pool can be read (the storage driver, the file system server, the
store service, the namespace server and the launcher). These are files
nova-boot loads while booting, the use the FAT licence covers. Once the
pool is open, the store service checks the boot image's paths against
their registrations.

- **Why:** which disk holds what is configuration, and principles 1 and 7
  put configuration in the recipe. Naming partitions by GUID makes the
  recipe independent of the order in which devices are found. nova-boot
  reads files through the firmware's FAT driver and no other file system,
  so whatever runs before the pool opens comes from the ESP.
- **Cost:** the recipe carries GUIDs particular to one machine; moving a
  volume to another disk is a recipe change and a copy; the ESP's size
  bounds how many generations stay bootable, though older ones stay in
  the store; boot files are copies of store content on a partition with
  no checksums, checked only once the pool is open; per-generation boot
  files are an addition to the boot protocol
  ([F13](#f13-profiles-system-and-garbage-collection)).
- **Not taken:**
  - One pool spread across every disk. Its cost is that losing any one
    disk loses the whole pool.
  - A reader for the native file system inside nova-boot, so that boot
    files stay in the store. Its cost is a second implementation of the
    pool format, in the firmware's environment.

## Replay

### F15. Store reads and replay

**Decided,** as a refinement of [replay.md](replay.md).

replay.md records "the results of every system call" and "every message
and handle a process reads from a channel" (Inputs through the kernel). It
forbids a recorded process to "map a writable section shared with a
process outside the recording", leaving "read-only sharing, such as store
paths" unaffected (Shared memory). Under I9 a file's bytes reach a process
in a message or in a section, and the claim that reads of `/store` need
not be recorded holds for each only as follows.

1. **Bytes in a message** (an inline read) are recorded whole, `/store`
   included. The kernel records messages without reading their protocol,
   so it cannot skip store reads, and it should not learn the file
   protocol to do so (objects.md O1). The saving belongs to whatever
   stores the recording: it may replace recorded bytes with a reference
   (a blob digest, an offset and a length) after hashing them and finding
   that they are that range of a blob. The reference is checked by hash,
   so it trusts no server's claim.
2. **Bytes in a section.** replay.md's rule covers sections writable from
   outside the recording. A section that nothing can write, because no
   handle to it carries `WRITE`, no writable mapping of it exists, and no
   pin lets a device write it ([ipc.md I8](ipc.md#i8-dma)), has fixed
   contents, since rights never grow (O5); a store section is one once
   the store service has given up its own write access
   ([F9](#f9-reading-and-verifying-the-store)). Its contents are still an
   input the recording must identify, which replay.md does not say. For a
   store section the identity is the blob's digest. The kernel can tell
   that no writable handle, mapping or pin exists, since it holds every
   handle table, mapping and pin; recording the digest needs either the kernel
   to hash the section once or the recording's store to hash its bytes
   and match them to a blob, as in item 1.
3. **The identity is the digest, not the name.** An input-addressed
   path's name does not fix its bytes ([fact 3](#what-the-store-holds)),
   so two machines can hold different bytes under one name. A recording
   that named store paths alone would replay against whatever bytes the
   replaying machine holds.
4. **Replay needs the blobs** a recording names: from the local store, a
   substituter, or a rebuild that reproduces the digest. A recording
   whose blobs cannot be found is reported unfaithful rather than
   replayed, as replay.md treats a CPUID mismatch.
5. **Reads of mutable data are recorded whole,** as replay.md requires:
   bytes, listings, attributes and times all arrive in messages. A
   recorded process cannot take bulk data through a section its file
   system server keeps writable (replay.md, Shared memory), so its reads
   of mutable files arrive inline, at most 65,536 bytes per message
   (ipc.md I1), or in a section the server has given up all write access
   to.

- **Why:** store content has a digest at every layer below, so a
  recording can name it instead of copying it, and each layer stays as it
  is: the kernel records messages without knowing protocols.
- **Cost:** telling whether a section is writable anywhere is a new query
  on the core; the recording's store hashes recorded bytes to find store
  ranges; a recorded process's reads of mutable files are copied through
  messages.
- **Not taken:** a kernel that understands the file protocol and logs
  store reads by reference as it delivers them. Its cost is protocol
  parsing in the privileged core, which O1 rules out.

## Milestone

### F16. The first milestone

**Decided.** The smallest system that runs a program from `/store`:

1. **A store image:** a read-only, single-file form of the store volume's
   content for a set of store paths. A header (magic, format version,
   counts, offsets, the digest of the registration table, and the base
   name of the store path holding the programs the first process starts)
   is followed by the registrations in base-name order, the
   nodes, and the blobs with their hash trees, laid out as
   [F11](#f11-reproducible-images) lays out a pool. It is the store volume
   without the pool: no allocator, no superblocks, nothing written after
   it is built.
2. **A host tool,** built by the chain beside `mkimage`, writes the image
   from NARs that nova-nix exports, with their references. Taking NARs
   keeps the tool from reading NTFS, where the executable flag is an
   alternate data stream ([fact 6](#what-the-store-holds)); the tool
   checks each NAR against the hash nova-nix registered for it.
3. **mkimage** puts the image on the EFI System Partition as
   `\NOVA\STORE.IMG`, beside the kernel and the first process's image
   `\NOVA\INIT.EXE`. It is the milestone's boot image.
4. **nova-boot** reads it whole and uninterpreted, as it reads the first
   process's image, and passes its address and size in `BOOT_INFO` beside
   `init_image`; this is an addition to the boot protocol
   (boot-protocol.md, undecided "Load options and further files").
5. **The kernel** gives the first process a read-only section over those
   pages, in its bootstrap message. A section is zero-filled memory or
   device memory (objects.md O2), so this is a kernel addition; neither
   the kernel nor nova-boot reads the image's format.
6. **The first process** starts the store service, which serves `/store`
   from the image's section with no pool, no writes and no collector. It
   copies each blob it serves into a section of its own, checking its
   blocks against the blob's hash tree as it copies, so no program ever
   receives the image's section, which holds every path. The first
   process also starts the namespace server, with `/store` published, and
   the launcher, which starts a shell with a view of its own closure.
7. **The shell** asks the launcher to run a program by its store path. The
   launcher gets the program's closure directory, copies the program's PE
   image into a new section from the store service's section for that
   file, and starts it.

Done when:

- the program runs from `/store` and its output appears;
- in the shell's view, listing `/store` shows the shell's closure only,
  and opening a store path outside it fails;
- a block changed in the image makes the read that needs it fail with an
  integrity error;
- two builds from the same inputs give byte-identical images.

Left out: the pool, writes, builds, the collector, data volumes,
snapshots, a storage driver and the Windows layer.

- **Why:** every part is one the full design keeps: the store volume's
  records, verification, closure directories, views and the launcher.
  Loading the image through the firmware, as the kernel is loaded, needs
  no storage driver, the largest piece left out.
- **Cost:** the whole image sits in memory, bounded by RAM and by the
  ESP's size, and each blob a program uses is copied once more into its
  own section; the first process reads the store image itself, with the
  store service's code, to find and start the store service.
- **Not taken:** the image in a partition of its own, read by nova-boot
  through the firmware's Block I/O protocol or by a nova-os storage
  driver. The first needs a partition type and a block reader in
  nova-boot for something the firmware's FAT driver already reads; the
  second needs a storage driver, PCI and DMA working before anything runs
  from `/store`.

## Changes elsewhere

What this design needs outside this document:

- **Kernel:** a read-only section over the store image nova-boot loaded,
  given to the first process ([F16](#f16-the-first-milestone)); a query
  whether any handle, mapping or pin can still write a section
  ([F15](#f15-store-reads-and-replay)).
- **Boot protocol:** the store image's place in `BOOT_INFO`
  ([F16](#f16-the-first-milestone)); a kernel, first process and boot
  image per system generation, and a boot menu
  ([F13](#f13-profiles-system-and-garbage-collection)).
- **Disk layout:** the pool partition's type GUID and identifiers
  ([F6](#f6-pools-volumes-and-the-commit),
  [F11](#f11-reproducible-images)).
- **nova-os components:** a FAT writer for the ESP alone
  ([F13](#f13-profiles-system-and-garbage-collection)).
- **nova-nix:** NAR export with references ([F16](#f16-the-first-milestone)),
  a store backend that speaks the store service's protocol, and a build
  runner role ([F12](#f12-builds-and-substitution)).

## References

Microsoft Learn, with the date each page shows as its last update:
[How NTFS Works][ntfs-works] (Windows Server 2003 documentation,
2009-10-08),
[ReFS integrity streams][refs-integrity] (2024-11-01).

Fuchsia, with the date each page shows:
[Fuchsia Namespaces][fx-ns] (2026-10-12),
[RFC-0136: Fxfs][fx-fxfs] (2026-09-22),
[RFC-0225: Fxblob][fx-fxblob] (2026-09-22),
[BlobFS][fx-blobfs] (2025-02-28),
[Fuchsia merkle roots][fx-merkle] (2025-03-22).

OpenZFS documentation: [Copy-on-Write][zfs-cow], [Checksums][zfs-checksums],
[Deduplication][zfs-dedup] and [Workload Tuning][zfs-tuning], each
updated 2026-07-22; [zfsconcepts(7)][zfs-concepts], dated 2024-10-02.

Btrfs documentation, which shows no dates, read 2026-11-06:
[Btrfs design][btrfs-design], [Checksumming][btrfs-csum],
[Defragmentation][btrfs-defrag].

OSTree documentation, which shows no per-page dates; the site was built
2026-11-03: [OSTree Overview][ostree-intro],
[Anatomy of an OSTree repository][ostree-repo],
[Atomic Upgrades][ostree-upgrades].

composefs, at commit `ec2573a0f68f548ae91f3e10acc990a08f9122dc`:
[README][composefs] (last changed 2026-01-15) and the
[mkcomposefs][composefs-mk] manual page.

NILFS: [What is NILFS?][nilfs], which shows no date, read 2026-11-06.

[ntfs-works]: https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2003/cc781134(v=ws.10)
[refs-integrity]: https://learn.microsoft.com/en-us/windows-server/storage/refs/integrity-streams
[fx-ns]: https://fuchsia.dev/fuchsia-src/concepts/process/namespaces
[fx-fxfs]: https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0136_fxfs
[fx-fxblob]: https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0225_fxblob
[fx-blobfs]: https://fuchsia.dev/fuchsia-src/concepts/filesystems/blobfs
[fx-merkle]: https://fuchsia.dev/fuchsia-src/concepts/security/merkleroot
[zfs-cow]: https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Copy-on-write.html
[zfs-checksums]: https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Data%20Storage/Checksums.html
[zfs-dedup]: https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Data%20Storage/Deduplication.html
[zfs-tuning]: https://openzfs.github.io/openzfs-docs/Performance%20and%20Tuning/Workload%20Tuning.html
[zfs-concepts]: https://openzfs.github.io/openzfs-docs/man/master/7/zfsconcepts.7.html
[btrfs-design]: https://btrfs.readthedocs.io/en/latest/dev/dev-btrfs-design.html
[btrfs-csum]: https://btrfs.readthedocs.io/en/latest/Checksumming.html
[btrfs-defrag]: https://btrfs.readthedocs.io/en/latest/Defragmentation.html
[ostree-intro]: https://ostreedev.github.io/ostree/introduction/
[ostree-repo]: https://ostreedev.github.io/ostree/repo/
[ostree-upgrades]: https://ostreedev.github.io/ostree/atomic-upgrades/
[composefs]: https://github.com/containers/composefs/blob/ec2573a0f68f548ae91f3e10acc990a08f9122dc/README.md
[composefs-mk]: https://github.com/containers/composefs/blob/ec2573a0f68f548ae91f3e10acc990a08f9122dc/man/mkcomposefs.md
[nilfs]: https://nilfs.sourceforge.io/en/about_nilfs.html
