/*
 * Nexora OS - ps/wincompat.c
 * Minimal, fail-closed Win32 import resolver. Its bounded kernel32/kernelbase surface includes
 * console I/O, process exit, last-error access, and handle close; it does not imply broader DLL
 * compatibility. Each IAT entry points to a per-process thunk or a small local TEB accessor.
 */
#include "pe.h"
#include "psp.h"

#define WINCOMPAT_IMPORT_MODULES 128u
#define WINCOMPAT_IMPORTS_PER_MODULE 4096u
#define WINCOMPAT_NAME_CAPACITY 260u
#define WINCOMPAT_THUNK_STRIDE 96u
#define WINCOMPAT_THUNK_COUNT 128u
#define WINCOMPAT_THUNK_PAGE_SIZE PSP_WINCOMPAT_THUNK_PAGE_SIZE
#define WINCOMPAT_REPORT_MAX 4096u
#define WINCOMPAT_DIRECTORY_DELAY_IMPORT 13u
#define WINCOMPAT_HEAP_HANDLE 0x100u
#define WINCOMPAT_HEAP_MAX_ALLOCATION (64u * 1024u * 1024u)
#define WINCOMPAT_HEAP_NO_SERIALIZE 0x00000001u
#define WINCOMPAT_HEAP_ZERO_MEMORY 0x00000008u
#define WINCOMPAT_HEAP_TAG HZ_POOL_TAG('W', 'H', 'e', 'p')

/* Windows apps access these fields directly through GS, so use the documented x64 TEB offsets. */
_Static_assert(offsetof(HZ_TEB, WindowsLastError) == 0x68, "WinCompat last-error TEB offset");
_Static_assert(offsetof(HZ_TEB, WindowsProcessId) == 0x40, "WinCompat process ID TEB offset");
_Static_assert(offsetof(HZ_TEB, WindowsThreadId) == 0x48, "WinCompat thread ID TEB offset");
_Static_assert(offsetof(HZ_PEB, ImageBase) == 0, "WinCompat image base PEB offset");
_Static_assert(offsetof(HZ_PEB, CommandLine) == 0x60, "WinCompat command line PEB offset");
_Static_assert(WINCOMPAT_THUNK_COUNT * WINCOMPAT_THUNK_STRIDE <= WINCOMPAT_THUNK_PAGE_SIZE,
               "WinCompat thunks fit in the mapped thunk region");

typedef struct HZ_PACKED _PE_IMPORT_DESCRIPTOR {
    uint32_t OriginalFirstThunk;
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
} PE_IMPORT_DESCRIPTOR;

typedef struct HZ_PACKED _PE_DELAY_IMPORT_DESCRIPTOR {
    uint32_t Attributes;
    uint32_t Name;
    uint32_t ModuleHandle;
    uint32_t ImportAddressTable;
    uint32_t ImportNameTable;
    uint32_t BoundImportAddressTable;
    uint32_t UnloadInformationTable;
    uint32_t TimeDateStamp;
} PE_DELAY_IMPORT_DESCRIPTOR;

typedef enum _WINCOMPAT_API {
    WinCompatApiGetStdHandle,
    WinCompatApiWriteFile,
    WinCompatApiExitProcess,
    WinCompatApiGetLastError,
    WinCompatApiSetLastError,
    WinCompatApiCloseHandle,
    WinCompatApiGetCurrentProcessId,
    WinCompatApiGetCurrentThreadId,
    WinCompatApiGetCurrentProcess,
    WinCompatApiGetCurrentThread,
    WinCompatApiQueryPerformanceCounter,
    WinCompatApiGetSystemTimeAsFileTime,
    WinCompatApiInitializeSListHead,
    WinCompatApiGetModuleHandleW,
    WinCompatApiGetModuleHandleA,
    WinCompatApiGetProcAddress,
    WinCompatApiGetTickCount,
    WinCompatApiGetTickCount64,
    WinCompatApiWsaStartup,
    WinCompatApiWsaCleanup,
    WinCompatApiWsaGetLastError,
    WinCompatApiWsaSetLastError,
    WinCompatApiSocket,
    WinCompatApiConnect,
    WinCompatApiSend,
    WinCompatApiRecv,
    WinCompatApiCloseSocket,
    WinCompatApiGetAddrInfo,
    WinCompatApiFreeAddrInfo,
    WinCompatApiGetAddrInfoW,
    WinCompatApiFreeAddrInfoW,
    WinCompatApiGetCommandLineA,
    WinCompatApiGetProcessHeap,
    WinCompatApiHeapAlloc,
    WinCompatApiHeapFree,
    WinCompatApiCrtMemcpy,
    WinCompatApiCrtMemset,
    WinCompatApiCrtStrlen,
    WinCompatApiCrtMalloc,
    WinCompatApiCrtFree,
    WinCompatApiCreateFileW,
    WinCompatApiGetFileAttributesW,
    WinCompatApiGetCommandLineW,
} WINCOMPAT_API;

typedef struct _WINCOMPAT_HEAP_ALLOCATION {
    LIST_ENTRY Link;
    void *Base;
} WINCOMPAT_HEAP_ALLOCATION;

static bool WinCompatReadString(const uint8_t *Image, uint64_t ImageSize, uint32_t Rva,
                                char *Output, size_t Capacity)
{
    if (!Rva || Rva >= ImageSize || Capacity < 2)
        return false;
    size_t Length = 0;
    while ((uint64_t)Rva + Length < ImageSize && Length + 1 < Capacity) {
        char C = (char)Image[Rva + Length];
        Output[Length++] = C;
        if (!C)
            return true;
    }
    Output[0] = 0;
    return false;
}

/* Terminal integration can expose this bounded formatter through a report-query service. */
size_t PspFormatWindowsImportReport(char *Buffer, size_t Capacity, const char *ImageName,
                                    uint16_t Machine, uint16_t OptionalMagic, const char *Stage,
                                    uint32_t DescriptorIndex, uint32_t SymbolIndex,
                                    const char *Module, const char *Api, STATUS Status)
{
    if (!Buffer && Capacity)
        return 0;
    const char *Architecture = "unknown/unsupported PE";
    if (Machine == PE_MACHINE_AMD64 && OptionalMagic == PE_OPTIONAL_MAGIC_PE32PLUS)
        Architecture = "AMD64 PE32+";
    else if (Machine == 0x014Cu && OptionalMagic == 0x010Bu)
        Architecture = "I386 PE32";
    else if (Machine == 0x01C4u && OptionalMagic == 0x010Bu)
        Architecture = "ARMNT PE32";
    else if (Machine == 0xAA64u && OptionalMagic == PE_OPTIONAL_MAGIC_PE32PLUS)
        Architecture = "ARM64 PE32+";
    if (!ImageName)
        ImageName = "<unknown image>";
    if (!Stage)
        Stage = "<unknown>";
    if (!Module)
        Module = "<unknown-module>";
    if (!Api)
        Api = "<unknown-api>";
    int Length = RtlFormatString(Buffer, Capacity,
                                 "%.*s: Win32 import report [%s, machine=0x%04X, optional=0x%04X, stage=%.*s, descriptor=%u, symbol=%u, import=%.*s!%.*s, status=0x%08X]",
                                 32, ImageName, Architecture, (uint32_t)Machine, (uint32_t)OptionalMagic,
                                 96, Stage, DescriptorIndex, SymbolIndex, (int)WINCOMPAT_NAME_CAPACITY, Module,
                                 (int)WINCOMPAT_NAME_CAPACITY, Api, (uint32_t)Status);
    return Length > 0 ? (size_t)Length : 0;
}

static STATUS WinCompatImportFailure(EPROCESS *Process, const PE_HEADERS *Headers, STATUS Status,
                                     const char *Stage, uint32_t DescriptorIndex, uint32_t SymbolIndex,
                                     const char *Module, const char *Api)
{
    char Report[1024];
    uint16_t Machine = Headers && Headers->File ? Headers->File->Machine : 0;
    uint16_t OptionalMagic = Headers && Headers->Optional ? Headers->Optional->Magic : 0;
    const char *ImageName = Process ? Process->ImageName : NULL;
    (void)PspFormatWindowsImportReport(Report, sizeof(Report), ImageName, Machine, OptionalMagic,
                                       Stage, DescriptorIndex, SymbolIndex, Module, Api, Status);
    DbgPrintEx(DPFLTR_WARNING_LEVEL, "%s\n", Report);
    return Status;
}

static char WinCompatLower(char C)
{
    return C >= 'A' && C <= 'Z' ? (char)(C + ('a' - 'A')) : C;
}

static bool WinCompatEqual(const char *A, const char *B)
{
    while (*A && *B && WinCompatLower(*A) == WinCompatLower(*B)) {
        ++A;
        ++B;
    }
    return *A == 0 && *B == 0;
}

static bool WinCompatFindApi(const char *Name, WINCOMPAT_API *Api);
static bool WinCompatApiBelongsToModule(const char *Module, WINCOMPAT_API Api);

typedef struct _WINCOMPAT_REPORT_WRITER {
    char *Buffer;
    size_t Capacity;
    size_t Length;
    bool Truncated;
} WINCOMPAT_REPORT_WRITER;

