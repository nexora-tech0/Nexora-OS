/*
 * Nexora OS - io/iopipe.c
 * Anonymous pipes (M9): a ring buffer with a read end and a write end, each its own file object on
 * \Device\Pipe, so the ordinary file services (ReadFile, WriteFile, Close) and the standard
 * handles of a new process work with them.
 *
 *   - A read returns as soon as some data is there (up to the requested length); it waits while
 *     the pipe is empty and returns STATUS_END_OF_FILE once it is empty and the write end has
 *     closed.
 *   - A write waits while the pipe is full and returns STATUS_PIPE_BROKEN once the read end has
 *     closed; otherwise it writes everything before returning.
 *   - Waits of requests from user mode are alertable, so a thread blocked on a pipe can still be
 *     terminated (the request then fails with STATUS_ALERTED).
 *   - An end closes when its file object is deleted (its last handle closed, IRP_MJ_CLOSE); the
 *     pipe is freed when both ends have closed.
 */
#include "iop.h"

#define IOP_PIPE_TAG HZ_POOL_TAG('I', 'o', 'P', 'p')
#define IOP_PIPE_READ_END 1
#define IOP_PIPE_WRITE_END 2

typedef struct _IOP_PIPE {
    KMUTEX Lock;
    KEVENT Changed;      /* notification event: data, space or an end's state changed */
    uint8_t *Buffer;
    uint32_t Size;
    uint32_t Head;       /* next byte to read */
    uint32_t Count;      /* bytes in the buffer */
    bool ReaderClosed;
    bool WriterClosed;
    uint32_t Ends;       /* ends still open (2 -> 0) */
} IOP_PIPE;

static DEVICE_OBJECT *IopPipeDevice;

static STATUS IopPipeWait(IOP_PIPE *Pipe, IRP *Irp)
{
    void *Object = &Pipe->Changed;
    STATUS Status = KeWaitForMultipleObjectsEx(1, &Object, WaitAny, NULL, NULL, Irp->RequestorMode == UserMode);
    return Status == STATUS_ALERTED ? STATUS_ALERTED : STATUS_SUCCESS;
}

static STATUS IopPipeRead(DEVICE_OBJECT *Device, IRP *Irp)
{
    IOP_PIPE *Pipe = Irp->FileObject->FsContext;
    if ((uintptr_t)Irp->FileObject->FsContext2 != IOP_PIPE_READ_END)
        return STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    if (Irp->Length == 0)
        return STATUS_SUCCESS;
    for (;;) {
        KeWaitForSingleObject(&Pipe->Lock, NULL);
        if (Pipe->Count > 0) {
            uint32_t Copy = HZ_MIN(Pipe->Count, Irp->Length);
            uint8_t *Out = Irp->Buffer;
            for (uint32_t i = 0; i < Copy; i++)
                Out[i] = Pipe->Buffer[(Pipe->Head + i) % Pipe->Size];
            Pipe->Head = (Pipe->Head + Copy) % Pipe->Size;
            Pipe->Count -= Copy;
            KeSetEvent(&Pipe->Changed); /* room for the writer */
            KeReleaseMutex(&Pipe->Lock);
            Irp->IoStatus.Information = Copy;
            return STATUS_SUCCESS;
        }
        if (Pipe->WriterClosed) {
            KeReleaseMutex(&Pipe->Lock);
            return STATUS_END_OF_FILE;
        }
        KeResetEvent(&Pipe->Changed); /* under the lock: a later change sets it again */
        KeReleaseMutex(&Pipe->Lock);
        STATUS Status = IopPipeWait(Pipe, Irp);
        if (Status == STATUS_ALERTED)
            return STATUS_ALERTED;
    }
}

static STATUS IopPipeWrite(DEVICE_OBJECT *Device, IRP *Irp)
{
    IOP_PIPE *Pipe = Irp->FileObject->FsContext;
    if ((uintptr_t)Irp->FileObject->FsContext2 != IOP_PIPE_WRITE_END)
        return STATUS_INVALID_DEVICE_REQUEST;
    const uint8_t *In = Irp->Buffer;
    uint32_t Done = 0;
    while (Done < Irp->Length) {
        KeWaitForSingleObject(&Pipe->Lock, NULL);
        if (Pipe->ReaderClosed) {
            KeReleaseMutex(&Pipe->Lock);
            Irp->IoStatus.Information = Done;
            return STATUS_PIPE_BROKEN;
        }
        uint32_t Space = Pipe->Size - Pipe->Count;
        if (Space > 0) {
            uint32_t Copy = HZ_MIN(Space, Irp->Length - Done);
            for (uint32_t i = 0; i < Copy; i++)
                Pipe->Buffer[(Pipe->Head + Pipe->Count + i) % Pipe->Size] = In[Done + i];
            Pipe->Count += Copy;
            Done += Copy;
            KeSetEvent(&Pipe->Changed); /* data for the reader */
            KeReleaseMutex(&Pipe->Lock);
            continue;
        }
        KeResetEvent(&Pipe->Changed);
        KeReleaseMutex(&Pipe->Lock);
        STATUS Status = IopPipeWait(Pipe, Irp);
        if (Status == STATUS_ALERTED) {
            Irp->IoStatus.Information = Done;
            return STATUS_ALERTED;
        }
    }
    Irp->IoStatus.Information = Done;
    return STATUS_SUCCESS;
}

