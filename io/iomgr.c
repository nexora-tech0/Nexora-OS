/*
 * Nexora OS - io/iomgr.c
 * Driver, device and file objects; IRP dispatch; volume mounting; drive letters; the built-in
 * driver table.
 */
#include "iop.h"

OBJECT_TYPE *IoDriverObjectType;
OBJECT_TYPE *IoDeviceObjectType;
OBJECT_TYPE *IoFileObjectType;

static FILE_SYSTEM *IopFileSystems;
static KMUTEX IopMountLock;

#define IOP_MAX_STORAGE 32
typedef struct _IOP_STORAGE_ENTRY {
    DEVICE_OBJECT *Device;
    char Path[64];
    uint32_t Flags;   /* IO_STORAGE_* */
    char DriveLetter; /* 0 = none */
} IOP_STORAGE_ENTRY;
static IOP_STORAGE_ENTRY IopStorage[IOP_MAX_STORAGE];
static uint32_t IopStorageCount;
static KSPIN_LOCK IopStorageLock; /* registration after boot (partition updates) */
static DEVICE_OBJECT *IopSystemStorage; /* behind C: */
static bool IopLiveSystem;

extern const IO_BUILTIN_DRIVER __DriverTableStart[], __DriverTableEnd[];

/* ---- Objects --------------------------------------------------------------------------------- */

static void IopDeleteFile(void *Object)
{
    FILE_OBJECT *File = Object;
    if (File->Flags & FO_OPENED) {
        IRP Irp;
        IoInitializeIrp(&Irp, IRP_MJ_CLOSE, File, NULL, 0);
        IoCallDriver(File->Device, &Irp);
    }
    if (File->Device)
        ObDereferenceObject(File->Device);
}

