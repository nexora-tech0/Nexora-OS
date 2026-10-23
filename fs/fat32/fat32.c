/*
 * Nexora OS - fs/fat32/fat32.c
 * FAT32 file system driver: mount, file control blocks, and the IRP dispatch routines.
 *
 * One FCB per open file or directory (found by its directory-entry position) is shared by all
 * opens; a CCB holds per-open state (enumeration cursor, delete-on-close). File data is read and
 * written directly between the caller's kernel buffer and the disk (whole sectors), with a
 * read-modify-write of partial sectors. There is no data cache.
 */
#include "fatp.h"

static DRIVER_OBJECT *FatDriver;

/* ---- FCBs ---------------------------------------------------------------------------------------- */

static FAT_FCB *FatFindFcb(FAT_VOLUME *Vol, uint32_t ParentCluster, uint32_t EntryOffset)
{
    for (LIST_ENTRY *E = Vol->Fcbs.Flink; E != &Vol->Fcbs; E = E->Flink) {
        FAT_FCB *Fcb = CONTAINING_RECORD(E, FAT_FCB, Link);
        if (!Fcb->IsRoot && Fcb->ParentCluster == ParentCluster && Fcb->EntryOffset == EntryOffset)
            return Fcb;
    }
    return NULL;
}

/* Referenced FCB for a directory item of the directory starting at ParentCluster. */
static STATUS FatGetFcb(FAT_VOLUME *Vol, uint32_t ParentCluster, const FAT_DIR_ITEM *Item, FAT_FCB **Out)
{
    FAT_FCB *Fcb = FatFindFcb(Vol, ParentCluster, Item->EntryOffset);
    if (Fcb) {
        Fcb->References++;
        *Out = Fcb;
        return STATUS_SUCCESS;
    }
    Fcb = ExAllocatePoolZero(NonPagedPool, sizeof(FAT_FCB), FAT_TAG);
    if (!Fcb)
        return STATUS_NO_MEMORY;
    Fcb->References = 1;
    Fcb->ParentCluster = ParentCluster;
    Fcb->EntryOffset = Item->EntryOffset;
    Fcb->FirstOffset = Item->FirstOffset;
    Fcb->Entry = Item->Entry;
    Fcb->FirstCluster = FatEntryCluster(&Item->Entry);
    Fcb->IsDirectory = (Item->Entry.Attributes & FAT_ATTR_DIRECTORY) != 0;
    Fcb->Size = Fcb->IsDirectory ? 0 : Item->Entry.Size;
    Fcb->Allocated = UINT32_MAX;
    Fcb->Cursor.First = Fcb->FirstCluster;
    RtlStringCopy(Fcb->Name, sizeof(Fcb->Name), Item->Name);
    InsertTailList(&Vol->Fcbs, &Fcb->Link);
    *Out = Fcb;
    return STATUS_SUCCESS;
}

static uint32_t FatDirectoryCluster(FAT_FCB *Dir)
{
    return Dir->FirstCluster;
}

/* Writes the FCB's size, first cluster, attributes and times back to its directory entry. */
static STATUS FatUpdateEntry(FAT_VOLUME *Vol, FAT_FCB *Fcb)
{
    if (Fcb->IsRoot)
        return STATUS_SUCCESS;
    Fcb->Entry.Size = Fcb->IsDirectory ? 0 : Fcb->Size;
    FatSetEntryCluster(&Fcb->Entry, Fcb->FirstCluster);
    FAT_CURSOR Parent = {Fcb->ParentCluster, 0, 0};
    return FatWriteDirEntry(Vol, &Parent, Fcb->EntryOffset, &Fcb->Entry);
}

static void FatTouch(FAT_FCB *Fcb)
{
    uint16_t Date, Clock;
    FatEncodeTime(KeQuerySystemTime(), &Date, &Clock, NULL);
    Fcb->Entry.WriteDate = Fcb->Entry.AccessDate = Date;
    Fcb->Entry.WriteTime = Clock;
    if (!Fcb->IsDirectory)
        Fcb->Entry.Attributes |= FAT_ATTR_ARCHIVE;
}

static STATUS FatAllocatedClusters(FAT_VOLUME *Vol, FAT_FCB *Fcb, uint32_t *Count)
{
    if (Fcb->Allocated == UINT32_MAX) {
        STATUS Status = FatCountChain(Vol, Fcb->FirstCluster, &Fcb->Allocated);
        if (!HZ_SUCCESS(Status)) {
            Fcb->Allocated = UINT32_MAX;
            return Status;
        }
    }
    *Count = Fcb->Allocated;
    return STATUS_SUCCESS;
}

/* Grows or shrinks the cluster chain to hold Bytes. */
static STATUS FatSetAllocation(FAT_VOLUME *Vol, FAT_FCB *Fcb, uint64_t Bytes)
{
    uint32_t Needed = (uint32_t)((Bytes + Vol->BytesPerCluster - 1) / Vol->BytesPerCluster);
    uint32_t Have;
    STATUS Status = FatAllocatedClusters(Vol, Fcb, &Have);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Needed > Have) {
        if (Needed - Have > Vol->FreeClusters)
            return STATUS_DISK_FULL;
        uint32_t Last = 0;
        if (Have > 0) {
            Status = FatSeek(Vol, &Fcb->Cursor, Have - 1, &Last);
            if (!HZ_SUCCESS(Status))
                return Status;
        }
        while (Have < Needed) {
            uint32_t New;
            Status = FatAllocateCluster(Vol, Last, Fcb->IsDirectory, &New);
            if (!HZ_SUCCESS(Status))
                return Status;
            if (Have == 0) {
                Fcb->FirstCluster = New;
                Fcb->Cursor.First = New;
                Fcb->Cursor.Cluster = 0;
            }
            Last = New;
            Fcb->Allocated = ++Have;
        }
    } else if (Needed < Have) {
        if (Needed == 0) {
            Status = FatFreeChain(Vol, Fcb->FirstCluster);
            Fcb->FirstCluster = 0;
            Fcb->Cursor.First = 0;
        } else {
            uint32_t Last, Next;
            Status = FatSeek(Vol, &Fcb->Cursor, Needed - 1, &Last);
            if (HZ_SUCCESS(Status))
                Status = FatGetEntry(Vol, Last, &Next);
            if (HZ_SUCCESS(Status))
                Status = FatSetEntry(Vol, Last, FAT_EOC);
            if (HZ_SUCCESS(Status) && Next < FAT_EOC_MIN)
                Status = FatFreeChain(Vol, Next);
        }
        Fcb->Cursor.Cluster = 0;
        Fcb->Allocated = HZ_SUCCESS(Status) ? Needed : UINT32_MAX;
    }
    return Status;
}