static STATUS IopPipeClose(DEVICE_OBJECT *Device, IRP *Irp)
{
    IOP_PIPE *Pipe = Irp->FileObject->FsContext;
    if (!Pipe)
        return STATUS_SUCCESS;
    KeWaitForSingleObject(&Pipe->Lock, NULL);
    if ((uintptr_t)Irp->FileObject->FsContext2 == IOP_PIPE_READ_END)
        Pipe->ReaderClosed = true;
    else
        Pipe->WriterClosed = true;
    bool Last = --Pipe->Ends == 0;
    KeSetEvent(&Pipe->Changed);
    KeReleaseMutex(&Pipe->Lock);
    if (Last) {
        ExFreePoolWithTag(Pipe->Buffer, IOP_PIPE_TAG);
        ExFreePoolWithTag(Pipe, IOP_PIPE_TAG);
    }
    return STATUS_SUCCESS;
}

static STATUS IopPipeQueryInformation(DEVICE_OBJECT *Device, IRP *Irp)
{
    return STATUS_INVALID_DEVICE_REQUEST;
}

STATUS IoCreatePipe(uint32_t Size, FILE_OBJECT **ReadEnd, FILE_OBJECT **WriteEnd)
{
    if (!IopPipeDevice)
        return STATUS_DEVICE_NOT_READY;
    if (Size == 0)
        Size = IO_PIPE_DEFAULT_SIZE;
    if (Size > IO_PIPE_MAXIMUM_SIZE)
        return STATUS_INVALID_PARAMETER;
    IOP_PIPE *Pipe = ExAllocatePoolZero(NonPagedPool, sizeof(IOP_PIPE), IOP_PIPE_TAG);
    uint8_t *Buffer = ExAllocatePoolWithTag(NonPagedPool, Size, IOP_PIPE_TAG);
    if (!Pipe || !Buffer) {
        if (Pipe)
            ExFreePoolWithTag(Pipe, IOP_PIPE_TAG);
        if (Buffer)
            ExFreePoolWithTag(Buffer, IOP_PIPE_TAG);
        return STATUS_NO_MEMORY;
    }
    KeInitializeMutex(&Pipe->Lock);
    KeInitializeEvent(&Pipe->Changed, NotificationEvent, false);
    Pipe->Buffer = Buffer;
    Pipe->Size = Size;
    Pipe->Ends = 2;

    FILE_OBJECT *Read = NULL, *Write = NULL;
    STATUS Status = IopCreateFileObject(IopPipeDevice, NULL, "pipe:read", &Read);
    if (HZ_SUCCESS(Status))
        Status = IopCreateFileObject(IopPipeDevice, NULL, "pipe:write", &Write);
    if (!HZ_SUCCESS(Status)) {
        if (Read)
            ObDereferenceObject(Read); /* not opened yet: no IRP_MJ_CLOSE */
        ExFreePoolWithTag(Buffer, IOP_PIPE_TAG);
        ExFreePoolWithTag(Pipe, IOP_PIPE_TAG);
        return Status;
    }
    Read->FsContext = Pipe;
    Read->FsContext2 = (void *)(uintptr_t)IOP_PIPE_READ_END;
    Read->GrantedAccess = FILE_READ_DATA | SYNCHRONIZE;
    Read->Flags |= FO_OPENED | FO_PIPE;
    Write->FsContext = Pipe;
    Write->FsContext2 = (void *)(uintptr_t)IOP_PIPE_WRITE_END;
    Write->GrantedAccess = FILE_WRITE_DATA | SYNCHRONIZE;
    Write->Flags |= FO_OPENED | FO_PIPE;
    *ReadEnd = Read;
    *WriteEnd = Write;
    return STATUS_SUCCESS;
}

static STATUS IopPipeDriverEntry(DRIVER_OBJECT *Driver)
{
    Driver->MajorFunction[IRP_MJ_READ] = IopPipeRead;
    Driver->MajorFunction[IRP_MJ_WRITE] = IopPipeWrite;
    Driver->MajorFunction[IRP_MJ_CLOSE] = IopPipeClose;
    Driver->MajorFunction[IRP_MJ_QUERY_INFORMATION] = IopPipeQueryInformation;
    return IoCreateDevice(Driver, 0, "\\Device\\Pipe", FILE_DEVICE_PIPE, 0, &IopPipeDevice);
}

IO_DECLARE_BUILTIN_DRIVER(IopPipeBuiltinDriver, "\\Driver\\Pipe", IopPipeDriverEntry, IO_ORDER_DEVICE);
