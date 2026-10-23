/*
 * Nexora OS - drivers/disk/disk.c
 * Disk class driver: whole-disk and partition devices over a port driver's transfer routine.
 *
 * Requests must be sector-aligned (offset and length); they are split into transfers of at most
 * MaxTransferSectors. Partition devices check their bounds and add their start LBA. Every
 * partition (or the whole disk when there is no partition table) is registered with the I/O
 * manager as a mountable storage device: the partition named by `hz.system=<unique GUID>` on the
 * kernel command line as the system volume (C:), EFI system partitions without a drive letter.
 *
 * Since M10 a program that wrote a new partition table (Setup) asks for it to be read again
 * (IOCTL_DISK_UPDATE_PARTITIONS on DR0): existing partition devices are updated in place (device
 * objects are never deleted), new ones are created and registered, and the ones beyond the new
 * table shrink to zero sectors. That is refused while a volume of the disk is mounted.
 */
#include "bootinfo.h"
#include "disk.h"
#include "ex.h"
#include "mm.h"
#include "rtl.h"

#define DISK_TAG HZ_POOL_TAG('D', 'i', 's', 'k')
#define DISK_MAX_PARTITIONS 128

typedef struct _DISK_EXTENSION {
    DISK_DESCRIPTION Disk;    /* copy, shared description for partitions */
    uint32_t DiskNumber;
    uint32_t PartitionNumber; /* 0 = whole disk */
    uint32_t Style;           /* HZ_PARTITION_STYLE_* of the disk */
    uint64_t StartLba;
    uint64_t SectorCount;
    uint8_t TypeGuid[16];
    uint8_t UniqueGuid[16];
    char Name[40];
    bool Registered;          /* known to the I/O manager as storage */
} DISK_EXTENSION;

const uint8_t DiskTestScratchType[16] = {0x3E, 0x5C, 0x1F, 0x7A, 0x4D, 0x2B, 0x8A, 0x4E,
                                         0x9F, 0x60, 0x48, 0x5A, 0x54, 0x45, 0x53, 0x54};
static const uint8_t DiskBasicDataType[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                              0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
static const uint8_t DiskEfiSystemType[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                              0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};

static DRIVER_OBJECT *DiskDriver;
static uint32_t DiskCount;
static KMUTEX DiskLock;           /* partition table updates */
static uint8_t DiskSystemGuid[16];
static bool DiskSystemGuidValid;

/* ---- GUIDs -------------------------------------------------------------------------------------- */

static int DiskHexValue(char C)
{
    if (C >= '0' && C <= '9')
        return C - '0';
    if (C >= 'a' && C <= 'f')
        return C - 'a' + 10;
    if (C >= 'A' && C <= 'F')
        return C - 'A' + 10;
    return -1;
}

/* "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" to the GPT on-disk byte order (the first three groups
   little-endian, the last two as written). */
bool DiskParseGuid(const char *Text, uint8_t Guid[16])
{
    static const uint8_t Order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t Bytes[16];
    uint32_t Count = 0;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (Text[i] != '-')
                return false;
            continue;
        }
        int High = DiskHexValue(Text[i]), Low = DiskHexValue(Text[i + 1]);
        if (High < 0 || Low < 0)
            return false;
        Bytes[Count++] = (uint8_t)(High * 16 + Low);
        i++;
    }
    if (Text[36] && Text[36] != ' ')
        return false;
    for (uint32_t i = 0; i < 16; i++)
        Guid[Order[i]] = Bytes[i];
    return true;
}

static void DiskFormatGuid(const uint8_t Guid[16], char *Out, size_t Size)
{
    RtlFormatString(Out, Size, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", Guid[3], Guid[2],
                    Guid[1], Guid[0], Guid[5], Guid[4], Guid[7], Guid[6], Guid[8], Guid[9], Guid[10], Guid[11], Guid[12],
                    Guid[13], Guid[14], Guid[15]);
}

/* ---- Dispatch ------------------------------------------------------------------------------- */

