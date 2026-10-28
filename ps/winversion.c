/*
 * Nexora OS - ps/winversion.c
 * VERSION.DLL compatibility backed by actual RT_VERSION resources in PE files.
 */
#include "pe.h"
#include "psp.h"
#include "io.h"

#define WINVER_MAX_FILE (256ull * 1024ull * 1024ull)
#define WINVER_MAX_RESOURCE (16u * 1024u * 1024u)
#define WINVER_TAG HZ_POOL_TAG('W', 'V', 'e', 'r')

typedef struct HZ_PACKED _WINVER_RESOURCE_DIRECTORY {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t NamedEntries;
    uint16_t IdEntries;
} WINVER_RESOURCE_DIRECTORY;

typedef struct HZ_PACKED _WINVER_RESOURCE_ENTRY {
    uint32_t Name;
    uint32_t Offset;
} WINVER_RESOURCE_ENTRY;

typedef struct HZ_PACKED _WINVER_RESOURCE_DATA_ENTRY {
    uint32_t DataRva;
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
} WINVER_RESOURCE_DATA_ENTRY;

typedef struct HZ_PACKED _WINVER_BLOCK_HEADER {
    uint16_t Length;
    uint16_t ValueLength;
    uint16_t Type;
} WINVER_BLOCK_HEADER;

static bool WinVerRange(uint64_t Size, uint64_t Offset, uint64_t Length)
{
    return Offset <= Size && Length <= Size - Offset;
}

static bool WinVerRvaToOffset(const PE_HEADERS *Headers, uint64_t FileSize, uint32_t Rva,
                              uint32_t Length, uint64_t *Offset)
{
    if (!WinVerRange(Headers->Optional->SizeOfImage, Rva, Length))
        return false;
    if (Rva < Headers->Optional->SizeOfHeaders) {
        if (!WinVerRange(FileSize, Rva, Length))
            return false;
        *Offset = Rva;
        return true;
    }
    for (uint32_t I = 0; I < Headers->File->NumberOfSections; ++I) {
        const PE_SECTION_HEADER *S = &Headers->Sections[I];
        if (Rva < S->VirtualAddress || !WinVerRange(S->SizeOfRawData, Rva - S->VirtualAddress, Length))
            continue;
        uint64_t FileOffset = (uint64_t)S->PointerToRawData + Rva - S->VirtualAddress;
        if (!WinVerRange(FileSize, FileOffset, Length))
            return false;
        *Offset = FileOffset;
        return true;
    }
    return false;
}

