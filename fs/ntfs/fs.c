/*
 * Nexora OS - fs/ntfs/fs.c
 * NTFS volume mount and read-side file-system dispatch.
 *
 * This first mounted path deliberately exposes NTFS as read-only until allocation metadata,
 * journaling, and crash-safe updates are implemented. It lets ordinary file opens use the NTFS
 * volume context and the same MFT/data stream reader exercised by the on-disk probe.
 */
#include "ntfs.h"
#include "ex.h"
#include "io.h"
#include "rtl.h"
#include "uapi/hzapi.h"

#define NTFS_FS_TAG HZ_POOL_TAG('N', 't', 'F', 's')

typedef struct _NTFS_OPEN_CONTEXT {
    NTFS_FILE_INFORMATION Information;
    bool IsRoot;
} NTFS_OPEN_CONTEXT;

static DRIVER_OBJECT *NtfsDriver;

static STATUS NtfsFsMount(DEVICE_OBJECT *Storage, VPB *Vpb, DEVICE_OBJECT **VolumeDevice)
{
    if (!Storage || !Vpb || !VolumeDevice || Storage->SectorSize < 512 || Storage->SectorSize > 4096)
        return STATUS_UNRECOGNIZED_VOLUME;

    DEVICE_OBJECT *Device = NULL;
    STATUS Status = IoCreateDevice(NtfsDriver, sizeof(NTFS_VOLUME_CONTEXT), NULL,
                                   FILE_DEVICE_FILE_SYSTEM, DO_READ_ONLY, &Device);
    if (!HZ_SUCCESS(Status))
        return Status;

    NTFS_VOLUME_CONTEXT *Volume = Device->Extension;
    Status = NtfsInitializeVolume(Storage, Volume);
    if (!HZ_SUCCESS(Status))
        return Status;

    Device->SectorSize = Storage->SectorSize;
    Device->SectorCount = Storage->SectorCount;
    RtlStringCopy(Vpb->Label, sizeof(Vpb->Label), "NTFS Volume");
    *VolumeDevice = Device;
    DbgPrint("NTFS: mounted read-only volume (%u-byte sectors, %u-byte clusters)\n",
             Volume->Geometry.BytesPerSector, Volume->Geometry.BytesPerCluster);
    return STATUS_SUCCESS;
}

static STATUS NtfsFsCreate(DEVICE_OBJECT *Device, IRP *Irp)
{
    NTFS_VOLUME_CONTEXT *Volume = Device->Extension;
    const char *Path = Irp->Parameters.Create.Path;
    while (*Path == '\\' || *Path == '/')
        ++Path;

    if (Irp->Parameters.Create.Disposition != FILE_OPEN)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Irp->Parameters.Create.DesiredAccess &
        (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | DELETE | GENERIC_WRITE))
        return STATUS_MEDIA_WRITE_PROTECTED;

    NTFS_OPEN_CONTEXT *Open = ExAllocatePoolZero(NonPagedPool, sizeof(*Open), NTFS_FS_TAG);
    if (!Open)
        return STATUS_NO_MEMORY;

    if (!*Path) {
        Open->IsRoot = true;
        Open->Information.IsDirectory = true;
    } else {
        uint16_t Name[256];
        size_t Characters = 0;
        while (Path[Characters]) {
            if (Characters == HZ_ARRAYSIZE(Name) - 1 || Path[Characters] == '/' || Path[Characters] == '\\') {
                ExFreePoolWithTag(Open, NTFS_FS_TAG);
                return STATUS_NOT_SUPPORTED; /* nested paths are added with directory traversal */
            }
            uint8_t Character = (uint8_t)Path[Characters];
            if (Character >= 0x80) {
                ExFreePoolWithTag(Open, NTFS_FS_TAG);
                return STATUS_NOT_SUPPORTED; /* UTF-8 to UTF-16 path decoding is not wired yet */
            }
            Name[Characters] = Character;
            ++Characters;
        }
        STATUS Status = NtfsLookupDirectoryEntry(Volume, 5, Name, Characters, &Open->Information);
        if (!HZ_SUCCESS(Status)) {
            ExFreePoolWithTag(Open, NTFS_FS_TAG);
            return Status;
        }
    }

    uint32_t Options = Irp->Parameters.Create.Options;
    if ((Options & FILE_DIRECTORY_FILE) && !Open->Information.IsDirectory) {
        ExFreePoolWithTag(Open, NTFS_FS_TAG);
        return STATUS_NOT_A_DIRECTORY;
    }
    if ((Options & FILE_NON_DIRECTORY_FILE) && Open->Information.IsDirectory) {
        ExFreePoolWithTag(Open, NTFS_FS_TAG);
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    Irp->FileObject->FsContext2 = Open;
    if (Open->Information.IsDirectory)
        Irp->FileObject->Flags |= FO_DIRECTORY;
    Irp->IoStatus.Information = FILE_OPENED;
    return STATUS_SUCCESS;
}