/* Deletes the file: data chain and directory entries. The FCB stays (it is being released). */
static STATUS FatDeleteFile(FAT_VOLUME *Vol, FAT_FCB *Fcb)
{
    STATUS Status = FatMarkDeleted(Vol, Fcb->ParentCluster, Fcb->FirstOffset, Fcb->EntryOffset);
    if (HZ_SUCCESS(Status) && Fcb->FirstCluster)
        Status = FatFreeChain(Vol, Fcb->FirstCluster);
    if (!HZ_SUCCESS(Status))
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Fat32: deleting '%s' failed (0x%08X)\n", Fcb->Name, (uint32_t)Status);
    return Status;
}

static void FatReleaseFcb(FAT_VOLUME *Vol, FAT_FCB *Fcb)
{
    if (--Fcb->References > 0 || Fcb->IsRoot)
        return;
    if (Fcb->DeletePending) {
        /* A directory may have gained entries since it was marked; it is only removed if empty. */
        bool Empty = true;
        if (Fcb->IsDirectory)
            FatIsDirectoryEmpty(Vol, Fcb->FirstCluster, &Empty);
        if (Empty)
            FatDeleteFile(Vol, Fcb);
    }
    RemoveEntryList(&Fcb->Link);
    ExFreePoolWithTag(Fcb, FAT_TAG);
}

/* ---- Paths ---------------------------------------------------------------------------------------- */

/* Splits off the next component of *Path ('\' or '/' separated); false at the end. */
static bool FatNextComponent(const char **Path, char *Component)
{
    const char *p = *Path;
    while (*p == '\\' || *p == '/')
        p++;
    if (!*p)
        return false;
    size_t n = 0;
    while (*p && *p != '\\' && *p != '/') {
        if (n < FAT_MAX_NAME + 1)
            Component[n++] = *p;
        p++;
    }
    Component[n < FAT_MAX_NAME + 1 ? n : FAT_MAX_NAME + 1] = '\0';
    if (n > FAT_MAX_NAME)
        Component[0] = '\0'; /* too long: reported as an invalid name */
    *Path = p;
    return true;
}

/*
 * Walks every component but the last. Returns the referenced parent directory FCB and the last
 * component (empty for the root). Avoid, if non-zero, is a directory cluster the walk must not
 * pass through (rename of a directory into its own subtree).
 */
static STATUS FatWalkParent(FAT_VOLUME *Vol, const char *Path, FAT_FCB **Parent, char *Leaf, uint32_t Avoid,
                            FAT_DIR_ITEM *Item)
{
    FAT_FCB *Dir = Vol->Root;
    Leaf[0] = '\0';
    char Component[FAT_MAX_NAME + 2];
    bool Have = FatNextComponent(&Path, Component);
    while (Have) {
        char Next[FAT_MAX_NAME + 2];
        bool More = FatNextComponent(&Path, Next);
        if (!More) {
            RtlStringCopy(Leaf, FAT_MAX_NAME + 1, Component);
            if (!Component[0]) {
                FatReleaseFcb(Vol, Dir);
                return STATUS_OBJECT_NAME_INVALID;
            }
            break;
        }
        if (!Component[0] || !strcmp(Component, ".") || !strcmp(Component, "..")) {
            FatReleaseFcb(Vol, Dir);
            return STATUS_OBJECT_NAME_INVALID;
        }
        STATUS Status = FatLookup(Vol, FatDirectoryCluster(Dir), Component, Item);
        if (HZ_SUCCESS(Status) && !(Item->Entry.Attributes & FAT_ATTR_DIRECTORY))
            Status = STATUS_OBJECT_PATH_NOT_FOUND;
        if (HZ_SUCCESS(Status) && Avoid && FatEntryCluster(&Item->Entry) == Avoid)
            Status = STATUS_INVALID_PARAMETER;
        FAT_FCB *Child = NULL;
        if (HZ_SUCCESS(Status))
            Status = FatGetFcb(Vol, FatDirectoryCluster(Dir), Item, &Child);
        FatReleaseFcb(Vol, Dir);
        if (!HZ_SUCCESS(Status))
            return Status == STATUS_OBJECT_NAME_NOT_FOUND ? STATUS_OBJECT_PATH_NOT_FOUND : Status;
        Dir = Child;
        RtlStringCopy(Component, sizeof(Component), Next);
    }
    *Parent = Dir;
    return STATUS_SUCCESS;
}

/* Creates a file or directory entry in Dir and returns its FCB. */
static STATUS FatCreateEntry(FAT_VOLUME *Vol, FAT_FCB *Dir, const char *Name, bool Directory, FAT_FCB **Out)
{
    FAT_DIRENT Short;
    memset(&Short, 0, sizeof(Short));
    uint16_t Date, Clock;
    uint8_t Tenths;
    FatEncodeTime(KeQuerySystemTime(), &Date, &Clock, &Tenths);
    Short.CreateDate = Date;
    Short.CreateTime = Clock;
    Short.CreateTenths = Tenths;
    Short.WriteDate = Short.AccessDate = Short.CreateDate;
    Short.WriteTime = Short.CreateTime;
    Short.Attributes = Directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;

    uint32_t Cluster = 0;
    STATUS Status = STATUS_SUCCESS;
    if (Directory) {
        /* A new directory starts with "." and "..". */
        Status = FatAllocateCluster(Vol, 0, true, &Cluster);
        if (!HZ_SUCCESS(Status))
            return Status;
        FAT_DIRENT Dot = Short;
        memset(Dot.Name, ' ', 11);
        Dot.Name[0] = '.';
        Dot.NtCase = 0;
        FatSetEntryCluster(&Dot, Cluster);
        FAT_DIRENT DotDot = Dot;
        DotDot.Name[1] = '.';
        FatSetEntryCluster(&DotDot, Dir->IsRoot ? 0 : Dir->FirstCluster);
        FAT_CURSOR New = {Cluster, 0, 0};
        Status = FatWriteDirEntry(Vol, &New, 0, &Dot);
        if (HZ_SUCCESS(Status))
            Status = FatWriteDirEntry(Vol, &New, FAT_DIRENT_SIZE, &DotDot);
        if (!HZ_SUCCESS(Status)) {
            FatFreeChain(Vol, Cluster);
            return Status;
        }
    }
    FatSetEntryCluster(&Short, Cluster);
    FAT_DIR_ITEM *Item = ExAllocatePoolZero(NonPagedPool, sizeof(FAT_DIR_ITEM), FAT_TAG);
    if (!Item) {
        if (Cluster)
            FatFreeChain(Vol, Cluster);
        return STATUS_NO_MEMORY;
    }
    Status = FatInsertEntries(Vol, FatDirectoryCluster(Dir), Name, &Short, &Item->FirstOffset, &Item->EntryOffset);
    if (HZ_SUCCESS(Status)) {
        Item->Entry = Short;
        RtlStringCopy(Item->Name, sizeof(Item->Name), Name);
        Status = FatGetFcb(Vol, FatDirectoryCluster(Dir), Item, Out);
        if (HZ_SUCCESS(Status))
            (*Out)->Allocated = Cluster ? 1 : 0;
    } else if (Cluster) {
        FatFreeChain(Vol, Cluster);
    }
    ExFreePoolWithTag(Item, FAT_TAG);
    return Status;
}

