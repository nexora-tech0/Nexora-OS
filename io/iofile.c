/*
 * Nexora OS - io/iofile.c
 * The kernel file API: reads, writes, information and directory queries on file objects.
 * Requests on one file object are serialized by its lock; the current byte offset belongs to the
 * file object (shared by every handle to it), as for NT synchronous file objects.
 */
#include "iop.h"

static void IopLockFile(FILE_OBJECT *File)
{
    KeWaitForSingleObject(&File->Lock, NULL);
}

static void IopUnlockFile(FILE_OBJECT *File)
{
    KeReleaseMutex(&File->Lock);
}

static STATUS IopTransfer(FILE_OBJECT *File, uint8_t Major, void *Buffer, uint32_t Length, const uint64_t *Offset,
                          KPROCESSOR_MODE Mode, uint32_t *Transferred)
{
    ACCESS_MASK Needed = Major == IRP_MJ_READ ? FILE_READ_DATA : FILE_WRITE_DATA;
    if (!(File->GrantedAccess & Needed))
        return STATUS_ACCESS_DENIED;
    if (File->Flags & FO_DIRECTORY)
        return STATUS_INVALID_DEVICE_REQUEST;
    IopLockFile(File);
    IRP Irp;
    IoInitializeIrp(&Irp, Major, File, Buffer, Length);
    Irp.RequestorMode = (uint8_t)Mode;
    Irp.Parameters.ReadWrite.ByteOffset = Offset ? *Offset : File->CurrentByteOffset;
    STATUS Status = IoCallDriver(File->Device, &Irp);
    if (HZ_SUCCESS(Status))
        File->CurrentByteOffset = Irp.Parameters.ReadWrite.ByteOffset + Irp.IoStatus.Information;
    IopUnlockFile(File);
    if (Transferred)
        *Transferred = (uint32_t)Irp.IoStatus.Information;
    return Status;
}

STATUS IoReadFile(FILE_OBJECT *File, void *Buffer, uint32_t Length, const uint64_t *Offset, KPROCESSOR_MODE Mode,
                  uint32_t *Transferred)
{
    return IopTransfer(File, IRP_MJ_READ, Buffer, Length, Offset, Mode, Transferred);
}

STATUS IoWriteFile(FILE_OBJECT *File, const void *Buffer, uint32_t Length, const uint64_t *Offset,
                   KPROCESSOR_MODE Mode, uint32_t *Transferred)
{
    return IopTransfer(File, IRP_MJ_WRITE, (void *)Buffer, Length, Offset, Mode, Transferred);
}

