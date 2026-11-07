/** @file
    What a 32-bit Windows program finds in its process and passes to the
    native routines, at the 32-bit (x86) sizes: every pointer and handle is
    a UINT32, so the launcher, built for x86-64, and the 32-bit ntdll lay
    the structures out alike. Layouts are from the Microsoft Learn pages
    docs/spec/compat.md pins, cited here by their titles; a field winternl.h
    marks Reserved is never read or written. Values are from those pages,
    from MS-ERREF 2.3.1 for NTSTATUS, from MS-SMB2 2.2.13, 2.2.13.1.1
    and 2.2.14, which carry the same values over the wire, and from
    MS-FSCC 2.4.40 and 2.4.47. What was learned only from stage0-pe32's
    programs, as a test of the layouts, is marked "observed in the seed"
    for its hex0 seed and "observed in the chain" for the programs the
    seed's chain builds. */

#ifndef NOVA_NT32_H
#define NOVA_NT32_H

#include "types.h"

/** An NT status code (MS-ERREF 2.3). */
typedef UINT32 NTSTATUS;

/** "_UNICODE_STRING (ntdef.h)": Length and MaximumLength in
    bytes, the first excluding a terminating NUL, then Buffer. */
typedef struct {
    UINT16 Length;        /**< the string's bytes, without a NUL */
    UINT16 MaximumLength; /**< the buffer's bytes */
    UINT32 Buffer;        /**< the buffer's address */
} UNICODE_STRING32;

/** "LIST_ENTRY (ntdef.h)". */
typedef struct {
    UINT32 Flink; /**< the next entry's address */
    UINT32 Blink; /**< the previous entry's address */
} LIST_ENTRY32;

#define TEB32_RESERVED1 12 /**< the PVOIDs of TEB32's Reserved1 */
/** "TEB (winternl.h)": Reserved1[12] of PVOID, then
    ProcessEnvironmentBlock. The rest of the structure is reserved; one
    page holds it. The seed reads the PEB pointer as fs:0x30 (observed in
    the seed). */
typedef struct {
    UINT32 Reserved1[TEB32_RESERVED1]; /**< reserved */
    UINT32 ProcessEnvironmentBlock;    /**< the PEB's address */
} TEB32;

#define PEB32_RESERVED3 2 /**< the PVOIDs of PEB32's Reserved3 */
/** "PEB (winternl.h)": Reserved1[2], BeingDebugged,
    Reserved2[1], Reserved3[2] of PVOID, Ldr, ProcessParameters. The seed
    reads Ldr at 0x0C and ProcessParameters at 0x10 (observed in the
    seed). */
typedef struct {
    UINT8 Reserved1[2];                /**< reserved */
    UINT8 BeingDebugged;               /**< whether a debugger is attached */
    UINT8 Reserved2[1];                /**< reserved */
    UINT32 Reserved3[PEB32_RESERVED3]; /**< reserved */
    UINT32 Ldr;                        /**< the loader data's address */
    UINT32 ProcessParameters;          /**< the process parameters' address */
} PEB32;

#define PEB_LDR32_RESERVED1 8 /**< the bytes of PEB_LDR_DATA32's Reserved1 */
#define PEB_LDR32_RESERVED2 3 /**< the PVOIDs of its Reserved2 */
/** "PEB_LDR_DATA (winternl.h)": Reserved1[8], Reserved2[3] of
    PVOID, then InMemoryOrderModuleList, "the head of a doubly-linked list
    that contains the loaded modules for the process". The seed takes the
    list's second entry as ntdll (observed in the seed). */
typedef struct {
    UINT8 Reserved1[PEB_LDR32_RESERVED1];  /**< reserved */
    UINT32 Reserved2[PEB_LDR32_RESERVED2]; /**< reserved */
    /** the head of the list of loaded modules, in memory order */
    LIST_ENTRY32 InMemoryOrderModuleList;
} PEB_LDR_DATA32;

/** @name Reserved fields of LDR_DATA_TABLE_ENTRY32
    @{ */
#define LDR_ENTRY32_RESERVED1 2 /**< the PVOIDs of Reserved1 */
#define LDR_ENTRY32_RESERVED2 2 /**< the PVOIDs of Reserved2 */
#define LDR_ENTRY32_RESERVED3 2 /**< the PVOIDs of Reserved3 */
/** @} */
/** LDR_DATA_TABLE_ENTRY, from the remarks of "PEB_LDR_DATA
    (winternl.h)": Reserved1[2] of PVOID, InMemoryOrderLinks,
    Reserved2[2], DllBase, Reserved3[2], FullDllName, and reserved fields
    after it. A list entry points at InMemoryOrderLinks, so DllBase is
    0x10 past it (observed in the seed). */
