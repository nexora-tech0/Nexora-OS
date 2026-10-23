/*
 * Nexora OS - io/iosvc.c
 * File and device system services. User buffers are copied through pool bounce buffers in
 * chunks, so drivers and file systems only ever see kernel memory.
 *
 * Paths: "C:\dir\file" (a drive letter, resolved through \??\) or an object path starting with
 * '\' ("\Device\Keyboard0"). There is no current directory yet, so relative paths are rejected.
 */
#include "iop.h"
#include "ps.h"

#define IOP_CHUNK (256u * 1024)
#define IOP_MAX_INFO 4096u
#define IOP_MAX_IOCTL (64u * 1024)
#define IOP_MAX_DIRECTORY_ENTRIES 32u

static STATUS IopCaptureUserPath(uint64_t UserPath, char *Path, size_t Size)
{
    char Raw[256];
    STATUS Status = MmCopyStringFromUser(Raw, sizeof(Raw), (const char *)UserPath);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Raw[0] == '\\')
        RtlStringCopy(Path, Size, Raw);
    else if (((Raw[0] >= 'A' && Raw[0] <= 'Z') || (Raw[0] >= 'a' && Raw[0] <= 'z')) && Raw[1] == ':')
        RtlFormatString(Path, Size, "\\??\\%c%s", Raw[0] & ~0x20, Raw + 1);
    else
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    return STATUS_SUCCESS;
}

static STATUS IopProbe(uint64_t Pointer, size_t Length)
{
    static const uint8_t Zero[8];
    return Pointer ? MmCopyToUser((void *)Pointer, Zero, Length) : STATUS_SUCCESS;
}

static STATUS IopReferenceFile(uint64_t Handle, ACCESS_MASK Access, FILE_OBJECT **File)
{
    return ObReferenceObjectByHandle((HANDLE)Handle, IoFileObjectType, Access, UserMode, (void **)File);
}

/* CreateFile(HANDLE *File, ACCESS_MASK Access, const char *Path, uint32_t Disposition, uint32_t Options,
              uint32_t *Result) */
STATUS NtCreateFile(const uint64_t *Args)
{
    char Path[300];
    STATUS Status = IopProbe(Args[0], sizeof(HANDLE));
    if (HZ_SUCCESS(Status))
        Status = IopProbe(Args[5], sizeof(uint32_t));
    if (HZ_SUCCESS(Status))
        Status = IopCaptureUserPath(Args[2], Path, sizeof(Path));
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!Args[0])
        return STATUS_ACCESS_VIOLATION;

    FILE_OBJECT *File;
    uint32_t Result = 0;
    Status = IoCreateFile(Path, (ACCESS_MASK)Args[1], (uint32_t)Args[3], (uint32_t)Args[4], UserMode, &File, &Result);
    if (!HZ_SUCCESS(Status))
        return Status;
    HANDLE Handle;
    Status = ObOpenObjectByPointer(File, File->GrantedAccess | SYNCHRONIZE, NULL, &Handle);
    ObDereferenceObject(File);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = MmCopyToUser((void *)Args[0], &Handle, sizeof(Handle));
    if (!HZ_SUCCESS(Status)) {
        ObCloseHandle(Handle, KernelMode);
        return Status;
    }
    if (Args[5])
        MmCopyToUser((void *)Args[5], &Result, sizeof(Result));
    return STATUS_SUCCESS;
}

/* CreatePipe(HANDLE *ReadEnd, HANDLE *WriteEnd, uint32_t Size) */
STATUS NtCreatePipe(const uint64_t *Args)
{
    STATUS Status = IopProbe(Args[0], sizeof(HANDLE));
    if (HZ_SUCCESS(Status))
        Status = IopProbe(Args[1], sizeof(HANDLE));
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!Args[0] || !Args[1])
        return STATUS_ACCESS_VIOLATION;
    FILE_OBJECT *Read, *Write;
    Status = IoCreatePipe((uint32_t)Args[2], &Read, &Write);
    if (!HZ_SUCCESS(Status))
        return Status;
    HANDLE ReadHandle = 0, WriteHandle = 0;
    Status = ObOpenObjectByPointer(Read, Read->GrantedAccess, NULL, &ReadHandle);
    if (HZ_SUCCESS(Status))
        Status = ObOpenObjectByPointer(Write, Write->GrantedAccess, NULL, &WriteHandle);
    ObDereferenceObject(Read);
    ObDereferenceObject(Write);
    if (HZ_SUCCESS(Status))
        Status = MmCopyToUser((void *)Args[0], &ReadHandle, sizeof(HANDLE));
    if (HZ_SUCCESS(Status))
        Status = MmCopyToUser((void *)Args[1], &WriteHandle, sizeof(HANDLE));
    if (!HZ_SUCCESS(Status)) {
        if (ReadHandle)
            ObCloseHandle(ReadHandle, KernelMode);
        if (WriteHandle)
            ObCloseHandle(WriteHandle, KernelMode);
    }
    return Status;
}

