/*
 * Nexora OS - ex/event.c
 * Event objects for user mode: an object-manager wrapper around KEVENT.
 */
#include "ex.h"
#include "mm.h"
#include "ob.h"
#include "rtl.h"

#define EXP_EVENT_TAG HZ_POOL_TAG('E', 'x', 'E', 'v')

OBJECT_TYPE *ExEventObjectType;

void ExpInitializeEventType(void)
{
    OBJECT_TYPE_INITIALIZER Init = {0};
    Init.PoolTag = EXP_EVENT_TAG;
    Init.ValidAccessMask = EVENT_ALL_ACCESS;
    Init.Waitable = true;
    if (!HZ_SUCCESS(ObCreateObjectType("Event", &Init, &ExEventObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0x30, 0, 0);
}

/* CreateEvent(HANDLE *Event, uint32_t EventType, uint32_t InitialState) */
STATUS NtCreateEvent(const uint64_t *Args)
{
    uint32_t Type = (uint32_t)Args[1];
    if (Type != HZ_NOTIFICATION_EVENT && Type != HZ_SYNCHRONIZATION_EVENT)
        return STATUS_INVALID_PARAMETER;
    HANDLE Handle = NULL;
    STATUS Status = MmCopyToUser((void *)Args[0], &Handle, sizeof(Handle)); /* probe */
    if (!HZ_SUCCESS(Status))
        return Status;
    KEVENT *Event;
    Status = ObCreateObject(ExEventObjectType, sizeof(KEVENT), NULL, (void **)&Event);
    if (!HZ_SUCCESS(Status))
        return Status;
    KeInitializeEvent(Event, Type == HZ_SYNCHRONIZATION_EVENT ? SynchronizationEvent : NotificationEvent,
                      Args[2] != 0);
    Status = ObOpenObjectByPointer(Event, EVENT_ALL_ACCESS, NULL, &Handle);
    ObDereferenceObject(Event);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = MmCopyToUser((void *)Args[0], &Handle, sizeof(Handle));
    if (!HZ_SUCCESS(Status))
        ObCloseHandle(Handle, KernelMode);
    return Status;
}

/* SetEvent(HANDLE Event, int32_t *PreviousState) */
STATUS NtSetEvent(const uint64_t *Args)
{
    KEVENT *Event;
    STATUS Status = ObReferenceObjectByHandle((HANDLE)Args[0], ExEventObjectType, EVENT_MODIFY_STATE, UserMode,
                                              (void **)&Event);
    if (!HZ_SUCCESS(Status))
        return Status;
    int32_t Previous = KeSetEvent(Event);
    ObDereferenceObject(Event);
    if (Args[1])
        Status = MmCopyToUser((void *)Args[1], &Previous, sizeof(Previous));
    return Status;
}

/* ResetEvent(HANDLE Event) */
STATUS NtResetEvent(const uint64_t *Args)
{
    KEVENT *Event;
    STATUS Status = ObReferenceObjectByHandle((HANDLE)Args[0], ExEventObjectType, EVENT_MODIFY_STATE, UserMode,
                                              (void **)&Event);
    if (!HZ_SUCCESS(Status))
        return Status;
    KeResetEvent(Event);
    ObDereferenceObject(Event);
    return STATUS_SUCCESS;
}