/* ---- Create / close ----------------------------------------------------------------------------- */

static STATUS FatCreate(DEVICE_OBJECT *Device, IRP *Irp)
{
    FAT_VOLUME *Vol = Device->Extension;
    FILE_OBJECT *File = Irp->FileObject;
    uint32_t Disposition = Irp->Parameters.Create.Disposition;
    uint32_t Options = Irp->Parameters.Create.Options;
    ACCESS_MASK Access = Irp->Parameters.Create.DesiredAccess;
    bool Modifying = (Access & (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | DELETE)) || Disposition != FILE_OPEN;
    if (Vol->ReadOnly && Modifying && Disposition != FILE_OPEN_IF)
        return STATUS_MEDIA_WRITE_PROTECTED;

    FAT_DIR_ITEM *Item = ExAllocatePoolZero(NonPagedPool, sizeof(FAT_DIR_ITEM), FAT_TAG);
    FAT_CCB *Ccb = ExAllocatePoolZero(NonPagedPool, sizeof(FAT_CCB), FAT_TAG);
    char *Leaf = ExAllocatePoolWithTag(NonPagedPool, FAT_MAX_NAME + 2, FAT_TAG);
    if (!Item || !Ccb || !Leaf) {
        if (Item)
            ExFreePoolWithTag(Item, FAT_TAG);
        if (Ccb)
            ExFreePoolWithTag(Ccb, FAT_TAG);
        if (Leaf)
            ExFreePoolWithTag(Leaf, FAT_TAG);
        return STATUS_NO_MEMORY;
    }

    KeWaitForSingleObject(&Vol->Lock, NULL);
    FAT_FCB *Parent = NULL, *Fcb = NULL;
    uint32_t Result = FILE_OPENED;
    Vol->Root->References++;
    STATUS Status = FatWalkParent(Vol, Irp->Parameters.Create.Path, &Parent, Leaf, 0, Item);
    if (HZ_SUCCESS(Status) && !Leaf[0]) {
        /* The root directory itself. */
        Fcb = Parent;
        Parent = NULL;
        if (Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF)
            Status = Disposition == FILE_CREATE ? STATUS_OBJECT_NAME_COLLISION : STATUS_ACCESS_DENIED;
        else if (Options & FILE_NON_DIRECTORY_FILE)
            Status = STATUS_FILE_IS_A_DIRECTORY;
        else if ((Options & FILE_DELETE_ON_CLOSE) || (Access & DELETE))
            Status = STATUS_CANNOT_DELETE;
    } else if (HZ_SUCCESS(Status)) {
        Status = FatLookup(Vol, FatDirectoryCluster(Parent), Leaf, Item);
        if (HZ_SUCCESS(Status)) {
            Status = FatGetFcb(Vol, FatDirectoryCluster(Parent), Item, &Fcb);
            if (HZ_SUCCESS(Status)) {
                bool Overwrite = Disposition == FILE_SUPERSEDE || Disposition == FILE_OVERWRITE ||
                                 Disposition == FILE_OVERWRITE_IF;
                if (Disposition == FILE_CREATE)
                    Status = STATUS_OBJECT_NAME_COLLISION;
                else if (Fcb->DeletePending)
                    Status = STATUS_DELETE_PENDING;
                else if ((Options & FILE_DIRECTORY_FILE) && !Fcb->IsDirectory)
                    Status = STATUS_NOT_A_DIRECTORY;
                else if ((Options & FILE_NON_DIRECTORY_FILE) && Fcb->IsDirectory)
                    Status = STATUS_FILE_IS_A_DIRECTORY;
                else if (Overwrite && Fcb->IsDirectory)
                    Status = STATUS_FILE_IS_A_DIRECTORY;
                else if ((Fcb->Entry.Attributes & FAT_ATTR_READONLY) &&
                         (Overwrite || (Access & (FILE_WRITE_DATA | DELETE))))
                    Status = STATUS_ACCESS_DENIED;
                else if (Vol->ReadOnly && (Overwrite || (Access & (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | DELETE))))
                    Status = STATUS_MEDIA_WRITE_PROTECTED;
                else if (Overwrite) {
                    Status = FatSetAllocation(Vol, Fcb, 0);
                    Fcb->Size = 0;
                    FatTouch(Fcb);
                    if (HZ_SUCCESS(Status))
                        Status = FatUpdateEntry(Vol, Fcb);
                    Result = Disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
                }
            }
        } else if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {
            if (Disposition == FILE_OPEN || Disposition == FILE_OVERWRITE)
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
            else if (!FatIsValidLongName(Leaf))
                Status = STATUS_OBJECT_NAME_INVALID;
            else if (Vol->ReadOnly)
                Status = STATUS_MEDIA_WRITE_PROTECTED;
            else {
                Status = FatCreateEntry(Vol, Parent, Leaf, (Options & FILE_DIRECTORY_FILE) != 0, &Fcb);
                Result = FILE_CREATED;
            }
        }
    }
    if (Parent)
        FatReleaseFcb(Vol, Parent);

    if (HZ_SUCCESS(Status)) {
        Ccb->DeleteOnClose = (Options & FILE_DELETE_ON_CLOSE) != 0;
        File->FsContext = Fcb;
        File->FsContext2 = Ccb;
        if (Fcb->IsDirectory)
            File->Flags |= FO_DIRECTORY;
        Irp->IoStatus.Information = Result;
        Ccb = NULL;
        Fcb = NULL;
    }
    if (Fcb)
        FatReleaseFcb(Vol, Fcb);
    FatFlushFsInfo(Vol);
    KeReleaseMutex(&Vol->Lock);

    if (Ccb)
        ExFreePoolWithTag(Ccb, FAT_TAG);
    ExFreePoolWithTag(Item, FAT_TAG);
    ExFreePoolWithTag(Leaf, FAT_TAG);
    return Status;
}

