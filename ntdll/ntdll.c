/* ntdll for 32-bit processes (docs/spec/compat.md): the six routines
   stage0-pe32's seed resolves by name, with the behaviour Microsoft
   Learn documents for them, over nova-os's native calls through the
   32-bit lane (docs/spec/syscalls.md S11). Files are reached through
   the process's view of storage, the directory channel its launcher
   passes as its bootstrap handle, with the stand-in file protocol of
   file_protocol.h.

   Each exported routine is __stdcall, the convention of the Win32 API,
   whose callee removes its arguments from the stack ("__stdcall"); the
   seed calls them so (observed in the seed). TinyCC decorates a
   __stdcall name as _name@N unless the function says nodecorate, and
   the seed looks the names up undecorated.

   Pointers a caller passes are dereferenced in user mode, where a bad
   one faults and ends the process; NT's native services return
   STATUS_ACCESS_VIOLATION instead. The message buffers are shared, so
   ntdll serves one thread at a time, and a 32-bit process has one. */

#include "compat_abi.h"
#include "file_protocol.h"
#include "native_abi.h"
#include "nt32.h"

#ifdef __TINYC__
#define EXPORT __attribute__((dllexport, nodecorate))
#else
#define EXPORT __attribute__((dllexport))
#endif
#define NTAPI __attribute__((stdcall))

typedef UINT8 BOOLEAN;

#define TRUE 1
#define FALSE 0

/* The NT name of a DOS path is the path under \??\ ("NtCreateFile
   function (winternl.h)" gives \??\B:\myfile.dat as a fully qualified
   file specification). */
static const UINT16 nt_prefix[] = {'\\', '?', '?', '\\'};
#define NT_PREFIX_CHARS 4

/* A drive designator and the backslash after it, "C:\" ("Naming Files,
   Paths, and Namespaces"). */
#define ROOT_CHARS 3

/* The longest DOS path RtlDosPathNameToNtPathName_U builds, in UTF-16
   units, and the longest name NtCreateFile sends, in UTF-8 bytes. */
#define PATH_MAX_CHARS 1024
#define NAME_MAX_BYTES (PATH_MAX_CHARS * 3)

#define HEAP_ALIGNMENT 8

/* One request outstanding on a channel at a time. */
#define FILE_CHANNEL_CAPACITY 1

/* A transaction ID a program assigns itself has bit 31 clear (ipc.md
   I3). */
#define TRANSACTION_MASK 0x7fffffffu

/* UTF-16 surrogates (Unicode 3.9, D91). */
#define SURROGATE_HIGH_FIRST 0xd800u
#define SURROGATE_LOW_FIRST 0xdc00u
#define SURROGATE_LAST 0xdfffu
#define SURROGATE_BASE 0x10000u
#define SURROGATE_BITS 10

/* entry.S. */
UINT32 gate_call(UINT32 number, const UINT64 *arguments, UINT32 *results);
_Noreturn void ntdll_process_start(UINT32 directory, const COMPAT_START *start);

static UINT32 view;
static UINT32 heap_next, heap_end;
static const UINT16 *current_directory;
static UINT32 transactions;
static UINT8 request_buffer[sizeof(FILE_REQUEST) + FILE_TRANSFER_MAX];
static UINT8 reply_buffer[sizeof(FILE_REPLY) + FILE_TRANSFER_MAX];

/* What a read or write passes as its handles: none, at an address the
   kernel takes as user memory (S6). */
static const HANDLE_TRANSFER no_handles[1];

_Static_assert(NAME_MAX_BYTES <= FILE_TRANSFER_MAX,
               "an open's name fits the request buffer");
_Static_assert(sizeof request_buffer <= CHANNEL_MAX_BYTES,
               "a request fits one message");

/* ------------------------------------------------------------------------
   Native calls over the 32-bit lane. Each argument is a 64-bit slot
   (S11); the arrays are filled one element at a time, since TinyCC
   compiles an initialized local array to a call to memset, which ntdll
   does not have. */

