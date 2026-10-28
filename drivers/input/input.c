/*
 * Nexora OS - drivers/input/input.c
 * Shared kernel input queues. Hardware drivers publish normalized key/mouse events here; the
 * logical Keyboard0 and Mouse0 devices remain stable as PS/2 and USB devices come and go.
 */
#include "ex.h"
#include "input.h"
#include "io.h"
#include "rtl.h"

#define INPUT_EVENT_QUEUE 128u
#define INPUT_EVENT_BYTES 24u

typedef struct _INPUT_QUEUE {
    KSPIN_LOCK Lock;
    KEVENT Available;
    uint32_t EventSize;
    uint32_t Head;
    uint32_t Count;
    uint8_t Events[INPUT_EVENT_QUEUE][INPUT_EVENT_BYTES];
    uint64_t Dropped;
} INPUT_QUEUE;

static INPUT_QUEUE InputKeyboardQueue;
static INPUT_QUEUE InputMouseQueue;

_Static_assert(sizeof(HZ_KEY_EVENT) <= INPUT_EVENT_BYTES && sizeof(HZ_MOUSE_EVENT) <= INPUT_EVENT_BYTES,
               "input event slot size");

static void InputQueueInitialize(INPUT_QUEUE *Queue, uint32_t EventSize)
{
    KeInitializeSpinLock(&Queue->Lock);
    KeInitializeEvent(&Queue->Available, NotificationEvent, false);
    Queue->EventSize = EventSize;
}

static void InputQueuePush(INPUT_QUEUE *Queue, const void *Event)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&Queue->Lock, &OldIrql);
    if (Queue->Count == INPUT_EVENT_QUEUE) {
        Queue->Head = (Queue->Head + 1) % INPUT_EVENT_QUEUE;
        Queue->Count--;
        Queue->Dropped++;
    }
    memcpy(Queue->Events[(Queue->Head + Queue->Count) % INPUT_EVENT_QUEUE], Event, Queue->EventSize);
    Queue->Count++;
    KeSetEvent(&Queue->Available);
    KeReleaseSpinLock(&Queue->Lock, OldIrql);
}

static STATUS InputQueueRead(INPUT_QUEUE *Queue, IRP *Irp)
{
    uint32_t Capacity = Irp->Length / Queue->EventSize;
    if (!Capacity)
        return STATUS_BUFFER_TOO_SMALL;
    for (;;) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&Queue->Lock, &OldIrql);
        uint32_t Count = HZ_MIN(Capacity, Queue->Count);
        for (uint32_t i = 0; i < Count; i++) {
            memcpy((uint8_t *)Irp->Buffer + i * Queue->EventSize, Queue->Events[Queue->Head], Queue->EventSize);
            Queue->Head = (Queue->Head + 1) % INPUT_EVENT_QUEUE;
            Queue->Count--;
        }
        if (!Queue->Count)
            KeResetEvent(&Queue->Available);
        KeReleaseSpinLock(&Queue->Lock, OldIrql);
        if (Count) {
            Irp->IoStatus.Information = Count * Queue->EventSize;
            return STATUS_SUCCESS;
        }
        void *Object = &Queue->Available;
        STATUS Status = KeWaitForMultipleObjectsEx(1, &Object, WaitAny, NULL, NULL,
                                                   Irp->RequestorMode == UserMode);
        if (Status == STATUS_ALERTED) {
            Irp->IoStatus.Information = 0;
            return STATUS_ALERTED;
        }
    }
}

static STATUS InputRead(DEVICE_OBJECT *Device, IRP *Irp)
{
    return InputQueueRead(Device->DeviceType == FILE_DEVICE_KEYBOARD ? &InputKeyboardQueue : &InputMouseQueue,
                          Irp);
}

void InputSubmitKeyEvent(const HZ_KEY_EVENT *Event)
{
    if (Event)
        InputQueuePush(&InputKeyboardQueue, Event);
}

void InputSubmitMouseEvent(const HZ_MOUSE_EVENT *Event)
{
    if (Event)
        InputQueuePush(&InputMouseQueue, Event);
}

static STATUS InputDriverEntry(DRIVER_OBJECT *Driver)
{
    InputQueueInitialize(&InputKeyboardQueue, sizeof(HZ_KEY_EVENT));
    InputQueueInitialize(&InputMouseQueue, sizeof(HZ_MOUSE_EVENT));
    Driver->MajorFunction[IRP_MJ_READ] = InputRead;

    DEVICE_OBJECT *Device;
    STATUS KeyboardStatus = IoCreateDevice(Driver, 0, "\\Device\\Keyboard0", FILE_DEVICE_KEYBOARD, 0, &Device);
    STATUS MouseStatus = IoCreateDevice(Driver, 0, "\\Device\\Mouse0", FILE_DEVICE_MOUSE, 0, &Device);
    if (!HZ_SUCCESS(KeyboardStatus) || !HZ_SUCCESS(MouseStatus))
        return STATUS_INSUFFICIENT_RESOURCES;
    return STATUS_SUCCESS;
}

IO_DECLARE_BUILTIN_DRIVER(InputBuiltinDriver, "\\Driver\\Input", InputDriverEntry, IO_ORDER_CLASS);