static STATUS FatClose(DEVICE_OBJECT *Device, IRP *Irp)
{
    FAT_VOLUME *Vol = Device->Extension;
    FAT_FCB *Fcb = Irp->FileObject->FsContext;
    FAT_CCB *Ccb = Irp->FileObject->FsContext2;
    KeWaitForSingleObject(&Vol->Lock, NULL);
    if (Ccb && Ccb->DeleteOnClose && !Fcb->IsRoot) {
        bool Empty = true;
        if (Fcb->IsDirectory)
            FatIsDirectoryEmpty(Vol, Fcb->FirstCluster, &Empty);
        if (Empty)
            Fcb->DeletePending = true;
    }
    FatReleaseFcb(Vol, Fcb);
    FatFlushFsInfo(Vol);
    KeReleaseMutex(&Vol->Lock);
    if (Ccb)
        ExFreePoolWithTag(Ccb, FAT_TAG);
    return STATUS_SUCCESS;
}

/* ---- Data --------------------------------------------------------------------------------------- */

/* Moves Length bytes at file offset Offset (inside the allocation) to or from Buffer; Buffer NULL
   on a write means zeros. */
static STATUS FatTransfer(FAT_VOLUME *Vol, FAT_FCB *Fcb, uint64_t Offset, uint8_t *Buffer, uint32_t Length, bool Write)
{
    uint32_t Bps = Vol->BytesPerSector;
    uint32_t Done = 0;
    while (Done < Length) {
        uint64_t Position = Offset + Done;
        uint32_t Cluster;
        STATUS Status = FatSeek(Vol, &Fcb->Cursor, (uint32_t)(Position / Vol->BytesPerCluster), &Cluster);
        if (!HZ_SUCCESS(Status))
            return Status == STATUS_END_OF_FILE ? STATUS_FILE_CORRUPT_ERROR : Status;
        uint32_t InCluster = (uint32_t)(Position % Vol->BytesPerCluster);
        uint32_t Span = HZ_MIN(Length - Done, Vol->BytesPerCluster - InCluster);
        uint64_t Lba = FatClusterToSector(Vol, Cluster) + InCluster / Bps;
        uint32_t Within = InCluster % Bps;
        if (Within == 0 && Span >= Bps && (Buffer || !Write)) {
            uint32_t Sectors = Span / Bps;
            /* Extend over the following clusters while they are physically contiguous (M11):
               one request instead of one per cluster. */
            if (InCluster + Sectors * Bps == Vol->BytesPerCluster) {
                uint32_t Index = (uint32_t)(Position / Vol->BytesPerCluster), Previous = Cluster;
                while (Sectors * Bps < (1u << 20)) {
                    uint32_t Remaining = Length - Done - Sectors * Bps, Next;
                    if (Remaining < Bps || !HZ_SUCCESS(FatSeek(Vol, &Fcb->Cursor, Index + 1, &Next)) ||
                        Next != Previous + 1)
                        break;
                    uint32_t Add = HZ_MIN(Remaining, Vol->BytesPerCluster) / Bps;
                    Sectors += Add;
                    Index++;
                    Previous = Next;
                    if (Add * Bps < Vol->BytesPerCluster)
                        break; /* ends inside this cluster */
                }
            }
            Status = Write ? IoWriteSectors(Vol->Storage, Lba, Sectors, Buffer + Done)
                           : IoReadSectors(Vol->Storage, Lba, Sectors, Buffer + Done);
            Span = Sectors * Bps;
        } else {
            Span = HZ_MIN(Span, Bps - Within);
            bool Whole = Within == 0 && Span == Bps;
            Status = (Write && Whole) ? STATUS_SUCCESS : IoReadSectors(Vol->Storage, Lba, 1, Vol->SectorBuffer);
            if (HZ_SUCCESS(Status)) {
                if (Write) {
                    if (Buffer)
                        memcpy(Vol->SectorBuffer + Within, Buffer + Done, Span);
                    else
                        memset(Vol->SectorBuffer + Within, 0, Span);
                    Status = IoWriteSectors(Vol->Storage, Lba, 1, Vol->SectorBuffer);
                } else {
                    memcpy(Buffer + Done, Vol->SectorBuffer + Within, Span);
                }
            }
        }
        if (!HZ_SUCCESS(Status))
            return Status;
        Done += Span;
    }
    return STATUS_SUCCESS;
}

static STATUS FatRead(DEVICE_OBJECT *Device, IRP *Irp)
{
    FAT_VOLUME *Vol = Device->Extension;
    FAT_FCB *Fcb = Irp->FileObject->FsContext;
    uint64_t Offset = Irp->Parameters.ReadWrite.ByteOffset;
    Irp->IoStatus.Information = 0;
    if (Fcb->IsDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Irp->Length == 0)
        return STATUS_SUCCESS;
    KeWaitForSingleObject(&Vol->Lock, NULL);
    STATUS Status;
    if (Offset >= Fcb->Size) {
        Status = STATUS_END_OF_FILE;
    } else {
        uint32_t Length = (uint32_t)HZ_MIN((uint64_t)Irp->Length, Fcb->Size - Offset);
        Status = FatTransfer(Vol, Fcb, Offset, Irp->Buffer, Length, false);
        if (HZ_SUCCESS(Status))
            Irp->IoStatus.Information = Length;
    }
    KeReleaseMutex(&Vol->Lock);
    return Status;
}

