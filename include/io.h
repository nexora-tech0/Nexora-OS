/*
 * Nexora OS - include/io.h
 * I/O manager (Io*): driver and device objects, I/O request packets, file objects, volume
 * mounting and the kernel file API.
 *
 * Model (NT-like, simplified):
 *   - Every request is an IRP handed to the target device's driver through its MajorFunction
 *     table (IoCallDriver). Requests complete synchronously: the dispatch routine returns the
 *     final status (a driver that waits for hardware blocks inside its dispatch routine).
 *   - Buffered I/O only: IRP buffers are kernel memory. The system services copy to and from
 *     user buffers, so drivers and file systems never touch user addresses.
 *   - A storage device (DO_STORAGE: a partition, a whole disk, the ramdisk) has a VPB. The first
 *     path open below it ("\Device\Harddisk0\Partition1\dir\file") mounts it: each registered
 *     file system's Mount routine is tried until one recognizes the volume and creates a volume
 *     device. The open then goes to the volume device as IRP_MJ_CREATE with the remaining path.
 *   - Opening a device object itself ("\Device\Keyboard0", "\Device\Harddisk0\DR0") yields a
 *     raw file object on that device.
 *
 * Namespace: \Driver\<name> (driver objects), \FileSystem\<name> (file system drivers),
 * \Device\... (devices), \??\X: (drive letters, symbolic links to storage devices).
 */
#ifndef IO_H
#define IO_H

#include "hzbase.h"
#include "ke.h"
#include "ob.h"
#include "uapi/hzapi.h"

HZ_EXTERN_C_BEGIN

typedef struct _DRIVER_OBJECT DRIVER_OBJECT;
typedef struct _DEVICE_OBJECT DEVICE_OBJECT;
typedef struct _FILE_OBJECT FILE_OBJECT;
typedef struct _IRP IRP;
typedef struct _VPB VPB;

/* ---- IRPs ----------------------------------------------------------------------------------- */

typedef enum _IRP_MAJOR_FUNCTION {
    IRP_MJ_CREATE = 0,
    IRP_MJ_CLOSE,               /* the file object is being deleted */
    IRP_MJ_READ,
    IRP_MJ_WRITE,
    IRP_MJ_QUERY_INFORMATION,
    IRP_MJ_SET_INFORMATION,
    IRP_MJ_DIRECTORY_CONTROL,   /* enumerate a directory */
    IRP_MJ_DEVICE_CONTROL,
    IRP_MJ_FLUSH_BUFFERS,
    IRP_MJ_MAXIMUM
} IRP_MAJOR_FUNCTION;

typedef struct _IO_STATUS_BLOCK {
    STATUS Status;
    uint64_t Information; /* bytes transferred, FILE_OPENED/CREATED/..., entries returned */
} IO_STATUS_BLOCK;

struct _IRP {
    uint8_t MajorFunction;
    uint8_t RequestorMode;   /* UserMode if a system service started it (waits are alertable) */
    FILE_OBJECT *FileObject; /* NULL for requests a driver sends to a lower device */
    void *Buffer;            /* kernel buffer: data for read/write, output for queries */
    uint32_t Length;
    IO_STATUS_BLOCK IoStatus;
    union {
        struct {
            const char *Path;       /* remaining path inside the volume ("\dir\file", "" = root) */
            ACCESS_MASK DesiredAccess;
            uint32_t Disposition;   /* FILE_OPEN, ... */
            uint32_t Options;       /* FILE_DIRECTORY_FILE, ... */
        } Create;
        struct {
            uint64_t ByteOffset;
        } ReadWrite;
        struct {
            uint32_t Class;         /* HZ_FILE_*_INFORMATION_CLASS */
        } Information;
        struct {
            uint32_t Flags;         /* HZ_DIRECTORY_RESTART_SCAN */
        } Directory;
        struct {
            uint32_t Code;
            const void *Input;
            uint32_t InputLength;
        } DeviceControl;
    } Parameters;
};

typedef STATUS (*DRIVER_DISPATCH)(DEVICE_OBJECT *Device, IRP *Irp);

/* ---- Drivers and devices --------------------------------------------------------------------- */

struct _DRIVER_OBJECT {
    char Name[32];
    DRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM];
    DEVICE_OBJECT *DeviceList;
    void *Extension;
};

/* Device types */
#define FILE_DEVICE_UNKNOWN 0
#define FILE_DEVICE_DISK 1         /* whole disk, partition, ramdisk */
#define FILE_DEVICE_FILE_SYSTEM 2  /* mounted volume */
#define FILE_DEVICE_KEYBOARD 3
#define FILE_DEVICE_MOUSE 4
#define FILE_DEVICE_CONTROLLER 5
#define FILE_DEVICE_PIPE 6

/* Device flags */
#define DO_STORAGE 0x0001          /* can hold a file system (has a VPB) */
#define DO_READ_ONLY 0x0002        /* media is write-protected */
#define DO_EXCLUSIVE 0x0004        /* reserved */