static UINT64 address(const void *pointer)
{
    return (UINT32)pointer;
}

static UINT32 handle_close(UINT32 handle)
{
    UINT64 arguments[1];
    UINT32 results[2];

    arguments[0] = handle;
    return gate_call(CALL_HANDLE_CLOSE, arguments, results);
}

static UINT32 object_wait_one(UINT32 handle, UINT32 signals,
                              UINT32 *observed)
{
    UINT64 arguments[3];
    UINT32 results[2];
    UINT32 status;

    arguments[0] = handle;
    arguments[1] = signals;
    arguments[2] = DEADLINE_INFINITE;
    status = gate_call(CALL_OBJECT_WAIT_ONE, arguments, results);
    *observed = results[0];
    return status;
}

static UINT32 channel_create(UINT32 *first, UINT32 *second)
{
    UINT64 arguments[1];
    UINT32 results[2];
    UINT32 status;

    arguments[0] = FILE_CHANNEL_CAPACITY;
    status = gate_call(CALL_CHANNEL_CREATE, arguments, results);
    *first = results[0];
    *second = results[1];
    return status;
}

static UINT32 channel_write(UINT32 channel, const void *bytes, UINT32 count,
                            const HANDLE_TRANSFER *handles,
                            UINT32 handle_count)
{
    UINT64 arguments[5];
    UINT32 results[2];

    arguments[0] = channel;
    arguments[1] = address(bytes);
    arguments[2] = count;
    arguments[3] = address(handles);
    arguments[4] = handle_count;
    return gate_call(CALL_CHANNEL_WRITE, arguments, results);
}

static UINT32 channel_read(UINT32 channel, void *bytes, UINT32 capacity,
                           UINT32 *count)
{
    RECEIVED_HANDLE handles[1];
    UINT64 arguments[5];
    UINT32 results[2];
    UINT32 status;

    arguments[0] = channel;
    arguments[1] = address(bytes);
    arguments[2] = capacity;
    arguments[3] = address(handles);
    arguments[4] = 0;
    status = gate_call(CALL_CHANNEL_READ, arguments, results);
    *count = results[0];
    return status;
}

_Noreturn static void process_exit(UINT32 code)
{
    UINT64 arguments[1];
    UINT32 results[2];

    arguments[0] = code;
    gate_call(CALL_PROCESS_EXIT, arguments, results);
    for (;;)
        ;
}

static UINT32 thread_set_tls_base(UINT32 base)
{
    UINT64 arguments[1];
    UINT32 results[2];

    arguments[0] = base;
    return gate_call(CALL_THREAD_SET_TLS_BASE, arguments, results);
}

/* ------------------------------------------------------------------------
   The process heap: the range the launcher mapped, given out in order
   and never taken back, since nothing ntdll exports frees. */

static void *heap_allocate(UINT32 size)
{
    UINT32 at = (heap_next + HEAP_ALIGNMENT - 1) & ~(UINT32)(HEAP_ALIGNMENT - 1);

    if (at < heap_next || at > heap_end || size > heap_end - at)
        return 0;
    heap_next = at + size;
    return (void *)at;
}

/* ------------------------------------------------------------------------
   The process's start: FS gets the TEB, as a 32-bit Windows program
   expects (TEB32, nt32.h), then the program runs, and its return value
   ends the process. */

static NTSTATUS terminate(UINT32 process, NTSTATUS status);

_Noreturn void ntdll_process_start(UINT32 directory, const COMPAT_START *start)
{
    UINT32 (*entry)(void) = (UINT32 (*)(void))start->entry;

    view = directory;
    heap_next = start->heap_base;
    heap_end = start->heap_base + start->heap_size;
    current_directory = (const UINT16 *)start->current_directory;
    if (thread_set_tls_base(start->teb) != OK)
        process_exit(STATUS_UNSUCCESSFUL);
    terminate(NT_CURRENT_PROCESS, entry());
    process_exit(STATUS_UNSUCCESSFUL);
}

/* ------------------------------------------------------------------------
   Requests to the view's server. */

