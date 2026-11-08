# The debug log

The debug log is the kernel's serial console: the kernel writes to it,
and so does every process holding the debug-log resource
([ipc.md I6](ipc.md#i6-device-memory-and-io-ports)), with `debug_write`
([syscalls.md S8](syscalls.md#s8-the-first-calls)). This document gives
the form of every line written there (T1), how tests report there (T2),
and which self-tests run on which boot (T3). The disks are described in
[disk-layout.md](disk-layout.md#files).

Specifications are cited by the versions in [README.md](README.md):
"KTAP" is the Kernel Test Anything Protocol, version 1, pinned under
[Test reports](README.md#test-reports).

## T1. Lines

**Decided.** Each line reports one event:

```
component: event key=value ...
```

- The **component** names the writer: `nova` for the kernel, and `nova-`
  followed by the program's name for a process (`nova-init`,
  `nova-console`, `nova-fs`). A colon and a space follow it.
- The **event** is one or more words, each of the characters `a` to
  `z`, `0` to `9` and `-`, separated by single spaces. It is the same
  text every time the line is written and holds no value.
- **key=value** pairs follow, each after a single space. A key is a
  word of `a` to `z`, `0` to `9` and `_` that starts with a letter; a
  key that holds a quantity ends with its unit (`free_mib`,
  `prepare_ms`). A value is written bare if it holds only printable
  ASCII other than the space and `"`; otherwise it is written between
  double quotes, with each byte that is not printable ASCII, and each
  `"`, written as `.`.
- Counts, sizes and the status a nova-os call returns
  ([syscalls.md S7](syscalls.md#s7-error-reporting)) are decimal;
  addresses, masks, signatures and a firmware's or a device's status
  words are hexadecimal with `0x`; a time is milliseconds to the tenth,
  rounded down; a range is `first..last`; a PCI function is
  `segment:bus:device.function` in hexadecimal digits, `0000:00:07.0`.
- A line is written whole, so that lines of different writers never
  mix: the kernel writes a line under its port's lock whenever another
  thread may write, and a process writes it with one `debug_write` of at
  most `DEBUG_WRITE_MAX_BYTES` (256).

A line is written for what a reader acts on or a test needs: a device
found, a service serving, a measurement, a refusal, a failure. A step
that a later line implies is not logged, nor is a check that passes,
which T2 reports.

A line reports a failure if its event ends with `failed`, or if it is
one of these:

| Line | Reports |
|---|---|
| `nova: halted` | The kernel stopped; the line before it says why |
| `nova: exception` | A fault in the kernel; `nova: halted` follows |
| `nova: user exception` | A fault in a process, which ends |
| `nova: first process ended` | The first process ended ([boot-protocol.md](boot-protocol.md#startup-done)) |
| `nova: random unseeded` | No source of entropy seeded the generator, so `random_read` fails ([syscalls.md S8](syscalls.md#s8-the-first-calls)) |
| `nova-init: child restarted` | The supervisor started a program again ([ipc.md I10](ipc.md#i10-driver-failure-and-restart)) |
| `nova-init: child given up` | The supervisor left a program down |

A refusal the system is meant to make, such as of a damaged data disk,
is an event ending with `refused`, never `failed`. A failure's cause is
its `status` or its `reason`, a short phrase, quoted.

```
nova: memory free_mib=247 total_mib=247
nova: msi unconfined pci=0000:00:07.0 vector=0x30
nova-fs: pool opened lba=2048 blocks=16123 block_bytes=4096 txg=1 slots=32 used=4
nova-console: pointer press button=left
nova-init: store service load failed status=16
```

- **Why:** one event per line, its values in keys, serves a person
  reading the boot and a program waiting on it alike. A harness matches
  the component, the event and the keys it needs, so what a line says
  of one value can change without breaking a check of another, and no
  line is written as a sentence for a harness to find. It is the form
  of Linux's kernel log, a subsystem's prefix and then its event
  (`pci 0000:00:02.0: [1234:1111] type 00`, from Linux v7.3's
  `drivers/pci/probe.c`, the tag [README.md](README.md#devices) pins),
  with logfmt's `key=value` pairs, which tools split at spaces and `=`.
- **Cost:** a line does not explain itself: what a key means is in the
  source that writes it, and a failure's reason is a phrase, not an
  explanation.
- **Not taken:** sentences, which a check must match whole, so that
  rewording one breaks the check and a value inside one is hard to pick
  out;
  JSON lines, whose braces and quoting every line pays on a console a
  person reads; RFC 5424's header, a priority, a timestamp and a host,
  where the log has one host and no wall clock; a severity on every
  line, which no reader filters by, while the table above finds the
  failures.

## T2. Test reports

**Decided.** Tests report in KTAP. On a test disk
([disk-layout.md](disk-layout.md#files)) the kernel opens the report,
the first process continues it, and the report ends when the first
process's startup does:

```
KTAP version 1
1..2
  KTAP version 1
  1..4
  ok 1 page
  ok 2 random
  ok 3 object
  ok 4 thread
ok 1 kernel
  KTAP version 1
  1..5
  ok 1 calls
    KTAP version 1
    1..8
    ok 1 launch
    ...
    ok 8 crash
  ok 2 fstest
    KTAP version 1
    1..5
    ok 1 launch
    ...
    ok 5 outside
  ok 3 hello
  ok 4 chain # SKIP not on this disk
  ok 5 boot-disk
ok 2 init
```

1. The kernel writes the version line and the plan, `1..2`. Test 1,
   `kernel`, is its own; test 2, `init`, is the first process's, whose
   result line the first process writes.
2. Each of the two holds its tests as subtests, at depth 1, two spaces
   in, with their own version line and plan. The kernel's are its
   self-tests (T3). The first process's are `calls`, its checks of the
   calls it makes against their contracts; `fstest`, the data disk's
   check ([storage.md F18](storage.md#f18-the-writable-milestone));
   `hello`, the test program from `/store`
   ([storage.md F16](storage.md#f16-the-first-milestone)); `chain`, the
   bootstrap check ([compat.md C4](compat.md#c4-files)); and
   `boot-disk`, the virtio block driver reading the boot disk's
   partition table and boot sector. Its plan counts all five on every
   test disk; one the disk or the boot cannot run is `ok` with the
   `SKIP` directive and the reason.
3. A test with cases of its own reports them at depth 2, four spaces
   in. A test program writes its version line, plan and results there
   itself, and the first process writes the program's result at depth
   1 once the program ends, from its exit code. The chain's steps are
   the cases of `chain`, each named by the file it writes; a link's
   step that passes carries `# pinned` and the link's name, its pin
   checked (`ok 1 hex0.exe # pinned hex0`).
4. A test that fails is `not ok`, after `#` diagnostic lines at its
   depth that say what failed. A test with cases fails if any of its
   cases does, whose diagnostics say why, and a test program's result
   gives its exit code. A test that passes writes no diagnostic but for
   a part of it that cannot run on the machine. A failure that stops a
   test program, or a step of the chain whose output the steps after it
   need, leaves the cases after it skipped, so that every list holds its
   plan.
5. A name holds no space and no `#`: a lowercase word, joined by `-`
   where it takes two, or for a chain step the name of the file it
   writes. Every KTAP line is written whole (T1), so the other lines of
   the log, which KTAP takes as unknown lines, fall only between them.

`ci/boot.py` reads the report and fails a boot on a missing version
line or plan, a result whose number is not the next, a count short of
its plan, any `not ok`, or a `SKIP` of a test the boot must run. A boot
of a disk that is not a test disk writes no report, and a line that
begins with a component but is not in T1's form fails a boot too.

- **Why:** KTAP is Linux's format for this case, tests that report on
  a console other code writes to at the same time. It allows the
  unknown lines a log is full of, nests a test's cases under it, and
  its plans let a harness tell a test that never reported from one
  that passed. The first process continuing the kernel's report keeps
  one report for a boot.
- **Cost:** the kernel's plan counts a test the first process reports,
  and a test program's depth is fixed by this layout, not passed to it.
  The first process's plan counts tests a disk need not hold, which it
  reports as skipped.
- **Not taken:** TAP 13 and 14, under which an unknown line is an
  error; a report of its own from each program, which no harness reads
  as one boot's; a T1 line for each check that passes, which says what
  KTAP does without a plan to count against.

## T3. Self-tests

**Decided.**

| Self-test | Checks | Runs |
|---|---|---|
| The kernel's `random` | ChaCha20 against RFC 8439 2.3.2, SHA-256 against RFC 6234, and the generator's fast-key-erasure construction against ChaCha20's output (`kernel/random/random_test.c`) | On every boot, before the generator is seeded |
| The accounts service's | BLAKE2b against RFC 7693 and Argon2id against RFC 9106 ([accounts.md A4](accounts.md#a4-hashing-passwords)) | Whenever the service starts |
| The kernel's `page` | Every free page against the firmware's memory map, and allocation, zeroing and freeing (`kernel/mm/page_test.c`) | On a test disk, before any page is allocated |
| The kernel's `object` | Interrupt quotas, waits a handle's close ends, watches, device memory types and `process_unmap` (`kernel/object/object_test.c`) | On a test disk, before the first process |
| The kernel's `thread` | Preemption, stacks freed at exit, sleep deadlines and waits (`kernel/sched/sched_test.c`) | On a test disk, before the first process |

On a boot of the system disk, a self-test that passes writes one line,
and one that fails writes its name and reason and stops: the kernel
halts, and the accounts service ends without serving.

```
nova: selftest passed name=random
nova-accounts: selftest passed name=argon2id
nova: selftest failed name=random reason="SHA-256 of TEST1 differs from RFC 6234"
```

On a test disk the kernel's self-tests are the cases of `kernel` (T2),
and a failure is `not ok` with its reason as a diagnostic. `random`,
`object` and `thread` then halt the kernel, `random` as it does on the
system disk, because what runs after them depends on what failed;
`page` goes on, so that the rest of the report shows what else the
allocator breaks. The
accounts service starts after the report has ended, and writes its line
on every disk.

- **Why:** a known-answer test guards code whose wrong output looks
  right: a generator built wrong still gives bytes, and nothing after
  it would show the error (accounts.md A4). The page allocator, object
  and thread self-tests check the kernel's own code, which each boot of
  one kernel runs alike, and take each boot's time: the thread
  self-test alone keeps four threads busy for 200 ms. CI boots the test
  disk on four processor models, where a change to that code shows.
- **Cost:** a machine booted only from the system disk never checks its
  memory map and timer against the kernel; booting the test disk on it
  does.
- **Not taken:** every self-test on every boot, which spends every
  boot's time on code no boot changes; none on the system disk, which
  would leave the generator unchecked before it makes keys.