static STATUS NtfsFsClose(DEVICE_OBJECT *Device, IRP *Irp)
{
    NTFS_OPEN_CONTEXT *Open = Irp->FileObject->FsContext2;
    if (Open)
        ExFreePoolWithTag(Open, NTFS_FS_TAG);
    Irp->FileObject->FsContext2 = NULL;
    return STATUS_SUCCESS;
}

static STATUS NtfsFsRead(DEVICE_OBJECT *Device, IRP *Irp)
{
    NTFS_VOLUME_CONTEXT *Volume = Device->Extension;
    NTFS_OPEN_CONTEXT *Open = Irp->FileObject->FsContext2;
    if (!Open)
        return STATUS_INVALID_PARAMETER;
    if (Open->Information.IsDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;
    uint32_t BytesRead = 0;
    STATUS Status = NtfsReadFileData(Volume, &Open->Information, Irp->Parameters.ReadWrite.ByteOffset,
                                     Irp->Length, Irp->Buffer, &BytesRead);
    Irp->IoStatus.Information = BytesRead;
    return Status;
}

static STATUS NtfsFsQueryInformation(DEVICE_OBJECT *Device, IRP *Irp)
{
    NTFS_VOLUME_CONTEXT *Volume = Device->Extension;
    NTFS_OPEN_CONTEXT *Open = Irp->FileObject->FsContext2;
    if (!Open)
        return STATUS_INVALID_PARAMETER;

    switch (Irp->Parameters.Information.Class) {
    case HZ_FILE_BASIC_INFORMATION_CLASS: {
        if (Irp->Length != sizeof(HZ_FILE_BASIC_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        HZ_FILE_BASIC_INFORMATION *Info = Irp->Buffer;
        memset(Info, 0, sizeof(*Info));
        Info->EndOfFile = Open->Information.IsDirectory ? 0 : Open->Information.DataBytes;
        Info->AllocationSize = Info->EndOfFile;
        Info->Attributes = FILE_ATTRIBUTE_READONLY |
                           (Open->Information.IsDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
        return STATUS_SUCCESS;
    }
    case HZ_FILE_VOLUME_INFORMATION_CLASS: {
        if (Irp->Length != sizeof(HZ_FILE_VOLUME_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        HZ_FILE_VOLUME_INFORMATION *Info = Irp->Buffer;
        memset(Info, 0, sizeof(*Info));
        Info->TotalBytes = Volume->Geometry.TotalSectors * Volume->Geometry.BytesPerSector;
        Info->BytesPerSector = Volume->Geometry.BytesPerSector;
        Info->BytesPerCluster = Volume->Geometry.BytesPerCluster;
        Info->SerialNumber = (uint32_t)Volume->Geometry.VolumeSerial;
        Info->ReadOnly = 1;
        RtlStringCopy(Info->FileSystem, sizeof(Info->FileSystem), "NTFS");
        RtlStringCopy(Info->Label, sizeof(Info->Label), "NTFS Volume");
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }
}

static STATUS NtfsFsWrite(DEVICE_OBJECT *Device, IRP *Irp)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static STATUS NtfsFsSetInformation(DEVICE_OBJECT *Device, IRP *Irp)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static STATUS NtfsFsDirectoryControl(DEVICE_OBJECT *Device, IRP *Irp)
{
    NTFS_OPEN_CONTEXT *Open = Irp->FileObject->FsContext2;
    if (!Open)
        return STATUS_INVALID_PARAMETER;
    return Open->Information.IsDirectory ? STATUS_NOT_SUPPORTED : STATUS_NOT_A_DIRECTORY;
}

static FILE_SYSTEM NtfsFileSystem = {.Name = "NTFS", .Mount = NtfsFsMount};

static STATUS NtfsDriverEntry(DRIVER_OBJECT *Driver)
{
    NtfsDriver = Driver;
    Driver->MajorFunction[IRP_MJ_CREATE] = NtfsFsCreate;
    Driver->MajorFunction[IRP_MJ_CLOSE] = NtfsFsClose;
    Driver->MajorFunction[IRP_MJ_READ] = NtfsFsRead;
    Driver->MajorFunction[IRP_MJ_WRITE] = NtfsFsWrite;
    Driver->MajorFunction[IRP_MJ_QUERY_INFORMATION] = NtfsFsQueryInformation;
    Driver->MajorFunction[IRP_MJ_SET_INFORMATION] = NtfsFsSetInformation;
    Driver->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = NtfsFsDirectoryControl;
    IoRegisterFileSystem(&NtfsFileSystem);
    return STATUS_SUCCESS;
}

IO_DECLARE_BUILTIN_DRIVER(NtfsBuiltinDriver, "\\FileSystem\\NTFS", NtfsDriverEntry, IO_ORDER_FILE_SYSTEM);
