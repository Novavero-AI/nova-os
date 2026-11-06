/* Minimal UEFI application: prints one line on the console, waits for a
   key so the line stays visible, and returns EFI_SUCCESS.

   Every type and layout is written out from the UEFI Specification 2.10
   (August 2022), with no gnu-efi or EDK2 headers:
     2.3.1  Table 2.3, common data types
     2.3.4  x64 calling convention (Microsoft x64, which is TinyCC's
            convention for TCC_TARGET_PE on x86_64, so no EFIAPI attribute)
     4.1.1  EFI_IMAGE_ENTRY_POINT
     4.2.1  EFI_TABLE_HEADER
     4.3.1  EFI_SYSTEM_TABLE
     4.4.1  EFI_BOOT_SERVICES (only the members through WaitForEvent)
     7.1.5  EFI_BOOT_SERVICES.WaitForEvent()
     12.3.1 EFI_SIMPLE_TEXT_INPUT_PROTOCOL
     12.4.1 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, 12.4.3 OutputString()
     Appendix D, EFI_SUCCESS
   A table member this program never calls is declared VOID * so the
   offsets of the members it does call stay right. */

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64; /* x86_64 PE is LLP64: long is 32 bits */
typedef UINT64 UINTN;
typedef UINT16 CHAR16;             /* TinyCC's PE wchar_t is 16 bits */
typedef void VOID;
typedef UINTN EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;

#define EFI_SUCCESS 0

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (*EFI_TEXT_STRING)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);

struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    VOID *Reset;
    EFI_TEXT_STRING OutputString;
    VOID *TestString;
    VOID *QueryMode;
    VOID *SetMode;
    VOID *SetAttribute;
    VOID *ClearScreen;
    VOID *SetCursorPosition;
    VOID *EnableCursor;
    VOID *Mode;
};

typedef struct {
    VOID *Reset;
    VOID *ReadKeyStroke;
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef EFI_STATUS (*EFI_WAIT_FOR_EVENT)(UINTN NumberOfEvents,
                                         EFI_EVENT *Event, UINTN *Index);

/* A prefix of the table: the members after WaitForEvent are not declared,
   and this program only ever reaches the table through the firmware's
   pointer, never by value or by sizeof. */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    VOID *AllocatePages;
    VOID *FreePages;
    VOID *GetMemoryMap;
    VOID *AllocatePool;
    VOID *FreePool;
    VOID *CreateEvent;
    VOID *SetTimer;
    EFI_WAIT_FOR_EVENT WaitForEvent;
} EFI_BOOT_SERVICES_PREFIX;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    VOID *RuntimeServices;
    EFI_BOOT_SERVICES_PREFIX *BootServices;
    UINTN NumberOfTableEntries;
    VOID *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* An initialized pointer holds an absolute address, so the image carries a
   DIR64 base relocation that the firmware must apply when it loads the
   image anywhere but its ImageBase. */
static CHAR16 *greeting = L"nova-os: hello from TinyCC\r\n";

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = SystemTable->ConOut;
    UINTN index;

    (void)ImageHandle;
    out->OutputString(out, greeting);
    SystemTable->BootServices->WaitForEvent(1, &SystemTable->ConIn->WaitForKey,
                                            &index);
    return EFI_SUCCESS;
}