static void WinCompatReportAppend(WINCOMPAT_REPORT_WRITER *Writer, const char *Format, ...)
{
    if (Writer->Truncated || Writer->Capacity <= Writer->Length)
        return;
    va_list Args;
    va_start(Args, Format);
    int Needed = RtlFormatStringV(Writer->Buffer + Writer->Length,
                                  Writer->Capacity - Writer->Length, Format, Args);
    va_end(Args);
    if (Needed < 0 || (size_t)Needed >= Writer->Capacity - Writer->Length) {
        Writer->Length = Writer->Capacity - 1;
        Writer->Truncated = true;
        return;
    }
    Writer->Length += (size_t)Needed;
}

static void WinCompatReportFinish(WINCOMPAT_REPORT_WRITER *Writer)
{
    if (!Writer->Capacity)
        return;
    if (Writer->Truncated) {
        static const char Marker[] = "[report truncated]";
        size_t Available = Writer->Capacity - 1;
        size_t MarkerLength = sizeof(Marker) - 1;
        size_t CopyLength = HZ_MIN(Available, MarkerLength);
        size_t Start = Available - CopyLength;
        memcpy(Writer->Buffer + Start, Marker + MarkerLength - CopyLength, CopyLength);
        Writer->Length = Start + CopyLength;
    }
    Writer->Buffer[Writer->Length] = 0;
}

static uint16_t WinCompatInspectFileWord(const uint8_t *File, uint64_t FileSize, uint64_t Offset)
{
    uint16_t Value = 0;
    if (Offset <= FileSize && FileSize - Offset >= sizeof(Value))
        memcpy(&Value, File + Offset, sizeof(Value));
    return Value;
}

static void WinCompatInspectFileArchitecture(const void *File, uint64_t FileSize,
                                             uint16_t *Machine, uint16_t *OptionalMagic)
{
    const uint8_t *Bytes = (const uint8_t *)File;
    *Machine = 0;
    *OptionalMagic = 0;
    if (!File || FileSize < sizeof(PE_DOS_HEADER))
        return;
    PE_DOS_HEADER Dos;
    memcpy(&Dos, Bytes, sizeof(Dos));
    if (Dos.Magic != PE_DOS_MAGIC || Dos.NtHeaderOffset <= 0 ||
        (uint64_t)Dos.NtHeaderOffset > FileSize ||
        FileSize - (uint64_t)Dos.NtHeaderOffset < 4 + sizeof(PE_FILE_HEADER))
        return;
    uint32_t Signature;
    memcpy(&Signature, Bytes + Dos.NtHeaderOffset, sizeof(Signature));
    if (Signature != PE_NT_SIGNATURE)
        return;
    uint64_t FileHeaderOffset = (uint64_t)Dos.NtHeaderOffset + sizeof(Signature);
    *Machine = WinCompatInspectFileWord(Bytes, FileSize, FileHeaderOffset);
    PE_FILE_HEADER Header;
    memcpy(&Header, Bytes + FileHeaderOffset, sizeof(Header));
    uint64_t OptionalOffset = FileHeaderOffset + sizeof(Header);
    if (Header.SizeOfOptionalHeader >= sizeof(uint16_t))
        *OptionalMagic = WinCompatInspectFileWord(Bytes, FileSize, OptionalOffset);
}

static bool WinCompatFileRvaRange(const PE_HEADERS *Headers, uint64_t FileSize,
                                  uint64_t Rva, uint64_t Length, uint64_t *FileOffset)
{
    if (!Headers || !Headers->File || !Headers->Optional || !FileOffset ||
        Rva > Headers->Optional->SizeOfImage ||
        Length > Headers->Optional->SizeOfImage - Rva)
        return false;
    if (Rva < Headers->Optional->SizeOfHeaders) {
        if (Length > Headers->Optional->SizeOfHeaders - Rva || Rva > FileSize ||
            Length > FileSize - Rva)
            return false;
        *FileOffset = Rva;
        return true;
    }
    for (uint32_t I = 0; I < Headers->File->NumberOfSections; ++I) {
        const PE_SECTION_HEADER *Section = &Headers->Sections[I];
        uint64_t RawStart = Section->VirtualAddress;
        uint64_t RawLength = Section->SizeOfRawData;
        if (Rva < RawStart || Rva - RawStart > RawLength ||
            Length > RawLength - (Rva - RawStart))
            continue;
        uint64_t Offset = (uint64_t)Section->PointerToRawData + (Rva - RawStart);
        if (Offset > FileSize || Length > FileSize - Offset)
            return false;
        *FileOffset = Offset;
        return true;
    }
    return false;
}

static bool WinCompatReadFileRva(const uint8_t *File, const PE_HEADERS *Headers, uint64_t FileSize,
                                 uint64_t Rva, void *Output, size_t Length)
{
    uint64_t Offset;
    if (!WinCompatFileRvaRange(Headers, FileSize, Rva, Length, &Offset))
        return false;
    memcpy(Output, File + Offset, Length);
    return true;
}

static bool WinCompatReadFileRvaString(const uint8_t *File, const PE_HEADERS *Headers,
                                       uint64_t FileSize, uint64_t Rva,
                                       char *Output, size_t Capacity)
{
    if (!Rva || Capacity < 2)
        return false;
    for (size_t I = 0; I + 1 < Capacity; ++I) {
        uint64_t Offset;
        if (Rva > UINT32_MAX || !WinCompatFileRvaRange(Headers, FileSize, Rva, 1, &Offset)) {
            Output[0] = 0;
            return false;
        }
        uint8_t Character = File[Offset];
        if (!Character) {
            Output[I] = 0;
            return true;
        }
        if (Character < 0x20 || Character > 0x7E) {
            Output[0] = 0;
            return false;
        }
        Output[I] = (char)Character;
        ++Rva;
    }
    Output[0] = 0;
    return false;
}

static STATUS WinCompatBuildReportError(WINCOMPAT_REPORT_WRITER *Writer, uint16_t Machine,
                                        uint16_t OptionalMagic, const char *Stage,
                                        uint32_t Descriptor, uint32_t Symbol,
                                        const char *Module, const char *Api, STATUS Status)
{
    char Line[768];
    (void)PspFormatWindowsImportReport(Line, sizeof(Line), "image", Machine, OptionalMagic,
                                       Stage, Descriptor, Symbol, Module, Api, Status);
    WinCompatReportAppend(Writer, "%s\n", Line);
    WinCompatReportFinish(Writer);
    return Status;
}