static STATUS IopReadWrite(const uint64_t *Args, bool Write)
{
    uint32_t Length = (uint32_t)Args[2];
    uint64_t Offset = 0;
    bool HasOffset = Args[3] != 0;
    STATUS Status = IopProbe(Args[4], sizeof(uint32_t));
    if (HZ_SUCCESS(Status) && HasOffset)
        Status = MmCopyFromUser(&Offset, (const void *)Args[3], sizeof(Offset));
    if (HZ_SUCCESS(Status) && !MmIsUserRange((const void *)Args[1], Length))
        Status = STATUS_ACCESS_VIOLATION;
    if (!HZ_SUCCESS(Status))
        return Status;

    FILE_OBJECT *File;
    Status = IopReferenceFile(Args[0], Write ? FILE_WRITE_DATA : FILE_READ_DATA, &File);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint32_t ChunkSize = HZ_MIN(Length, IOP_CHUNK);
    uint8_t *Bounce = ExAllocatePoolWithTag(NonPagedPool, ChunkSize ? ChunkSize : 1, IOP_BUFFER_TAG);
    if (!Bounce) {
        ObDereferenceObject(File);
        return STATUS_NO_MEMORY;
    }

    uint32_t Done = 0;
    Status = STATUS_SUCCESS;
    do {
        uint32_t Chunk = HZ_MIN(Length - Done, ChunkSize);
        uint32_t Moved = 0;
        uint64_t ChunkOffset = Offset + Done;
        if (Write) {
            Status = MmCopyFromUser(Bounce, (const uint8_t *)Args[1] + Done, Chunk);
            if (HZ_SUCCESS(Status))
                Status = IoWriteFile(File, Bounce, Chunk, HasOffset ? &ChunkOffset : NULL, UserMode, &Moved);
        } else {
            Status = IoReadFile(File, Bounce, Chunk, HasOffset ? &ChunkOffset : NULL, UserMode, &Moved);
            if (HZ_SUCCESS(Status))
                Status = MmCopyToUser((uint8_t *)Args[1] + Done, Bounce, Moved);
        }
        Done += Moved;
        if (!HZ_SUCCESS(Status) || Moved < Chunk)
            break; /* end of file, a short device read, or an error */
    } while (Done < Length);
    if (Status == STATUS_END_OF_FILE && Done > 0)
        Status = STATUS_SUCCESS; /* some data was read before the end */

    ExFreePoolWithTag(Bounce, IOP_BUFFER_TAG);
    ObDereferenceObject(File);
    if (Args[4])
        MmCopyToUser((void *)Args[4], &Done, sizeof(Done));
    return Status;
}

/* ReadFile(HANDLE File, void *Buffer, uint32_t Length, const uint64_t *ByteOffset, uint32_t *BytesRead) */
STATUS NtReadFile(const uint64_t *Args)
{
    return IopReadWrite(Args, false);
}

/* WriteFile(HANDLE File, const void *Buffer, uint32_t Length, const uint64_t *ByteOffset, uint32_t *BytesWritten) */
STATUS NtWriteFile(const uint64_t *Args)
{
    return IopReadWrite(Args, true);
}

/* QueryInformationFile(HANDLE File, uint32_t Class, void *Buffer, uint32_t Length) */
STATUS NtQueryInformationFile(const uint64_t *Args)
{
    uint32_t Length = (uint32_t)Args[3];
    if (Length == 0 || Length > IOP_MAX_INFO)
        return STATUS_INFO_LENGTH_MISMATCH;
    STATUS Status = IopProbe(Args[2], 1);
    if (!HZ_SUCCESS(Status))
        return Status;
    FILE_OBJECT *File;
    Status = IopReferenceFile(Args[0], 0, &File);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint8_t *Buffer = ExAllocatePoolZero(NonPagedPool, Length, IOP_BUFFER_TAG);
    if (!Buffer) {
        ObDereferenceObject(File);
        return STATUS_NO_MEMORY;
    }
    Status = IoQueryInformationFile(File, (uint32_t)Args[1], Buffer, Length);
    ObDereferenceObject(File);
    if (HZ_SUCCESS(Status))
        Status = MmCopyToUser((void *)Args[2], Buffer, Length);
    ExFreePoolWithTag(Buffer, IOP_BUFFER_TAG);
    return Status;
}

/* SetInformationFile(HANDLE File, uint32_t Class, const void *Buffer, uint32_t Length) */
STATUS NtSetInformationFile(const uint64_t *Args)
{
    uint32_t Length = (uint32_t)Args[3];
    if (Length == 0 || Length > IOP_MAX_INFO)
        return STATUS_INFO_LENGTH_MISMATCH;
    uint8_t *Buffer = ExAllocatePoolZero(NonPagedPool, Length, IOP_BUFFER_TAG);
    if (!Buffer)
        return STATUS_NO_MEMORY;
    STATUS Status = MmCopyFromUser(Buffer, (const void *)Args[2], Length);
    if (HZ_SUCCESS(Status) && (uint32_t)Args[1] == HZ_FILE_RENAME_INFORMATION_CLASS &&
        Length == sizeof(HZ_FILE_RENAME_INFORMATION))
        ((HZ_FILE_RENAME_INFORMATION *)Buffer)->NewName[255] = '\0';
    FILE_OBJECT *File = NULL;
    if (HZ_SUCCESS(Status))
        Status = IopReferenceFile(Args[0], 0, &File);
    if (HZ_SUCCESS(Status)) {
        Status = IoSetInformationFile(File, (uint32_t)Args[1], Buffer, Length);
        ObDereferenceObject(File);
    }
    ExFreePoolWithTag(Buffer, IOP_BUFFER_TAG);
    return Status;
}