STATUS IoQueryInformationFile(FILE_OBJECT *File, uint32_t Class, void *Buffer, uint32_t Length)
{
    if (Class == HZ_FILE_POSITION_INFORMATION_CLASS) {
        if (Length != sizeof(HZ_FILE_POSITION_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        ((HZ_FILE_POSITION_INFORMATION *)Buffer)->CurrentByteOffset = File->CurrentByteOffset;
        return STATUS_SUCCESS;
    }
    IopLockFile(File);
    IRP Irp;
    IoInitializeIrp(&Irp, IRP_MJ_QUERY_INFORMATION, File, Buffer, Length);
    Irp.Parameters.Information.Class = Class;
    STATUS Status = IoCallDriver(File->Device, &Irp);
    IopUnlockFile(File);

    /* Devices without a query routine still describe themselves. */
    if (Status == STATUS_INVALID_DEVICE_REQUEST && (File->Flags & FO_RAW_DEVICE) &&
        Class == HZ_FILE_BASIC_INFORMATION_CLASS) {
        if (Length != sizeof(HZ_FILE_BASIC_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        HZ_FILE_BASIC_INFORMATION *Info = Buffer;
        memset(Info, 0, sizeof(*Info));
        Info->Attributes = FILE_ATTRIBUTE_DEVICE;
        Info->EndOfFile = Info->AllocationSize = File->Device->SectorCount * File->Device->SectorSize;
        Status = STATUS_SUCCESS;
    }
    return Status;
}

STATUS IoSetInformationFile(FILE_OBJECT *File, uint32_t Class, const void *Buffer, uint32_t Length)
{
    if (Class == HZ_FILE_POSITION_INFORMATION_CLASS) {
        if (Length != sizeof(HZ_FILE_POSITION_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        IopLockFile(File);
        File->CurrentByteOffset = ((const HZ_FILE_POSITION_INFORMATION *)Buffer)->CurrentByteOffset;
        IopUnlockFile(File);
        return STATUS_SUCCESS;
    }
    if ((Class == HZ_FILE_END_OF_FILE_INFORMATION_CLASS && !(File->GrantedAccess & FILE_WRITE_DATA)) ||
        ((Class == HZ_FILE_DISPOSITION_INFORMATION_CLASS || Class == HZ_FILE_RENAME_INFORMATION_CLASS) &&
         !(File->GrantedAccess & DELETE)))
        return STATUS_ACCESS_DENIED;
    IopLockFile(File);
    IRP Irp;
    IoInitializeIrp(&Irp, IRP_MJ_SET_INFORMATION, File, (void *)Buffer, Length);
    Irp.Parameters.Information.Class = Class;
    STATUS Status = IoCallDriver(File->Device, &Irp);
    IopUnlockFile(File);
    return Status;
}

STATUS IoQueryDirectoryFile(FILE_OBJECT *File, HZ_FILE_DIRECTORY_INFORMATION *Entries, uint32_t Count, uint32_t Flags,
                            uint32_t *Returned)
{
    if (!(File->GrantedAccess & FILE_LIST_DIRECTORY))
        return STATUS_ACCESS_DENIED;
    if (Count == 0)
        return STATUS_BUFFER_TOO_SMALL;
    IopLockFile(File);
    IRP Irp;
    IoInitializeIrp(&Irp, IRP_MJ_DIRECTORY_CONTROL, File, Entries, Count * (uint32_t)sizeof(*Entries));
    Irp.Parameters.Directory.Flags = Flags;
    STATUS Status = IoCallDriver(File->Device, &Irp);
    IopUnlockFile(File);
    *Returned = (uint32_t)Irp.IoStatus.Information;
    return Status;
}

STATUS IoReadWholeFile(const char *Path, void **Out, uint64_t *Size, uint64_t MaximumSize)
{
    FILE_OBJECT *File;
    STATUS Status = IoOpenFile(Path, &File);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_BASIC_INFORMATION Info;
    Status = IoQueryInformationFile(File, HZ_FILE_BASIC_INFORMATION_CLASS, &Info, sizeof(Info));
    if (HZ_SUCCESS(Status) && (Info.EndOfFile > MaximumSize || Info.EndOfFile > UINT32_MAX))
        Status = STATUS_FILE_CORRUPT_ERROR; /* too large for an in-memory read */
    uint8_t *Buffer = NULL;
    if (HZ_SUCCESS(Status)) {
        Buffer = ExAllocatePoolWithTag(NonPagedPool, Info.EndOfFile ? Info.EndOfFile : 1, IOP_READ_FILE_TAG);
        if (!Buffer)
            Status = STATUS_NO_MEMORY;
    }
    uint64_t Done = 0;
    while (HZ_SUCCESS(Status) && Done < Info.EndOfFile) {
        uint32_t Chunk = (uint32_t)HZ_MIN(Info.EndOfFile - Done, 1024 * 1024);
        uint32_t Got = 0;
        Status = IoReadFile(File, Buffer + Done, Chunk, &Done, KernelMode, &Got);
        if (HZ_SUCCESS(Status) && Got == 0)
            Status = STATUS_END_OF_FILE;
        Done += Got;
    }
    ObDereferenceObject(File);
    if (!HZ_SUCCESS(Status)) {
        if (Buffer)
            ExFreePoolWithTag(Buffer, IOP_READ_FILE_TAG);
        return Status;
    }
    *Out = Buffer;
    *Size = Info.EndOfFile;
    return STATUS_SUCCESS;
}