STATUS PspBuildWindowsImportReport(const void *File, uint64_t FileSize,
                                   char *Buffer, size_t Capacity)
{
    if (!Buffer || Capacity == 0)
        return STATUS_INVALID_PARAMETER;
    size_t OutputCapacity = HZ_MIN(Capacity, (size_t)WINCOMPAT_REPORT_MAX);
    WINCOMPAT_REPORT_WRITER Writer = {
        .Buffer = Buffer,
        .Capacity = OutputCapacity,
    };
    Buffer[0] = 0;
    if (!File || (uint64_t)(size_t)FileSize != FileSize) {
        return WinCompatBuildReportError(&Writer, 0, 0, "invalid file arguments or size",
                                         0, 0, NULL, NULL, STATUS_INVALID_PARAMETER);
    }

    PE_HEADERS Headers;
    STATUS Status = PspLocatePeHeaders(File, (size_t)FileSize, &Headers);
    uint16_t Machine = 0, OptionalMagic = 0;
    if (!HZ_SUCCESS(Status)) {
        WinCompatInspectFileArchitecture(File, FileSize, &Machine, &OptionalMagic);
        return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                         "PE header validation", 0, 0, NULL, NULL, Status);
    }
    Machine = Headers.File->Machine;
    OptionalMagic = Headers.Optional->Magic;
    size_t HeaderLength = PspFormatWindowsImportReport(Buffer, OutputCapacity, "image",
                                                        Machine, OptionalMagic, "inventory",
                                                        0, 0, "all modules", "all imports",
                                                        STATUS_SUCCESS);
    if (HeaderLength >= OutputCapacity) {
        Writer.Length = OutputCapacity - 1;
        Writer.Truncated = true;
    } else {
        Writer.Length = HeaderLength;
    }
    WinCompatReportAppend(&Writer, "\n");

    const PE_DATA_DIRECTORY *Tls = PeDirectory(&Headers, PE_DIRECTORY_TLS);
    if ((Tls->Size == 0) != (Tls->VirtualAddress == 0))
        return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                         "malformed TLS directory metadata", 0, 0,
                                         NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);
    if (Tls->Size != 0)
        WinCompatReportAppend(&Writer, "[UNSUPPORTED-TLS] TLS callbacks/data are not loaded\n");
    const PE_DATA_DIRECTORY *Delay = PeDirectory(&Headers, WINCOMPAT_DIRECTORY_DELAY_IMPORT);
    if ((Delay->Size == 0) != (Delay->VirtualAddress == 0))
        return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                         "malformed delay-import directory metadata", 0, 0,
                                         NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);
    if (Delay->Size != 0)
        WinCompatReportAppend(&Writer, "[UNSUPPORTED-DELAY-IMPORT] delay-load table is not resolved\n");

    const PE_DATA_DIRECTORY *Imports = PeDirectory(&Headers, PE_DIRECTORY_IMPORT);
    if ((Imports->Size == 0) != (Imports->VirtualAddress == 0))
        return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                         "malformed import directory metadata", 0, 0,
                                         NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);
    if (Imports->Size == 0) {
        WinCompatReportAppend(&Writer, "No standard imports.\n");
        WinCompatReportFinish(&Writer);
        return Writer.Truncated ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    if (Imports->Size < sizeof(PE_IMPORT_DESCRIPTOR) ||
        Imports->Size % sizeof(PE_IMPORT_DESCRIPTOR) != 0 ||
        Imports->Size > (WINCOMPAT_IMPORT_MODULES + 1u) * sizeof(PE_IMPORT_DESCRIPTOR))
        return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                         "import directory size or descriptor limit", 0, 0,
                                         NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);
    uint64_t DirectoryFileOffset;
    if (Imports->VirtualAddress == 0 ||
        (uint64_t)Imports->VirtualAddress + Imports->Size > Headers.Optional->SizeOfImage ||
        !WinCompatFileRvaRange(&Headers, FileSize, Imports->VirtualAddress, Imports->Size,
                               &DirectoryFileOffset))
        return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                         "import directory file bounds", 0, 0,
                                         NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);
    (void)DirectoryFileOffset;

    bool Terminated = false;
    bool HasImports = false;
    uint32_t DescriptorIndex = 0;
    uint64_t ImportEnd = (uint64_t)Imports->VirtualAddress + Imports->Size;
    for (uint64_t DescriptorRva = Imports->VirtualAddress; DescriptorRva < ImportEnd;
         DescriptorRva += sizeof(PE_IMPORT_DESCRIPTOR), ++DescriptorIndex) {
        PE_IMPORT_DESCRIPTOR Descriptor;
        if (!WinCompatReadFileRva(File, &Headers, FileSize, DescriptorRva,
                                  &Descriptor, sizeof(Descriptor)))
            return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                             "import descriptor bounds", DescriptorIndex, 0,
                                             NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);
        if (!Descriptor.OriginalFirstThunk && !Descriptor.TimeDateStamp &&
            !Descriptor.ForwarderChain && !Descriptor.Name && !Descriptor.FirstThunk) {
            Terminated = true;
            for (uint64_t Tail = DescriptorRva + sizeof(Descriptor); Tail < ImportEnd; ++Tail) {
                uint8_t Byte;
                if (!WinCompatReadFileRva(File, &Headers, FileSize, Tail, &Byte, sizeof(Byte)) || Byte)
                    return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                                     "nonzero or malformed descriptor tail",
                                                     DescriptorIndex, 0, NULL, NULL,
                                                     STATUS_INVALID_IMAGE_FORMAT);
            }
            break;
        }
        if (DescriptorIndex >= WINCOMPAT_IMPORT_MODULES)
            return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                             "import descriptor limit", DescriptorIndex, 0,
                                             NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);
        if (!Descriptor.Name || !Descriptor.FirstThunk)
            return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                             "import descriptor fields", DescriptorIndex, 0,
                                             NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);

        char Module[WINCOMPAT_NAME_CAPACITY] = {0};
        if (!WinCompatReadFileRvaString(File, &Headers, FileSize, Descriptor.Name,
                                        Module, sizeof(Module)))
            return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                             "import module name", DescriptorIndex, 0,
                                             NULL, "<malformed-name>", STATUS_INVALID_IMAGE_FORMAT);
        bool SupportedModule = WinCompatEqual(Module, "kernel32.dll") ||
                               WinCompatEqual(Module, "kernelbase.dll") ||
                               WinCompatEqual(Module, "ws2_32.dll") ||
                               WinCompatEqual(Module, "msvcrt.dll") ||
                               WinCompatEqual(Module, "ucrtbase.dll");
        uint64_t LookupRva = Descriptor.OriginalFirstThunk ?
                             Descriptor.OriginalFirstThunk : Descriptor.FirstThunk;
        bool ThunkTerminated = false;
        uint32_t SymbolIndex = 0;
        for (; SymbolIndex < WINCOMPAT_IMPORTS_PER_MODULE; ++SymbolIndex) {
            uint64_t SlotRva = LookupRva + (uint64_t)SymbolIndex * sizeof(uint64_t);
            uint64_t IatRva = (uint64_t)Descriptor.FirstThunk +
                              (uint64_t)SymbolIndex * sizeof(uint64_t);
            uint64_t Thunk, FileOffset;
            if (SlotRva > UINT32_MAX || IatRva > UINT32_MAX ||
                !WinCompatReadFileRva(File, &Headers, FileSize, SlotRva,
                                      &Thunk, sizeof(Thunk)) ||
                !WinCompatFileRvaRange(&Headers, FileSize, IatRva, sizeof(uint64_t),
                                       &FileOffset))
                return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                                 "import lookup or IAT table bounds",
                                                 DescriptorIndex, SymbolIndex, Module, NULL,
                                                 STATUS_INVALID_IMAGE_FORMAT);
            (void)FileOffset;
            if (!Thunk) {
                ThunkTerminated = true;
                break;
            }
            HasImports = true;
            if (Thunk & (1ull << 63)) {
                if (Thunk & 0x7FFFFFFFFFFF0000ull)
                    return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                                     "malformed ordinal import", DescriptorIndex,
                                                     SymbolIndex, Module, "<malformed-ordinal>",
                                                     STATUS_INVALID_IMAGE_FORMAT);
                WinCompatReportAppend(&Writer, "[%s] %s!#%u (ordinal imports unsupported)\n",
                                      SupportedModule ? "module=SUPPORTED api=UNSUPPORTED-ORDINAL" :
                                                        "module=UNSUPPORTED-DLL api=UNSUPPORTED-ORDINAL",
                                      Module, (uint32_t)(Thunk & 0xFFFF));
                continue;
            }
            if (Thunk > UINT32_MAX - sizeof(uint16_t))
                return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                                 "import-by-name hint bounds", DescriptorIndex,
                                                 SymbolIndex, Module, "<malformed-name>",
                                                 STATUS_INVALID_IMAGE_FORMAT);
            uint16_t Hint;
            if (!WinCompatReadFileRva(File, &Headers, FileSize, Thunk, &Hint, sizeof(Hint)))
                return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                                 "import-by-name hint bounds", DescriptorIndex,
                                                 SymbolIndex, Module, "<malformed-name>",
                                                 STATUS_INVALID_IMAGE_FORMAT);
            (void)Hint;
            char ApiName[WINCOMPAT_NAME_CAPACITY] = {0};
            if (!WinCompatReadFileRvaString(File, &Headers, FileSize,
                                            Thunk + sizeof(uint16_t), ApiName, sizeof(ApiName)))
                return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                                 "import API name", DescriptorIndex, SymbolIndex,
                                                 Module, "<malformed-name>",
                                                 STATUS_INVALID_IMAGE_FORMAT);
            WINCOMPAT_API Api = WinCompatApiGetStdHandle;
            bool SupportedApi = WinCompatFindApi(ApiName, &Api);
            SupportedApi = SupportedApi && WinCompatApiBelongsToModule(Module, Api);
            const char *Label = !SupportedModule ?
                                (SupportedApi ? "module=UNSUPPORTED-DLL api=RECOGNIZED" :
                                                "module=UNSUPPORTED-DLL api=UNSUPPORTED") :
                                (SupportedApi ? "module=SUPPORTED api=SUPPORTED" :
                                                "module=SUPPORTED api=UNSUPPORTED");
            WinCompatReportAppend(&Writer, "[%s] %s!%s\n", Label, Module, ApiName);
        }
        if (!ThunkTerminated)
            return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                             "unterminated import thunk table", DescriptorIndex,
                                             WINCOMPAT_IMPORTS_PER_MODULE, Module, NULL,
                                             STATUS_INVALID_IMAGE_FORMAT);
        if (SymbolIndex == 0)
            WinCompatReportAppend(&Writer, "[module=%s api=none] %s (no imported APIs)\n",
                                  SupportedModule ? "SUPPORTED" : "UNSUPPORTED-DLL",
                                  Module);
    }
    if (!Terminated)
        return WinCompatBuildReportError(&Writer, Machine, OptionalMagic,
                                         "missing import descriptor terminator", DescriptorIndex,
                                         0, NULL, NULL, STATUS_INVALID_IMAGE_FORMAT);
    if (!HasImports)
        WinCompatReportAppend(&Writer, "No imported APIs.\n");
    WinCompatReportFinish(&Writer);
    return Writer.Truncated ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
}

