# 32-bit Windows programs

How nova-os runs a 32-bit Windows program: the process a launcher builds
for it, the ntdll it calls, how its DOS paths reach its view of storage,
and the stand-in file protocol behind them. The programs are
stage0-pe32's seed, `hex0-seed.exe`, and the links of its chain up to
M1, which the first process runs when it boots from the chain disk
(`init/chain.c`). The kernel's side, the 32-bit lane, is
[syscalls.md S11](syscalls.md#s11-the-32-bit-lane).

Microsoft Learn pages are cited by title and pinned in
[README.md](README.md#windows-compatibility); "MS-ERREF", "MS-SMB2" and
"MS-FSCC" are the open specifications pinned there. Every structure
layout and value comes from them. The chain's programs and sources,
stage0-pe32's and those of the M2libc, M2-Planet and mescc-tools trees
it builds from, are GPL-3.0 and were read only to learn which routines,
fields and values the programs use, as a test of these layouts; what
was learned that way is marked "observed in the seed" or "observed in
the chain" in `include/nt32.h`. Nothing of theirs is copied.

## C1. The process

**Decided.**

A launcher creates the process with `PROCESS_OPTION_32_BIT` (S11) and,
for a program whose image has a page both writable and executable,
`PROCESS_OPTION_WRITE_EXECUTE` ([objects.md O5](objects.md#o5-access-rights)).
It maps, below 4 GiB (`include/compat_abi.h`):

| Range | What |
|---|---|
| The program's `ImageBase` | The program, a PE32 (PE Format, `Magic` 0x10B, `Machine` 0x14C) with no imports, placed without relocation: its headers and section data, and zeros to `SizeOfImage`; each page has the rights of the sections on it |
| ntdll's `ImageBase`, 0x10000000 | ntdll, placed the same way |
| 0x100000, 64 KiB | The heap ntdll allocates from |
| 0x300000, 8 KiB | The environment: the TEB in the first page, then the PEB, the loader data, the process parameters, their strings, the environment block and the `COMPAT_START` block |
| The program's `SizeOfStackReserve`, ending at 0x10000000 | The stack |

The environment holds what winternl.h documents: the TEB's
`ProcessEnvironmentBlock` ("TEB (winternl.h)"); the PEB's `Ldr` and
`ProcessParameters` ("PEB (winternl.h)"); the loader data's
`InMemoryOrderModuleList` ("PEB_LDR_DATA (winternl.h)"), whose entries are
the program, then ntdll, each with `DllBase` and `FullDllName`; and the
parameters' `ImagePathName` and `CommandLine` ("RTL_USER_PROCESS_PARAMETERS
(winternl.h)"), each NUL-terminated after its `Length`. The command line is
the program's name and its arguments, separated by spaces. The
parameters also hold the address of the environment block at 0x48, after
`CommandLine`, where winternl.h documents nothing, and the chain's C
library reads it there (observed in the chain). The block is empty, the
two NULs that end every environment block ("Environment Variables"). The
same library reads the standard input, output and error handles at 0x18,
0x1C and 0x20, in the parameters' reserved fields (observed in the
chain); the launcher writes no reserved field, so they hold 0, no
handle, and a read or write through one fails with
`STATUS_INVALID_HANDLE`.

The first thread starts at ntdll's `AddressOfEntryPoint` with its
bootstrap handle, the view's directory channel (C3), in ECX and the
address of `COMPAT_START` in EDX. ntdll sets the thread's FS base to the
TEB with `thread_set_tls_base`, calls the program's entry point with no
arguments, and ends the process with what it returns.

- **Why `COMPAT_START`:** winternl.h documents no field for the current
  directory, the heap or the entry point, so ntdll takes them from a
  block of nova-os's own rather than from reserved fields.
- **Why the program first in the list:** the seed takes the list's
  second entry as ntdll (observed in the seed); the first is the
  program, as on Windows.
- **Why the heap and the environment below 0x400000:** a chain
  program's image starts there and declares as much as 128 MiB
  (observed in the chain), so they go below it rather than among it.
- **Why the whole `SizeOfStackReserve`:** it is the stack the program
  asks for, 64 MiB from M0 on (observed in the chain), and cc_x86
  compiling M2-Planet runs past 64 KiB. Ending where ntdll starts, it
  has 124 MiB of room above an image of 128 MiB at 0x400000.
- **Why an empty environment:** no step through M1 reads a variable.
- **Cost:** the layout is fixed, so a program whose image or stack
  overlaps it cannot be loaded. The kernel maps no page on demand, so
  every page of the image and the stack is memory from the start: 192
  MiB for a chain program from M0 on.

## C2. ntdll

**Decided.**

`ntdll.dll` is an i386 PE32 DLL that TinyCC builds from source, with no
imports. It exports, `__stdcall` and undecorated, the routines the
chain's programs resolve by name from its export table and call:
`NtCreateFile`, `NtReadFile`, `NtWriteFile`, `NtClose`,
`NtTerminateProcess`, `RtlDosPathNameToNtPathName_U`,
`NtSetInformationFile` and `NtQueryInformationFile`. Each calls the
kernel through the 32-bit lane and returns an NTSTATUS (MS-ERREF
2.3.1). The programs from M0 on look up some thirty names in all, and
take the address of a name ntdll does not export as zero (observed in
the chain); no step through M1 calls one.

| Routine | Behaviour |
|---|---|
| `NtCreateFile` | Opens a file of the view (C3) with the dispositions of "NtCreateFile function (winternl.h)" and MS-SMB2 2.2.13, reporting `FILE_OPENED`, `FILE_CREATED`, `FILE_OVERWRITTEN` or `FILE_SUPERSEDED` (MS-SMB2 2.2.14) in `Information`. The handle is a channel to the file's server. |
| `NtReadFile`, `NtWriteFile` | Move bytes at `ByteOffset`, or at the file's current position when it is null, and move the position past them ("NtReadFile function (ntifs.h)"). A read that finds no byte at the end of the file returns `STATUS_END_OF_FILE`. |
| `NtClose` | Closes the handle. |
| `NtTerminateProcess` | Given the current process's pseudo-handle, ends it with `ExitStatus` as its exit code and does not return ("ZwTerminateProcess function (ntddk.h)"). |
| `RtlDosPathNameToNtPathName_U` | Builds the full DOS path of its argument against the current directory, then its NT name under `\??\`, in a buffer from ntdll's heap, NUL-terminated. Microsoft Learn does not document it; its four arguments and its use of the result are observed in the seed. |
| `NtSetInformationFile` | With `FilePositionInformation`, moves the file's position to `CurrentByteOffset` for a handle opened to read or write ("FILE_POSITION_INFORMATION structure (wdm.h)"); a negative offset fails with `STATUS_INVALID_PARAMETER` (MS-FSCC 2.4.40). `Information` receives the bytes set ("NtSetInformationFile function (ntifs.h)"), the structure's size. hex1 and hex2 rewind their input with it (observed in the chain). |
| `NtQueryInformationFile` | Writes `FilePositionInformation`, the position, for a handle opened to read or write, or `FileStandardInformation`: `EndOfFile` the file's size, `AllocationSize` the same, since the view's server allocates by the byte, one link, and neither a directory nor pending deletion (MS-FSCC 2.4.47). `Information` receives the bytes written ("NtQueryInformationFile function (ntifs.h)"). The chain's C library seeks with both (observed in the chain); no step through M1 seeks. |

A `Length` smaller than the information class's structure fails with
`STATUS_INFO_LENGTH_MISMATCH` (MS-FSCC 2.4.40, 2.4.47).

Not provided, each with `STATUS_NOT_IMPLEMENTED`: directories,
asynchronous handles (no `FILE_SYNCHRONOUS_IO_*` option, or an event or
APC), extended attributes, names relative to `RootDirectory`, other
information classes, and terminating another process.
`RtlDosPathNameToNtPathName_U` refuses a
`RelativeName` argument, whose structure is not documented. A bad
pointer faults in ntdll and ends the process, where NT's native services
return `STATUS_ACCESS_VIOLATION`.

- **Why in user mode:** the routines translate NT's file model into
  messages to a server, which needs no privilege
  ([objects.md O1](objects.md#o1-admission-to-the-core)).
- **Cost:** ntdll's message buffers are shared, so it serves one thread
  at a time; a 32-bit process has one.

## C3. Paths and the view

**Decided** for this stage.

A 32-bit process's view of storage is drive `C:`, whose root is the
directory channel its launcher passes as its bootstrap handle, and holds
exactly what its derivation names (DESIGN.md, principle 7). Its current
directory is `C:\`.

`RtlDosPathNameToNtPathName_U` follows "Naming Files, Paths, and
Namespaces": a path with a drive and a backslash is full; one that starts
with a backslash starts at the current drive's root; any other, or one
with the current drive and no backslash, is relative to the current
directory; `.` and `..` resolve textually and never climb above the
root; `/` counts as `\`. UNC names, the `\\?\` and `\\.\` forms, and
another drive's relative paths are refused. `NtCreateFile` takes an NT
name under `\??\` ("NtCreateFile function (winternl.h)" gives `\??\B:\myfile.dat` as
a fully qualified file specification), refuses any drive but `C:` with
`STATUS_OBJECT_PATH_NOT_FOUND`, and sends the rest to the view's server
as UTF-8 with `/` between components, as
[objects.md O8](objects.md#o8-the-namespace) names paths; a component
that is empty, `.` or `..`, or holds a `/`, fails with
`STATUS_OBJECT_NAME_INVALID`.
`OBJ_CASE_INSENSITIVE` asks the server to match names regardless of
ASCII case.

- **Cost:** one drive, and one current directory, which a program cannot
  change.

## C4. Files

**Decided** for this stage; the storage design replaces it.

Until a server speaks the directory protocol
([storage.md F1](storage.md#f1-the-directory-protocol)) for directories
a process writes, the first process serves a 32-bit process's view
itself, with the protocol of `include/file_protocol.h`,
shaped as [ipc.md I9](ipc.md#i9-asynchronous-io-requests) shapes I/O
requests: an open is a message on the directory channel carrying one
endpoint of a new channel, which the server answers on and serves from
then on, one channel per open file; a read, a write, a move or query of
the file's position and a query of its size are requests on that
channel, answered with one reply each, data inline. Each reply's status
is the protocol's own, since "not found" and "exists" are no kernel
error ([syscalls.md S7](syscalls.md#s7-error-reporting)).

The first process takes the steps nova-nix's stage0-pe32 package
(`pkgs/windows/by-name/st/stage0-pe32/package.nix`) takes from the seed
to M1, in its order and with its arguments, each in a process of its
own started once the one before has ended (`init/chain.c`). A step's
view holds the files its command line names, read-only, each a file of
the boot volume under the name its tree gives it or an earlier step's
output, and the one file the step writes, which its first open creates.
When a step ends, the first process checks each link, hex0, hex1,
hex2, catm, M0, cc_x86, M2 and M1, against the sha256 nova-nix's Stage0
(Windows) CI job pins for it; hex0's is the seed's own, since
`hex0_x86.hex0` assembles into the seed. It reports the chain as the
`chain` test of the boot's test report, each step a case named by the
file it writes ([log.md T2](log.md#t2-test-reports)); a link that is not
nova-nix's fails its step with the sha256 it has.

The chain, the bootstrap check, runs on a test disk whose boot volume
holds all 26 of its inputs, as the chain disk's does
([disk-layout.md](disk-layout.md#files)). On a test disk that holds
none, the first process reports the test skipped and goes on to its
other tests; on the system's disk, which runs no tests, it does
nothing. A volume that holds some but not all fails the test, with the
inputs it lacks named.

The outputs lie one after another in a region of the first process's
memory, which grows by sections as the step writing the last output
writes. A step writes only its own output, so the one growing is always
the last. An output holds at most 16 MiB, eight sections; the chain's
largest, `M2-0-0.M1`, is 1.7 MB.

- **Why through M1:** M2 is the first link built on the chain's C
  library, and nothing pins the file it writes, `M1-macro.M1`. The four
  steps after it build M1 from that file with links already checked,
  and M1 is pinned, so M2's own run is checked too.
- **Why:** the boot volume is the one storage nova-os reads today, and
  the outputs need nowhere to persist for the checks.
- **Why only from the chain disk:** the chain takes minutes under QEMU's
  emulation, and a boot of the system, or of the test disk, does not
  wait for it.
- **Why not the directory protocol yet:** each step writes its output,
  and the one server of that protocol so far, the store service, serves
  the store read only ([storage.md F16](storage.md#f16-the-first-milestone)).
  The steps' inputs are files of the boot volume, not store paths, and
  their outputs need a writable directory, a build's scratch directory
  or staging ([F12](storage.md#f12-builds-and-substitution)), whose
  server is the one that replaces this one.
- **Why some of the inputs fail:** such a volume is a chain disk with
  inputs missing, and passing it would report the test passed with the
  chain unrun.
- **Cost:** the views are fixed in the first process, and the outputs
  stay in its memory until it ends.
