/* The UEFI interfaces nova-boot calls, written out from the UEFI
   Specification 2.11: 2.3 (data types, x64 calling convention), 4.3.1
   (EFI_SYSTEM_TABLE), 4.4.1 (EFI_BOOT_SERVICES), 7.2 (AllocatePages,
   FreePages, GetMemoryMap, AllocatePool, FreePool), 7.3
   (HandleProtocol, LocateHandleBuffer), 7.4.6 (ExitBootServices), 7.5
   (Stall, SetWatchdogTimer), 9.1 (EFI_LOADED_IMAGE_PROTOCOL), 12.9
   (EFI_GRAPHICS_OUTPUT_PROTOCOL, EFI_EDID_DISCOVERED_PROTOCOL,
   EFI_EDID_ACTIVE_PROTOCOL), 13.4 and 13.5 (simple file system, file
   protocol), 37.5 (EFI_RNG_PROTOCOL). Members nova-boot never calls are
   VOID * so the offsets of those it does call stay right; the static
   assertions pin them. */

#ifndef NOVA_EFI_H
#define NOVA_EFI_H

#include <stddef.h>
#include "types.h"

typedef UINTN EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT16 CHAR16;

#define EFI_SUCCESS 0
#define EFI_ERROR_BIT 0x8000000000000000ULL
#define EFI_LOAD_ERROR (EFI_ERROR_BIT | 1)
#define EFI_UNSUPPORTED (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND (EFI_ERROR_BIT | 14)
#define EFI_INCOMPATIBLE_VERSION (EFI_ERROR_BIT | 25)

/* EFI_ALLOCATE_TYPE and EFI_MEMORY_TYPE values. */
#define ALLOCATE_ANY_PAGES 0
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_CONVENTIONAL_MEMORY 7

#define EFI_PAGE_SIZE 4096
#define EFI_MEMORY_DESCRIPTOR_VERSION 1
#define EFI_FILE_MODE_READ 1
#define EFI_FILE_END 0xFFFFFFFFFFFFFFFFULL /* SetPosition to end of file */
#define BY_PROTOCOL 2 /* EFI_LOCATE_SEARCH_TYPE ByProtocol */

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct {
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    UINT64 VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* The table through LocateHandleBuffer, the last member nova-boot
   calls. */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    EFI_STATUS (*AllocatePages)(UINTN Type, UINTN MemoryType, UINTN Pages,
                                EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize, VOID *MemoryMap,
                               UINTN *MapKey, UINTN *DescriptorSize,
                               UINT32 *DescriptorVersion);
    EFI_STATUS (*AllocatePool)(UINTN PoolType, UINTN Size, VOID **Buffer);
    EFI_STATUS (*FreePool)(VOID *Buffer);
    VOID *CreateEvent;
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;
    VOID *InstallProtocolInterface;
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                 VOID **Interface);
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    VOID *LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;
    VOID *LoadImage;
    VOID *StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    VOID *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN Microseconds);
    EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode,
                                   UINTN DataSize, CHAR16 *Data);
    VOID *ConnectController;
    VOID *DisconnectController;
    VOID *OpenProtocol;
    VOID *CloseProtocol;
    VOID *OpenProtocolInformation;
    VOID *ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(UINTN SearchType, EFI_GUID *Protocol,
                                     VOID *SearchKey, UINTN *NoHandles,
                                     EFI_HANDLE **Buffer);
} EFI_BOOT_SERVICES;

_Static_assert(offsetof(EFI_BOOT_SERVICES, AllocatePages) == 0x28, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, FreePages) == 0x30, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, GetMemoryMap) == 0x38, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, AllocatePool) == 0x40, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, FreePool) == 0x48, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, HandleProtocol) == 0x98, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, ExitBootServices) == 0xE8, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, Stall) == 0xF8, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, SetWatchdogTimer) == 0x100, "");
_Static_assert(offsetof(EFI_BOOT_SERVICES, LocateHandleBuffer) == 0x138, "");

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    VOID *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    VOID *ConOut;
    EFI_HANDLE StandardErrorHandle;
    VOID *StdErr;
    VOID *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    VOID *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    VOID *FilePath;
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    VOID *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                       CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *This);
    VOID *Delete;
    EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize,
                       VOID *Buffer);
    VOID *Write;
    EFI_STATUS (*GetPosition)(EFI_FILE_PROTOCOL *This, UINT64 *Position);
    EFI_STATUS (*SetPosition)(EFI_FILE_PROTOCOL *This, UINT64 Position);
};

/* A directory's Read returns one of these per entry, and an empty buffer
   after the last (UEFI 13.5.5, 13.5.16). The three times are EFI_TIME,
   16 bytes each (UEFI 8.3), which nova-boot does not read. */
#define EFI_FILE_DIRECTORY 0x10ULL
#define EFI_TIME_SIZE 16

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    UINT8 CreateTime[EFI_TIME_SIZE];
    UINT8 LastAccessTime[EFI_TIME_SIZE];
    UINT8 ModificationTime[EFI_TIME_SIZE];
    UINT64 Attribute;
    CHAR16 FileName[];
} EFI_FILE_INFO;

_Static_assert(offsetof(EFI_FILE_INFO, FileName) == 80, "");

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (*OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                             EFI_FILE_PROTOCOL **Root);
};

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;
    UINT32 HorizontalResolution;
    UINT32 VerticalResolution;
    UINT32 PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32 MaxMode;
    UINT32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                            UINT32 ModeNumber, UINTN *SizeOfInfo,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (*SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
                          UINT32 ModeNumber);
    VOID *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* The two EDID protocols share one layout. */
typedef struct {
    UINT32 SizeOfEdid;
    UINT8 *Edid;
} EFI_EDID_ACTIVE_PROTOCOL;

typedef EFI_EDID_ACTIVE_PROTOCOL EFI_EDID_DISCOVERED_PROTOCOL;

typedef struct EFI_RNG_PROTOCOL EFI_RNG_PROTOCOL;

/* A null RNGAlgorithm asks for the driver's default algorithm. */
struct EFI_RNG_PROTOCOL {
    VOID *GetInfo;
    EFI_STATUS (*GetRNG)(EFI_RNG_PROTOCOL *This, EFI_GUID *RNGAlgorithm,
                         UINTN RNGValueLength, UINT8 *RNGValue);
};

#endif