void IoInitializeSystem(void)
{
    KeInitializeMutex(&IopMountLock);
    KeInitializeSpinLock(&IopStorageLock);

    OBJECT_TYPE_INITIALIZER Init = {0};
    Init.PoolTag = HZ_POOL_TAG('I', 'o', 'D', 'r');
    if (!HZ_SUCCESS(ObCreateObjectType("Driver", &Init, &IoDriverObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0x12, 0, 0);

    Init.PoolTag = HZ_POOL_TAG('I', 'o', 'D', 'v');
    Init.ParseProcedure = IopParseDevice;
    if (!HZ_SUCCESS(ObCreateObjectType("Device", &Init, &IoDeviceObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0x10, 0, 0);

    OBJECT_TYPE_INITIALIZER FileInit = {0};
    FileInit.PoolTag = IOP_FILE_TAG;
    FileInit.ValidAccessMask = FILE_ALL_ACCESS;
    FileInit.DeleteProcedure = IopDeleteFile;
    if (!HZ_SUCCESS(ObCreateObjectType("File", &FileInit, &IoFileObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0x11, 0, 0);
}

STATUS IoCreateDriver(const char *Name, DRIVER_OBJECT **Out)
{
    DRIVER_OBJECT *Driver;
    STATUS Status = ObCreateObject(IoDriverObjectType, sizeof(DRIVER_OBJECT), Name, (void **)&Driver);
    if (!HZ_SUCCESS(Status))
        return Status;
    const char *Leaf = Name;
    for (const char *p = Name; *p; p++) {
        if (*p == '\\')
            Leaf = p + 1;
    }
    RtlStringCopy(Driver->Name, sizeof(Driver->Name), Leaf);
    ObDereferenceObject(Driver); /* permanent: the namespace keeps it */
    *Out = Driver;
    return STATUS_SUCCESS;
}

STATUS IoCreateDevice(DRIVER_OBJECT *Driver, size_t ExtensionSize, const char *Name, uint32_t DeviceType, uint32_t Flags,
                      DEVICE_OBJECT **Out)
{
    size_t VpbOffset = HZ_ALIGN_UP(sizeof(DEVICE_OBJECT), 16);
    size_t ExtensionOffset = VpbOffset + ((Flags & DO_STORAGE) ? HZ_ALIGN_UP(sizeof(VPB), 16) : 0);
    DEVICE_OBJECT *Device;
    STATUS Status = ObCreateObject(IoDeviceObjectType, ExtensionOffset + ExtensionSize, Name, (void **)&Device);
    if (!HZ_SUCCESS(Status))
        return Status;
    Device->Driver = Driver;
    Device->DeviceType = DeviceType;
    Device->Flags = Flags;
    if (Flags & DO_STORAGE) {
        Device->Vpb = (VPB *)((uint8_t *)Device + VpbOffset);
        Device->Vpb->StorageDevice = Device;
    }
    Device->Extension = ExtensionSize ? (uint8_t *)Device + ExtensionOffset : NULL;
    Device->NextDevice = Driver->DeviceList;
    Driver->DeviceList = Device;
    if (Name)
        ObDereferenceObject(Device); /* named devices are permanent */
    *Out = Device;
    return STATUS_SUCCESS;
}

void IoInitializeIrp(IRP *Irp, uint8_t MajorFunction, FILE_OBJECT *File, void *Buffer, uint32_t Length)
{
    memset(Irp, 0, sizeof(*Irp));
    Irp->MajorFunction = MajorFunction;
    Irp->RequestorMode = KernelMode;
    Irp->FileObject = File;
    Irp->Buffer = Buffer;
    Irp->Length = Length;
}

STATUS IoCallDriver(DEVICE_OBJECT *Device, IRP *Irp)
{
    DRIVER_DISPATCH Dispatch = Irp->MajorFunction < IRP_MJ_MAXIMUM ? Device->Driver->MajorFunction[Irp->MajorFunction] : NULL;
    STATUS Status;
    if (Dispatch) {
        Status = Dispatch(Device, Irp);
    } else if (Irp->MajorFunction == IRP_MJ_CREATE || Irp->MajorFunction == IRP_MJ_CLOSE ||
               Irp->MajorFunction == IRP_MJ_FLUSH_BUFFERS) {
        Status = STATUS_SUCCESS; /* nothing to do for drivers without per-open state */
        Irp->IoStatus.Information = Irp->MajorFunction == IRP_MJ_CREATE ? FILE_OPENED : 0;
    } else {
        Status = STATUS_INVALID_DEVICE_REQUEST;
    }
    Irp->IoStatus.Status = Status;
    return Status;
}

static STATUS IopSectorTransfer(DEVICE_OBJECT *Storage, uint8_t Major, uint64_t Sector, uint32_t Count, void *Buffer)
{
    if (!Storage->SectorSize)
        return STATUS_INVALID_DEVICE_REQUEST;
    uint64_t Bytes = (uint64_t)Count * Storage->SectorSize;
    if (Bytes > UINT32_MAX)
        return STATUS_INVALID_PARAMETER;
    IRP Irp;
    IoInitializeIrp(&Irp, Major, NULL, Buffer, (uint32_t)Bytes);
    Irp.Parameters.ReadWrite.ByteOffset = Sector * Storage->SectorSize;
    STATUS Status = IoCallDriver(Storage, &Irp);
    if (HZ_SUCCESS(Status) && Irp.IoStatus.Information != Bytes)
        Status = STATUS_DEVICE_DATA_ERROR;
    return Status;
}

STATUS IoReadSectors(DEVICE_OBJECT *Storage, uint64_t Sector, uint32_t Count, void *Buffer)
{
    return IopSectorTransfer(Storage, IRP_MJ_READ, Sector, Count, Buffer);
}

STATUS IoWriteSectors(DEVICE_OBJECT *Storage, uint64_t Sector, uint32_t Count, const void *Buffer)
{
    return IopSectorTransfer(Storage, IRP_MJ_WRITE, Sector, Count, (void *)Buffer);
}

STATUS IoDeviceControl(DEVICE_OBJECT *Device, uint32_t Code, const void *Input, uint32_t InputLength, void *Output,
                       uint32_t OutputLength, uint32_t *Returned)
{
    IRP Irp;
    IoInitializeIrp(&Irp, IRP_MJ_DEVICE_CONTROL, NULL, Output, OutputLength);
    Irp.Parameters.DeviceControl.Code = Code;
    Irp.Parameters.DeviceControl.Input = Input;
    Irp.Parameters.DeviceControl.InputLength = InputLength;
    STATUS Status = IoCallDriver(Device, &Irp);
    if (Returned)
        *Returned = (uint32_t)Irp.IoStatus.Information;
    return Status;
}

/* ---- File systems and mounting --------------------------------------------------------------- */

void IoRegisterFileSystem(FILE_SYSTEM *FileSystem)
{
    FileSystem->Next = IopFileSystems;
    IopFileSystems = FileSystem;
}

STATUS IoMountVolume(DEVICE_OBJECT *Storage)
{
    VPB *Vpb = Storage->Vpb;
    if (!Vpb)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Vpb->Flags & VPB_MOUNTED)
        return STATUS_SUCCESS;
    KeWaitForSingleObject(&IopMountLock, NULL);
    STATUS Status = STATUS_UNRECOGNIZED_VOLUME;
    if (Vpb->Flags & VPB_MOUNTED) {
        Status = STATUS_SUCCESS;
    } else if (!(Vpb->Flags & VPB_UNRECOGNIZED)) {
        for (FILE_SYSTEM *Fs = IopFileSystems; Fs; Fs = Fs->Next) {
            DEVICE_OBJECT *Volume = NULL;
            Status = Fs->Mount(Storage, Vpb, &Volume);
            if (HZ_SUCCESS(Status)) {
                Vpb->VolumeDevice = Volume;
                RtlStringCopy(Vpb->FileSystem, sizeof(Vpb->FileSystem), Fs->Name);
                Vpb->Flags |= VPB_MOUNTED;
                break;
            }
            if (Status != STATUS_UNRECOGNIZED_VOLUME)
                DbgPrintEx(DPFLTR_WARNING_LEVEL, "Volume: %s could not mount a volume it recognized (0x%08X)\n",
                           Fs->Name, (uint32_t)Status);
        }
        if (!(Vpb->Flags & VPB_MOUNTED)) {
            Vpb->Flags |= VPB_UNRECOGNIZED;
            Status = STATUS_UNRECOGNIZED_VOLUME;
        }
    }
    KeReleaseMutex(&IopMountLock);
    return Status;
}

void IoAcquireMountLock(void)
{
    KeWaitForSingleObject(&IopMountLock, NULL);
}

void IoReleaseMountLock(void)
{
    KeReleaseMutex(&IopMountLock);
}

void IoRegisterStorageDevice(DEVICE_OBJECT *Storage, const char *Path, uint32_t Flags)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&IopStorageLock, &OldIrql);
    if (IopStorageCount < IOP_MAX_STORAGE) {
        IOP_STORAGE_ENTRY *Entry = &IopStorage[IopStorageCount];
        Entry->Device = Storage;
        Entry->Flags = Flags;
        Entry->DriveLetter = 0;
        RtlStringCopy(Entry->Path, sizeof(Entry->Path), Path);
        IopStorageCount++;
    }
    KeReleaseSpinLock(&IopStorageLock, OldIrql);
}

bool IoIsSystemStorage(DEVICE_OBJECT *Storage)
{
    return Storage && Storage == IopSystemStorage;
}

bool IoIsLiveSystem(void)
{
    return IopLiveSystem;
}

static bool IopLinkDriveLetter(IOP_STORAGE_ENTRY *Entry, char Letter)
{
    char Link[8];
    RtlFormatString(Link, sizeof(Link), "\\??\\%c:", Letter);
    STATUS Status = ObCreateSymbolicLink(Link, Entry->Path);
    if (!HZ_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Volume: could not create %s (0x%08X)\n", Link, (uint32_t)Status);
        return false;
    }
    Entry->DriveLetter = Letter;
    return true;
}

static void IopAssignDriveLetters(void)
{
    /* C: is the system volume if one was named and mounts, else the live image. */
    bool SystemRequested = false;
    for (uint32_t Pass = 0; Pass < 2 && !IopSystemStorage; Pass++) {
        uint32_t Wanted = Pass == 0 ? IO_STORAGE_SYSTEM : IO_STORAGE_LIVE;
        for (uint32_t i = 0; i < IopStorageCount && !IopSystemStorage; i++) {
            IOP_STORAGE_ENTRY *Entry = &IopStorage[i];
            if (!(Entry->Flags & Wanted))
                continue;
            SystemRequested |= Pass == 0;
            if (HZ_SUCCESS(IoMountVolume(Entry->Device)) && IopLinkDriveLetter(Entry, 'C')) {
                IopSystemStorage = Entry->Device;
                IopLiveSystem = Pass == 1;
            }
        }
    }
    if (SystemRequested && IopLiveSystem)
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Volume: the system partition did not mount; running from the live image\n");
    /* The live image stays mounted without a letter when it is not C: (\Device\Ramdisk0\...). */
    for (uint32_t i = 0; i < IopStorageCount; i++)
        if ((IopStorage[i].Flags & IO_STORAGE_LIVE) && !IopStorage[i].DriveLetter)
            IoMountVolume(IopStorage[i].Device);
    char Next = 'D';
    for (uint32_t i = 0; i < IopStorageCount && Next <= 'Z'; i++) {
        IOP_STORAGE_ENTRY *Entry = &IopStorage[i];
        if (Entry->DriveLetter || (Entry->Flags & (IO_STORAGE_HIDDEN | IO_STORAGE_LIVE | IO_STORAGE_SYSTEM)))
            continue;
        if (HZ_SUCCESS(IoMountVolume(Entry->Device)) && IopLinkDriveLetter(Entry, Next))
            Next++;
    }
}

void IoReportVolumes(void)
{
    /* Drive letters in order, then the storage without a recognized file system. */
    for (char Letter = 'C'; Letter <= 'Z'; Letter++) {
        for (uint32_t i = 0; i < IopStorageCount; i++) {
            IOP_STORAGE_ENTRY *Entry = &IopStorage[i];
            if (Entry->DriveLetter != Letter)
                continue;
            VPB *Vpb = Entry->Device->Vpb;
            char Size[24];
            RtlFormatByteSize(Size, sizeof(Size), Entry->Device->SectorCount * Entry->Device->SectorSize);
            DbgPrint("Volume: %c: is %s, %s '%s', %s%s\n", Letter, Entry->Path, Vpb->FileSystem, Vpb->Label, Size,
                     (Entry->Device->Flags & DO_READ_ONLY) ? ", read-only" : "");
        }
    }
    for (uint32_t i = 0; i < IopStorageCount; i++) {
        IOP_STORAGE_ENTRY *Entry = &IopStorage[i];
        if (Entry->DriveLetter)
            continue;
        char Size[24];
        RtlFormatByteSize(Size, sizeof(Size), Entry->Device->SectorCount * Entry->Device->SectorSize);
        if (Entry->Flags & (IO_STORAGE_HIDDEN | IO_STORAGE_LIVE))
            DbgPrintEx(DPFLTR_TRACE_LEVEL, "Volume: %s (%s) has no drive letter (%s)\n", Entry->Path, Size,
                       (Entry->Flags & IO_STORAGE_LIVE) ? "the live image" : "hidden");
        else
            DbgPrintEx(DPFLTR_TRACE_LEVEL, "Volume: %s (%s) has no recognized file system\n", Entry->Path, Size);
    }
}

void IoShutdownSystem(void)
{
    for (uint32_t i = 0; i < IopStorageCount; i++) {
        VPB *Vpb = IopStorage[i].Device->Vpb;
        if (Vpb && (Vpb->Flags & VPB_MOUNTED)) {
            IRP Irp;
            IoInitializeIrp(&Irp, IRP_MJ_FLUSH_BUFFERS, NULL, NULL, 0);
            IoCallDriver(Vpb->VolumeDevice, &Irp);
        }
    }
}

/* ---- Built-in drivers ---------------------------------------------------------------------- */

void IoInitializeDrivers(void)
{
    const IO_BUILTIN_DRIVER *Sorted[64];
    uint32_t Count = 0;
    for (const IO_BUILTIN_DRIVER *D = __DriverTableStart; D < __DriverTableEnd && Count < HZ_ARRAYSIZE(Sorted); D++) {
        uint32_t i = Count++;
        while (i > 0 && Sorted[i - 1]->Order > D->Order) {
            Sorted[i] = Sorted[i - 1];
            i--;
        }
        Sorted[i] = D;
    }
    for (uint32_t i = 0; i < Count; i++) {
        DRIVER_OBJECT *Driver;
        STATUS Status = IoCreateDriver(Sorted[i]->Name, &Driver);
        if (HZ_SUCCESS(Status))
            Status = Sorted[i]->Entry(Driver);
        if (!HZ_SUCCESS(Status) && Status != STATUS_NO_SUCH_DEVICE)
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "I/O: driver %s failed to start (0x%08X)\n", Sorted[i]->Name,
                       (uint32_t)Status);
    }
    IopAssignDriveLetters();
}

/* ---- Opening files ---------------------------------------------------------------------------- */

ACCESS_MASK IopMapFileAccess(ACCESS_MASK Access)
{
    if (Access & GENERIC_ALL)
        Access |= FILE_ALL_ACCESS;
    if (Access & GENERIC_READ)
        Access |= FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    if (Access & GENERIC_WRITE)
        Access |= FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
    return Access & FILE_ALL_ACCESS;
}

STATUS IopCreateFileObject(DEVICE_OBJECT *Device, VPB *Vpb, const char *Path, FILE_OBJECT **Out)
{
    FILE_OBJECT *File;
    STATUS Status = ObCreateObject(IoFileObjectType, sizeof(FILE_OBJECT), NULL, (void **)&File);
    if (!HZ_SUCCESS(Status))
        return Status;
    ObReferenceObject(Device);
    File->Device = Device;
    File->Vpb = Vpb;
    KeInitializeMutex(&File->Lock);
    RtlStringCopy(File->Path, sizeof(File->Path), Path);
    *Out = File;
    return STATUS_SUCCESS;
}

static STATUS IopSendCreate(FILE_OBJECT *File, const char *Path, const IOP_OPEN_PARAMETERS *Open, uint32_t *Result)
{
    IRP Irp;
    IoInitializeIrp(&Irp, IRP_MJ_CREATE, File, NULL, 0);
    Irp.RequestorMode = (uint8_t)Open->Mode;
    Irp.Parameters.Create.Path = Path;
    Irp.Parameters.Create.DesiredAccess = Open->DesiredAccess;
    Irp.Parameters.Create.Disposition = Open->Disposition;
    Irp.Parameters.Create.Options = Open->Options;
    STATUS Status = IoCallDriver(File->Device, &Irp);
    if (HZ_SUCCESS(Status)) {
        File->Flags |= FO_OPENED;
        File->GrantedAccess = Open->DesiredAccess;
        *Result = (uint32_t)Irp.IoStatus.Information;
    }
    return Status;
}

STATUS IopParseDevice(void *Object, const char *RemainingPath, void *ParseContext, void **Result)
{
    DEVICE_OBJECT *Device = Object;
    IOP_OPEN_PARAMETERS Default = {FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN, 0, KernelMode, 0};
    IOP_OPEN_PARAMETERS *Open = ParseContext ? ParseContext : &Default;
    if (!(Device->Flags & DO_STORAGE))
        return STATUS_OBJECT_PATH_NOT_FOUND; /* only volumes have paths below them */
    STATUS Status = IoMountVolume(Device);
    if (!HZ_SUCCESS(Status))
        return Status;

    FILE_OBJECT *File;
    Status = IopCreateFileObject(Device->Vpb->VolumeDevice, Device->Vpb, RemainingPath, &File);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = IopSendCreate(File, RemainingPath, Open, &Open->Result);
    if (!HZ_SUCCESS(Status)) {
        ObDereferenceObject(File);
        return Status;
    }
    *Result = File;
    return STATUS_SUCCESS;
}

STATUS IoCreateFile(const char *Path, ACCESS_MASK DesiredAccess, uint32_t Disposition, uint32_t Options,
                    KPROCESSOR_MODE Mode, FILE_OBJECT **Out, uint32_t *Result)
{
    if (Disposition > FILE_OVERWRITE_IF ||
        (Options & ~(FILE_DIRECTORY_FILE | FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE)) ||
        ((Options & FILE_DIRECTORY_FILE) && (Options & FILE_NON_DIRECTORY_FILE)))
        return STATUS_INVALID_PARAMETER;
    IOP_OPEN_PARAMETERS Open = {IopMapFileAccess(DesiredAccess), Disposition, Options, Mode, FILE_OPENED};
    if (Options & FILE_DELETE_ON_CLOSE)
        Open.DesiredAccess |= DELETE;

    void *Object;
    STATUS Status = ObReferenceObjectByNameEx(Path, NULL, &Open, &Object);
    if (!HZ_SUCCESS(Status))
        return Status;

    FILE_OBJECT *File;
    OBJECT_TYPE *Type = ObGetObjectType(Object);
    if (Type == IoFileObjectType) {
        File = Object;
    } else if (Type == IoDeviceObjectType && (((DEVICE_OBJECT *)Object)->Flags & DO_STORAGE) && Path[0] &&
               Path[strlen(Path) - 1] == '\\') {
        /* "X:\" or "\Device\...\Partition1\": the root directory of the mounted volume (the
           object manager consumed the trailing separator before reaching the device). */
        DEVICE_OBJECT *Device = Object;
        Status = IopParseDevice(Device, "\\", &Open, (void **)&File);
        ObDereferenceObject(Device);
        if (!HZ_SUCCESS(Status))
            return Status;
    } else if (Type == IoDeviceObjectType) {
        /* The path named a device: a raw open (keyboard, a whole disk, a partition). */
        DEVICE_OBJECT *Device = Object;
        if (Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF) {
            ObDereferenceObject(Device);
            return STATUS_OBJECT_NAME_COLLISION;
        }
        if ((Options & FILE_DIRECTORY_FILE) ||
            ((Device->Flags & DO_READ_ONLY) && (Open.DesiredAccess & (FILE_WRITE_DATA | DELETE)))) {
            ObDereferenceObject(Device);
            return (Options & FILE_DIRECTORY_FILE) ? STATUS_NOT_A_DIRECTORY : STATUS_MEDIA_WRITE_PROTECTED;
        }
        const char *Name = ObGetObjectName(Device);
        Status = IopCreateFileObject(Device, NULL, Name ? Name : "", &File);
        ObDereferenceObject(Device);
        if (!HZ_SUCCESS(Status))
            return Status;
        File->Flags |= FO_RAW_DEVICE;
        Status = IopSendCreate(File, "", &Open, &Open.Result);
        if (!HZ_SUCCESS(Status)) {
            ObDereferenceObject(File);
            return Status;
        }
    } else {
        ObDereferenceObject(Object);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    if (Options & FILE_DELETE_ON_CLOSE)
        File->Flags |= FO_DELETE_ON_CLOSE;
    *Out = File;
    if (Result)
        *Result = Open.Result;
    return STATUS_SUCCESS;
}

STATUS IoOpenFile(const char *Path, FILE_OBJECT **File)
{
    return IoCreateFile(Path, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE, KernelMode,
                        File, NULL);
}