struct _DEVICE_OBJECT {
    DRIVER_OBJECT *Driver;
    DEVICE_OBJECT *NextDevice;     /* in Driver->DeviceList */
    uint32_t DeviceType;
    uint32_t Flags;
    uint32_t SectorSize;           /* storage devices */
    uint64_t SectorCount;
    VPB *Vpb;                      /* storage devices */
    void *Extension;               /* driver-defined, allocated with the device */
};

/* Volume parameter block: links a storage device to the volume device of its file system. */
#define VPB_MOUNTED 0x1
#define VPB_UNRECOGNIZED 0x2       /* every file system declined it (raw opens still work) */

struct _VPB {
    DEVICE_OBJECT *StorageDevice;
    DEVICE_OBJECT *VolumeDevice;   /* referenced while mounted */
    uint32_t Flags;
    char FileSystem[16];
    char Label[32];
};

extern OBJECT_TYPE *IoDriverObjectType;
extern OBJECT_TYPE *IoDeviceObjectType;
extern OBJECT_TYPE *IoFileObjectType;

STATUS IoCreateDriver(const char *Name, DRIVER_OBJECT **Driver);       /* "\Driver\x" or "\FileSystem\x" */
/* Creates a device with ExtensionSize zeroed bytes of extension; Name may be NULL (unnamed). */
STATUS IoCreateDevice(DRIVER_OBJECT *Driver, size_t ExtensionSize, const char *Name, uint32_t DeviceType,
                      uint32_t Flags, DEVICE_OBJECT **Device);
STATUS IoCallDriver(DEVICE_OBJECT *Device, IRP *Irp);
void IoInitializeIrp(IRP *Irp, uint8_t MajorFunction, FILE_OBJECT *File, void *Buffer, uint32_t Length);

/* Sector I/O on a storage device (file systems use these on the device they mounted). */
STATUS IoReadSectors(DEVICE_OBJECT *Storage, uint64_t Sector, uint32_t Count, void *Buffer);
STATUS IoWriteSectors(DEVICE_OBJECT *Storage, uint64_t Sector, uint32_t Count, const void *Buffer);
STATUS IoDeviceControl(DEVICE_OBJECT *Device, uint32_t Code, const void *Input, uint32_t InputLength, void *Output,
                       uint32_t OutputLength, uint32_t *Returned);

/* ---- Built-in drivers ------------------------------------------------------------------------
 * Drivers are linked into the kernel and listed in the .hzdrivers section; IoInitializeDrivers
 * calls their entry routines in Order (bus drivers first). */

typedef STATUS (*DRIVER_INITIALIZE)(DRIVER_OBJECT *Driver);

typedef struct _IO_BUILTIN_DRIVER {
    const char *Name;              /* object path, "\Driver\Ahci" */
    DRIVER_INITIALIZE Entry;
    uint32_t Order;
} IO_BUILTIN_DRIVER;

#define IO_ORDER_BUS 10
#define IO_ORDER_FILE_SYSTEM 20
#define IO_ORDER_CLASS 30
#define IO_ORDER_DEVICE 40

#define IO_DECLARE_BUILTIN_DRIVER(Symbol, Path, Routine, OrderValue)                                     \
    __attribute__((used, section(".hzdrivers"), aligned(8))) static const IO_BUILTIN_DRIVER Symbol = { \
        .Name = Path, .Entry = Routine, .Order = OrderValue}

/* ---- File systems ------------------------------------------------------------------------------ */

typedef struct _FILE_SYSTEM {
    const char *Name;
    /* Recognizes the volume on Storage; on success creates the volume device (owned by the file
       system's driver) and fills Vpb->Label. STATUS_UNRECOGNIZED_VOLUME if it is not ours. */
    STATUS (*Mount)(DEVICE_OBJECT *Storage, VPB *Vpb, DEVICE_OBJECT **VolumeDevice);
    struct _FILE_SYSTEM *Next;
} FILE_SYSTEM;

void IoRegisterFileSystem(FILE_SYSTEM *FileSystem);
/* Mounts Storage if it is not mounted yet (trying every file system). */
STATUS IoMountVolume(DEVICE_OBJECT *Storage);

/* ---- File objects ------------------------------------------------------------------------------ */

#define FO_RAW_DEVICE 0x0001       /* opened on a device, no file system */
#define FO_DIRECTORY 0x0002
#define FO_DELETE_ON_CLOSE 0x0004
#define FO_PIPE 0x0008             /* an end of an anonymous pipe (io/iopipe.c) */

struct _FILE_OBJECT {
    DEVICE_OBJECT *Device;         /* referenced: the volume device, or the raw device */
    VPB *Vpb;                      /* volume of a file system file */
    void *FsContext;               /* file system: per-file state (shared by opens) */
    void *FsContext2;              /* file system: per-open state */
    uint64_t CurrentByteOffset;
    ACCESS_MASK GrantedAccess;
    uint32_t Flags;
    KMUTEX Lock;                   /* serializes requests on this file object */
    char Path[256];                /* path inside the volume or device, for logs */
};