/* Changes the file size: frees or allocates clusters; new bytes read as zero. */
static STATUS FatSetSize(FAT_VOLUME *Vol, FAT_FCB *Fcb, uint64_t NewSize)
{
    if (NewSize > 0xFFFFFFFFull)
        return STATUS_DISK_FULL; /* FAT file size limit */
    STATUS Status = FatSetAllocation(Vol, Fcb, NewSize);
    if (HZ_SUCCESS(Status) && NewSize > Fcb->Size)
        Status = FatTransfer(Vol, Fcb, Fcb->Size, NULL, (uint32_t)(NewSize - Fcb->Size), true);
    if (!HZ_SUCCESS(Status))
        return Status;
    Fcb->Size = (uint32_t)NewSize;
    FatTouch(Fcb);
    return FatUpdateEntry(Vol, Fcb);
}

static STATUS FatWrite(DEVICE_OBJECT *Device, IRP *Irp)
{
    FAT_VOLUME *Vol = Device->Extension;
    FAT_FCB *Fcb = Irp->FileObject->FsContext;
    uint64_t Offset = Irp->Parameters.ReadWrite.ByteOffset;
    Irp->IoStatus.Information = 0;
    if (Fcb->IsDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Vol->ReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Irp->Length == 0)
        return STATUS_SUCCESS;
    uint64_t End = Offset + Irp->Length;
    if (End > 0xFFFFFFFFull)
        return STATUS_DISK_FULL;
    KeWaitForSingleObject(&Vol->Lock, NULL);
    STATUS Status = STATUS_SUCCESS;
    if (End > Fcb->Size) {
        /* Allocate, and zero any gap between the old end and the write. */
        Status = FatSetAllocation(Vol, Fcb, End);
        if (HZ_SUCCESS(Status) && Offset > Fcb->Size)
            Status = FatTransfer(Vol, Fcb, Fcb->Size, NULL, (uint32_t)(Offset - Fcb->Size), true);
    }
    if (HZ_SUCCESS(Status))
        Status = FatTransfer(Vol, Fcb, Offset, Irp->Buffer, Irp->Length, true);
    if (HZ_SUCCESS(Status)) {
        if (End > Fcb->Size)
            Fcb->Size = (uint32_t)End;
        FatTouch(Fcb);
        Status = FatUpdateEntry(Vol, Fcb);
        if (HZ_SUCCESS(Status))
            Irp->IoStatus.Information = Irp->Length;
    } else if (End > Fcb->Size) {
        FatSetAllocation(Vol, Fcb, Fcb->Size); /* give back what the failed write allocated */
    }
    FatFlushFsInfo(Vol);
    KeReleaseMutex(&Vol->Lock);
    return Status;
}

/* ---- Information --------------------------------------------------------------------------------- */

