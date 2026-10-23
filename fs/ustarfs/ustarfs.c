/*
 * Nexora OS - fs/ustarfs/ustarfs.c
 * Read-only file system for USTAR archives (the boot ramdisk, the live session's C:).
 *
 * Mount reads every header through the storage device and builds an in-memory index (path,
 * data offset, size, directory flag). Paths are matched case-insensitively with '\' or '/'.
 * Directories that only appear as path prefixes are synthesized. Anything that would modify the
 * volume fails with STATUS_MEDIA_WRITE_PROTECTED.
 */
#include "ex.h"
#include "io.h"
#include "rtl.h"

#define USTAR_TAG HZ_POOL_TAG('U', 's', 't', 'r')
#define USTAR_BLOCK 512
#define USTAR_MAX_ENTRIES 1024
#define USTAR_READ_CHUNK (64u * 1024)

typedef struct _USTAR_ENTRY {
    char Path[256];   /* '\'-separated, no leading separator; "" = root */
    uint64_t Offset;  /* byte offset of the data on the device */
    uint64_t Size;
    uint64_t Time;    /* HZ time */
    bool IsDirectory;
} USTAR_ENTRY;

typedef struct _USTAR_VOLUME {
    DEVICE_OBJECT *Storage;
    USTAR_ENTRY *Entries;
    uint32_t Count;
    USTAR_ENTRY Root;
} USTAR_VOLUME;

typedef struct _USTAR_CCB {
    uint32_t Index; /* directory enumeration cursor */
} USTAR_CCB;

static DRIVER_OBJECT *UstarDriver;

static uint64_t UstarOctal(const char *Field, size_t Length)
{
    uint64_t Value = 0;
    for (size_t i = 0; i < Length && Field[i]; i++) {
        if (Field[i] == ' ')
            continue;
        if (Field[i] < '0' || Field[i] > '7')
            break;
        Value = Value * 8 + (uint64_t)(Field[i] - '0');
    }
    return Value;
}

static void UstarNormalize(char *Path)
{
    char *Out = Path;
    const char *In = Path;
    while (*In == '.' && (In[1] == '/' || In[1] == '\\'))
        In += 2;
    while (*In == '/' || *In == '\\')
        In++;
    for (; *In; In++)
        *Out++ = *In == '/' ? '\\' : *In;
    *Out = '\0';
    while (Out > Path && Out[-1] == '\\')
        *--Out = '\0';
}

/* Length of the parent part of Path ("a\b\c" -> 3, "a" -> 0). */
static size_t UstarParentLength(const char *Path)
{
    size_t Length = strlen(Path);
    while (Length > 0 && Path[Length - 1] != '\\')
        Length--;
    return Length ? Length - 1 : 0;
}

static USTAR_ENTRY *UstarLookup(USTAR_VOLUME *Vol, const char *Path, USTAR_ENTRY *Synthetic)
{
    if (!Path[0])
        return &Vol->Root;
    size_t Length = strlen(Path);
    for (uint32_t i = 0; i < Vol->Count; i++) {
        if (_stricmp(Vol->Entries[i].Path, Path) == 0)
            return &Vol->Entries[i];
    }
    for (uint32_t i = 0; i < Vol->Count; i++) {
        const char *Other = Vol->Entries[i].Path;
        if (_strnicmp(Other, Path, Length) == 0 && Other[Length] == '\\') {
            memset(Synthetic, 0, sizeof(*Synthetic));
            memcpy(Synthetic->Path, Other, Length);
            Synthetic->IsDirectory = true;
            return Synthetic; /* an implied directory */
        }
    }
    return NULL;
}

/* ---- Mount -------------------------------------------------------------------------------------- */