typedef struct {
    UINT32 Reserved1[LDR_ENTRY32_RESERVED1]; /**< reserved */
    LIST_ENTRY32 InMemoryOrderLinks;         /**< its links in the list */
    UINT32 Reserved2[LDR_ENTRY32_RESERVED2]; /**< reserved */
    UINT32 DllBase;                          /**< the module's address */
    UINT32 Reserved3[LDR_ENTRY32_RESERVED3]; /**< reserved */
    UNICODE_STRING32 FullDllName;            /**< the module's full path */
} LDR_DATA_TABLE_ENTRY32;

/** @name Reserved fields of RTL_USER_PROCESS_PARAMETERS32
    @{ */
#define PROCESS_PARAMETERS32_RESERVED1 16 /**< the bytes of Reserved1 */
#define PROCESS_PARAMETERS32_RESERVED2 10 /**< the PVOIDs of Reserved2 */
/** @} */
/** "RTL_USER_PROCESS_PARAMETERS (winternl.h)": Reserved1[16],
    Reserved2[10] of PVOID, ImagePathName, CommandLine. The seed reads
    CommandLine.Buffer at 0x44 and writes NULs into the command line
    (observed in the seed). The page documents nothing after CommandLine;
    the chain's C programs read the address of the environment block at
    0x48, and the standard input, output and error handles from
    Reserved2[2], [3] and [4], at 0x18, 0x1C and 0x20 (observed in the
    chain). */
typedef struct {
    UINT8 Reserved1[PROCESS_PARAMETERS32_RESERVED1]; /**< reserved */
    /** reserved; [2], [3] and [4] hold the standard handles */
    UINT32 Reserved2[PROCESS_PARAMETERS32_RESERVED2];
    UNICODE_STRING32 ImagePathName; /**< the image's path */
    UNICODE_STRING32 CommandLine;   /**< the command line */
    UINT32 Environment;             /**< the environment block's address */
} RTL_USER_PROCESS_PARAMETERS32;

/** "_OBJECT_ATTRIBUTES (ntdef.h)": Length, RootDirectory,
    ObjectName, Attributes, SecurityDescriptor, SecurityQualityOfService.
    "NtCreateFile function (winternl.h)": Length "must be at least
    sizeof(OBJECT_ATTRIBUTES)". */
typedef struct {
    UINT32 Length; /**< the structure's bytes */
    /** the directory ObjectName is relative to */
    UINT32 RootDirectory;
    UINT32 ObjectName;               /**< the object's name's address */
    UINT32 Attributes;               /**< the attribute flags */
    UINT32 SecurityDescriptor;       /**< a security descriptor's address */
    UINT32 SecurityQualityOfService; /**< a quality of service's address */
} OBJECT_ATTRIBUTES32;

/** "_IO_STATUS_BLOCK (wdm.h)": a Status, or a Pointer, then
    Information. */
typedef struct {
    UINT32 Status;      /**< the request's NTSTATUS */
    UINT32 Information; /**< what the request reports, as it defines */
} IO_STATUS_BLOCK32;

/** LARGE_INTEGER as NtReadFile's ByteOffset: the low half, then the
    high. */
typedef struct {
    UINT32 LowPart;  /**< the low 32 bits */
    UINT32 HighPart; /**< the high 32 bits */
} LARGE_INTEGER32;

/** @name File information classes
    The FileInformationClass values ntdll serves, as "NtQueryInformationFile
    function (ntifs.h)" and "NtSetInformationFile function (ntifs.h)" number
    them. The chain queries both and sets the position (observed in the
    chain).
    @{ */
/** FileStandardInformation */
#define FILE_STANDARD_INFORMATION_CLASS 5u
/** FilePositionInformation */
#define FILE_POSITION_INFORMATION_CLASS 14u
/** @} */

/** "FILE_POSITION_INFORMATION structure (wdm.h)": the byte offset of the
    current file pointer. MS-FSCC 2.4.40: only an offset of zero or more
    may be set. */
typedef struct {
    LARGE_INTEGER32 CurrentByteOffset; /**< the file pointer's offset */
} FILE_POSITION_INFORMATION32;

/** The bytes of FILE_STANDARD_INFORMATION32's Reserved. */
#define FILE_STANDARD_INFORMATION32_RESERVED 2
/** "FILE_STANDARD_INFORMATION structure (wdm.h)": AllocationSize,
    EndOfFile, NumberOfLinks, DeletePending, Directory. MS-FSCC 2.4.47 lays
    it out on the wire with two Reserved bytes after Directory, which the
    structure's own alignment gives it in memory. The chain reads EndOfFile
    at 8 of a 24-byte buffer (observed in the chain). */
typedef struct {
    LARGE_INTEGER32 AllocationSize; /**< the bytes allocated to the file */
    LARGE_INTEGER32 EndOfFile;      /**< the file's size in bytes */
    UINT32 NumberOfLinks;           /**< the file's hard links */
    UINT8 DeletePending;            /**< whether a delete is pending */
    UINT8 Directory;                /**< whether it is a directory */
    /** reserved */
    UINT8 Reserved[FILE_STANDARD_INFORMATION32_RESERVED];
} FILE_STANDARD_INFORMATION32;