static STATUS FatQueryInformation(DEVICE_OBJECT *Device, IRP *Irp)
{
    FAT_VOLUME *Vol = Device->Extension;
    FAT_FCB *Fcb = Irp->FileObject->FsContext;
    STATUS Status = STATUS_SUCCESS;
    KeWaitForSingleObject(&Vol->Lock, NULL);
    switch (Irp->Parameters.Information.Class) {
    case HZ_FILE_BASIC_INFORMATION_CLASS: {
        if (Irp->Length != sizeof(HZ_FILE_BASIC_INFORMATION)) {
            Status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        HZ_FILE_BASIC_INFORMATION *Info = Irp->Buffer;
        memset(Info, 0, sizeof(*Info));
        uint32_t Clusters = 0;
        FatAllocatedClusters(Vol, Fcb, &Clusters);
        Info->EndOfFile = Fcb->Size;
        Info->AllocationSize = (uint64_t)Clusters * Vol->BytesPerCluster;
        if (Fcb->IsRoot) {
            Info->Attributes = FILE_ATTRIBUTE_DIRECTORY;
        } else {
            Info->Attributes = Fcb->Entry.Attributes & 0x37;
            Info->CreationTime = FatDecodeTime(Fcb->Entry.CreateDate, Fcb->Entry.CreateTime, Fcb->Entry.CreateTenths);
            Info->LastWriteTime = FatDecodeTime(Fcb->Entry.WriteDate, Fcb->Entry.WriteTime, 0);
        }
        break;
    }
    case HZ_FILE_VOLUME_INFORMATION_CLASS: {
        if (Irp->Length != sizeof(HZ_FILE_VOLUME_INFORMATION)) {
            Status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        HZ_FILE_VOLUME_INFORMATION *Info = Irp->Buffer;
        memset(Info, 0, sizeof(*Info));
        Info->TotalBytes = (uint64_t)Vol->ClusterCount * Vol->BytesPerCluster;
        Info->FreeBytes = (uint64_t)Vol->FreeClusters * Vol->BytesPerCluster;
        Info->BytesPerCluster = Vol->BytesPerCluster;
        Info->BytesPerSector = Vol->BytesPerSector;
        Info->SerialNumber = Vol->Serial;
        Info->ReadOnly = Vol->ReadOnly;
        RtlStringCopy(Info->FileSystem, sizeof(Info->FileSystem), "FAT32");
        RtlStringCopy(Info->Label, sizeof(Info->Label), Vol->Label);
        break;
    }
    default:
        Status = STATUS_INVALID_INFO_CLASS;
        break;
    }
    KeReleaseMutex(&Vol->Lock);
    return Status;
}

static STATUS FatRename(FAT_VOLUME *Vol, FAT_FCB *Fcb, const HZ_FILE_RENAME_INFORMATION *Rename)
{
    if (Fcb->IsRoot)
        return STATUS_ACCESS_DENIED;
    FAT_DIR_ITEM *Item = ExAllocatePoolZero(NonPagedPool, sizeof(FAT_DIR_ITEM), FAT_TAG);
    char *Leaf = ExAllocatePoolWithTag(NonPagedPool, FAT_MAX_NAME + 2, FAT_TAG);
    if (!Item || !Leaf) {
        if (Item)
            ExFreePoolWithTag(Item, FAT_TAG);
        if (Leaf)
            ExFreePoolWithTag(Leaf, FAT_TAG);
        return STATUS_NO_MEMORY;
    }
    STATUS Status;
    FAT_FCB *Target = NULL;
    const char *NewName = Rename->NewName;
    bool HasPath = false;
    for (const char *p = NewName; *p; p++)
        HasPath |= *p == '\\' || *p == '/';
    if (HasPath) {
        Vol->Root->References++;
        Status = FatWalkParent(Vol, NewName, &Target, Leaf, Fcb->IsDirectory ? Fcb->FirstCluster : 0, Item);
    } else {
        /* Same directory: find its FCB (the root, or by position in its own parent). */
        RtlStringCopy(Leaf, FAT_MAX_NAME + 2, NewName);
        Status = STATUS_SUCCESS;
        if (Fcb->ParentCluster == Vol->RootCluster) {
            Target = Vol->Root;
            Target->References++;
        } else {
            /* A private stand-in for the parent directory: only its cluster is needed. */
            Target = ExAllocatePoolZero(NonPagedPool, sizeof(FAT_FCB), FAT_TAG);
            if (!Target) {
                Status = STATUS_NO_MEMORY;
            } else {
                Target->References = 1;
                Target->IsDirectory = true;
                Target->IsRoot = true; /* never listed, never written back; freed below */
                Target->FirstCluster = Fcb->ParentCluster;
                Target->Cursor.First = Fcb->ParentCluster;
            }
        }
    }
    if (HZ_SUCCESS(Status) && (!Leaf[0] || !FatIsValidLongName(Leaf)))
        Status = STATUS_OBJECT_NAME_INVALID;
    uint32_t TargetCluster = Target ? FatDirectoryCluster(Target) : 0;

    if (HZ_SUCCESS(Status)) {
        Status = FatLookup(Vol, TargetCluster, Leaf, Item);
        if (HZ_SUCCESS(Status)) {
            bool Same = TargetCluster == Fcb->ParentCluster && Item->EntryOffset == Fcb->EntryOffset;
            if (Same) {
                Status = STATUS_OBJECT_NAME_NOT_FOUND; /* a case change of the same entry */
            } else if (!Rename->ReplaceIfExists) {
                Status = STATUS_OBJECT_NAME_COLLISION;
            } else if ((Item->Entry.Attributes & (FAT_ATTR_DIRECTORY | FAT_ATTR_READONLY)) ||
                       FatFindFcb(Vol, TargetCluster, Item->EntryOffset)) {
                Status = STATUS_ACCESS_DENIED; /* never replace a directory, a read-only or an open file */
            } else {
                Status = FatMarkDeleted(Vol, TargetCluster, Item->FirstOffset, Item->EntryOffset);
                if (HZ_SUCCESS(Status) && FatEntryCluster(&Item->Entry))
                    Status = FatFreeChain(Vol, FatEntryCluster(&Item->Entry));
                if (HZ_SUCCESS(Status))
                    Status = STATUS_OBJECT_NAME_NOT_FOUND;
            }
        }
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {
            FAT_DIRENT Short = Fcb->Entry;
            uint32_t First, Entry;
            Status = FatInsertEntries(Vol, TargetCluster, Leaf, &Short, &First, &Entry);
            if (HZ_SUCCESS(Status))
                Status = FatMarkDeleted(Vol, Fcb->ParentCluster, Fcb->FirstOffset, Fcb->EntryOffset);
            if (HZ_SUCCESS(Status) && Fcb->IsDirectory && TargetCluster != Fcb->ParentCluster) {
                /* The moved directory's ".." now names its new parent. */
                FAT_CURSOR Self = {Fcb->FirstCluster, 0, 0};
                FAT_DIRENT DotDot;
                Status = FatReadDirEntry(Vol, &Self, FAT_DIRENT_SIZE, &DotDot);
                if (HZ_SUCCESS(Status)) {
                    FatSetEntryCluster(&DotDot, TargetCluster == Vol->RootCluster ? 0 : TargetCluster);
                    Status = FatWriteDirEntry(Vol, &Self, FAT_DIRENT_SIZE, &DotDot);
                }
            }
            if (HZ_SUCCESS(Status)) {
                Fcb->ParentCluster = TargetCluster;
                Fcb->FirstOffset = First;
                Fcb->EntryOffset = Entry;
                Fcb->Entry.NtCase = Short.NtCase;
                memcpy(Fcb->Entry.Name, Short.Name, 11);
                RtlStringCopy(Fcb->Name, sizeof(Fcb->Name), Leaf);
            }
        }
    }
    if (Target) {
        if (Target->IsRoot && Target != Vol->Root)
            ExFreePoolWithTag(Target, FAT_TAG);
        else
            FatReleaseFcb(Vol, Target);
    }
    ExFreePoolWithTag(Item, FAT_TAG);
    ExFreePoolWithTag(Leaf, FAT_TAG);
    return Status;
}

static STATUS FatSetInformation(DEVICE_OBJECT *Device, IRP *Irp)
{
    FAT_VOLUME *Vol = Device->Extension;
    FAT_FCB *Fcb = Irp->FileObject->FsContext;
    if (Vol->ReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;
    STATUS Status;
    KeWaitForSingleObject(&Vol->Lock, NULL);
    switch (Irp->Parameters.Information.Class) {
    case HZ_FILE_END_OF_FILE_INFORMATION_CLASS:
        if (Irp->Length != sizeof(HZ_FILE_END_OF_FILE_INFORMATION))
            Status = STATUS_INFO_LENGTH_MISMATCH;
        else if (Fcb->IsDirectory)
            Status = STATUS_INVALID_PARAMETER;
        else
            Status = FatSetSize(Vol, Fcb, ((HZ_FILE_END_OF_FILE_INFORMATION *)Irp->Buffer)->EndOfFile);
        break;
    case HZ_FILE_DISPOSITION_INFORMATION_CLASS: {
        if (Irp->Length != sizeof(HZ_FILE_DISPOSITION_INFORMATION)) {
            Status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }
        bool Delete = ((HZ_FILE_DISPOSITION_INFORMATION *)Irp->Buffer)->DeleteFile != 0;
        Status = STATUS_SUCCESS;
        if (Delete && (Fcb->IsRoot || (Fcb->Entry.Attributes & FAT_ATTR_READONLY))) {
            Status = STATUS_CANNOT_DELETE;
        } else if (Delete && Fcb->IsDirectory) {
            bool Empty;
            Status = FatIsDirectoryEmpty(Vol, Fcb->FirstCluster, &Empty);
            if (HZ_SUCCESS(Status) && !Empty)
                Status = STATUS_DIRECTORY_NOT_EMPTY;
        }
        if (HZ_SUCCESS(Status))
            Fcb->DeletePending = Delete;
        break;
    }
    case HZ_FILE_RENAME_INFORMATION_CLASS:
        if (Irp->Length != sizeof(HZ_FILE_RENAME_INFORMATION))
            Status = STATUS_INFO_LENGTH_MISMATCH;
        else if (Fcb->DeletePending)
            Status = STATUS_DELETE_PENDING;
        else
            Status = FatRename(Vol, Fcb, Irp->Buffer);
        break;
    default:
        Status = STATUS_INVALID_INFO_CLASS;
        break;
    }
    FatFlushFsInfo(Vol);
    KeReleaseMutex(&Vol->Lock);
    return Status;
}

static STATUS FatDirectoryControl(DEVICE_OBJECT *Device, IRP *Irp)
{
    FAT_VOLUME *Vol = Device->Extension;
    FAT_FCB *Fcb = Irp->FileObject->FsContext;
    FAT_CCB *Ccb = Irp->FileObject->FsContext2;
    if (!Fcb->IsDirectory && !Fcb->IsRoot)
        return STATUS_NOT_A_DIRECTORY;
    FAT_DIR_ITEM *Item = ExAllocatePoolWithTag(NonPagedPool, sizeof(FAT_DIR_ITEM), FAT_TAG);
    if (!Item)
        return STATUS_NO_MEMORY;
    KeWaitForSingleObject(&Vol->Lock, NULL);
    if (Irp->Parameters.Directory.Flags & HZ_DIRECTORY_RESTART_SCAN)
        Ccb->DirectoryOffset = 0;
    uint32_t Capacity = Irp->Length / sizeof(HZ_FILE_DIRECTORY_INFORMATION);
    HZ_FILE_DIRECTORY_INFORMATION *Out = Irp->Buffer;
    uint32_t Returned = 0;
    FAT_CURSOR Dir = {FatDirectoryCluster(Fcb), 0, 0};
    STATUS Status = STATUS_SUCCESS;
    while (Returned < Capacity) {
        uint32_t Offset = Ccb->DirectoryOffset;
        Status = FatReadDirectory(Vol, &Dir, &Offset, Item);
        if (!HZ_SUCCESS(Status))
            break;
        Ccb->DirectoryOffset = Offset;
        HZ_FILE_DIRECTORY_INFORMATION *Info = &Out[Returned++];
        memset(Info, 0, sizeof(*Info));
        bool IsDir = (Item->Entry.Attributes & FAT_ATTR_DIRECTORY) != 0;
        Info->EndOfFile = IsDir ? 0 : Item->Entry.Size;
        Info->AllocationSize = HZ_ALIGN_UP(Info->EndOfFile, (uint64_t)Vol->BytesPerCluster);
        Info->Attributes = Item->Entry.Attributes & 0x37;
        Info->CreationTime = FatDecodeTime(Item->Entry.CreateDate, Item->Entry.CreateTime, Item->Entry.CreateTenths);
        Info->LastWriteTime = FatDecodeTime(Item->Entry.WriteDate, Item->Entry.WriteTime, 0);
        Info->NameLength = (uint32_t)RtlStringCopy(Info->Name, sizeof(Info->Name), Item->Name);
    }
    KeReleaseMutex(&Vol->Lock);
    ExFreePoolWithTag(Item, FAT_TAG);
    Irp->IoStatus.Information = Returned;
    if (Returned)
        return STATUS_SUCCESS;
    return Status == STATUS_SUCCESS ? STATUS_NO_MORE_FILES : Status;
}

static STATUS FatFlush(DEVICE_OBJECT *Device, IRP *Irp)
{
    FAT_VOLUME *Vol = Device->Extension;
    KeWaitForSingleObject(&Vol->Lock, NULL);
    STATUS Status = FatFlushFsInfo(Vol);
    KeReleaseMutex(&Vol->Lock);
    return Status;
}

/* ---- Mount ---------------------------------------------------------------------------------------- */

static STATUS FatMount(DEVICE_OBJECT *Storage, VPB *Vpb, DEVICE_OBJECT **VolumeDevice)
{
    uint32_t SectorSize = Storage->SectorSize;
    if (SectorSize < 512 || SectorSize > 4096 || Storage->SectorCount < 64)
        return STATUS_UNRECOGNIZED_VOLUME;
    uint8_t *Boot = ExAllocatePoolWithTag(NonPagedPool, SectorSize, FAT_TAG);
    if (!Boot)
        return STATUS_NO_MEMORY;
    STATUS Status = IoReadSectors(Storage, 0, 1, Boot);
    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Boot, FAT_TAG);
        return Status;
    }
    uint16_t Bps, Reserved, RootEntries, Total16, FatSize16, ExtFlags, Version;
    uint32_t Total32, FatSize32, RootCluster, Serial;
    uint16_t FsInfo;
    memcpy(&Bps, Boot + 11, 2);
    uint8_t Spc = Boot[13];
    memcpy(&Reserved, Boot + 14, 2);
    uint8_t FatCount = Boot[16];
    memcpy(&RootEntries, Boot + 17, 2);
    memcpy(&Total16, Boot + 19, 2);
    memcpy(&FatSize16, Boot + 22, 2);
    memcpy(&Total32, Boot + 32, 4);
    memcpy(&FatSize32, Boot + 36, 4);
    memcpy(&ExtFlags, Boot + 40, 2);
    memcpy(&Version, Boot + 42, 2);
    memcpy(&RootCluster, Boot + 44, 4);
    memcpy(&FsInfo, Boot + 48, 2);
    memcpy(&Serial, Boot + 67, 4);
    char Label[12];
    memcpy(Label, Boot + 71, 11);
    Label[11] = '\0';
    bool Signature = Boot[510] == 0x55 && Boot[511] == 0xAA;
    ExFreePoolWithTag(Boot, FAT_TAG);

    uint32_t TotalSectors = Total16 ? Total16 : Total32;
    if (!Signature || Bps != SectorSize || Spc == 0 || (Spc & (Spc - 1)) || (uint32_t)Spc * Bps > 65536 ||
        Reserved == 0 || FatCount == 0 || FatCount > 4 || RootEntries != 0 || FatSize16 != 0 || FatSize32 == 0 ||
        Version != 0 || TotalSectors > Storage->SectorCount)
        return STATUS_UNRECOGNIZED_VOLUME;
    uint64_t Metadata = Reserved + (uint64_t)FatCount * FatSize32;
    if (Metadata >= TotalSectors)
        return STATUS_UNRECOGNIZED_VOLUME;
    uint32_t Clusters = (uint32_t)((TotalSectors - Metadata) / Spc);
    if (Clusters < 2 || (uint64_t)FatSize32 * Bps / 4 < (uint64_t)Clusters + 2 || RootCluster < 2 ||
        RootCluster > Clusters + 1)
        return STATUS_UNRECOGNIZED_VOLUME;

    DEVICE_OBJECT *Volume;
    Status = IoCreateDevice(FatDriver, sizeof(FAT_VOLUME), NULL, FILE_DEVICE_FILE_SYSTEM,
                            Storage->Flags & DO_READ_ONLY, &Volume);
    if (!HZ_SUCCESS(Status))
        return Status;
    FAT_VOLUME *Vol = Volume->Extension;
    Vol->Storage = Storage;
    KeInitializeMutex(&Vol->Lock);
    InitializeListHead(&Vol->Fcbs);
    Vol->ReadOnly = (Storage->Flags & DO_READ_ONLY) != 0;
    Vol->BytesPerSector = Bps;
    Vol->SectorsPerCluster = Spc;
    Vol->BytesPerCluster = (uint32_t)Spc * Bps;
    Vol->ReservedSectors = Reserved;
    Vol->FatCount = FatCount;
    Vol->SectorsPerFat = FatSize32;
    Vol->ActiveFat = (ExtFlags & 0x80) ? (ExtFlags & 0x0F) : UINT32_MAX;
    if (Vol->ActiveFat != UINT32_MAX && Vol->ActiveFat >= FatCount)
        Vol->ActiveFat = UINT32_MAX;
    Vol->RootCluster = RootCluster;
    Vol->FsInfoSector = FsInfo;
    Vol->FirstDataSector = Metadata;
    Vol->ClusterCount = Clusters;
    Vol->Serial = Serial;
    Vol->FatCacheLba = Vol->MetaCacheLba = UINT64_MAX;
    Vol->FatCache = ExAllocatePoolWithTag(NonPagedPool, Bps, FAT_TAG);
    Vol->MetaCache = ExAllocatePoolWithTag(NonPagedPool, Bps, FAT_TAG);
    Vol->SectorBuffer = ExAllocatePoolWithTag(NonPagedPool, Bps, FAT_TAG);
    Vol->ClusterBuffer = ExAllocatePoolZero(NonPagedPool, Vol->BytesPerCluster, FAT_TAG);
    Vol->Root = ExAllocatePoolZero(NonPagedPool, sizeof(FAT_FCB), FAT_TAG);
    if (!Vol->FatCache || !Vol->MetaCache || !Vol->SectorBuffer || !Vol->ClusterBuffer || !Vol->Root)
        return STATUS_NO_MEMORY; /* the unnamed volume device stays referenced: boot-time only */

    size_t n = 11;
    while (n > 0 && Label[n - 1] == ' ')
        Label[--n] = '\0';
    RtlStringCopy(Vol->Label, sizeof(Vol->Label), strcmp(Label, "NO NAME") ? Label : "");

    /* The free count from FSInfo if it is plausible, else counted from the FAT. */
    Vol->FreeClusters = UINT32_MAX;
    Vol->NextFree = 2;
    uint8_t *Info;
    if (FsInfo && FsInfo < Reserved && HZ_SUCCESS(FatReadMeta(Vol, FsInfo, &Info))) {
        uint32_t Lead, Struct, Free, Next;
        memcpy(&Lead, Info, 4);
        memcpy(&Struct, Info + 484, 4);
        memcpy(&Free, Info + 488, 4);
        memcpy(&Next, Info + 492, 4);
        if (Lead == 0x41615252 && Struct == 0x61417272) {
            if (Free <= Clusters)
                Vol->FreeClusters = Free;
            if (FatIsValidCluster(Vol, Next))
                Vol->NextFree = Next;
        }
    }
    if (Vol->FreeClusters == UINT32_MAX) {
        Status = FatCountFreeClusters(Vol, &Vol->FreeClusters);
        if (!HZ_SUCCESS(Status))
            return Status;
        Vol->FsInfoDirty = true;
    }

    FAT_FCB *Root = Vol->Root;
    Root->References = 1; /* held by the volume */
    Root->IsRoot = Root->IsDirectory = true;
    Root->FirstCluster = RootCluster;
    Root->Cursor.First = RootCluster;
    Root->Allocated = UINT32_MAX;
    Root->Entry.Attributes = FAT_ATTR_DIRECTORY;
    InsertTailList(&Vol->Fcbs, &Root->Link);

    Volume->SectorSize = Bps;
    Volume->SectorCount = TotalSectors;
    RtlStringCopy(Vpb->Label, sizeof(Vpb->Label), Vol->Label);
    char Size[24], Free[24];
    RtlFormatByteSize(Size, sizeof(Size), (uint64_t)Clusters * Vol->BytesPerCluster);
    RtlFormatByteSize(Free, sizeof(Free), (uint64_t)Vol->FreeClusters * Vol->BytesPerCluster);
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "Fat32: mounted '%s' (serial %04X-%04X): %s in %u clusters of %u bytes, %s free%s\n",
               Vol->Label, Serial >> 16, Serial & 0xFFFF, Size, Clusters, Vol->BytesPerCluster, Free,
               Vol->ActiveFat == UINT32_MAX ? "" : ", FAT mirroring off");
    *VolumeDevice = Volume;
    return STATUS_SUCCESS;
}

static FILE_SYSTEM FatFileSystem = {.Name = "FAT32", .Mount = FatMount};

static STATUS FatDriverEntry(DRIVER_OBJECT *Driver)
{
    FatDriver = Driver;
    Driver->MajorFunction[IRP_MJ_CREATE] = FatCreate;
    Driver->MajorFunction[IRP_MJ_CLOSE] = FatClose;
    Driver->MajorFunction[IRP_MJ_READ] = FatRead;
    Driver->MajorFunction[IRP_MJ_WRITE] = FatWrite;
    Driver->MajorFunction[IRP_MJ_QUERY_INFORMATION] = FatQueryInformation;
    Driver->MajorFunction[IRP_MJ_SET_INFORMATION] = FatSetInformation;
    Driver->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = FatDirectoryControl;
    Driver->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = FatFlush;
    IoRegisterFileSystem(&FatFileSystem);
    return STATUS_SUCCESS;
}

IO_DECLARE_BUILTIN_DRIVER(FatBuiltinDriver, "\\FileSystem\\Fat32", FatDriverEntry, IO_ORDER_FILE_SYSTEM);