static STATUS UstarMount(DEVICE_OBJECT *Storage, VPB *Vpb, DEVICE_OBJECT **VolumeDevice)
{
    if (Storage->SectorSize != USTAR_BLOCK)
        return STATUS_UNRECOGNIZED_VOLUME;
    uint8_t *Block = ExAllocatePoolWithTag(NonPagedPool, USTAR_BLOCK, USTAR_TAG);
    if (!Block)
        return STATUS_NO_MEMORY;
    STATUS Status = IoReadSectors(Storage, 0, 1, Block);
    if (!HZ_SUCCESS(Status) || memcmp(Block + 257, "ustar", 5) != 0) {
        ExFreePoolWithTag(Block, USTAR_TAG);
        return HZ_SUCCESS(Status) ? STATUS_UNRECOGNIZED_VOLUME : Status;
    }

    USTAR_ENTRY *Entries = ExAllocatePoolZero(NonPagedPool, USTAR_MAX_ENTRIES * sizeof(USTAR_ENTRY), USTAR_TAG);
    if (!Entries) {
        ExFreePoolWithTag(Block, USTAR_TAG);
        return STATUS_NO_MEMORY;
    }
    uint32_t Count = 0;
    uint64_t Sector = 0;
    while (Sector < Storage->SectorCount && Count < USTAR_MAX_ENTRIES) {
        Status = IoReadSectors(Storage, Sector, 1, Block);
        if (!HZ_SUCCESS(Status) || Block[0] == 0)
            break; /* end-of-archive marker */
        if (memcmp(Block + 257, "ustar", 5) != 0) {
            Status = STATUS_DISK_CORRUPT_ERROR;
            break;
        }
        uint64_t Size = UstarOctal((const char *)Block + 124, 12);
        char Type = (char)Block[156];
        USTAR_ENTRY *E = &Entries[Count];
        char Prefix[156] = "", Name[101] = "";
        memcpy(Prefix, Block + 345, 155);
        memcpy(Name, Block, 100);
        if (Prefix[0])
            RtlFormatString(E->Path, sizeof(E->Path), "%s/%s", Prefix, Name);
        else
            RtlStringCopy(E->Path, sizeof(E->Path), Name);
        UstarNormalize(E->Path);
        E->Offset = (Sector + 1) * USTAR_BLOCK;
        E->Size = Size;
        E->Time = UstarOctal((const char *)Block + 136, 12) * HZ_TIME_UNITS_PER_SECOND + HZ_TIME_UNIX_EPOCH;
        E->IsDirectory = Type == '5';
        if ((Type == '0' || Type == '\0' || Type == '5') && E->Path[0])
            Count++;
        if (Type == '5')
            Size = 0;
        Sector += 1 + (Size + USTAR_BLOCK - 1) / USTAR_BLOCK;
    }
    ExFreePoolWithTag(Block, USTAR_TAG);
    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Entries, USTAR_TAG);
        return Status;
    }

    DEVICE_OBJECT *Volume;
    Status = IoCreateDevice(UstarDriver, sizeof(USTAR_VOLUME), NULL, FILE_DEVICE_FILE_SYSTEM, DO_READ_ONLY, &Volume);
    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Entries, USTAR_TAG);
        return Status;
    }
    USTAR_VOLUME *Vol = Volume->Extension;
    Vol->Storage = Storage;
    Vol->Entries = Entries;
    Vol->Count = Count;
    Vol->Root.IsDirectory = true;
    Volume->SectorSize = USTAR_BLOCK;
    Volume->SectorCount = Storage->SectorCount;
    RtlStringCopy(Vpb->Label, sizeof(Vpb->Label), "Nexora OS live");
    *VolumeDevice = Volume;
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "UstarFs: mounted %u entries\n", Count);
    return STATUS_SUCCESS;
}

/* ---- Dispatch ------------------------------------------------------------------------------------ */