/* Kernel file API (PASSIVE_LEVEL). Paths are object paths ("\??\C:\x", "\Device\Keyboard0").
   *File receives a referenced file object; release with ObDereferenceObject. */
STATUS IoCreateFile(const char *Path, ACCESS_MASK DesiredAccess, uint32_t Disposition, uint32_t Options,
                    KPROCESSOR_MODE Mode, FILE_OBJECT **File, uint32_t *Result);
STATUS IoOpenFile(const char *Path, FILE_OBJECT **File); /* existing non-directory, read access */
/* Offset NULL = current position (advanced by the transfer). */
STATUS IoReadFile(FILE_OBJECT *File, void *Buffer, uint32_t Length, const uint64_t *Offset, KPROCESSOR_MODE Mode,
                  uint32_t *Transferred);
STATUS IoWriteFile(FILE_OBJECT *File, const void *Buffer, uint32_t Length, const uint64_t *Offset,
                   KPROCESSOR_MODE Mode, uint32_t *Transferred);
STATUS IoQueryInformationFile(FILE_OBJECT *File, uint32_t Class, void *Buffer, uint32_t Length);
STATUS IoSetInformationFile(FILE_OBJECT *File, uint32_t Class, const void *Buffer, uint32_t Length);
STATUS IoQueryDirectoryFile(FILE_OBJECT *File, HZ_FILE_DIRECTORY_INFORMATION *Entries, uint32_t Count, uint32_t Flags,
                            uint32_t *Returned);
/* Anonymous pipes (io/iopipe.c): two referenced file objects, the read end (FILE_READ_DATA) and
   the write end (FILE_WRITE_DATA). Size 0 = IO_PIPE_DEFAULT_SIZE. */
#define IO_PIPE_DEFAULT_SIZE 16384u
#define IO_PIPE_MAXIMUM_SIZE (1024u * 1024u)
STATUS IoCreatePipe(uint32_t Size, FILE_OBJECT **ReadEnd, FILE_OBJECT **WriteEnd);
/* Reads a whole (small) file into a new pool buffer (tag 'IoRf'; free with ExFreePool). */
STATUS IoReadWholeFile(const char *Path, void **Buffer, uint64_t *Size, uint64_t MaximumSize);

/* ---- Initialization ------------------------------------------------------------------------- */

/* Phase 0 (after Ob): object types. */
void IoInitializeSystem(void);
/* Phase 1: runs every built-in driver, mounts the volumes found and assigns drive letters:
   C: is the system volume (IO_STORAGE_SYSTEM, from hz.system) if it mounts, else the live image
   (IO_STORAGE_LIVE, the boot ramdisk); then D:, E:, ... for the other mountable volumes. */
void IoInitializeDrivers(void);
/* Logs the drive letters and their file systems. */
void IoReportVolumes(void);
/* Updates on-disk volume state before power off (file systems are write-through already). */
void IoShutdownSystem(void);

/* Storage registration flags */
#define IO_STORAGE_SYSTEM 0x1u /* the installed system volume: C: when it mounts */
#define IO_STORAGE_LIVE 0x2u   /* the live image: C: unless a system volume mounted; no letter otherwise */
#define IO_STORAGE_HIDDEN 0x4u /* never gets a drive letter (EFI system partitions, the install image);
                                  reachable through its device path */
/* Registers a mountable storage device (by its object path) for drive-letter assignment by
   IoInitializeDrivers; devices registered later (a new partition table) get no letter. */
void IoRegisterStorageDevice(DEVICE_OBJECT *Storage, const char *Path, uint32_t Flags);
/* True if Storage is the device behind C:. */
bool IoIsSystemStorage(DEVICE_OBJECT *Storage);
/* True if C: is the live image (the system was not started from an installed disk). */
bool IoIsLiveSystem(void);
/* Serializes against mounting (a disk driver changing partitions checks VPB_MOUNTED under it). */
void IoAcquireMountLock(void);
void IoReleaseMountLock(void);

/* Runs the file system self-tests (io/iotest.c). */
bool IoRunSelfTests(void);

/* ---- System services (io/iosvc.c) ----------------------------------------------------------- */

STATUS NtCreateFile(const uint64_t *Args);
STATUS NtReadFile(const uint64_t *Args);
STATUS NtWriteFile(const uint64_t *Args);
STATUS NtQueryInformationFile(const uint64_t *Args);
STATUS NtSetInformationFile(const uint64_t *Args);
STATUS NtQueryDirectoryFile(const uint64_t *Args);
STATUS NtDeviceIoControlFile(const uint64_t *Args);
STATUS NtCreatePipe(const uint64_t *Args);

HZ_EXTERN_C_END

#endif /* IO_H */