static STATUS DiskReadWrite(DEVICE_OBJECT *Device, IRP *Irp)
{
    DISK_EXTENSION *Ext = Device->Extension;
    bool Write = Irp->MajorFunction == IRP_MJ_WRITE;
    uint32_t SectorSize = Ext->Disk.SectorSize;
    uint64_t Offset = Irp->Parameters.ReadWrite.ByteOffset;
    if (Offset % SectorSize || Irp->Length % SectorSize)
        return STATUS_INVALID_PARAMETER;
    if (Write && Ext->Disk.ReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;
    uint64_t Lba = Offset / SectorSize;
    uint64_t Count = Irp->Length / SectorSize;
    uint64_t Start = Ext->StartLba, Limit = Ext->SectorCount; /* read once: an update may change them */
    if (Lba >= Limit) {
        Irp->IoStatus.Information = 0;
        return Count ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    }
    if (Count > Limit - Lba)
        return STATUS_INVALID_PARAMETER; /* would cross the end of the partition */

    uint8_t *Buffer = Irp->Buffer;
    uint64_t Done = 0;
    while (Done < Count) {
        uint32_t Chunk = (uint32_t)HZ_MIN(Count - Done, Ext->Disk.MaxTransferSectors);
        STATUS Status = Ext->Disk.Transfer(Ext->Disk.Context, Start + Lba + Done, Chunk, Buffer + Done * SectorSize, Write);
        if (!HZ_SUCCESS(Status)) {
            Irp->IoStatus.Information = Done * SectorSize;
            return Status;
        }
        Done += Chunk;
    }
    Irp->IoStatus.Information = Count * SectorSize;
    return STATUS_SUCCESS;
}

/* True if the device (a partition or the whole disk) holds a mounted volume. */
static bool DiskIsMounted(DEVICE_OBJECT *Device)
{
    return Device->Vpb && (Device->Vpb->Flags & VPB_MOUNTED);
}

static STATUS DiskUpdatePartitions(DEVICE_OBJECT *Whole, uint32_t *CountOut);

static STATUS DiskGetInfo(DEVICE_OBJECT *Device, HZ_DISK_INFORMATION *Info)
{
    DISK_EXTENSION *Ext = Device->Extension;
    memset(Info, 0, sizeof(*Info));
    RtlStringCopy(Info->Model, sizeof(Info->Model), Ext->Disk.Model);
    Info->SectorCount = Ext->Disk.SectorCount;
    Info->BytesPerSector = Ext->Disk.SectorSize;
    Info->Style = Ext->Style;
    Info->DiskNumber = Ext->DiskNumber;
    if (Ext->Disk.ReadOnly)
        Info->Flags |= HZ_DISK_READ_ONLY;
    for (DEVICE_OBJECT *D = DiskDriver->DeviceList; D; D = D->NextDevice) {
        DISK_EXTENSION *E = D->Extension;
        if (E->DiskNumber != Ext->DiskNumber)
            continue;
        if (E->PartitionNumber && E->SectorCount)
            Info->PartitionCount++;
        if (DiskIsMounted(D))
            Info->Flags |= HZ_DISK_IN_USE;
        if (IoIsSystemStorage(D))
            Info->Flags |= HZ_DISK_SYSTEM | HZ_DISK_IN_USE;
    }
    return STATUS_SUCCESS;
}

static STATUS DiskDeviceControl(DEVICE_OBJECT *Device, IRP *Irp)
{
    DISK_EXTENSION *Ext = Device->Extension;
    switch (Irp->Parameters.DeviceControl.Code) {
    case IOCTL_DISK_GET_GEOMETRY: {
        if (Irp->Length < sizeof(HZ_DISK_GEOMETRY))
            return STATUS_BUFFER_TOO_SMALL;
        HZ_DISK_GEOMETRY *Geometry = Irp->Buffer;
        memset(Geometry, 0, sizeof(*Geometry));
        Geometry->SectorCount = Ext->SectorCount;
        Geometry->BytesPerSector = Ext->Disk.SectorSize;
        Irp->IoStatus.Information = sizeof(*Geometry);
        return STATUS_SUCCESS;
    }
    case IOCTL_DISK_GET_PARTITION_INFO: {
        if (Irp->Length < sizeof(HZ_PARTITION_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        HZ_PARTITION_INFORMATION *Info = Irp->Buffer;
        memset(Info, 0, sizeof(*Info));
        Info->StartingOffset = Ext->StartLba * Ext->Disk.SectorSize;
        Info->Length = Ext->SectorCount * Ext->Disk.SectorSize;
        Info->Number = Ext->PartitionNumber;
        Info->Style = Ext->Style;
        memcpy(Info->TypeGuid, Ext->TypeGuid, 16);
        memcpy(Info->UniqueGuid, Ext->UniqueGuid, 16);
        RtlStringCopy(Info->Name, sizeof(Info->Name), Ext->Name);
        Irp->IoStatus.Information = sizeof(*Info);
        return STATUS_SUCCESS;
    }
    case IOCTL_DISK_GET_INFO: {
        if (Ext->PartitionNumber)
            return STATUS_INVALID_DEVICE_REQUEST;
        if (Irp->Length < sizeof(HZ_DISK_INFORMATION))
            return STATUS_BUFFER_TOO_SMALL;
        KeWaitForSingleObject(&DiskLock, NULL);
        STATUS Status = DiskGetInfo(Device, Irp->Buffer);
        KeReleaseMutex(&DiskLock);
        Irp->IoStatus.Information = sizeof(HZ_DISK_INFORMATION);
        return Status;
    }
    case IOCTL_DISK_UPDATE_PARTITIONS: {
        if (Ext->PartitionNumber)
            return STATUS_INVALID_DEVICE_REQUEST;
        if (Irp->FileObject && !(Irp->FileObject->GrantedAccess & FILE_WRITE_DATA))
            return STATUS_ACCESS_DENIED;
        uint32_t Count = 0;
        STATUS Status = DiskUpdatePartitions(Device, &Count);
        if (HZ_SUCCESS(Status) && Irp->Length >= sizeof(uint32_t)) {
            memcpy(Irp->Buffer, &Count, sizeof(Count));
            Irp->IoStatus.Information = sizeof(Count);
        }
        return Status;
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static STATUS DiskDriverEntry(DRIVER_OBJECT *Driver)
{
    Driver->MajorFunction[IRP_MJ_READ] = DiskReadWrite;
    Driver->MajorFunction[IRP_MJ_WRITE] = DiskReadWrite;
    Driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DiskDeviceControl;
    KeInitializeMutex(&DiskLock);
    DiskDriver = Driver;
    char Value[64];
    if (BiGetOptionValue("hz.system", Value, sizeof(Value))) {
        DiskSystemGuidValid = DiskParseGuid(Value, DiskSystemGuid);
        if (!DiskSystemGuidValid)
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "Disk: hz.system=%s is not a partition GUID\n", Value);
    }
    return STATUS_SUCCESS;
}

IO_DECLARE_BUILTIN_DRIVER(DiskBuiltinDriver, "\\Driver\\Disk", DiskDriverEntry, IO_ORDER_CLASS);

/* ---- Partition tables --------------------------------------------------------------------------- */

typedef struct _DISK_PARTITION {
    uint64_t StartLba;
    uint64_t SectorCount;
    uint8_t TypeGuid[16];
    uint8_t UniqueGuid[16];
    char Name[40];
} DISK_PARTITION;

static const char *DiskTypeName(const uint8_t Type[16], uint32_t Style)
{
    if (Style == HZ_PARTITION_STYLE_MBR) {
        switch (Type[0]) {
        case 0x0B:
        case 0x0C: return "FAT32";
        case 0x07: return "NTFS/exFAT";
        case 0xEF: return "EFI system";
        default: return "MBR partition";
        }
    }
    if (!memcmp(Type, DiskBasicDataType, 16))
        return "basic data";
    if (!memcmp(Type, DiskEfiSystemType, 16))
        return "EFI system";
    if (!memcmp(Type, DiskTestScratchType, 16))
        return "Nexora OS test scratch";
    return "GPT partition";
}

static bool DiskIsEfiSystem(const uint8_t Type[16], uint32_t Style)
{
    return Style == HZ_PARTITION_STYLE_GPT ? !memcmp(Type, DiskEfiSystemType, 16) : Type[0] == 0xEF;
}

static uint32_t DiskReadGpt(const DISK_DESCRIPTION *Disk, uint8_t *Sector, DISK_PARTITION *Out, bool *Valid)
{
    *Valid = false;
    uint32_t Ss = Disk->SectorSize;
    if (!HZ_SUCCESS(Disk->Transfer(Disk->Context, 1, 1, Sector, false)) || memcmp(Sector, "EFI PART", 8) != 0)
        return 0;
    uint32_t HeaderSize, HeaderCrc, EntryCount, EntrySize, EntriesCrc;
    uint64_t EntriesLba;
    memcpy(&HeaderSize, Sector + 12, 4);
    memcpy(&HeaderCrc, Sector + 16, 4);
    memcpy(&EntriesLba, Sector + 72, 8);
    memcpy(&EntryCount, Sector + 80, 4);
    memcpy(&EntrySize, Sector + 84, 4);
    memcpy(&EntriesCrc, Sector + 88, 4);
    if (HeaderSize < 92 || HeaderSize > Ss || EntrySize < 128 || EntrySize > 1024 || (EntrySize & 7) ||
        EntryCount == 0 || EntryCount > 1024 || EntriesLba >= Disk->SectorCount)
        return 0;
    memset(Sector + 16, 0, 4);
    if (RtlComputeCrc32(0, Sector, HeaderSize) != HeaderCrc) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Disk: GPT header checksum mismatch\n");
        return 0;
    }
    uint64_t Bytes = (uint64_t)EntryCount * EntrySize;
    uint32_t Sectors = (uint32_t)((Bytes + Ss - 1) / Ss);
    uint8_t *Entries = ExAllocatePoolWithTag(NonPagedPool, (size_t)Sectors * Ss, DISK_TAG);
    if (!Entries)
        return 0;
    uint32_t Count = 0;
    bool Ok = true;
    for (uint32_t i = 0; i < Sectors && Ok; i++)
        Ok = HZ_SUCCESS(Disk->Transfer(Disk->Context, EntriesLba + i, 1, Entries + (size_t)i * Ss, false));
    if (Ok && RtlComputeCrc32(0, Entries, (size_t)Bytes) != EntriesCrc) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Disk: GPT partition array checksum mismatch\n");
        Ok = false;
    }
    for (uint32_t i = 0; Ok && i < EntryCount && Count < DISK_MAX_PARTITIONS; i++) {
        const uint8_t *E = Entries + (size_t)i * EntrySize;
        static const uint8_t Zero[16];
        if (!memcmp(E, Zero, 16))
            continue;
        uint64_t First, Last;
        memcpy(&First, E + 32, 8);
        memcpy(&Last, E + 40, 8);
        if (Last < First || Last >= Disk->SectorCount)
            continue;
        DISK_PARTITION *P = &Out[Count++];
        memcpy(P->TypeGuid, E, 16);
        memcpy(P->UniqueGuid, E + 16, 16);
        P->StartLba = First;
        P->SectorCount = Last - First + 1;
        uint16_t Name16[36];
        memcpy(Name16, E + 56, sizeof(Name16));
        uint32_t Length = 0;
        while (Length < 36 && Name16[Length])
            Length++;
        if (RtlUtf16ToUtf8(Name16, Length, P->Name, sizeof(P->Name)) < 0)
            P->Name[0] = '\0';
    }
    ExFreePoolWithTag(Entries, DISK_TAG);
    *Valid = Ok;
    return Ok ? Count : 0;
}

static uint32_t DiskReadMbr(const DISK_DESCRIPTION *Disk, const uint8_t *Sector, DISK_PARTITION *Out)
{
    if (Sector[510] != 0x55 || Sector[511] != 0xAA)
        return 0;
    uint32_t Count = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *E = Sector + 446 + i * 16;
        uint8_t Type = E[4];
        uint32_t Start, Sectors;
        memcpy(&Start, E + 8, 4);
        memcpy(&Sectors, E + 12, 4);
        if (Type == 0 || Type == 0x05 || Type == 0x0F || Type == 0xEE || !Sectors ||
            (uint64_t)Start + Sectors > Disk->SectorCount)
            continue;
        DISK_PARTITION *P = &Out[Count++];
        memset(P, 0, sizeof(*P));
        P->TypeGuid[0] = Type;
        P->StartLba = Start;
        P->SectorCount = Sectors;
    }
    return Count;
}

/* Reads the partition table: GPT if valid, else MBR, else none. */
static STATUS DiskScan(const DISK_DESCRIPTION *Disk, DISK_PARTITION *Partitions, uint32_t *Count, uint32_t *Style)
{
    uint8_t *Sector = ExAllocatePoolZero(NonPagedPool, Disk->SectorSize, DISK_TAG);
    if (!Sector)
        return STATUS_NO_MEMORY;
    bool GptValid = false;
    *Style = HZ_PARTITION_STYLE_RAW;
    *Count = DiskReadGpt(Disk, Sector, Partitions, &GptValid);
    if (GptValid) {
        *Style = HZ_PARTITION_STYLE_GPT;
    } else if (HZ_SUCCESS(Disk->Transfer(Disk->Context, 0, 1, Sector, false))) {
        *Count = DiskReadMbr(Disk, Sector, Partitions);
        if (*Count)
            *Style = HZ_PARTITION_STYLE_MBR;
    } else {
        *Count = 0;
    }
    ExFreePoolWithTag(Sector, DISK_TAG);
    return STATUS_SUCCESS;
}

/* ---- Devices ------------------------------------------------------------------------------------- */

static void DiskSetPartition(DISK_EXTENSION *Ext, DEVICE_OBJECT *Device, const DISK_PARTITION *Partition,
                             uint64_t DiskSectors)
{
    Ext->StartLba = Partition ? Partition->StartLba : 0;
    Ext->SectorCount = Partition ? Partition->SectorCount : DiskSectors;
    memset(Ext->TypeGuid, 0, 16);
    memset(Ext->UniqueGuid, 0, 16);
    Ext->Name[0] = '\0';
    if (Partition) {
        memcpy(Ext->TypeGuid, Partition->TypeGuid, 16);
        memcpy(Ext->UniqueGuid, Partition->UniqueGuid, 16);
        RtlStringCopy(Ext->Name, sizeof(Ext->Name), Partition->Name);
    }
    Device->SectorCount = Ext->SectorCount;
}

static STATUS DiskCreateDevice(const DISK_DESCRIPTION *Disk, uint32_t DiskNumber, uint32_t PartitionNumber,
                               uint32_t Style, const DISK_PARTITION *Partition, const char *Path, DEVICE_OBJECT **Out)
{
    DEVICE_OBJECT *Device;
    STATUS Status = IoCreateDevice(DiskDriver, sizeof(DISK_EXTENSION), Path, FILE_DEVICE_DISK,
                                   DO_STORAGE | (Disk->ReadOnly ? DO_READ_ONLY : 0), &Device);
    if (!HZ_SUCCESS(Status))
        return Status;
    DISK_EXTENSION *Ext = Device->Extension;
    Ext->Disk = *Disk;
    Ext->DiskNumber = DiskNumber;
    Ext->PartitionNumber = PartitionNumber;
    Ext->Style = Style;
    DiskSetPartition(Ext, Device, Partition, Disk->SectorCount);
    Device->SectorSize = Disk->SectorSize;
    *Out = Device;
    return STATUS_SUCCESS;
}

/* Registers a partition as storage: the system volume if it is the hz.system partition, without a
   drive letter if it is an EFI system partition. */
static void DiskRegisterPartition(DEVICE_OBJECT *Device, const char *Path)
{
    DISK_EXTENSION *Ext = Device->Extension;
    uint32_t Flags = 0;
    if (DiskIsEfiSystem(Ext->TypeGuid, Ext->Style))
        Flags |= IO_STORAGE_HIDDEN;
    if (DiskSystemGuidValid && Ext->Style == HZ_PARTITION_STYLE_GPT && !memcmp(Ext->UniqueGuid, DiskSystemGuid, 16)) {
        Flags |= IO_STORAGE_SYSTEM;
        DbgPrint("Disk: %s is the system partition (hz.system)\n", Path);
    }
    IoRegisterStorageDevice(Device, Path, Flags);
    Ext->Registered = true;
}

static void DiskLogPartition(const char *Path, const DISK_PARTITION *P, uint32_t Style, uint32_t SectorSize)
{
    char Size[24];
    RtlFormatByteSize(Size, sizeof(Size), P->SectorCount * SectorSize);
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "Disk: %s: %s at LBA %llu, %s%s%s%s\n", Path, Size, (unsigned long long)P->StartLba,
               DiskTypeName(P->TypeGuid, Style), P->Name[0] ? " '" : "", P->Name, P->Name[0] ? "'" : "");
}

STATUS DiskCreateDisk(const DISK_DESCRIPTION *Disk, uint32_t *DiskNumberOut)
{
    if (!DiskDriver)
        return STATUS_DEVICE_NOT_READY;
    if (Disk->SectorSize < 512 || Disk->SectorSize > 4096 || (Disk->SectorSize & (Disk->SectorSize - 1)) ||
        !Disk->MaxTransferSectors)
        return STATUS_NOT_SUPPORTED;
    uint32_t Number = DiskCount++;
    char Path[64];
    RtlFormatString(Path, sizeof(Path), "\\Device\\Harddisk%u", Number);
    STATUS Status = ObCreateDirectoryObject(Path);
    if (!HZ_SUCCESS(Status))
        return Status;

    DISK_PARTITION *Partitions = ExAllocatePoolZero(NonPagedPool, DISK_MAX_PARTITIONS * sizeof(DISK_PARTITION), DISK_TAG);
    if (!Partitions)
        return STATUS_NO_MEMORY;
    uint32_t Count = 0, Style = HZ_PARTITION_STYLE_RAW;
    DiskScan(Disk, Partitions, &Count, &Style);

    DEVICE_OBJECT *Whole;
    char DevicePath[80];
    RtlFormatString(DevicePath, sizeof(DevicePath), "%s\\DR0", Path);
    Status = DiskCreateDevice(Disk, Number, 0, Style, NULL, DevicePath, &Whole);
    if (HZ_SUCCESS(Status)) {
        char Link[80];
        RtlFormatString(Link, sizeof(Link), "%s\\Partition0", Path);
        ObCreateSymbolicLink(Link, DevicePath);
        if (Count == 0) { /* a superfloppy may still hold a volume */
            IoRegisterStorageDevice(Whole, DevicePath, 0);
            ((DISK_EXTENSION *)Whole->Extension)->Registered = true;
        }
    }

    char Size[24];
    RtlFormatByteSize(Size, sizeof(Size), Disk->SectorCount * Disk->SectorSize);
    DbgPrint("Disk: Harddisk%u is '%s', %s (%u-byte sectors), %s%s\n", Number, Disk->Model, Size, Disk->SectorSize,
             Style == HZ_PARTITION_STYLE_GPT ? "GPT" : Style == HZ_PARTITION_STYLE_MBR ? "MBR" : "no partition table",
             Disk->ReadOnly ? ", read-only" : "");

    for (uint32_t i = 0; HZ_SUCCESS(Status) && i < Count; i++) {
        DEVICE_OBJECT *Part;
        RtlFormatString(DevicePath, sizeof(DevicePath), "%s\\Partition%u", Path, i + 1);
        if (!HZ_SUCCESS(DiskCreateDevice(Disk, Number, i + 1, Style, &Partitions[i], DevicePath, &Part)))
            continue;
        DiskRegisterPartition(Part, DevicePath);
        DiskLogPartition(DevicePath, &Partitions[i], Style, Disk->SectorSize);
    }
    ExFreePoolWithTag(Partitions, DISK_TAG);
    if (DiskNumberOut)
        *DiskNumberOut = Number;
    return Status;
}

/* Reads the table of Whole's disk again and brings its partition devices in line with it. */
static STATUS DiskUpdatePartitions(DEVICE_OBJECT *Whole, uint32_t *CountOut)
{
    DISK_EXTENSION *WholeExt = Whole->Extension;
    DISK_PARTITION *Partitions = ExAllocatePoolZero(NonPagedPool, DISK_MAX_PARTITIONS * sizeof(DISK_PARTITION), DISK_TAG);
    if (!Partitions)
        return STATUS_NO_MEMORY;
    KeWaitForSingleObject(&DiskLock, NULL);
    IoAcquireMountLock(); /* no volume of this disk may be mounted meanwhile */
    STATUS Status = STATUS_SUCCESS;
    for (DEVICE_OBJECT *D = DiskDriver->DeviceList; D; D = D->NextDevice) {
        DISK_EXTENSION *E = D->Extension;
        if (E->DiskNumber == WholeExt->DiskNumber && (DiskIsMounted(D) || IoIsSystemStorage(D)))
            Status = STATUS_DEVICE_BUSY;
    }
    uint32_t Count = 0, Style = HZ_PARTITION_STYLE_RAW;
    if (HZ_SUCCESS(Status))
        Status = DiskScan(&WholeExt->Disk, Partitions, &Count, &Style);
    if (HZ_SUCCESS(Status)) {
        /* Existing devices: the whole disk and partitions 1..n. */
        DEVICE_OBJECT *Existing[DISK_MAX_PARTITIONS + 1];
        memset(Existing, 0, sizeof(Existing));
        for (DEVICE_OBJECT *D = DiskDriver->DeviceList; D; D = D->NextDevice) {
            DISK_EXTENSION *E = D->Extension;
            if (E->DiskNumber == WholeExt->DiskNumber && E->PartitionNumber <= DISK_MAX_PARTITIONS)
                Existing[E->PartitionNumber] = D;
        }
        for (uint32_t i = 0; i <= DISK_MAX_PARTITIONS; i++) {
            DEVICE_OBJECT *D = Existing[i];
            if (D) {
                DISK_EXTENSION *E = D->Extension;
                E->Style = Style;
                if (i > 0)
                    DiskSetPartition(E, D, i <= Count ? &Partitions[i - 1] : NULL, 0);
                if (i > Count && i > 0) {
                    E->SectorCount = 0; /* gone */
                    D->SectorCount = 0;
                }
                if (D->Vpb)
                    D->Vpb->Flags &= ~VPB_UNRECOGNIZED; /* may hold a new volume now */
            }
        }
        char Path[64], DevicePath[80];
        RtlFormatString(Path, sizeof(Path), "\\Device\\Harddisk%u", WholeExt->DiskNumber);
        for (uint32_t i = 0; i < Count; i++) {
            RtlFormatString(DevicePath, sizeof(DevicePath), "%s\\Partition%u", Path, i + 1);
            DEVICE_OBJECT *D = Existing[i + 1];
            if (!D && !HZ_SUCCESS(DiskCreateDevice(&WholeExt->Disk, WholeExt->DiskNumber, i + 1, Style, &Partitions[i],
                                                    DevicePath, &D)))
                continue;
            if (!((DISK_EXTENSION *)D->Extension)->Registered)
                DiskRegisterPartition(D, DevicePath);
            DiskLogPartition(DevicePath, &Partitions[i], Style, WholeExt->Disk.SectorSize);
        }
        DbgPrint("Disk: Harddisk%u partition table read again: %s, %u partition(s)\n", WholeExt->DiskNumber,
                 Style == HZ_PARTITION_STYLE_GPT ? "GPT" : Style == HZ_PARTITION_STYLE_MBR ? "MBR" : "none", Count);
        *CountOut = Count;
    }
    IoReleaseMountLock();
    KeReleaseMutex(&DiskLock);
    ExFreePoolWithTag(Partitions, DISK_TAG);
    return Status;
}

DEVICE_OBJECT *DiskFindPartitionByType(const uint8_t TypeGuid[16])
{
    if (!DiskDriver)
        return NULL;
    for (DEVICE_OBJECT *Device = DiskDriver->DeviceList; Device; Device = Device->NextDevice) {
        DISK_EXTENSION *Ext = Device->Extension;
        if (Ext->PartitionNumber && Ext->SectorCount && Ext->Style == HZ_PARTITION_STYLE_GPT &&
            !memcmp(Ext->TypeGuid, TypeGuid, 16)) {
            ObReferenceObject(Device);
            return Device;
        }
    }
    return NULL;
}

void DiskDescribeSystemGuid(char *Out, size_t Size)
{
    if (DiskSystemGuidValid)
        DiskFormatGuid(DiskSystemGuid, Out, Size);
    else if (Size)
        Out[0] = '\0';
}