/* QueryDirectoryFile(HANDLE Directory, HZ_FILE_DIRECTORY_INFORMATION *Buffer, uint32_t Length, uint32_t Flags,
                      uint32_t *EntriesReturned) */
STATUS NtQueryDirectoryFile(const uint64_t *Args)
{
    uint32_t Count = HZ_MIN((uint32_t)Args[2] / (uint32_t)sizeof(HZ_FILE_DIRECTORY_INFORMATION), IOP_MAX_DIRECTORY_ENTRIES);
    if (Count == 0)
        return STATUS_BUFFER_TOO_SMALL;
    STATUS Status = IopProbe(Args[4], sizeof(uint32_t));
    if (HZ_SUCCESS(Status) && !MmIsUserRange((const void *)Args[1], Args[2]))
        Status = STATUS_ACCESS_VIOLATION;
    if (!HZ_SUCCESS(Status))
        return Status;
    FILE_OBJECT *File;
    Status = IopReferenceFile(Args[0], FILE_LIST_DIRECTORY, &File);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_FILE_DIRECTORY_INFORMATION *Entries =
        ExAllocatePoolZero(NonPagedPool, Count * sizeof(HZ_FILE_DIRECTORY_INFORMATION), IOP_BUFFER_TAG);
    if (!Entries) {
        ObDereferenceObject(File);
        return STATUS_NO_MEMORY;
    }
    uint32_t Returned = 0;
    Status = IoQueryDirectoryFile(File, Entries, Count, (uint32_t)Args[3], &Returned);
    ObDereferenceObject(File);
    if (HZ_SUCCESS(Status) && Returned)
        Status = MmCopyToUser((void *)Args[1], Entries, Returned * sizeof(HZ_FILE_DIRECTORY_INFORMATION));
    ExFreePoolWithTag(Entries, IOP_BUFFER_TAG);
    if (Args[4])
        MmCopyToUser((void *)Args[4], &Returned, sizeof(Returned));
    return Status;
}

/* DeviceIoControlFile(HANDLE File, uint32_t Code, const void *Input, uint32_t InputLength, void *Output,
                       uint32_t OutputLength, uint32_t *BytesReturned) */
STATUS NtDeviceIoControlFile(const uint64_t *Args)
{
    uint32_t InputLength = (uint32_t)Args[3], OutputLength = (uint32_t)Args[5];
    if (InputLength > IOP_MAX_IOCTL || OutputLength > IOP_MAX_IOCTL)
        return STATUS_INVALID_PARAMETER;
    STATUS Status = IopProbe(Args[6], sizeof(uint32_t));
    if (HZ_SUCCESS(Status) && !MmIsUserRange((const void *)Args[4], OutputLength))
        Status = STATUS_ACCESS_VIOLATION;
    if (!HZ_SUCCESS(Status))
        return Status;
    uint8_t *Input = ExAllocatePoolZero(NonPagedPool, InputLength + 1, IOP_BUFFER_TAG);
    uint8_t *Output = ExAllocatePoolZero(NonPagedPool, OutputLength + 1, IOP_BUFFER_TAG);
    FILE_OBJECT *File = NULL;
    if (!Input || !Output)
        Status = STATUS_NO_MEMORY;
    if (HZ_SUCCESS(Status))
        Status = MmCopyFromUser(Input, (const void *)Args[2], InputLength);
    if (HZ_SUCCESS(Status))
        Status = IopReferenceFile(Args[0], 0, &File);
    uint32_t Returned = 0;
    if (HZ_SUCCESS(Status)) {
        KeWaitForSingleObject(&File->Lock, NULL);
        IRP Irp;
        IoInitializeIrp(&Irp, IRP_MJ_DEVICE_CONTROL, File, Output, OutputLength);
        Irp.RequestorMode = UserMode;
        Irp.Parameters.DeviceControl.Code = (uint32_t)Args[1];
        Irp.Parameters.DeviceControl.Input = Input;
        Irp.Parameters.DeviceControl.InputLength = InputLength;
        Status = IoCallDriver(File->Device, &Irp);
        KeReleaseMutex(&File->Lock);
        Returned = (uint32_t)HZ_MIN(Irp.IoStatus.Information, OutputLength);
        ObDereferenceObject(File);
        if (HZ_SUCCESS(Status))
            Status = MmCopyToUser((void *)Args[4], Output, Returned);
    }
    if (Input)
        ExFreePoolWithTag(Input, IOP_BUFFER_TAG);
    if (Output)
        ExFreePoolWithTag(Output, IOP_BUFFER_TAG);
    if (Args[6])
        MmCopyToUser((void *)Args[6], &Returned, sizeof(Returned));
    return Status;
}