static NTSTATUS transport_status(UINT32 status)
{
    switch (status) {
    case OK:
        return STATUS_SUCCESS;
    case ERR_NO_MEMORY:
        return STATUS_NO_MEMORY;
    case ERR_BAD_HANDLE:
    case ERR_WRONG_TYPE:
    case ERR_ACCESS_DENIED:
    case ERR_PEER_CLOSED:
        return STATUS_INVALID_HANDLE;
    }
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS file_status(UINT32 status)
{
    switch (status) {
    case FILE_OK:
        return STATUS_SUCCESS;
    case FILE_NOT_FOUND:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case FILE_EXISTS:
        return STATUS_OBJECT_NAME_COLLISION;
    case FILE_DENIED:
        return STATUS_ACCESS_DENIED;
    case FILE_INVALID:
        return STATUS_INVALID_PARAMETER;
    case FILE_FULL:
        return STATUS_DISK_FULL;
    }
    return STATUS_UNSUCCESSFUL;
}

static FILE_REQUEST *new_request(UINT32 operation, UINT64 offset,
                                 UINT32 length, UINT32 flags)
{
    FILE_REQUEST *request = (FILE_REQUEST *)request_buffer;

    transactions = (transactions + 1) & TRANSACTION_MASK;
    request->transaction = transactions;
    request->operation = operation;
    request->offset = offset;
    request->length = length;
    request->flags = flags;
    return request;
}

/* Write the request's bytes, and handles, to channel, waiting while its
   queue is full. */
static NTSTATUS send(UINT32 channel, UINT32 size,
                     const HANDLE_TRANSFER *handles, UINT32 handle_count)
{
    UINT32 status, observed;

    for (;;) {
        status = channel_write(channel, request_buffer, size, handles,
                               handle_count);
        if (status != ERR_SHOULD_WAIT)
            return transport_status(status);
        status = object_wait_one(channel, CHANNEL_WRITABLE | CHANNEL_PEER_CLOSED,
                                 &observed);
        if (status != OK)
            return transport_status(status);
    }
}

/* Wait for the reply to the request just sent on channel and read it
   into reply_buffer. Returns its FILE_REPLY, whose bytes after the
   header number *data, or 0 with *status set. */
static const FILE_REPLY *receive(UINT32 channel, UINT32 *data,
                                 NTSTATUS *status)
{
    const FILE_REPLY *reply = (const FILE_REPLY *)reply_buffer;
    UINT32 result, observed, count;

    result = object_wait_one(channel, CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                             &observed);
    if (result == OK)
        result = channel_read(channel, reply_buffer, sizeof reply_buffer,
                              &count);
    if (result != OK) {
        *status = transport_status(result);
        return 0;
    }
    if (count < sizeof(FILE_REPLY) ||
        reply->transaction != ((const FILE_REQUEST *)request_buffer)->transaction) {
        *status = STATUS_UNSUCCESSFUL;
        return 0;
    }
    *data = count - sizeof(FILE_REPLY);
    *status = file_status(reply->status);
    return reply;
}

/* ------------------------------------------------------------------------
   Names. */

static UINT16 upper(UINT16 c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

static int is_separator(UINT16 c)
{
    return c == '\\' || c == '/';
}

static int is_letter(UINT16 c)
{
    return upper(c) >= 'A' && upper(c) <= 'Z';
}

/* Append the UTF-8 encoding of code point c at *at, below end. */
static int put_utf8(UINT8 **at, const UINT8 *end, UINT32 c)
{
    UINT8 *p = *at;
    UINT32 count = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;

    if ((UINT32)(end - p) < count)
        return 0;
    if (count == 1) {
        p[0] = (UINT8)c;
    } else if (count == 2) {
        p[0] = (UINT8)(0xc0 | c >> 6);
        p[1] = (UINT8)(0x80 | (c & 0x3f));
    } else if (count == 3) {
        p[0] = (UINT8)(0xe0 | c >> 12);
        p[1] = (UINT8)(0x80 | (c >> 6 & 0x3f));
        p[2] = (UINT8)(0x80 | (c & 0x3f));
    } else {
        p[0] = (UINT8)(0xf0 | c >> 18);
        p[1] = (UINT8)(0x80 | (c >> 12 & 0x3f));
        p[2] = (UINT8)(0x80 | (c >> 6 & 0x3f));
        p[3] = (UINT8)(0x80 | (c & 0x3f));
    }
    *at = p + count;
    return 1;
}

/* Whether the count units at name are a component the view takes: not
   empty, not . or .., the textual directory steps a DOS path resolves
   before it is an NT name. */
static int plain_component(const UINT16 *name, UINT32 count)
{
    return count != 0 && !(name[0] == '.' && count == 1) &&
           !(name[0] == '.' && name[1] == '.' && count == 2);
}

/* The view's name, in UTF-8 at out, for an NT name: \??\, the view's
   drive, and a path below its root. An NT name separates components with
   \ alone, and the view's names with /, so a / inside a component is
   refused rather than passed on as a separator. Returns STATUS_SUCCESS
   and the name's bytes in *size, or why not. */
static NTSTATUS view_name(const UNICODE_STRING32 *name, UINT8 *out,
                          UINT32 *size)
{
    const UINT16 *units = (const UINT16 *)name->Buffer;
    UINT32 count = name->Length / sizeof(UINT16);
    UINT8 *at = out, *end = out + NAME_MAX_BYTES;
    UINT32 i, start;

    for (i = 0; i < NT_PREFIX_CHARS; i++)
        if (i >= count || units[i] != nt_prefix[i])
            return STATUS_OBJECT_PATH_NOT_FOUND;
    units += NT_PREFIX_CHARS;
    count -= NT_PREFIX_CHARS;
    if (count < ROOT_CHARS || units[1] != ':' || units[2] != '\\')
        return STATUS_OBJECT_NAME_INVALID;
    if (upper(units[0]) != COMPAT_VIEW_DRIVE)
        return STATUS_OBJECT_PATH_NOT_FOUND;
    for (start = i = ROOT_CHARS; i <= count; i++) {
        UINT32 c;

        if (i < count && units[i] != '\\')
            continue;
        if (!plain_component(units + start, i - start))
            return STATUS_OBJECT_NAME_INVALID;
        if (at != out && !put_utf8(&at, end, '/'))
            return STATUS_OBJECT_NAME_INVALID;
        for (; start < i; start++) {
            c = units[start];
            if (c >= SURROGATE_LOW_FIRST && c <= SURROGATE_LAST)
                return STATUS_OBJECT_NAME_INVALID;
            if (c >= SURROGATE_HIGH_FIRST && c < SURROGATE_LOW_FIRST) {
                if (start + 1 >= i || units[start + 1] < SURROGATE_LOW_FIRST ||
                    units[start + 1] > SURROGATE_LAST)
                    return STATUS_OBJECT_NAME_INVALID;
                c = SURROGATE_BASE +
                    ((c - SURROGATE_HIGH_FIRST) << SURROGATE_BITS) +
                    (units[start + 1] - SURROGATE_LOW_FIRST);
                start++;
            }
            if (c == 0 || c == '/' || !put_utf8(&at, end, c))
                return STATUS_OBJECT_NAME_INVALID;
        }
        start = i + 1;
    }
    *size = (UINT32)(at - out);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------
   The exported routines. */

static UINT32 access_flags(UINT32 desired)
{
    return ((desired & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA))
                ? FILE_ACCESS_READ
                : 0) |
           ((desired & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA |
                        FILE_APPEND_DATA))
                ? FILE_ACCESS_WRITE
                : 0);
}

/* Directory files, asynchronous handles, extended attributes and names
   relative to a directory handle are not provided. FileAttributes,
   ShareAccess and AllocationSize change nothing a view's file has. The
   name matches regardless of case when Attributes asks for it, which
   OBJ_CASE_INSENSITIVE does; its value is observed in the seed. */
#define OBJ_CASE_INSENSITIVE 0x40u
#define CREATE_OPTIONS_PROVIDED                                            \
    (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT |            \
     FILE_NON_DIRECTORY_FILE)

EXPORT NTSTATUS NTAPI NtCreateFile(UINT32 *FileHandle, UINT32 DesiredAccess,
                                   const OBJECT_ATTRIBUTES32 *ObjectAttributes,
                                   IO_STATUS_BLOCK32 *IoStatusBlock,
                                   const LARGE_INTEGER32 *AllocationSize,
                                   UINT32 FileAttributes, UINT32 ShareAccess,
                                   UINT32 CreateDisposition,
                                   UINT32 CreateOptions, const void *EaBuffer,
                                   UINT32 EaLength)
{
    HANDLE_TRANSFER transfer[1];
    const FILE_REPLY *reply;
    FILE_REQUEST *request;
    UINT32 mine, theirs, size, data, flags;
    NTSTATUS status;

    (void)AllocationSize;
    (void)FileAttributes;
    (void)ShareAccess;
    (void)EaBuffer;
    if (ObjectAttributes->Length < sizeof(OBJECT_ATTRIBUTES32) ||
        CreateDisposition > FILE_OVERWRITE_IF)
        return STATUS_INVALID_PARAMETER;
    if (ObjectAttributes->RootDirectory || EaLength ||
        (CreateOptions & ~CREATE_OPTIONS_PROVIDED) ||
        !(CreateOptions &
          (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)))
        return STATUS_NOT_IMPLEMENTED;
    status = view_name((const UNICODE_STRING32 *)ObjectAttributes->ObjectName,
                       request_buffer + sizeof(FILE_REQUEST), &size);
    if (status != STATUS_SUCCESS)
        return status;
    flags = CreateDisposition | access_flags(DesiredAccess) |
            ((ObjectAttributes->Attributes & OBJ_CASE_INSENSITIVE)
                 ? FILE_NAME_ANY_CASE
                 : 0);
    if (channel_create(&mine, &theirs) != OK)
        return STATUS_NO_MEMORY;
    request = new_request(FILE_OPERATION_OPEN, 0, size, flags);
    transfer[0].handle = theirs;
    transfer[0].rights = RIGHTS_SAME;
    status = send(view, sizeof(FILE_REQUEST) + request->length, transfer, 1);
    if (status != STATUS_SUCCESS) {
        handle_close(theirs);
        handle_close(mine);
        return status;
    }
    reply = receive(mine, &data, &status);
    if (status != STATUS_SUCCESS) {
        handle_close(mine);
        return status;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = (UINT32)reply->information;
    *FileHandle = mine;
    return STATUS_SUCCESS;
}

/* The position a read or write starts at: ByteOffset's, or the file's
   current position when ByteOffset is null ("NtReadFile function
   (ntifs.h)"). */
static int start_offset(const LARGE_INTEGER32 *ByteOffset, UINT64 *offset)
{
    if (!ByteOffset) {
        *offset = FILE_OFFSET_CURRENT;
        return 1;
    }
    *offset = (UINT64)ByteOffset->HighPart << 32 | ByteOffset->LowPart;
    return !(ByteOffset->HighPart & 0x80000000u);
}

/* An event or APC is how an asynchronous request completes; every
   handle here is synchronous, and Key does not apply to a view's
   files. */
EXPORT NTSTATUS NTAPI NtReadFile(UINT32 FileHandle, UINT32 Event,
                                 UINT32 ApcRoutine, UINT32 ApcContext,
                                 IO_STATUS_BLOCK32 *IoStatusBlock,
                                 void *Buffer, UINT32 Length,
                                 const LARGE_INTEGER32 *ByteOffset,
                                 const UINT32 *Key)
{
    UINT8 *to = Buffer;
    UINT32 done = 0, data, i;
    UINT64 offset;
    NTSTATUS status = STATUS_SUCCESS;

    (void)ApcContext;
    (void)Key;
    if (Event || ApcRoutine)
        return STATUS_NOT_IMPLEMENTED;
    if (!start_offset(ByteOffset, &offset))
        return STATUS_INVALID_PARAMETER;
    while (done < Length) {
        UINT32 chunk = Length - done < FILE_TRANSFER_MAX ? Length - done
                                                         : FILE_TRANSFER_MAX;
        const FILE_REPLY *reply;

        new_request(FILE_OPERATION_READ, offset, chunk, 0);
        status = send(FileHandle, sizeof(FILE_REQUEST), no_handles, 0);
        if (status != STATUS_SUCCESS)
            break;
        reply = receive(FileHandle, &data, &status);
        if (status != STATUS_SUCCESS)
            break;
        if (reply->information > chunk || reply->information > data) {
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        for (i = 0; i < reply->information; i++)
            to[done + i] = reply_buffer[sizeof(FILE_REPLY) + i];
        done += (UINT32)reply->information;
        offset = FILE_OFFSET_CURRENT;
        if (reply->information < chunk)
            break;
    }
    if (status == STATUS_SUCCESS && done == 0 && Length > 0)
        status = STATUS_END_OF_FILE;
    IoStatusBlock->Status = status;
    IoStatusBlock->Information = done;
    return status;
}

EXPORT NTSTATUS NTAPI NtWriteFile(UINT32 FileHandle, UINT32 Event,
                                  UINT32 ApcRoutine, UINT32 ApcContext,
                                  IO_STATUS_BLOCK32 *IoStatusBlock,
                                  const void *Buffer, UINT32 Length,
                                  const LARGE_INTEGER32 *ByteOffset,
                                  const UINT32 *Key)
{
    const UINT8 *from = Buffer;
    UINT32 done = 0, data, i;
    UINT64 offset;
    NTSTATUS status = STATUS_SUCCESS;

    (void)ApcContext;
    (void)Key;
    if (Event || ApcRoutine)
        return STATUS_NOT_IMPLEMENTED;
    if (!start_offset(ByteOffset, &offset))
        return STATUS_INVALID_PARAMETER;
    while (done < Length) {
        UINT32 chunk = Length - done < FILE_TRANSFER_MAX ? Length - done
                                                         : FILE_TRANSFER_MAX;
        const FILE_REPLY *reply;

        new_request(FILE_OPERATION_WRITE, offset, chunk, 0);
        for (i = 0; i < chunk; i++)
            request_buffer[sizeof(FILE_REQUEST) + i] = from[done + i];
        status = send(FileHandle, sizeof(FILE_REQUEST) + chunk, no_handles,
                      0);
        if (status != STATUS_SUCCESS)
            break;
        reply = receive(FileHandle, &data, &status);
        if (status != STATUS_SUCCESS)
            break;
        if (reply->information > chunk) {
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        done += (UINT32)reply->information;
        offset = FILE_OFFSET_CURRENT;
        if (reply->information < chunk)
            break;
    }
    IoStatusBlock->Status = status;
    IoStatusBlock->Information = done;
    return status;
}

EXPORT NTSTATUS NTAPI NtClose(UINT32 Handle)
{
    return handle_close(Handle) == OK ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

/* "ZwTerminateProcess function (ntddk.h)": given the current process,
   the call does not return. Ending another process needs a handle this
   ntdll never hands out. */
static NTSTATUS terminate(UINT32 process, NTSTATUS status)
{
    if (process != NT_CURRENT_PROCESS)
        return STATUS_NOT_IMPLEMENTED;
    process_exit(status);
}

EXPORT NTSTATUS NTAPI NtTerminateProcess(UINT32 ProcessHandle,
                                         NTSTATUS ExitStatus)
{
    return terminate(ProcessHandle, ExitStatus);
}

/* Append the component of count units at name to the DOS path at out,
   whose length *length is at least ROOT_CHARS, resolving . and ..
   textually; .. never climbs above the root ("Naming Files, Paths, and
   Namespaces"). */
static int append_component(UINT16 *out, UINT32 *length, const UINT16 *name,
                            UINT32 count)
{
    UINT32 i;

    if (count == 0 || (count == 1 && name[0] == '.'))
        return 1;
    if (count == 2 && name[0] == '.' && name[1] == '.') {
        while (*length > ROOT_CHARS && out[*length - 1] != '\\')
            (*length)--;
        if (*length > ROOT_CHARS)
            (*length)--;
        return 1;
    }
    if (*length + 1 + count > PATH_MAX_CHARS)
        return 0;
    if (out[*length - 1] != '\\')
        out[(*length)++] = '\\';
    for (i = 0; i < count; i++)
        out[(*length)++] = name[i];
    return 1;
}

/* Append each component of the NUL-terminated path at name. */
static int append_path(UINT16 *out, UINT32 *length, const UINT16 *name)
{
    UINT32 start = 0, i;

    for (i = 0;; i++) {
        if (name[i] && !is_separator(name[i]))
            continue;
        if (!append_component(out, length, name + start, i - start))
            return 0;
        if (!name[i])
            return 1;
        start = i + 1;
    }
}

/* The full DOS path of dos: a path with a drive and a backslash stands
   alone; one with a drive and no backslash is relative to that drive's
   current directory, which is known for the current drive only; one
   that starts with a backslash starts at the current drive's root; any
   other is relative to the current directory ("Naming Files, Paths, and
   Namespaces"). UNC names and the \\?\ and \\.\ forms are not provided. */
static int full_path(const UINT16 *dos, UINT16 *out, UINT32 *length)
{
    out[0] = current_directory[0];
    out[1] = ':';
    out[2] = '\\';
    *length = ROOT_CHARS;
    if (is_separator(dos[0]) && is_separator(dos[1]))
        return 0;
    if (is_letter(dos[0]) && dos[1] == ':') {
        if (is_separator(dos[2])) {
            out[0] = upper(dos[0]);
            return append_path(out, length, dos + ROOT_CHARS);
        }
        if (upper(dos[0]) != upper(current_directory[0]))
            return 0;
        return append_path(out, length, current_directory + ROOT_CHARS) &&
               append_path(out, length, dos + 2);
    }
    if (is_separator(dos[0]))
        return append_path(out, length, dos + 1);
    return append_path(out, length, current_directory + ROOT_CHARS) &&
           append_path(out, length, dos);
}

/* The seed passes four arguments and reads only NtFileName, whose buffer
   ntdll allocates (observed in the seed). The result is the full DOS
   path under \??\, NUL-terminated; FilePart, when given, receives the
   address of its last component. The RelativeName structure is not
   documented, so a caller that asks for it is refused. */
EXPORT BOOLEAN NTAPI RtlDosPathNameToNtPathName_U(const UINT16 *DosFileName,
                                                  UNICODE_STRING32 *NtFileName,
                                                  UINT32 *FilePart,
                                                  void *RelativeName)
{
    UINT16 path[PATH_MAX_CHARS];
    UINT16 *name;
    UINT32 length, i, last = 0;

    if (RelativeName || !DosFileName[0] ||
        !full_path(DosFileName, path, &length))
        return FALSE;
    name = heap_allocate((NT_PREFIX_CHARS + length + 1) * sizeof(UINT16));
    if (!name)
        return FALSE;
    for (i = 0; i < NT_PREFIX_CHARS; i++)
        name[i] = nt_prefix[i];
    for (i = 0; i < length; i++) {
        name[NT_PREFIX_CHARS + i] = path[i];
        if (path[i] == '\\')
            last = NT_PREFIX_CHARS + i + 1;
    }
    name[NT_PREFIX_CHARS + length] = 0;
    NtFileName->Length = (UINT16)((NT_PREFIX_CHARS + length) * sizeof(UINT16));
    NtFileName->MaximumLength = NtFileName->Length + sizeof(UINT16);
    NtFileName->Buffer = (UINT32)name;
    if (FilePart)
        *FilePart = last < NT_PREFIX_CHARS + length ? (UINT32)(name + last) : 0;
    return TRUE;
}