static uint32_t WinCompatService(WINCOMPAT_API Api)
{
    switch (Api) {
    case WinCompatApiGetStdHandle: return HzSysWinGetStdHandle;
    case WinCompatApiWriteFile: return HzSysWinWriteFile;
    case WinCompatApiExitProcess: return HzSysWinExitProcess;
    case WinCompatApiCloseHandle: return HzSysClose;
    case WinCompatApiQueryPerformanceCounter: return HzSysQueryUptime;
    case WinCompatApiGetTickCount:
    case WinCompatApiGetTickCount64: return HzSysQueryUptime;
    case WinCompatApiGetModuleHandleW:
    case WinCompatApiGetModuleHandleA: return HzSysWinGetModuleHandle;
    case WinCompatApiGetProcAddress: return HzSysWinGetProcAddress;
    case WinCompatApiCreateFileW: return HzSysWinCreateFile;
    case WinCompatApiGetFileAttributesW: return HzSysWinGetFileAttributes;
    case WinCompatApiGetCommandLineW:
        return UINT32_MAX;
    case WinCompatApiWsaStartup:
    case WinCompatApiWsaCleanup:
    case WinCompatApiSocket:
    case WinCompatApiConnect:
    case WinCompatApiSend:
    case WinCompatApiRecv:
    case WinCompatApiCloseSocket: return HzSysWinSocketApi;
    case WinCompatApiGetAddrInfo:
    case WinCompatApiFreeAddrInfo: return HzSysWinSocketApi;
    case WinCompatApiGetAddrInfoW:
    case WinCompatApiFreeAddrInfoW: return HzSysWinSocketApi;
    case WinCompatApiGetSystemTimeAsFileTime: return HzSysQuerySystemTime;
    case WinCompatApiGetLastError:
    case WinCompatApiSetLastError:
    case WinCompatApiGetCurrentProcessId:
    case WinCompatApiGetCurrentThreadId:
    case WinCompatApiGetCurrentProcess:
    case WinCompatApiGetCurrentThread:
    case WinCompatApiInitializeSListHead:
    case WinCompatApiGetCommandLineA:
    case WinCompatApiGetProcessHeap:
    case WinCompatApiWsaGetLastError:
    case WinCompatApiWsaSetLastError:
    case WinCompatApiCrtMemcpy:
    case WinCompatApiCrtMemset:
    case WinCompatApiCrtStrlen:
        return UINT32_MAX;
    case WinCompatApiHeapAlloc: return HzSysWinHeapAlloc;
    case WinCompatApiHeapFree: return HzSysWinHeapFree;
    case WinCompatApiCrtMalloc: return HzSysWinHeapAlloc;
    case WinCompatApiCrtFree: return HzSysWinHeapFree;
    }
    return UINT32_MAX;
}

static bool WinCompatFindApi(const char *Name, WINCOMPAT_API *Api)
{
    if (WinCompatEqual(Name, "GetStdHandle"))
        *Api = WinCompatApiGetStdHandle;
    else if (WinCompatEqual(Name, "WriteFile"))
        *Api = WinCompatApiWriteFile;
    else if (WinCompatEqual(Name, "ExitProcess"))
        *Api = WinCompatApiExitProcess;
    else if (WinCompatEqual(Name, "GetLastError"))
        *Api = WinCompatApiGetLastError;
    else if (WinCompatEqual(Name, "SetLastError"))
        *Api = WinCompatApiSetLastError;
    else if (WinCompatEqual(Name, "CloseHandle"))
        *Api = WinCompatApiCloseHandle;
    else if (WinCompatEqual(Name, "GetCurrentProcessId"))
        *Api = WinCompatApiGetCurrentProcessId;
    else if (WinCompatEqual(Name, "GetCurrentThreadId"))
        *Api = WinCompatApiGetCurrentThreadId;
    else if (WinCompatEqual(Name, "GetCurrentProcess"))
        *Api = WinCompatApiGetCurrentProcess;
    else if (WinCompatEqual(Name, "GetCurrentThread"))
        *Api = WinCompatApiGetCurrentThread;
    else if (WinCompatEqual(Name, "QueryPerformanceCounter"))
        *Api = WinCompatApiQueryPerformanceCounter;
    else if (WinCompatEqual(Name, "GetSystemTimeAsFileTime"))
        *Api = WinCompatApiGetSystemTimeAsFileTime;
    else if (WinCompatEqual(Name, "InitializeSListHead"))
        *Api = WinCompatApiInitializeSListHead;
    else if (WinCompatEqual(Name, "GetModuleHandleW"))
        *Api = WinCompatApiGetModuleHandleW;
    else if (WinCompatEqual(Name, "GetModuleHandleA"))
        *Api = WinCompatApiGetModuleHandleA;
    else if (WinCompatEqual(Name, "GetProcAddress"))
        *Api = WinCompatApiGetProcAddress;
    else if (WinCompatEqual(Name, "CreateFileW"))
        *Api = WinCompatApiCreateFileW;
    else if (WinCompatEqual(Name, "GetFileAttributesW"))
        *Api = WinCompatApiGetFileAttributesW;
    else if (WinCompatEqual(Name, "GetCommandLineW"))
        *Api = WinCompatApiGetCommandLineW;
    else if (WinCompatEqual(Name, "GetTickCount"))
        *Api = WinCompatApiGetTickCount;
    else if (WinCompatEqual(Name, "GetTickCount64"))
        *Api = WinCompatApiGetTickCount64;
    else if (WinCompatEqual(Name, "WSAStartup"))
        *Api = WinCompatApiWsaStartup;
    else if (WinCompatEqual(Name, "WSACleanup"))
        *Api = WinCompatApiWsaCleanup;
    else if (WinCompatEqual(Name, "WSAGetLastError"))
        *Api = WinCompatApiWsaGetLastError;
    else if (WinCompatEqual(Name, "WSASetLastError"))
        *Api = WinCompatApiWsaSetLastError;
    else if (WinCompatEqual(Name, "socket"))
        *Api = WinCompatApiSocket;
    else if (WinCompatEqual(Name, "connect"))
        *Api = WinCompatApiConnect;
    else if (WinCompatEqual(Name, "send"))
        *Api = WinCompatApiSend;
    else if (WinCompatEqual(Name, "recv"))
        *Api = WinCompatApiRecv;
    else if (WinCompatEqual(Name, "closesocket"))
        *Api = WinCompatApiCloseSocket;
    else if (WinCompatEqual(Name, "getaddrinfo"))
        *Api = WinCompatApiGetAddrInfo;
    else if (WinCompatEqual(Name, "freeaddrinfo"))
        *Api = WinCompatApiFreeAddrInfo;
    else if (WinCompatEqual(Name, "GetAddrInfoW"))
        *Api = WinCompatApiGetAddrInfoW;
    else if (WinCompatEqual(Name, "FreeAddrInfoW"))
        *Api = WinCompatApiFreeAddrInfoW;
    else if (WinCompatEqual(Name, "GetCommandLineA"))
        *Api = WinCompatApiGetCommandLineA;
    else if (WinCompatEqual(Name, "GetProcessHeap"))
        *Api = WinCompatApiGetProcessHeap;
    else if (WinCompatEqual(Name, "HeapAlloc"))
        *Api = WinCompatApiHeapAlloc;
    else if (WinCompatEqual(Name, "HeapFree"))
        *Api = WinCompatApiHeapFree;
    else if (WinCompatEqual(Name, "memcpy"))
        *Api = WinCompatApiCrtMemcpy;
    else if (WinCompatEqual(Name, "memset"))
        *Api = WinCompatApiCrtMemset;
    else if (WinCompatEqual(Name, "strlen"))
        *Api = WinCompatApiCrtStrlen;
    else if (WinCompatEqual(Name, "malloc"))
        *Api = WinCompatApiCrtMalloc;
    else if (WinCompatEqual(Name, "free"))
        *Api = WinCompatApiCrtFree;
    else
        return false;
    return true;
}

static bool WinCompatApiBelongsToModule(const char *Module, WINCOMPAT_API Api)
{
    bool IsSocketApi = Api >= WinCompatApiWsaStartup && Api <= WinCompatApiFreeAddrInfoW;
    bool IsCrtApi = Api >= WinCompatApiCrtMemcpy && Api <= WinCompatApiCrtFree;
    if (WinCompatEqual(Module, "ws2_32.dll"))
        return IsSocketApi;
    if (WinCompatEqual(Module, "msvcrt.dll") || WinCompatEqual(Module, "ucrtbase.dll"))
        return IsCrtApi;
    if (WinCompatEqual(Module, "kernel32.dll") || WinCompatEqual(Module, "kernelbase.dll"))
        return !IsSocketApi && !IsCrtApi;
    return false;
}

/* Service thunks preserve the Win64 argument layout. BOOL-returning calls convert nonnegative
   native status to TRUE; CloseHandle does not translate failures to Win32 last-error values.
   QueryPerformanceCounter returns uptime microseconds, so its matching frequency is 1 MHz. */