static STATUS UstarCreate(DEVICE_OBJECT *Device, IRP *Irp)
{
    USTAR_VOLUME *Vol = Device->Extension;
    char Path[256];
    RtlStringCopy(Path, sizeof(Path), Irp->Parameters.Create.Path);
    UstarNormalize(Path);
    uint32_t Disposition = Irp->Parameters.Create.Disposition;
    uint32_t Options = Irp->Parameters.Create.Options;

    USTAR_ENTRY Synthetic;
    USTAR_ENTRY *E = UstarLookup(Vol, Path, &Synthetic);
    if (!E) {
        char Parent[256];
        size_t ParentLength = UstarParentLength(Path);
        memcpy(Parent, Path, ParentLength);
        Parent[ParentLength] = '\0';
        USTAR_ENTRY ParentSynthetic;
        USTAR_ENTRY *P = UstarLookup(Vol, Parent, &ParentSynthetic);
        if (!P || !P->IsDirectory)
            return STATUS_OBJECT_PATH_NOT_FOUND;
        return (Disposition == FILE_OPEN || Disposition == FILE_OVERWRITE) ? STATUS_OBJECT_NAME_NOT_FOUND
                                                                            : STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (Disposition == FILE_CREATE)
        return STATUS_OBJECT_NAME_COLLISION;
    if (Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Irp->Parameters.Create.DesiredAccess & (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | DELETE))
        return STATUS_MEDIA_WRITE_PROTECTED;
    if ((Options & FILE_DIRECTORY_FILE) && !E->IsDirectory)
        return STATUS_NOT_A_DIRECTORY;
    if ((Options & FILE_NON_DIRECTORY_FILE) && E->IsDirectory)
        return STATUS_FILE_IS_A_DIRECTORY;

    USTAR_CCB *Ccb = ExAllocatePoolZero(NonPagedPool, sizeof(USTAR_CCB), USTAR_TAG);
    if (!Ccb)
        return STATUS_NO_MEMORY;
    if (E == &Synthetic) {
        /* Keep implied directories alive for the open: copy into the CCB-owned memory. */
        USTAR_ENTRY *Copy = ExAllocatePoolWithTag(NonPagedPool, sizeof(USTAR_ENTRY), USTAR_TAG);
        if (!Copy) {
            ExFreePoolWithTag(Ccb, USTAR_TAG);
            return STATUS_NO_MEMORY;
        }
        *Copy = Synthetic;
        E = Copy;
        Ccb->Index = 0x80000000u; /* marks an owned synthetic entry (low bits = cursor) */
    }
    FILE_OBJECT *File = Irp->FileObject;
    File->FsContext = E;
    File->FsContext2 = Ccb;
    if (E->IsDirectory)
        File->Flags |= FO_DIRECTORY;
    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

static STATUS UstarClose(DEVICE_OBJECT *Device, IRP *Irp)
{
    USTAR_CCB *Ccb = Irp->FileObject->FsContext2;
    if (Ccb) {
        if (Ccb->Index & 0x80000000u)
            ExFreePoolWithTag(Irp->FileObject->FsContext, USTAR_TAG);
        ExFreePoolWithTag(Ccb, USTAR_TAG);
    }
    return STATUS_SUCCESS;
}

static STATUS UstarRead(DEVICE_OBJECT *Device, IRP *Irp)
{
    USTAR_VOLUME *Vol = Device->Extension;
    USTAR_ENTRY *E = Irp->FileObject->FsContext;
    uint64_t Offset = Irp->Parameters.ReadWrite.ByteOffset;
    Irp->IoStatus.Information = 0;
    if (E->IsDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Irp->Length == 0)
        return STATUS_SUCCESS;
    if (Offset >= E->Size)
        return STATUS_END_OF_FILE;
    uint64_t Length = HZ_MIN((uint64_t)Irp->Length, E->Size - Offset);
    uint8_t *Temp = ExAllocatePoolWithTag(NonPagedPool, USTAR_READ_CHUNK + USTAR_BLOCK, USTAR_TAG);
    if (!Temp)
        return STATUS_NO_MEMORY;
    uint64_t Done = 0;
    STATUS Status = STATUS_SUCCESS;
    while (Done < Length) {
        uint64_t Position = E->Offset + Offset + Done;
        uint64_t First = Position / USTAR_BLOCK;
        uint32_t Skip = (uint32_t)(Position % USTAR_BLOCK);
        uint32_t Chunk = (uint32_t)HZ_MIN(Length - Done, USTAR_READ_CHUNK);
        uint32_t Sectors = (Skip + Chunk + USTAR_BLOCK - 1) / USTAR_BLOCK;
        Status = IoReadSectors(Vol->Storage, First, Sectors, Temp);
        if (!HZ_SUCCESS(Status))
            break;
        memcpy((uint8_t *)Irp->Buffer + Done, Temp + Skip, Chunk);
        Done += Chunk;
    }
    ExFreePoolWithTag(Temp, USTAR_TAG);
    Irp->IoStatus.Information = Done;
    return Status;
}

static STATUS UstarWrite(DEVICE_OBJECT *Device, IRP *Irp)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static void UstarDescribe(const USTAR_ENTRY *E, uint64_t *Time, uint64_t *Size, uint64_t *Allocation,
                          uint32_t *Attributes)
{
    *Time = E->Time;
    *Size = E->IsDirectory ? 0 : E->Size;
    *Allocation = HZ_ALIGN_UP(*Size, (uint64_t)USTAR_BLOCK);
    *Attributes = FILE_ATTRIBUTE_READONLY | (E->IsDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
}

static STATUS UstarQueryInformation(DEVICE_OBJECT *Device, IRP *Irp)
{
    USTAR_VOLUME *Vol = Device->Extension;
    USTAR_ENTRY *E = Irp->FileObject->FsContext;
    switch (Irp->Parameters.Information.Class) {
    case HZ_FILE_BASIC_INFORMATION_CLASS: {
        if (Irp->Length != sizeof(HZ_FILE_BASIC_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        HZ_FILE_BASIC_INFORMATION *Info = Irp->Buffer;
        memset(Info, 0, sizeof(*Info));
        UstarDescribe(E, &Info->LastWriteTime, &Info->EndOfFile, &Info->AllocationSize, &Info->Attributes);
        Info->CreationTime = Info->LastWriteTime;
        return STATUS_SUCCESS;
    }
    case HZ_FILE_VOLUME_INFORMATION_CLASS: {
        if (Irp->Length != sizeof(HZ_FILE_VOLUME_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        HZ_FILE_VOLUME_INFORMATION *Info = Irp->Buffer;
        memset(Info, 0, sizeof(*Info));
        Info->TotalBytes = Vol->Storage->SectorCount * USTAR_BLOCK;
        Info->BytesPerCluster = Info->BytesPerSector = USTAR_BLOCK;
        Info->ReadOnly = 1;
        RtlStringCopy(Info->FileSystem, sizeof(Info->FileSystem), "UstarFs");
        RtlStringCopy(Info->Label, sizeof(Info->Label), "Nexora OS live");
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

static STATUS UstarSetInformation(DEVICE_OBJECT *Device, IRP *Irp)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

/* Direct child of Directory? Writes the child's leaf name. For implied subdirectories (deeper
   entries only), *Implied is set and the leaf is the first component below Directory. */
static bool UstarChildName(const char *Directory, const char *Path, char *Leaf, bool *Implied)
{
    size_t Length = strlen(Directory);
    if (Length) {
        if (_strnicmp(Path, Directory, Length) != 0 || Path[Length] != '\\')
            return false;
        Path += Length + 1;
    }
    if (!*Path)
        return false;
    const char *Separator = Path;
    while (*Separator && *Separator != '\\')
        Separator++;
    size_t LeafLength = (size_t)(Separator - Path);
    if (LeafLength >= 256)
        return false;
    memcpy(Leaf, Path, LeafLength);
    Leaf[LeafLength] = '\0';
    *Implied = *Separator != '\0';
    return true;
}

static STATUS UstarDirectoryControl(DEVICE_OBJECT *Device, IRP *Irp)
{
    USTAR_VOLUME *Vol = Device->Extension;
    USTAR_ENTRY *Dir = Irp->FileObject->FsContext;
    USTAR_CCB *Ccb = Irp->FileObject->FsContext2;
    if (!Dir->IsDirectory)
        return STATUS_NOT_A_DIRECTORY;
    uint32_t Owned = Ccb->Index & 0x80000000u;
    uint32_t Index = Ccb->Index & 0x7FFFFFFFu;
    if (Irp->Parameters.Directory.Flags & HZ_DIRECTORY_RESTART_SCAN)
        Index = 0;
    uint32_t Capacity = Irp->Length / sizeof(HZ_FILE_DIRECTORY_INFORMATION);
    HZ_FILE_DIRECTORY_INFORMATION *Out = Irp->Buffer;
    uint32_t Returned = 0;
    for (; Index < Vol->Count && Returned < Capacity; Index++) {
        char Leaf[256];
        bool Implied;
        if (!UstarChildName(Dir->Path, Vol->Entries[Index].Path, Leaf, &Implied))
            continue;
        if (Implied) {
            /* Report an implied directory once: at its first occurrence, if not declared. */
            bool Seen = false;
            for (uint32_t j = 0; j < Index && !Seen; j++) {
                char Other[256];
                bool OtherImplied;
                Seen = UstarChildName(Dir->Path, Vol->Entries[j].Path, Other, &OtherImplied) &&
                       _stricmp(Other, Leaf) == 0;
            }
            for (uint32_t j = Index + 1; j < Vol->Count && !Seen; j++) {
                char Other[256];
                bool OtherImplied;
                Seen = UstarChildName(Dir->Path, Vol->Entries[j].Path, Other, &OtherImplied) && !OtherImplied &&
                       _stricmp(Other, Leaf) == 0;
            }
            if (Seen)
                continue;
        }
        HZ_FILE_DIRECTORY_INFORMATION *Info = &Out[Returned++];
        memset(Info, 0, sizeof(*Info));
        USTAR_ENTRY Implicit = {0};
        Implicit.IsDirectory = true;
        UstarDescribe(Implied ? &Implicit : &Vol->Entries[Index], &Info->LastWriteTime, &Info->EndOfFile,
                      &Info->AllocationSize, &Info->Attributes);
        Info->CreationTime = Info->LastWriteTime;
        Info->NameLength = (uint32_t)RtlStringCopy(Info->Name, sizeof(Info->Name), Leaf);
    }
    Ccb->Index = Owned | Index;
    Irp->IoStatus.Information = Returned;
    return Returned ? STATUS_SUCCESS : STATUS_NO_MORE_FILES;
}

static FILE_SYSTEM UstarFileSystem = {.Name = "UstarFs", .Mount = UstarMount};

static STATUS UstarDriverEntry(DRIVER_OBJECT *Driver)
{
    UstarDriver = Driver;
    Driver->MajorFunction[IRP_MJ_CREATE] = UstarCreate;
    Driver->MajorFunction[IRP_MJ_CLOSE] = UstarClose;
    Driver->MajorFunction[IRP_MJ_READ] = UstarRead;
    Driver->MajorFunction[IRP_MJ_WRITE] = UstarWrite;
    Driver->MajorFunction[IRP_MJ_QUERY_INFORMATION] = UstarQueryInformation;
    Driver->MajorFunction[IRP_MJ_SET_INFORMATION] = UstarSetInformation;
    Driver->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = UstarDirectoryControl;
    IoRegisterFileSystem(&UstarFileSystem);
    return STATUS_SUCCESS;
}

IO_DECLARE_BUILTIN_DRIVER(UstarBuiltinDriver, "\\FileSystem\\UstarFs", UstarDriverEntry, IO_ORDER_FILE_SYSTEM);
