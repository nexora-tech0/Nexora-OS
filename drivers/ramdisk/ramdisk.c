/*
 * Nexora OS - drivers/ramdisk/ramdisk.c
 * Boot modules as read-only block devices with 512-byte sectors:
 *   - "ramdisk" (the live system files) is \Device\Ramdisk0; its USTAR file system is C: unless
 *     the system started from an installed disk (then it has no drive letter);
 *   - "install" (M10, live boots only: the boot loader, kernel and symbols laid out as on an EFI
 *     system partition) is \Device\Ramdisk1, without a drive letter; Setup copies it.
 */
#include "bootinfo.h"
#include "io.h"
#include "rtl.h"

#define RAMDISK_SECTOR_SIZE 512

typedef struct _RAMDISK_EXTENSION {
    const uint8_t *Data;
    uint64_t Size;
} RAMDISK_EXTENSION;

static STATUS RamdiskRead(DEVICE_OBJECT *Device, IRP *Irp)
{
    RAMDISK_EXTENSION *Ext = Device->Extension;
    uint64_t Offset = Irp->Parameters.ReadWrite.ByteOffset;
    if (Offset % RAMDISK_SECTOR_SIZE || Irp->Length % RAMDISK_SECTOR_SIZE)
        return STATUS_INVALID_PARAMETER;
    uint64_t Capacity = Device->SectorCount * RAMDISK_SECTOR_SIZE;
    if (Offset >= Capacity) {
        Irp->IoStatus.Information = 0;
        return Irp->Length ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    }
    if (Irp->Length > Capacity - Offset)
        return STATUS_INVALID_PARAMETER;
    /* The last sector may extend past the module: zero-fill it. */
    uint64_t Available = Offset < Ext->Size ? Ext->Size - Offset : 0;
    uint64_t Copy = HZ_MIN(Available, Irp->Length);
    memcpy(Irp->Buffer, Ext->Data + Offset, Copy);
    memset((uint8_t *)Irp->Buffer + Copy, 0, Irp->Length - Copy);
    Irp->IoStatus.Information = Irp->Length;
    return STATUS_SUCCESS;
}

static STATUS RamdiskWrite(DEVICE_OBJECT *Device, IRP *Irp)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static STATUS RamdiskDeviceControl(DEVICE_OBJECT *Device, IRP *Irp)
{
    if (Irp->Parameters.DeviceControl.Code != IOCTL_DISK_GET_GEOMETRY)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Irp->Length < sizeof(HZ_DISK_GEOMETRY))
        return STATUS_BUFFER_TOO_SMALL;
    HZ_DISK_GEOMETRY *Geometry = Irp->Buffer;
    memset(Geometry, 0, sizeof(*Geometry));
    Geometry->SectorCount = Device->SectorCount;
    Geometry->BytesPerSector = RAMDISK_SECTOR_SIZE;
    Irp->IoStatus.Information = sizeof(*Geometry);
    return STATUS_SUCCESS;
}

static STATUS RamdiskCreate(DRIVER_OBJECT *Driver, const BOOT_MODULE *Module, const char *Path, uint32_t Flags)
{
    DEVICE_OBJECT *Device;
    STATUS Status =
        IoCreateDevice(Driver, sizeof(RAMDISK_EXTENSION), Path, FILE_DEVICE_DISK, DO_STORAGE | DO_READ_ONLY, &Device);
    if (!HZ_SUCCESS(Status))
        return Status;
    RAMDISK_EXTENSION *Ext = Device->Extension;
    Ext->Data = Module->Address;
    Ext->Size = Module->Size;
    Device->SectorSize = RAMDISK_SECTOR_SIZE;
    Device->SectorCount = (Module->Size + RAMDISK_SECTOR_SIZE - 1) / RAMDISK_SECTOR_SIZE;
    IoRegisterStorageDevice(Device, Path, Flags);

    char Size[24];
    RtlFormatByteSize(Size, sizeof(Size), Module->Size);
    DbgPrint("Ramdisk: %s, %s at %s\n", Module->Path, Size, Path);
    return STATUS_SUCCESS;
}

static STATUS RamdiskDriverEntry(DRIVER_OBJECT *Driver)
{
    const BOOT_MODULE *Module = BiFindModule("ramdisk");
    if (!Module) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Ramdisk: boot module not found\n");
        return STATUS_NO_SUCH_DEVICE;
    }
    Driver->MajorFunction[IRP_MJ_READ] = RamdiskRead;
    Driver->MajorFunction[IRP_MJ_WRITE] = RamdiskWrite;
    Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = RamdiskDeviceControl;
    STATUS Status = RamdiskCreate(Driver, Module, "\\Device\\Ramdisk0", IO_STORAGE_LIVE);
    const BOOT_MODULE *Install = BiFindModule("install");
    if (HZ_SUCCESS(Status) && Install)
        RamdiskCreate(Driver, Install, "\\Device\\Ramdisk1", IO_STORAGE_HIDDEN);
    return Status;
}

IO_DECLARE_BUILTIN_DRIVER(RamdiskBuiltinDriver, "\\Driver\\Ramdisk", RamdiskDriverEntry, IO_ORDER_DEVICE);
