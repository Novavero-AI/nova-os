/* What a 32-bit Windows program finds in its process and passes to the
   native routines, at the 32-bit (x86) sizes: every pointer and handle is
   a UINT32, so the launcher, built for x86-64, and the 32-bit ntdll lay
   the structures out alike. Layouts are from the Microsoft Learn pages
   docs/spec/compat.md pins, cited here by their titles; a field winternl.h
   marks Reserved is never read or written. Values are from those pages,
   from MS-ERREF 2.3.1 for NTSTATUS, and from MS-SMB2 2.2.13, 2.2.13.1.1
   and 2.2.14, which carry the same values over the wire. What was
   learned only from stage0-pe32's hex0 seed, as a test of the layouts,
   is marked "observed in the seed". */

#ifndef NOVA_NT32_H
#define NOVA_NT32_H

#include "types.h"

typedef UINT32 NTSTATUS;

/* "_UNICODE_STRING (ntdef.h)": Length and MaximumLength in
   bytes, the first excluding a terminating NUL, then Buffer. */
typedef struct {
    UINT16 Length;
    UINT16 MaximumLength;
    UINT32 Buffer;
} UNICODE_STRING32;

/* "LIST_ENTRY (ntdef.h)". */
typedef struct {
    UINT32 Flink;
    UINT32 Blink;
} LIST_ENTRY32;

/* "TEB (winternl.h)": Reserved1[12] of PVOID, then
   ProcessEnvironmentBlock. The rest of the structure is reserved; one
   page holds it. The seed reads the PEB pointer as fs:0x30 (observed in
   the seed). */
#define TEB32_RESERVED1 12
typedef struct {
    UINT32 Reserved1[TEB32_RESERVED1];
    UINT32 ProcessEnvironmentBlock;
} TEB32;

/* "PEB (winternl.h)": Reserved1[2], BeingDebugged,
   Reserved2[1], Reserved3[2] of PVOID, Ldr, ProcessParameters. The seed
   reads Ldr at 0x0C and ProcessParameters at 0x10 (observed in the
   seed). */
#define PEB32_RESERVED3 2
typedef struct {
    UINT8 Reserved1[2];
    UINT8 BeingDebugged;
    UINT8 Reserved2[1];
    UINT32 Reserved3[PEB32_RESERVED3];
    UINT32 Ldr;
    UINT32 ProcessParameters;
} PEB32;

/* "PEB_LDR_DATA (winternl.h)": Reserved1[8], Reserved2[3] of
   PVOID, then InMemoryOrderModuleList, "the head of a doubly-linked list
   that contains the loaded modules for the process". The seed takes the
   list's second entry as ntdll (observed in the seed). */
#define PEB_LDR32_RESERVED1 8
#define PEB_LDR32_RESERVED2 3
typedef struct {
    UINT8 Reserved1[PEB_LDR32_RESERVED1];
    UINT32 Reserved2[PEB_LDR32_RESERVED2];
    LIST_ENTRY32 InMemoryOrderModuleList;
} PEB_LDR_DATA32;

/* LDR_DATA_TABLE_ENTRY, from the remarks of "PEB_LDR_DATA
   (winternl.h)": Reserved1[2] of PVOID, InMemoryOrderLinks,
   Reserved2[2], DllBase, Reserved3[2], FullDllName, and reserved fields
   after it. A list entry points at InMemoryOrderLinks, so DllBase is
   0x10 past it (observed in the seed). */
#define LDR_ENTRY32_RESERVED1 2
#define LDR_ENTRY32_RESERVED2 2
#define LDR_ENTRY32_RESERVED3 2
typedef struct {
    UINT32 Reserved1[LDR_ENTRY32_RESERVED1];
    LIST_ENTRY32 InMemoryOrderLinks;
    UINT32 Reserved2[LDR_ENTRY32_RESERVED2];
    UINT32 DllBase;
    UINT32 Reserved3[LDR_ENTRY32_RESERVED3];
    UNICODE_STRING32 FullDllName;
} LDR_DATA_TABLE_ENTRY32;

/* "RTL_USER_PROCESS_PARAMETERS (winternl.h)": Reserved1[16],
   Reserved2[10] of PVOID, ImagePathName, CommandLine. The seed reads
   CommandLine.Buffer at 0x44 and writes NULs into the command line
   (observed in the seed). */