/* Locate the first language variant of RT_VERSION in a PE's resource directory. */
static STATUS WinVerFindResource(const uint8_t *File, uint64_t FileSize,
                                 const PE_HEADERS *Headers, const uint8_t **Data, uint32_t *DataSize)
{
    const PE_DATA_DIRECTORY *Dir = PeDirectory(Headers, 2);
    if (!Dir->VirtualAddress || Dir->Size < sizeof(WINVER_RESOURCE_DIRECTORY) ||
        !WinVerRange(Headers->Optional->SizeOfImage, Dir->VirtualAddress, Dir->Size))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    uint64_t RootFile;
    if (!WinVerRvaToOffset(Headers, FileSize, Dir->VirtualAddress, Dir->Size, &RootFile))
        return STATUS_INVALID_IMAGE_FORMAT;

    const uint8_t *Root = File + RootFile;
    uint32_t RootSize = HZ_MIN(Dir->Size, WINVER_MAX_RESOURCE);
    uint32_t DirectoryOffset = 0;
    for (uint32_t Level = 0; Level < 3; ++Level) {
        if (!WinVerRange(RootSize, DirectoryOffset, sizeof(WINVER_RESOURCE_DIRECTORY)))
            return STATUS_INVALID_IMAGE_FORMAT;
        WINVER_RESOURCE_DIRECTORY Directory;
        memcpy(&Directory, Root + DirectoryOffset, sizeof(Directory));
        uint32_t Count = (uint32_t)Directory.NamedEntries + Directory.IdEntries;
        uint64_t EntriesAt = (uint64_t)DirectoryOffset + sizeof(Directory);
        if (!WinVerRange(RootSize, EntriesAt, (uint64_t)Count * sizeof(WINVER_RESOURCE_ENTRY)))
            return STATUS_INVALID_IMAGE_FORMAT;

        bool Found = false;
        for (uint32_t I = 0; I < Count; ++I) {
            WINVER_RESOURCE_ENTRY Entry;
            memcpy(&Entry, Root + EntriesAt + (uint64_t)I * sizeof(Entry), sizeof(Entry));
            if (Level == 0 && ((Entry.Name & 0x80000000u) || (Entry.Name & 0xFFFFu) != 16u))
                continue;
            if (Level == 0 || Level == 1) {
                if (!(Entry.Offset & 0x80000000u))
                    continue;
                DirectoryOffset = Entry.Offset & 0x7FFFFFFFu;
                Found = true;
                break;
            }
            if (Entry.Offset & 0x80000000u)
                continue;
            uint32_t DataEntryOffset = Entry.Offset & 0x7FFFFFFFu;
            if (!WinVerRange(RootSize, DataEntryOffset, sizeof(WINVER_RESOURCE_DATA_ENTRY)))
                return STATUS_INVALID_IMAGE_FORMAT;
            WINVER_RESOURCE_DATA_ENTRY Resource;
            memcpy(&Resource, Root + DataEntryOffset, sizeof(Resource));
            uint64_t ResourceFile;
            if (!Resource.Size || Resource.Size > WINVER_MAX_RESOURCE ||
                !WinVerRvaToOffset(Headers, FileSize, Resource.DataRva, Resource.Size, &ResourceFile))
                return STATUS_INVALID_IMAGE_FORMAT;
            *Data = File + ResourceFile;
            *DataSize = Resource.Size;
            return STATUS_SUCCESS;
        }
        if (!Found)
            return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static STATUS WinVerReadPath(const void *UserPath, char Path[272])
{
    if (!UserPath)
        return STATUS_INVALID_PARAMETER;
    char Input[260] = {0};
    size_t I = 0;
    for (; I < sizeof(Input) - 1; ++I) {
        uint16_t C;
        STATUS Status = MmCopyFromUser(&C, (const uint16_t *)UserPath + I, sizeof(C));
        if (!HZ_SUCCESS(Status))
            return Status;
        if (!C)
            break;
        if (C > 0x7F)
            return STATUS_NOT_SUPPORTED;
        Input[I] = (char)C;
    }
    if (!I || I == sizeof(Input) - 1)
        return STATUS_NAME_TOO_LONG;
    if (Input[0] == '\\') {
        RtlStringCopy(Path, 272, Input);
        return STATUS_SUCCESS;
    }
    if (!(((Input[0] >= 'A' && Input[0] <= 'Z') || (Input[0] >= 'a' && Input[0] <= 'z')) && Input[1] == ':'))
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    char Drive = Input[0] >= 'a' && Input[0] <= 'z' ? Input[0] - ('a' - 'A') : Input[0];
    RtlFormatString(Path, 272, "\\??\\%c%s", Drive, Input + 2);
    return STATUS_SUCCESS;
}

static STATUS WinVerLoadResource(const void *UserPath, void **FileOut, uint64_t *FileSizeOut,
                                 const uint8_t **Resource, uint32_t *ResourceSize)
{
    char Path[272];
    STATUS Status = WinVerReadPath(UserPath, Path);
    if (!HZ_SUCCESS(Status))
        return Status;
    void *File = NULL;
    uint64_t FileSize = 0;
    Status = IoReadWholeFile(Path, &File, &FileSize, WINVER_MAX_FILE);
    if (!HZ_SUCCESS(Status))
        return Status;
    PE_HEADERS Headers;
    Status = PspLocatePeHeaders(File, (size_t)FileSize, &Headers);
    if (HZ_SUCCESS(Status))
        Status = WinVerFindResource(File, FileSize, &Headers, Resource, ResourceSize);
    if (!HZ_SUCCESS(Status)) {
        ExFreePool(File);
        return Status;
    }
    *FileOut = File;
    *FileSizeOut = FileSize;
    return STATUS_SUCCESS;
}

STATUS NtWinGetFileVersionInfoSizeW(const uint64_t *Args)
{
    void *File;
    uint64_t FileSize;
    const uint8_t *Resource;
    uint32_t ResourceSize;
    if (!HZ_SUCCESS(WinVerLoadResource((const void *)Args[0], &File, &FileSize, &Resource, &ResourceSize)))
        return 0; /* Win32 contract: zero means no version resource or unreadable file. */
    (void)FileSize;
    (void)Resource;
    ExFreePool(File);
    if (Args[1]) {
        uint32_t Handle = 0;
        if (!HZ_SUCCESS(MmCopyToUser((void *)Args[1], &Handle, sizeof(Handle))) || ResourceSize > INT32_MAX)
            return 0;
    }
    return (STATUS)ResourceSize;
}

STATUS NtWinGetFileVersionInfoW(const uint64_t *Args)
{
    void *File;
    uint64_t FileSize;
    const uint8_t *Resource;
    uint32_t ResourceSize;
    STATUS Status = WinVerLoadResource((const void *)Args[0], &File, &FileSize, &Resource, &ResourceSize);
    (void)FileSize;
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!Args[3] || Args[2] < ResourceSize)
        Status = STATUS_BUFFER_TOO_SMALL;
    else
        Status = MmCopyToUser((void *)Args[3], Resource, ResourceSize);
    ExFreePool(File);
    return Status;
}

static uint16_t WinVerReadU16(const uint8_t *Data, uint32_t Offset)
{
    uint16_t Value;
    memcpy(&Value, Data + Offset, sizeof(Value));
    return Value;
}

typedef struct _WINVER_BLOCK {
    uint32_t Start;
    uint32_t End;
    uint32_t Value;
    uint32_t ValueBytes;
    uint16_t Type;
} WINVER_BLOCK;

static bool WinVerParseBlock(const uint8_t *Data, uint32_t Total, uint32_t Start, WINVER_BLOCK *Block)
{
    if (!WinVerRange(Total, Start, sizeof(WINVER_BLOCK_HEADER)))
        return false;
    uint16_t Length = WinVerReadU16(Data, Start);
    uint16_t ValueLength = WinVerReadU16(Data, Start + 2);
    uint16_t Type = WinVerReadU16(Data, Start + 4);
    if (Length < sizeof(WINVER_BLOCK_HEADER) || !WinVerRange(Total, Start, Length))
        return false;
    uint32_t End = Start + Length;
    uint32_t Key = Start + sizeof(WINVER_BLOCK_HEADER);
    uint32_t P = Key;
    bool Terminated = false;
    while (WinVerRange(End, P, sizeof(uint16_t))) {
        uint16_t C = WinVerReadU16(Data, P);
        P += sizeof(uint16_t);
        if (!C) { Terminated = true; break; }
    }
    if (!Terminated)
        return false;
    uint32_t Value = (P + 3u) & ~3u;
    uint32_t ValueBytes = Type == 1 ? (uint32_t)ValueLength * 2u : ValueLength;
    if (!WinVerRange(End, Value, ValueBytes))
        return false;
    Block->Start = Start;
    Block->End = End;
    Block->Value = Value;
    Block->ValueBytes = ValueBytes;
    Block->Type = Type;
    return true;
}

static bool WinVerKeyEquals(const uint8_t *Data, uint32_t BlockStart, const char *Key)
{
    uint16_t Length = WinVerReadU16(Data, BlockStart);
    uint32_t P = BlockStart + sizeof(WINVER_BLOCK_HEADER);
    size_t I = 0;
    for (;;) {
        if (!WinVerRange(Length, sizeof(WINVER_BLOCK_HEADER) + I * sizeof(uint16_t), sizeof(uint16_t)))
            return false;
        uint16_t C = WinVerReadU16(Data, P + (uint32_t)(I * 2));
        if (!Key[I])
            return C == 0;
        if (!C || Key[I] != (char)(C >= 'A' && C <= 'Z' ? C + ('a' - 'A') : C)) {
            char K = Key[I] >= 'A' && Key[I] <= 'Z' ? (char)(Key[I] + ('a' - 'A')) : Key[I];
            char Ch = C >= 'A' && C <= 'Z' ? (char)(C + ('a' - 'A')) : (char)C;
            if (K != Ch)
                return false;
        }
        ++I;
    }
}

static bool WinVerFindChild(const uint8_t *Data, uint32_t Total, const WINVER_BLOCK *Parent,
                            const char *Key, WINVER_BLOCK *Child)
{
    uint32_t Offset = (Parent->Value + Parent->ValueBytes + 3u) & ~3u;
    while (WinVerRange(Parent->End, Offset, sizeof(WINVER_BLOCK_HEADER))) {
        WINVER_BLOCK Candidate;
        if (!WinVerParseBlock(Data, Total, Offset, &Candidate))
            return false;
        if (WinVerKeyEquals(Data, Offset, Key)) {
            *Child = Candidate;
            return true;
        }
        uint32_t Next = (Candidate.End + 3u) & ~3u;
        if (Next <= Offset)
            return false;
        Offset = Next;
    }
    return false;
}

STATUS NtWinVerQueryValueW(const uint64_t *Args)
{
    if (!Args[0] || !Args[1] || !Args[2] || !Args[3])
        return STATUS_INVALID_PARAMETER;
    uint16_t Query[260] = {0};
    uint32_t QueryLength = 0;
    for (; QueryLength < 259u; ++QueryLength) {
        STATUS Status = MmCopyFromUser(&Query[QueryLength], (const uint16_t *)Args[1] + QueryLength,
                                       sizeof(uint16_t));
        if (!HZ_SUCCESS(Status))
            return Status;
        if (!Query[QueryLength])
            break;
    }
    if (QueryLength == 259u)
        return STATUS_NAME_TOO_LONG;
    WINVER_BLOCK_HEADER RootHeader;
    STATUS Status = MmCopyFromUser(&RootHeader, (const void *)Args[0], sizeof(RootHeader));
    uint16_t RootLength = HZ_SUCCESS(Status) ? RootHeader.Length : 0;
    if (!HZ_SUCCESS(Status) || RootLength < sizeof(WINVER_BLOCK_HEADER)) {
        return HZ_SUCCESS(Status) ? STATUS_INVALID_IMAGE_FORMAT : Status;
    }
    uint8_t *Data = ExAllocatePoolWithTag(NonPagedPool, RootLength, WINVER_TAG);
    if (!Data)
        return STATUS_NO_MEMORY;
    Status = MmCopyFromUser(Data, (const void *)Args[0], RootLength);
    WINVER_BLOCK Root, Found = {0};
    if (!HZ_SUCCESS(Status) || !WinVerParseBlock((uint8_t *)Data, RootLength, 0, &Root) ||
        !WinVerKeyEquals((uint8_t *)Data, 0, "VS_VERSION_INFO")) {
        ExFreePoolWithTag(Data, WINVER_TAG);
        return HZ_SUCCESS(Status) ? STATUS_INVALID_IMAGE_FORMAT : Status;
    }
    uint32_t ResultOffset = Root.Value;
    uint32_t ResultBytes = Root.ValueBytes;
    uint16_t ResultType = Root.Type;
    if (Query[0]) {
        uint32_t Position = 0;
        while (Position < QueryLength && Query[Position] == '\\')
            ++Position;
        char Key[260];
        bool Any = false;
        while (Position < QueryLength) {
            size_t N = 0;
            while (Position < QueryLength && Query[Position] != '\\' && N + 1 < sizeof(Key))
                Key[N++] = Query[Position++] < 0x80 ? (char)Query[Position - 1] : 0;
            Key[N] = 0;
            if (!N || (Position < QueryLength && Query[Position] != '\\')) {
                Status = STATUS_OBJECT_NAME_INVALID;
                break;
            }
            while (Position < QueryLength && Query[Position] == '\\')
                ++Position;
            if (!WinVerFindChild((uint8_t *)Data, RootLength, &Root, Key, &Found)) {
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                break;
            }
            Root = Found;
            Any = true;
        }
        if (HZ_SUCCESS(Status) && Any) {
            ResultOffset = Found.Value;
            ResultBytes = Found.ValueBytes;
            ResultType = Found.Type;
        }
    }
    if (HZ_SUCCESS(Status)) {
        uint64_t UserValue = Args[0] + ResultOffset;
        uint32_t Length = ResultType == 1 ? ResultBytes / 2u : ResultBytes;
        Status = MmCopyToUser((void *)Args[2], &UserValue, sizeof(UserValue));
        if (HZ_SUCCESS(Status))
            Status = MmCopyToUser((void *)Args[3], &Length, sizeof(Length));
    }
    ExFreePoolWithTag(Data, WINVER_TAG);
    return Status;
}