static void WinCompatBuildThunk(uint8_t *Code, uint32_t Service, WINCOMPAT_API Api)
{
    if (Api == WinCompatApiGetLastError || Api == WinCompatApiWsaGetLastError) {
        static const uint8_t Get[] = {0x65, 0x8B, 0x04, 0x25, 0x68, 0x00, 0x00, 0x00, 0xC3};
        memcpy(Code, Get, sizeof(Get)); /* mov eax, dword ptr gs:[0x68]; ret */
        return;
    }
    if (Api == WinCompatApiSetLastError || Api == WinCompatApiWsaSetLastError) {
        static const uint8_t Set[] = {0x65, 0x89, 0x0C, 0x25, 0x68, 0x00, 0x00, 0x00, 0xC3};
        memcpy(Code, Set, sizeof(Set)); /* mov dword ptr gs:[0x68], ecx; ret */
        return;
    }
    if (Api == WinCompatApiGetCurrentProcessId) {
        static const uint8_t Get[] = {0x65, 0x8B, 0x04, 0x25, 0x40, 0x00, 0x00, 0x00, 0xC3};
        memcpy(Code, Get, sizeof(Get)); /* mov eax, dword ptr gs:[0x40]; ret */
        return;
    }
    if (Api == WinCompatApiGetCurrentThreadId) {
        static const uint8_t Get[] = {0x65, 0x8B, 0x04, 0x25, 0x48, 0x00, 0x00, 0x00, 0xC3};
        memcpy(Code, Get, sizeof(Get)); /* mov eax, dword ptr gs:[0x48]; ret */
        return;
    }
    if (Api == WinCompatApiGetCurrentProcess || Api == WinCompatApiGetCurrentThread) {
        static const uint8_t Process[] = {0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3};
        static const uint8_t Thread[] = {0x48, 0xC7, 0xC0, 0xFE, 0xFF, 0xFF, 0xFF, 0xC3};
        memcpy(Code, Api == WinCompatApiGetCurrentProcess ? Process : Thread, sizeof(Process));
        return;
    }
    if (Api == WinCompatApiGetCommandLineA || Api == WinCompatApiGetCommandLineW) {
        static const uint8_t Get[] = {
            0x65, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, /* mov rax,gs:[TEB.Peb] */
            0x48, 0x8D, 0x40, 0x60,                               /* lea rax,[rax+0x60] */
            0xC3,                                                  /* ret */
        };
        if (Api == WinCompatApiGetCommandLineA) {
            memcpy(Code, Get, sizeof(Get));
            uint32_t TebPebOffset = offsetof(HZ_TEB, Peb);
            memcpy(Code + 5, &TebPebOffset, sizeof(TebPebOffset));
        } else {
            static const uint8_t GetWide[] = {
                0x65,0x48,0x8B,0x04,0x25,0,0,0,0, /* rax = TEB.Peb */
                0x48,0x8D,0x80,0,0,0,0,             /* rax = &PEB.CommandLineW */
                0xC3,
            };
            memcpy(Code, GetWide, sizeof(GetWide));
            uint32_t TebPebOffset = offsetof(HZ_TEB, Peb);
            memcpy(Code + 5, &TebPebOffset, sizeof(TebPebOffset));
            uint32_t Offset = offsetof(HZ_PEB, CommandLineW);
            memcpy(Code + 12, &Offset, sizeof(Offset));
        }
        return;
    }
    if (Api == WinCompatApiGetProcessHeap) {
        static const uint8_t Get[] = {0xB8, 0x00, 0x01, 0x00, 0x00, 0xC3};
        memcpy(Code, Get, sizeof(Get)); /* fixed process-local heap token */
        return;
    }
    if (Api == WinCompatApiHeapAlloc) {
        /* HeapAlloc(heap, flags, bytes): reserve shadow space, pass the result slot as arg4. */
        static const uint8_t Allocate[] = {
            0x48, 0x83, 0xEC, 0x38,                   /* sub rsp,38h */
            0x4C, 0x8D, 0x4C, 0x24, 0x20,             /* lea r9,[rsp+20h] */
            0x4C, 0x8B, 0xD1,                         /* mov r10,rcx */
            0xB8, 0, 0, 0, 0,                         /* mov eax,service */
            0x0F, 0x05,                               /* syscall */
            0x85, 0xC0, 0x79, 0x08,                   /* test eax,eax; jns success */
            0x31, 0xC0, 0x48, 0x83, 0xC4, 0x38, 0xC3, /* failure: NULL; restore; ret */
            0x48, 0x8B, 0x44, 0x24, 0x20,             /* success: mov rax,[rsp+20h] */
            0x48, 0x83, 0xC4, 0x38, 0xC3,            /* restore; ret */
        };
        memcpy(Code, Allocate, sizeof(Allocate));
        memcpy(Code + 13, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiInitializeSListHead) {
        static const uint8_t Initialize[] = {
            0x48, 0x31, 0xC0,             /* xor rax,rax */
            0x48, 0x89, 0x01,             /* mov [rcx],rax */
            0x48, 0x89, 0x41, 0x08,       /* mov [rcx+8],rax */
            0xC3,                          /* ret */
        };
        memcpy(Code, Initialize, sizeof(Initialize));
        return;
    }
    if (Api == WinCompatApiCrtMemcpy) {
        static const uint8_t Copy[] = {
            0x48, 0x89, 0xC8,       /* mov rax,rcx (return destination) */
            0x57, 0x56,             /* push rdi; push rsi */
            0x48, 0x89, 0xCF,       /* mov rdi,rcx */
            0x48, 0x89, 0xD6,       /* mov rsi,rdx */
            0x4C, 0x89, 0xC1,       /* mov rcx,r8 */
            0xF3, 0xA4,             /* rep movsb */
            0x5E, 0x5F, 0xC3,       /* pop rsi; pop rdi; ret */
        };
        memcpy(Code, Copy, sizeof(Copy));
        return;
    }
    if (Api == WinCompatApiCrtMemset) {
        static const uint8_t Set[] = {
            0x57, 0x51,             /* push rdi; push rcx (destination) */
            0x48, 0x89, 0xCF,       /* mov rdi,rcx */
            0x88, 0xD0,             /* mov al,dl */
            0x4C, 0x89, 0xC1,       /* mov rcx,r8 */
            0xF3, 0xAA,             /* rep stosb */
            0x58, 0x5F, 0xC3,       /* pop rax; pop rdi; ret */
        };
        memcpy(Code, Set, sizeof(Set));
        return;
    }
    if (Api == WinCompatApiCrtStrlen) {
        static const uint8_t Length[] = {
            0x31, 0xC0,                   /* xor eax,eax */
            0x80, 0x3C, 0x01, 0x00,       /* loop: cmp byte ptr [rcx+rax],0 */
            0x74, 0x05,                   /* je done */
            0x48, 0xFF, 0xC0,             /* inc rax */
            0xEB, 0xF5,                   /* jmp loop */
            0xC3,                          /* ret */
        };
        memcpy(Code, Length, sizeof(Length));
        return;
    }
    if (Api == WinCompatApiCrtMalloc) {
        static const uint8_t Allocate[] = {
            0x48, 0x83, 0xEC, 0x38,             /* reserve shadow space and result slot */
            0x4C, 0x8D, 0x4C, 0x24, 0x20,       /* arg4 = result slot */
            0x49, 0x89, 0xC8,                   /* arg2 = requested size */
            0x31, 0xD2,                         /* flags = 0 */
            0x41, 0xBA, 0x00, 0x01, 0x00, 0x00, /* arg0 = process heap token */
            0xB8, 0, 0, 0, 0,                   /* mov eax,service */
            0x0F, 0x05,                         /* syscall */
            0x85, 0xC0, 0x79, 0x08,             /* success skips null return */
            0x31, 0xC0, 0x48, 0x83, 0xC4, 0x38, 0xC3,
            0x48, 0x8B, 0x44, 0x24, 0x20,
            0x48, 0x83, 0xC4, 0x38, 0xC3,
        };
        memcpy(Code, Allocate, sizeof(Allocate));
        memcpy(Code + 21, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiCrtFree) {
        static const uint8_t Free[] = {
            0x49, 0x89, 0xC8,                   /* arg2 = pointer */
            0x31, 0xD2,                         /* flags = 0 */
            0x41, 0xBA, 0x00, 0x01, 0x00, 0x00, /* arg0 = process heap token */
            0xB8, 0, 0, 0, 0,                   /* mov eax,service */
            0x0F, 0x05, 0xC3,                   /* syscall; ret (void) */
        };
        memcpy(Code, Free, sizeof(Free));
        memcpy(Code + 12, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiGetModuleHandleW || Api == WinCompatApiGetModuleHandleA) {
        static const uint8_t Get[] = {
            0x48, 0x83, 0xEC, 0x38,                   /* sub rsp,38h */
            0x4C, 0x8D, 0x44, 0x24, 0x20,             /* lea r8,[rsp+20h] */
            0xBA, 0, 0, 0, 0,                         /* mov edx,is-wide */
            0x4C, 0x8B, 0xD1,                         /* mov r10,rcx */
            0xB8, 0, 0, 0, 0,                         /* mov eax,service */
            0x0F, 0x05,                               /* syscall */
            0x85, 0xC0, 0x79, 0x13,                   /* test eax,eax; jns success */
            0x65, 0xC7, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00,
            0x7E, 0x00, 0x00, 0x00,       /* mov dword ptr gs:[0x30], ERROR_MOD_NOT_FOUND */
            0x31, 0xC0,                               /* xor eax,eax */
            0x48, 0x83, 0xC4, 0x38, 0xC3,             /* add rsp,38h; ret */
            0x48, 0x8B, 0x44, 0x24, 0x20,             /* success: mov rax,[rsp+20h] */
            0x48, 0x83, 0xC4, 0x38, 0xC3,             /* add rsp,38h; ret */
        };
        memcpy(Code, Get, sizeof(Get));
        uint32_t Wide = Api == WinCompatApiGetModuleHandleW;
        memcpy(Code + 10, &Wide, sizeof(Wide));
        memcpy(Code + 18, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiGetProcAddress) {
        static const uint8_t Get[] = {
            0x48, 0x83, 0xEC, 0x38,                         /* reserve shadow space and result slot */
            0x4C, 0x8D, 0x44, 0x24, 0x20,                   /* arg3 = result slot */
            0x4C, 0x8B, 0xD1,                               /* arg0 = module handle */
            0xB8, 0, 0, 0, 0,                               /* mov eax,service */
            0x0F, 0x05,                                     /* syscall */
            0x85, 0xC0, 0x0F, 0x89, 0x13, 0, 0, 0,         /* success -> return resolved address */
            0x65, 0xC7, 0x04, 0x25, 0x30, 0, 0, 0,
            0x7F, 0, 0, 0,                                  /* ERROR_PROC_NOT_FOUND */
            0x31, 0xC0,                                     /* return NULL */
            0x48, 0x83, 0xC4, 0x38, 0xC3,
            0x48, 0x8B, 0x44, 0x24, 0x20,
            0x48, 0x83, 0xC4, 0x38, 0xC3,
        };
        memcpy(Code, Get, sizeof(Get));
        memcpy(Code + 13, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiCreateFileW) {
        /* CreateFileW(path, access, share, security, disposition, flags, template).
           Share/security/template are ignored; this service returns Nexora's small handle value. */
        static const uint8_t Create[] = {
            0x48,0x83,0xEC,0x38,                   /* reserve shadow space/result slot */
            0x4C,0x8B,0x44,0x24,0x60,             /* r8 = disposition */
            0x4C,0x8B,0x4C,0x24,0x68,             /* r9 = flags */
            0x4C,0x8B,0xD1,                         /* r10 = path */
            0xB8,0,0,0,0,                         /* mov eax,service */
            0x0F,0x05,                             /* syscall */
            0x85,0xC0,0x79,0x18,                   /* test eax,eax; jns success */
            0x65,0xC7,0x04,0x25,0x30,0,0,0,0x57,0,0,0,
            0x48,0xC7,0xC0,0xFF,0xFF,0xFF,0xFF,   /* INVALID_HANDLE_VALUE */
            0x48,0x83,0xC4,0x38,0xC3,
            0x48,0x83,0xC4,0x38,0xC3,
        };
        memcpy(Code, Create, sizeof(Create));
        memcpy(Code + 18, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiGetFileAttributesW) {
        static const uint8_t Get[] = {
            0x4C,0x8B,0xD1,                         /* r10 = path */
            0xB8,0,0,0,0,                           /* mov eax,service */
            0x0F,0x05,
            0x85,0xC0,0x79,0x11,                     /* success jumps to the final ret */
            0x65,0xC7,0x04,0x25,0x30,0,0,0,0x02,0,0,0,
            0xB8,0xFF,0xFF,0xFF,0xFF,               /* INVALID_FILE_ATTRIBUTES */
            0xC3,
        };
        memcpy(Code, Get, sizeof(Get));
        memcpy(Code + 4, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiGetTickCount || Api == WinCompatApiGetTickCount64) {
        /* Query monotonic uptime into a local slot and convert the kernel's microseconds to
           Windows milliseconds. The low DWORD naturally implements GetTickCount wraparound. */
        static const uint8_t Get[] = {
            0x48, 0x83, 0xEC, 0x38,                   /* sub rsp,38h */
            0x48, 0x8D, 0x4C, 0x24, 0x20,             /* lea rcx,[rsp+20h] */
            0x4C, 0x8B, 0xD1,                         /* mov r10,rcx */
            0xB8, 0, 0, 0, 0,                         /* mov eax,service */
            0x0F, 0x05,                               /* syscall */
            0x85, 0xC0, 0x75, 0x14,                   /* test eax,eax; jnz failure */
            0x48, 0x8B, 0x44, 0x24, 0x20,             /* mov rax,[rsp+20h] */
            0x31, 0xD2,                               /* xor edx,edx */
            0xB9, 0xE8, 0x03, 0x00, 0x00,             /* mov ecx,1000 */
            0x48, 0xF7, 0xF1,                         /* div rcx */
            0x48, 0x83, 0xC4, 0x38, 0xC3,             /* add rsp,38h; ret */
            0x31, 0xC0, 0x48, 0x83, 0xC4, 0x38, 0xC3, /* failure: return zero */
        };
        memcpy(Code, Get, sizeof(Get));
        memcpy(Code + 13, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiWsaStartup) {
        static const uint8_t Startup[] = {
            0x48, 0x83, 0xEC, 0x38,                   /* sub rsp,38h */
            0x48, 0xC7, 0x44, 0x24, 0x28, 0, 0, 0, 0, /* zero fifth argument */
            0x4C, 0x8B, 0xC2,                         /* mov r8,rdx (WSADATA) */
            0x89, 0xCA,                               /* mov edx,ecx (version) */
            0x45, 0x31, 0xC9,                         /* xor r9d,r9d */
            0xB9, 1, 0, 0, 0,                         /* mov ecx,WSA_STARTUP */
            0x4C, 0x8B, 0xD1,                         /* mov r10,rcx */
            0xB8, 0, 0, 0, 0,                         /* mov eax,service */
            0x0F, 0x05,                               /* syscall */
            0x85, 0xC0, 0x79, 0x05,                   /* success returns zero */
            0xB8, 0x6C, 0x27, 0, 0,                   /* WSAVERNOTSUPPORTED (10092) */
            0x48, 0x83, 0xC4, 0x38, 0xC3,
        };
        memcpy(Code, Startup, sizeof(Startup));
        memcpy(Code + 30, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiWsaCleanup) {
        static const uint8_t Cleanup[] = {
            0x48, 0x83, 0xEC, 0x38,
            0x48, 0xC7, 0x44, 0x24, 0x28, 0, 0, 0, 0,
            0xB9, 7, 0, 0, 0,                         /* WSACleanup operation */
            0x4C, 0x8B, 0xD1,
            0xB8, 0, 0, 0, 0,
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x38, 0xC3,
        };
        memcpy(Code, Cleanup, sizeof(Cleanup));
        memcpy(Code + 22, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiSocket) {
        static const uint8_t Socket[] = {
            0x48, 0x83, 0xEC, 0x38,
            0x48, 0xC7, 0x44, 0x24, 0x28, 0, 0, 0, 0, /* service fifth arg = 0 */
            0x4D, 0x8B, 0xC8,                         /* mov r9,r8 (protocol) */
            0x4C, 0x8B, 0xC2,                         /* mov r8,rdx (type) */
            0x48, 0x8B, 0xD1,                         /* mov rdx,rcx (domain) */
            0xB9, 2, 0, 0, 0,                         /* socket op */
            0x4C, 0x8B, 0xD1,
            0xB8, 0, 0, 0, 0,
            0x0F, 0x05,
            0x85, 0xC0, 0x79, 0x18,
            0x65, 0xC7, 0x04, 0x25, 0x30, 0, 0, 0, 0x43, 0x27, 0, 0,
            0x48, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,
            0x48, 0x83, 0xC4, 0x38, 0xC3,
            0x48, 0x83, 0xC4, 0x38, 0xC3,
        };
        memcpy(Code, Socket, sizeof(Socket));
        memcpy(Code + 31, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiGetAddrInfo || Api == WinCompatApiGetAddrInfoW) {
        static const uint8_t Resolve[] = {
            0x48, 0x83, 0xEC, 0x38,
            0x4C, 0x89, 0x4C, 0x24, 0x28,             /* result pointer is service arg 5 */
            0x4D, 0x8B, 0xC8,                         /* hints -> r9 */
            0x4C, 0x8B, 0xC2,                         /* service -> r8 */
            0x48, 0x8B, 0xD1,                         /* node -> rdx */
            0xB9, 0, 0, 0, 0,                         /* getaddrinfo operation */
            0x4C, 0x8B, 0xD1,
            0xB8, 0, 0, 0, 0,
            0x0F, 0x05,
            0x85, 0xC0, 0x79, 0x05,
            0xB8, 0xFE, 0xFF, 0xFF, 0xFF,             /* EAI_NONAME */
            0x48, 0x83, 0xC4, 0x38, 0xC3,
        };
        memcpy(Code, Resolve, sizeof(Resolve));
        uint32_t Operation = Api == WinCompatApiGetAddrInfoW ? 10u : 8u;
        memcpy(Code + 19, &Operation, sizeof(Operation));
        memcpy(Code + 27, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiFreeAddrInfo || Api == WinCompatApiFreeAddrInfoW) {
        static const uint8_t Free[] = {
            0x48, 0x83, 0xEC, 0x38,
            0x48, 0xC7, 0x44, 0x24, 0x28, 0, 0, 0, 0,
            0x48, 0x8B, 0xD1,                         /* addrinfo -> rdx */
            0x45, 0x31, 0xC0,
            0x45, 0x31, 0xC9,
            0xB9, 9, 0, 0, 0,                         /* freeaddrinfo operation */
            0x4C, 0x8B, 0xD1,
            0xB8, 0, 0, 0, 0,
            0x0F, 0x05,
            0x48, 0x83, 0xC4, 0x38, 0xC3,
        };
        memcpy(Code, Free, sizeof(Free));
        memcpy(Code + 31, &Service, sizeof(Service));
        return;
    }
    if (Api == WinCompatApiConnect || Api == WinCompatApiSend || Api == WinCompatApiRecv ||
        Api == WinCompatApiCloseSocket) {
        /* All these calls pass a fifth custom operation argument through the user stack. */
        uint32_t Operation = Api == WinCompatApiConnect ? 3u :
                             Api == WinCompatApiSend ? 4u :
                             Api == WinCompatApiRecv ? 5u : 6u;
        if (Api == WinCompatApiSend || Api == WinCompatApiRecv) {
            static const uint8_t Transfer[] = {
                0x48, 0x83, 0xEC, 0x38,
                0x4C, 0x89, 0x4C, 0x24, 0x28,             /* fifth arg = flags */
                0x4D, 0x8B, 0xC8,                         /* mov r9,r8 (length) */
                0x4C, 0x8B, 0xC2,                         /* mov r8,rdx (buffer) */
                0x48, 0x8B, 0xD1,                         /* mov rdx,rcx (socket) */
                0xB9, 0, 0, 0, 0,
                0x4C, 0x8B, 0xD1,
                0xB8, 0, 0, 0, 0,
                0x0F, 0x05,
                0x85, 0xC0, 0x79, 0x16,
                0x65, 0xC7, 0x04, 0x25, 0x30, 0, 0, 0, 0x43, 0x27, 0, 0,
                0xB8, 0xFF, 0xFF, 0xFF, 0xFF,
                0x48, 0x83, 0xC4, 0x38, 0xC3,
                0x48, 0x83, 0xC4, 0x38, 0xC3,
            };
            memcpy(Code, Transfer, sizeof(Transfer));
            memcpy(Code + 19, &Operation, sizeof(Operation));
            memcpy(Code + 27, &Service, sizeof(Service));
            return;
        }
        static const uint8_t Simple[] = {
            0x48, 0x83, 0xEC, 0x38,
            0x48, 0xC7, 0x44, 0x24, 0x28, 0, 0, 0, 0,
            0x4D, 0x8B, 0xC8,                         /* mov r9,r8 (connect length) */
            0x4C, 0x8B, 0xC2,                         /* mov r8,rdx (sockaddr) */
            0x48, 0x8B, 0xD1,                         /* mov rdx,rcx (socket) */
            0xB9, 0, 0, 0, 0,
            0x4C, 0x8B, 0xD1,
            0xB8, 0, 0, 0, 0,
            0x0F, 0x05,
            0x85, 0xC0, 0x79, 0x16,
            0x65, 0xC7, 0x04, 0x25, 0x30, 0, 0, 0, 0x43, 0x27, 0, 0,
            0xB8, 0xFF, 0xFF, 0xFF, 0xFF,
            0x48, 0x83, 0xC4, 0x38, 0xC3,
            0x31, 0xC0,
            0x48, 0x83, 0xC4, 0x38, 0xC3,
        };
        memcpy(Code, Simple, sizeof(Simple));
        if (Api == WinCompatApiCloseSocket) {
            static const uint8_t CloseArgs[] = {
                0x48, 0x8B, 0xD1,             /* socket -> rdx */
                0x45, 0x31, 0xC0,             /* r8 = 0 */
                0x45, 0x31, 0xC9,             /* r9 = 0 */
            };
            memcpy(Code + 13, CloseArgs, sizeof(CloseArgs));
        }
        memcpy(Code + 23, &Operation, sizeof(Operation));
        memcpy(Code + 31, &Service, sizeof(Service));
        return;
    }

    static const uint8_t Prefix[] = {0x4C, 0x8B, 0xD1, 0xB8}; /* mov r10,rcx; mov eax,imm32 */
    static const uint8_t Call[] = {0x0F, 0x05};                /* syscall */
    size_t At = 0;
    memcpy(Code + At, Prefix, sizeof(Prefix)); At += sizeof(Prefix);
    memcpy(Code + At, &Service, sizeof(Service)); At += sizeof(Service);
    memcpy(Code + At, Call, sizeof(Call)); At += sizeof(Call);
    if (Api == WinCompatApiWriteFile || Api == WinCompatApiCloseHandle || Api == WinCompatApiHeapFree ||
        Api == WinCompatApiQueryPerformanceCounter) {
        static const uint8_t ToBool[] = {0x85, 0xC0, 0x0F, 0x99, 0xC0, 0x0F, 0xB6, 0xC0};
        memcpy(Code + At, ToBool, sizeof(ToBool)); At += sizeof(ToBool);
    }
    Code[At] = 0xC3; /* ret */
}

static STATUS WinCompatAllocateThunkPage(EPROCESS *Process, uint64_t *Base)
{
    *Base = 0;
    void *Address = NULL;
    STATUS Status = MmAllocateVirtualMemory(Process->AddressSpace, &Address, WINCOMPAT_THUNK_PAGE_SIZE,
                                             MEM_RESERVE | MEM_COMMIT, MM_PROT_READWRITE);
    if (HZ_SUCCESS(Status))
        *Base = (uint64_t)Address;
    return Status;
}

STATUS PspResolveWindowsImports(EPROCESS *Process, uint8_t *Image, uint64_t ImageSize,
                               const PE_HEADERS *Headers, uint64_t *ThunkBase,
                               uint8_t ThunkPage[WINCOMPAT_THUNK_PAGE_SIZE])
{
    if (!Process || !Image || !Headers || !Headers->File || !Headers->Optional || !ThunkBase || !ThunkPage ||
        ImageSize < Headers->Optional->SizeOfImage)
        return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                      "resolver arguments or image bounds", 0, 0, NULL, NULL);
    if (Headers->File->Machine != PE_MACHINE_AMD64 ||
        Headers->Optional->Magic != PE_OPTIONAL_MAGIC_PE32PLUS)
        return WinCompatImportFailure(Process, Headers, STATUS_NOT_SUPPORTED,
                                      "unsupported image architecture", 0, 0, NULL, NULL);
    *ThunkBase = 0;
    memset(ThunkPage, 0, WINCOMPAT_THUNK_PAGE_SIZE);
    const PE_DATA_DIRECTORY *NormalDirectory = PeDirectory(Headers, PE_DIRECTORY_IMPORT);
    const PE_DATA_DIRECTORY *DelayDirectory = PeDirectory(Headers, WINCOMPAT_DIRECTORY_DELAY_IMPORT);
    if (!NormalDirectory->Size && !DelayDirectory->Size)
        return STATUS_SUCCESS;

    WINCOMPAT_API Used[WINCOMPAT_THUNK_COUNT];
    uint32_t UsedCount = 0, ModuleCount = 0, SymbolCount = 0;
    for (uint32_t Pass = 0; Pass < 2; Pass++) {
        bool IsDelay = Pass != 0;
        const PE_DATA_DIRECTORY *Directory = IsDelay ? DelayDirectory : NormalDirectory;
        if (!Directory->Size)
            continue;
        uint32_t DescriptorSize = IsDelay ? sizeof(PE_DELAY_IMPORT_DESCRIPTOR) : sizeof(PE_IMPORT_DESCRIPTOR);
        if (Directory->Size < DescriptorSize || Directory->Size % DescriptorSize != 0 ||
            Directory->Size > (WINCOMPAT_IMPORT_MODULES + 1u) * DescriptorSize ||
            (uint64_t)Directory->VirtualAddress + Directory->Size > ImageSize)
            return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                          IsDelay ? "delay-import directory bounds or descriptor limit" :
                                                    "import directory bounds or descriptor limit",
                                          0, 0, NULL, NULL);
        bool Terminated = false;
        uint64_t End = (uint64_t)Directory->VirtualAddress + Directory->Size;
        uint32_t DescriptorIndex = 0;
        for (uint64_t Offset = Directory->VirtualAddress; Offset < End;
             Offset += DescriptorSize, ++DescriptorIndex) {
            PE_IMPORT_DESCRIPTOR D = {0};
            if (!IsDelay) {
                memcpy(&D, Image + Offset, sizeof(D));
                if (!D.OriginalFirstThunk && !D.TimeDateStamp && !D.ForwarderChain && !D.Name && !D.FirstThunk)
                    Terminated = true;
            } else {
                PE_DELAY_IMPORT_DESCRIPTOR Delay;
                memcpy(&Delay, Image + Offset, sizeof(Delay));
                if (!(Delay.Attributes | Delay.Name | Delay.ModuleHandle | Delay.ImportAddressTable |
                      Delay.ImportNameTable | Delay.BoundImportAddressTable | Delay.UnloadInformationTable |
                      Delay.TimeDateStamp))
                    Terminated = true;
                else {
                    if (!(Delay.Attributes & 1u) || (Delay.Attributes & ~1u))
                        return WinCompatImportFailure(Process, Headers, STATUS_NOT_SUPPORTED,
                                                      "unsupported delay-import descriptor attributes",
                                                      DescriptorIndex, 0, NULL, NULL);
                    D.OriginalFirstThunk = Delay.ImportNameTable;
                    D.TimeDateStamp = Delay.TimeDateStamp;
                    D.Name = Delay.Name;
                    D.FirstThunk = Delay.ImportAddressTable;
                }
            }
            if (Terminated) {
                for (uint64_t Tail = Offset + DescriptorSize; Tail < End; ++Tail) {
                    if (Image[Tail] != 0)
                        return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                                      "nonzero data after import descriptor terminator",
                                                      DescriptorIndex, 0, NULL, NULL);
                }
                break;
            }
            if (ModuleCount >= WINCOMPAT_IMPORT_MODULES)
                return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                              "import descriptor limit", DescriptorIndex, 0, NULL, NULL);
        ++ModuleCount;
        if (!D.Name || !D.FirstThunk)
            return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                          "import descriptor fields", DescriptorIndex, 0, NULL, NULL);
        char Module[WINCOMPAT_NAME_CAPACITY] = {0};
        if (!WinCompatReadString(Image, ImageSize, D.Name, Module, sizeof(Module)))
            return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                          "import module name", DescriptorIndex, 0, NULL, "<malformed-name>");
        bool SupportedModule = WinCompatEqual(Module, "kernel32.dll") ||
                               WinCompatEqual(Module, "kernelbase.dll") ||
                               WinCompatEqual(Module, "ws2_32.dll") ||
                               WinCompatEqual(Module, "msvcrt.dll") ||
                               WinCompatEqual(Module, "ucrtbase.dll");

        uint64_t LookupRva = D.OriginalFirstThunk ? D.OriginalFirstThunk : D.FirstThunk;
        bool ThunkTerminated = false;
        for (uint32_t I = 0; I < WINCOMPAT_IMPORTS_PER_MODULE; ++I) {
            uint64_t Slot = LookupRva + (uint64_t)I * sizeof(uint64_t);
            uint64_t IatSlot = (uint64_t)D.FirstThunk + (uint64_t)I * sizeof(uint64_t);
            if (Slot + sizeof(uint64_t) > ImageSize || IatSlot + sizeof(uint64_t) > ImageSize)
                return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                              "import lookup or IAT table bounds",
                                              DescriptorIndex, I, Module, NULL);
            uint64_t Thunk;
            memcpy(&Thunk, Image + Slot, sizeof(Thunk));
            if (!Thunk) {
                ThunkTerminated = true;
                break;
            }
            ++SymbolCount;
            if (Thunk & (1ull << 63)) {
                STATUS Status = SupportedModule ? STATUS_NOT_SUPPORTED : STATUS_DLL_NOT_FOUND;
                char OrdinalApi[16];
                RtlFormatString(OrdinalApi, sizeof(OrdinalApi), "#%u", (uint32_t)(Thunk & 0xFFFF));
                return WinCompatImportFailure(Process, Headers, Status,
                                              SupportedModule ? "ordinal import" : "unsupported DLL ordinal import",
                                              DescriptorIndex, I, Module, OrdinalApi);
            }
            if (Thunk > UINT32_MAX - sizeof(uint16_t) || Thunk + sizeof(uint16_t) >= ImageSize)
                return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                              "import-by-name hint bounds", DescriptorIndex, I, Module,
                                              "<malformed-name>");
            char Name[WINCOMPAT_NAME_CAPACITY] = {0};
            if (!WinCompatReadString(Image, ImageSize, (uint32_t)(Thunk + sizeof(uint16_t)),
                                     Name, sizeof(Name)))
                return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                              "import API name", DescriptorIndex, I, Module,
                                              "<malformed-name>");
            if (!SupportedModule)
                return WinCompatImportFailure(Process, Headers, STATUS_DLL_NOT_FOUND,
                                              "unsupported import DLL", DescriptorIndex, I, Module, Name);
            WINCOMPAT_API Api;
            if (!WinCompatFindApi(Name, &Api)) {
                return WinCompatImportFailure(Process, Headers, STATUS_NOT_SUPPORTED,
                                              "unsupported import API", DescriptorIndex, I, Module, Name);
            }
            if (!WinCompatApiBelongsToModule(Module, Api))
                return WinCompatImportFailure(Process, Headers, STATUS_NOT_SUPPORTED,
                                              "API is not exported by this compatibility module",
                                              DescriptorIndex, I, Module, Name);

            uint32_t ApiIndex = 0;
            while (ApiIndex < UsedCount && Used[ApiIndex] != Api)
                ++ApiIndex;
            if (ApiIndex == UsedCount) {
                if (UsedCount == WINCOMPAT_THUNK_COUNT)
                    return WinCompatImportFailure(Process, Headers, STATUS_NOT_SUPPORTED,
                                                  "thunk capacity", DescriptorIndex, I, Module, Name);
                Used[UsedCount++] = Api;
                WinCompatBuildThunk(ThunkPage + ApiIndex * WINCOMPAT_THUNK_STRIDE,
                                    WinCompatService(Api), Api);
            }
            if (!*ThunkBase) {
                STATUS Status = WinCompatAllocateThunkPage(Process, ThunkBase);
                if (!HZ_SUCCESS(Status))
                    return WinCompatImportFailure(Process, Headers, Status, "thunk page allocation",
                                                  DescriptorIndex, I, Module, Name);
            }
            uint64_t Address = *ThunkBase + (uint64_t)ApiIndex * WINCOMPAT_THUNK_STRIDE;
            memcpy(Image + IatSlot, &Address, sizeof(Address));
        }
        if (!ThunkTerminated)
            return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                          "unterminated import thunk table", DescriptorIndex,
                                          WINCOMPAT_IMPORTS_PER_MODULE, Module, NULL);
        if (!SupportedModule)
            return WinCompatImportFailure(Process, Headers, STATUS_DLL_NOT_FOUND,
                                          "unsupported import DLL with no symbols",
                                          DescriptorIndex, 0, Module, NULL);
        }
        if (!Terminated)
            return WinCompatImportFailure(Process, Headers, STATUS_INVALID_IMAGE_FORMAT,
                                          IsDelay ? "missing delay-import descriptor terminator" :
                                                    "missing import descriptor terminator",
                                          DescriptorIndex, 0, NULL, NULL);
    }
    if (UsedCount)
        DbgPrintEx(DPFLTR_INFO_LEVEL, "WinCompat: patched %u import reference(s) in %u descriptor(s)\n",
                   SymbolCount, ModuleCount);
    return STATUS_SUCCESS;
}

/* A small page-backed process heap for applications importing the basic Win32 heap APIs.
   It intentionally implements only whole-allocation HeapAlloc/HeapFree; it is not the Windows
   heap allocator, and pointers must be returned to HeapFree unchanged. */
STATUS NtWinHeapAlloc(const uint64_t *Args)
{
    EPROCESS *Process = PsGetCurrentProcess();
    uint32_t Flags = (uint32_t)Args[1];
    uint64_t Requested = Args[2];
    if (!Process || Process->IsSystem || Args[0] != WINCOMPAT_HEAP_HANDLE || !Args[3] || !Requested ||
        Requested > WINCOMPAT_HEAP_MAX_ALLOCATION ||
        (Flags & ~(WINCOMPAT_HEAP_NO_SERIALIZE | WINCOMPAT_HEAP_ZERO_MEMORY)))
        return STATUS_INVALID_PARAMETER;

    WINCOMPAT_HEAP_ALLOCATION *Record =
        ExAllocatePoolZero(NonPagedPool, sizeof(*Record), WINCOMPAT_HEAP_TAG);
    if (!Record)
        return STATUS_NO_MEMORY;
    uint64_t Size = HZ_ALIGN_UP(Requested, PAGE_SIZE);
    void *Base = NULL;
    STATUS Status = MmAllocateVirtualMemory(Process->AddressSpace, &Base, Size, MEM_RESERVE | MEM_COMMIT,
                                             MM_PROT_READWRITE);
    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Record, WINCOMPAT_HEAP_TAG);
        return Status;
    }

    Record->Base = Base;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    InsertTailList(&Process->WinCompatHeapAllocations, &Record->Link);
    KeReleaseSpinLock(&PspLock, OldIrql);
    Status = MmCopyToUser((void *)Args[3], &Base, sizeof(Base));
    if (!HZ_SUCCESS(Status)) {
        KeAcquireSpinLock(&PspLock, &OldIrql);
        RemoveEntryList(&Record->Link);
        KeReleaseSpinLock(&PspLock, OldIrql);
        MmFreeVirtualMemory(Process->AddressSpace, Base);
        ExFreePoolWithTag(Record, WINCOMPAT_HEAP_TAG);
    }
    return Status;
}