_Static_assert(sizeof(UNICODE_STRING32) == 8, "UNICODE_STRING");
_Static_assert(sizeof(TEB32) == 0x34, "TEB.ProcessEnvironmentBlock at 0x30");
_Static_assert(sizeof(PEB32) == 0x14, "PEB.Ldr at 0x0C");
_Static_assert(sizeof(PEB_LDR_DATA32) == 0x1C,
               "InMemoryOrderModuleList at 0x14");
_Static_assert(sizeof(LDR_DATA_TABLE_ENTRY32) == 0x2C,
               "DllBase at 0x18, FullDllName at 0x24");
_Static_assert(sizeof(RTL_USER_PROCESS_PARAMETERS32) == 0x4C,
               "CommandLine at 0x40, Environment at 0x48");
_Static_assert(sizeof(OBJECT_ATTRIBUTES32) == 24, "OBJECT_ATTRIBUTES");
_Static_assert(sizeof(FILE_POSITION_INFORMATION32) == 8,
               "FILE_POSITION_INFORMATION");
_Static_assert(sizeof(FILE_STANDARD_INFORMATION32) == 24,
               "FILE_STANDARD_INFORMATION, EndOfFile at 8");

/** @name NTSTATUS values
    NTSTATUS values, MS-ERREF 2.3.1.
    @{ */
#define STATUS_SUCCESS 0x00000000u      /**< success */
#define STATUS_UNSUCCESSFUL 0xC0000001u /**< the request failed */
/** the request is not implemented */
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
/** a length that does not match the information class */
#define STATUS_INFO_LENGTH_MISMATCH 0xC0000004u
#define STATUS_INVALID_HANDLE 0xC0000008u    /**< an invalid handle */
#define STATUS_INVALID_PARAMETER 0xC000000Du /**< an invalid parameter */
#define STATUS_END_OF_FILE 0xC0000011u       /**< the end of the file */
#define STATUS_NO_MEMORY 0xC0000017u         /**< not enough memory */
#define STATUS_ACCESS_DENIED 0xC0000022u     /**< access denied */
/** a name that is not valid */
#define STATUS_OBJECT_NAME_INVALID 0xC0000033u
/** no object has the name */
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
/** an object has the name already */
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
/** the path does not exist */
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_DISK_FULL 0xC000007Fu /**< the disk is full */
/** @} */

/** @name Access rights
    Access rights, MS-SMB2 2.2.13.1.1.
    @{ */
#define FILE_READ_DATA 0x00000001u   /**< read the file's data */
#define FILE_WRITE_DATA 0x00000002u  /**< write the file's data */
#define FILE_APPEND_DATA 0x00000004u /**< append to the file's data */
#define GENERIC_ALL 0x10000000u      /**< every access */
#define GENERIC_WRITE 0x40000000u    /**< write access */
#define GENERIC_READ 0x80000000u     /**< read access */
/** @} */

/** @name Create dispositions
    CreateDisposition, MS-SMB2 2.2.13.
    @{ */
/** replace the file if it exists, otherwise create it */
#define FILE_SUPERSEDE 0u
/** open the file if it exists, otherwise fail */
#define FILE_OPEN 1u
/** create the file if it does not exist, otherwise fail */
#define FILE_CREATE 2u
/** open the file if it exists, otherwise create it */
#define FILE_OPEN_IF 3u
/** overwrite the file if it exists, otherwise fail */
#define FILE_OVERWRITE 4u
/** overwrite the file if it exists, otherwise create it */
#define FILE_OVERWRITE_IF 5u
/** @} */

/** @name Create options
    CreateOptions, MS-SMB2 2.2.13.
    @{ */
/** synchronous I/O, whose waits an alert can end */
#define FILE_SYNCHRONOUS_IO_ALERT 0x00000010u
/** synchronous I/O, whose waits no alert ends */
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
/** the file opened is not a directory */
#define FILE_NON_DIRECTORY_FILE 0x00000040u
/** @} */

/** @name Create actions
    What NtCreateFile reports in IoStatusBlock->Information, MS-SMB2
    2.2.14's CreateAction.
    @{ */
#define FILE_SUPERSEDED 0u  /**< the file was replaced */
#define FILE_OPENED 1u      /**< the file was opened */
#define FILE_CREATED 2u     /**< the file was created */
#define FILE_OVERWRITTEN 3u /**< the file was overwritten */
/** @} */

/** The current process's pseudo-handle, which "ZwCurrentProcess Macro"
    documents as "a special value that always represents the current
    process"; its value, all ones, is observed in the seed. */
#define NT_CURRENT_PROCESS 0xFFFFFFFFu

#endif
