# 32-bit Windows programs

How nova-os runs a 32-bit Windows program: the process a launcher builds
for it, the ntdll it calls, how its DOS paths reach its view of storage,
and the stand-in file protocol behind them. The first program is
stage0-pe32's seed, `hex0-seed.exe`, which the first process runs at
boot (`init/seed.c`). The kernel's side, the 32-bit lane, is
[syscalls.md S11](syscalls.md#s11-the-32-bit-lane).

Microsoft Learn pages are cited by title and pinned in
[README.md](README.md#windows-compatibility); "MS-ERREF" and "MS-SMB2"
are the open specifications pinned there. Every structure layout and
value comes from them. stage0-pe32's seed and its source are GPL-3.0
and were read only to learn which fields and values the seed uses, as a
test of these layouts; what was learned that way is marked "observed in
the seed" in `include/nt32.h`. Nothing of the seed's is copied.

## C1. The process

**Decided.**

A launcher creates the process with `PROCESS_OPTION_32_BIT` (S11) and,
for a program whose image has a page both writable and executable,
`PROCESS_OPTION_WRITE_EXECUTE` ([objects.md O5](objects.md#o5-access-rights)).
It maps, below 4 GiB (`include/compat_abi.h`):

| Range | What |
|---|---|
| The program's `ImageBase` | The program, a PE32 (PE Format, `Magic` 0x10B, `Machine` 0x14C) with no imports, placed without relocation; each page has the rights of the sections on it |
| ntdll's `ImageBase`, 0x10000000 | ntdll, placed the same way |
| 0x200000, 64 KiB | The stack |
| 0x300000, 8 KiB | The environment: the TEB in the first page, then the PEB, the loader data, the process parameters, their strings, and the `COMPAT_START` block |
| 0x1000000, 64 KiB | The heap ntdll allocates from |

The environment holds what winternl.h documents and nothing it marks
reserved: the TEB's `ProcessEnvironmentBlock` ("TEB (winternl.h)"); the
PEB's `Ldr` and `ProcessParameters` ("PEB (winternl.h)"); the loader data's
`InMemoryOrderModuleList` ("PEB_LDR_DATA (winternl.h)"), whose entries are
the program, then ntdll, each with `DllBase` and `FullDllName`; and the
parameters' `ImagePathName` and `CommandLine` ("RTL_USER_PROCESS_PARAMETERS
(winternl.h)"), each NUL-terminated after its `Length`. The command line is
the program's name and its arguments, separated by spaces.

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
- **Cost:** the layout is fixed, so a program whose image overlaps it
  cannot be loaded.

## C2. ntdll

**Decided.**

`ntdll.dll` is an i386 PE32 DLL that TinyCC builds from source, with no
imports. It exports, `__stdcall` and undecorated, the routines the seed
resolves by name from its export table: `NtCreateFile`, `NtReadFile`,
`NtWriteFile`, `NtClose`, `NtTerminateProcess` and
`RtlDosPathNameToNtPathName_U`. Each calls the kernel through the 32-bit
lane and returns an NTSTATUS (MS-ERREF 2.3.1).

| Routine | Behaviour |
|---|---|
| `NtCreateFile` | Opens a file of the view (C3) with the dispositions of "NtCreateFile function (winternl.h)" and MS-SMB2 2.2.13, reporting `FILE_OPENED`, `FILE_CREATED`, `FILE_OVERWRITTEN` or `FILE_SUPERSEDED` (MS-SMB2 2.2.14) in `Information`. The handle is a channel to the file's server. |
| `NtReadFile`, `NtWriteFile` | Move bytes at `ByteOffset`, or at the file's current position when it is null, and move the position past them ("NtReadFile function (ntifs.h)"). A read that finds no byte at the end of the file returns `STATUS_END_OF_FILE`. |
| `NtClose` | Closes the handle. |
| `NtTerminateProcess` | Given the current process's pseudo-handle, ends it with `ExitStatus` as its exit code and does not return ("ZwTerminateProcess function (ntddk.h)"). |
| `RtlDosPathNameToNtPathName_U` | Builds the full DOS path of its argument against the current directory, then its NT name under `\??\`, in a buffer from ntdll's heap, NUL-terminated. Microsoft Learn does not document it; its four arguments and its use of the result are observed in the seed. |

Not provided, each with `STATUS_NOT_IMPLEMENTED`: directories,
asynchronous handles (no `FILE_SYNCHRONOUS_IO_*` option, or an event or
APC), extended attributes, names relative to `RootDirectory`, and
terminating another process. `RtlDosPathNameToNtPathName_U` refuses a
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

Until nova-os has a file system, the first process serves a 32-bit
process's view itself, with the protocol of `include/file_protocol.h`,
shaped as [ipc.md I9](ipc.md#i9-asynchronous-io-requests) shapes I/O
requests: an open is a message on the directory channel carrying one
endpoint of a new channel, which the server answers on and serves from
then on, one channel per open file; reads and writes are requests on
that channel, answered with one reply each, data inline. Each reply's
status is the protocol's own, since "not found" and "exists" are no
kernel error ([syscalls.md S7](syscalls.md#s7-error-reporting)).

The seed's view holds two files: `hex0_x86.hex0`, read-only, the
contents of the boot volume's `\nova\hex0_x86.hex`, and `hex0.exe`, the
output, which the first open creates in the first process's memory, up
to 256 KiB. When the seed ends, the first process logs the output's
size and sha256 and checks it against `\nova\hex0seed.exe` byte for
byte, since `hex0_x86.hex0` assembles into the seed itself.

- **Why:** the boot volume is the one storage nova-os reads today, and
  the output needs nowhere to persist for the check.
- **Cost:** the view is fixed in the first process, and the output is
  lost at its end.