/* HeapFree(heap, flags, pointer): only allocations made by this compatibility heap can be freed. */
STATUS NtWinHeapFree(const uint64_t *Args)
{
    EPROCESS *Process = PsGetCurrentProcess();
    uint32_t Flags = (uint32_t)Args[1];
    if (!Process || Process->IsSystem || Args[0] != WINCOMPAT_HEAP_HANDLE || !Args[2] ||
        (Flags & ~WINCOMPAT_HEAP_NO_SERIALIZE))
        return STATUS_INVALID_PARAMETER;

    WINCOMPAT_HEAP_ALLOCATION *Record = NULL;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    for (LIST_ENTRY *Entry = Process->WinCompatHeapAllocations.Flink;
         Entry != &Process->WinCompatHeapAllocations; Entry = Entry->Flink) {
        WINCOMPAT_HEAP_ALLOCATION *Candidate = CONTAINING_RECORD(Entry, WINCOMPAT_HEAP_ALLOCATION, Link);
        if ((uint64_t)Candidate->Base == Args[2]) {
            RemoveEntryList(&Candidate->Link);
            Record = Candidate;
            break;
        }
    }
    KeReleaseSpinLock(&PspLock, OldIrql);
    if (!Record)
        return STATUS_INVALID_PARAMETER;

    STATUS Status = MmFreeVirtualMemory(Process->AddressSpace, Record->Base);
    ExFreePoolWithTag(Record, WINCOMPAT_HEAP_TAG);
    return Status;
}

void PspFreeWindowsHeapAllocations(EPROCESS *Process)
{
    for (;;) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&PspLock, &OldIrql);
        if (IsListEmpty(&Process->WinCompatHeapAllocations)) {
            KeReleaseSpinLock(&PspLock, OldIrql);
            return;
        }
        LIST_ENTRY *Entry = RemoveHeadList(&Process->WinCompatHeapAllocations);
        KeReleaseSpinLock(&PspLock, OldIrql);
        WINCOMPAT_HEAP_ALLOCATION *Record = CONTAINING_RECORD(Entry, WINCOMPAT_HEAP_ALLOCATION, Link);
        ExFreePoolWithTag(Record, WINCOMPAT_HEAP_TAG);
    }
}