#define PROCESS_PARAMETERS32_RESERVED1 16
#define PROCESS_PARAMETERS32_RESERVED2 10
typedef struct {
    UINT8 Reserved1[PROCESS_PARAMETERS32_RESERVED1];
    UINT32 Reserved2[PROCESS_PARAMETERS32_RESERVED2];
    UNICODE_STRING32 ImagePathName;
    UNICODE_STRING32 CommandLine;
} RTL_USER_PROCESS_PARAMETERS32;

/* "_OBJECT_ATTRIBUTES (ntdef.h)": Length, RootDirectory,
   ObjectName, Attributes, SecurityDescriptor, SecurityQualityOfService.
   "NtCreateFile function (winternl.h)": Length "must be at least
   sizeof(OBJECT_ATTRIBUTES)". */
typedef struct {
    UINT32 Length;
    UINT32 RootDirectory;
    UINT32 ObjectName;
    UINT32 Attributes;
    UINT32 SecurityDescriptor;
    UINT32 SecurityQualityOfService;
} OBJECT_ATTRIBUTES32;

/* "_IO_STATUS_BLOCK (wdm.h)": a Status, or a Pointer, then
   Information. */
typedef struct {
    UINT32 Status;
    UINT32 Information;
} IO_STATUS_BLOCK32;

/* LARGE_INTEGER as NtReadFile's ByteOffset: the low half, then the
   high. */
typedef struct {
    UINT32 LowPart;
    UINT32 HighPart;
} LARGE_INTEGER32;

_Static_assert(sizeof(UNICODE_STRING32) == 8, "UNICODE_STRING");
_Static_assert(sizeof(TEB32) == 0x34, "TEB.ProcessEnvironmentBlock at 0x30");
_Static_assert(sizeof(PEB32) == 0x14, "PEB.Ldr at 0x0C");
_Static_assert(sizeof(PEB_LDR_DATA32) == 0x1C,
               "InMemoryOrderModuleList at 0x14");
_Static_assert(sizeof(LDR_DATA_TABLE_ENTRY32) == 0x2C,
               "DllBase at 0x18, FullDllName at 0x24");
_Static_assert(sizeof(RTL_USER_PROCESS_PARAMETERS32) == 0x48,
               "CommandLine at 0x40");
_Static_assert(sizeof(OBJECT_ATTRIBUTES32) == 24, "OBJECT_ATTRIBUTES");

/* NTSTATUS values, MS-ERREF 2.3.1. */
#define STATUS_SUCCESS 0x00000000u
#define STATUS_UNSUCCESSFUL 0xC0000001u
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define STATUS_INVALID_HANDLE 0xC0000008u
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_NO_MEMORY 0xC0000017u
#define STATUS_ACCESS_DENIED 0xC0000022u
#define STATUS_OBJECT_NAME_INVALID 0xC0000033u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_DISK_FULL 0xC000007Fu

/* Access rights, MS-SMB2 2.2.13.1.1. */
#define FILE_READ_DATA 0x00000001u
#define FILE_WRITE_DATA 0x00000002u
#define FILE_APPEND_DATA 0x00000004u
#define GENERIC_ALL 0x10000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_READ 0x80000000u

/* CreateDisposition, MS-SMB2 2.2.13. */
#define FILE_SUPERSEDE 0u
#define FILE_OPEN 1u
#define FILE_CREATE 2u
#define FILE_OPEN_IF 3u
#define FILE_OVERWRITE 4u
#define FILE_OVERWRITE_IF 5u

/* CreateOptions, MS-SMB2 2.2.13. */
#define FILE_SYNCHRONOUS_IO_ALERT 0x00000010u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
#define FILE_NON_DIRECTORY_FILE 0x00000040u

/* What NtCreateFile reports in IoStatusBlock->Information, MS-SMB2
   2.2.14's CreateAction. */
#define FILE_SUPERSEDED 0u
#define FILE_OPENED 1u
#define FILE_CREATED 2u
#define FILE_OVERWRITTEN 3u

/* The current process's pseudo-handle, which "ZwCurrentProcess Macro"
   documents as "a special value that always represents the current
   process"; its value, all ones, is observed in the seed. */
#define NT_CURRENT_PROCESS 0xFFFFFFFFu

#endif
