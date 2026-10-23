/*
 * Nexora OS - ob/obsvc.c
 * Handle and wait system services. Waits on behalf of user mode are alertable, so a thread
 * blocked in one can still be terminated (it wakes with STATUS_ALERTED and exits on its way
 * back to ring 3).
 */
#include "obp.h"

#define OBP_WAIT_TAG HZ_POOL_TAG('O', 'b', 'W', 't')

/* Close(HANDLE) */
STATUS NtClose(const uint64_t *Args)
{
    return ObCloseHandle((HANDLE)Args[0], UserMode);
}

static STATUS ObpCaptureTimeout(uint64_t UserPointer, uint64_t *Timeout, const uint64_t **Pointer)
{
    *Pointer = NULL;
    if (!UserPointer)
        return STATUS_SUCCESS;
    STATUS Status = MmCopyFromUser(Timeout, (const void *)UserPointer, sizeof(*Timeout));
    if (HZ_SUCCESS(Status))
        *Pointer = Timeout;
    return Status;
}

static STATUS ObpReferenceWaitable(HANDLE Handle, void **Object)
{
    STATUS Status = ObReferenceObjectByHandle(Handle, NULL, SYNCHRONIZE, UserMode, Object);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!ObGetObjectType(*Object)->Info.Waitable) {
        ObDereferenceObject(*Object);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    return STATUS_SUCCESS;
}

/* WaitForSingleObject(HANDLE Object, uint32_t Reserved, const uint64_t *TimeoutUs) */
STATUS NtWaitForSingleObject(const uint64_t *Args)
{
    uint64_t Timeout;
    const uint64_t *TimeoutPointer;
    STATUS Status = ObpCaptureTimeout(Args[2], &Timeout, &TimeoutPointer);
    if (!HZ_SUCCESS(Status))
        return Status;
    void *Object;
    Status = ObpReferenceWaitable((HANDLE)Args[0], &Object);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = KeWaitForMultipleObjectsEx(1, &Object, WaitAny, TimeoutPointer, NULL, true);
    ObDereferenceObject(Object);
    return Status;
}

/* WaitForMultipleObjects(uint32_t Count, const HANDLE *Objects, uint32_t WaitType, const uint64_t *TimeoutUs) */
STATUS NtWaitForMultipleObjects(const uint64_t *Args)
{
    uint32_t Count = (uint32_t)Args[0];
    uint32_t WaitType = (uint32_t)Args[2];
    if (Count == 0 || Count > HZ_MAXIMUM_WAIT_OBJECTS || (WaitType != HZ_WAIT_ALL && WaitType != HZ_WAIT_ANY))
        return STATUS_INVALID_PARAMETER;
    uint64_t Timeout;
    const uint64_t *TimeoutPointer;
    STATUS Status = ObpCaptureTimeout(Args[3], &Timeout, &TimeoutPointer);
    if (!HZ_SUCCESS(Status))
        return Status;

    HANDLE Handles[HZ_MAXIMUM_WAIT_OBJECTS];
    void *Objects[HZ_MAXIMUM_WAIT_OBJECTS];
    Status = MmCopyFromUser(Handles, (const void *)Args[1], Count * sizeof(HANDLE));
    if (!HZ_SUCCESS(Status))
        return Status;
    KWAIT_BLOCK *Blocks = NULL;
    if (Count > THREAD_WAIT_OBJECTS) {
        Blocks = ExAllocatePoolWithTag(NonPagedPool, Count * sizeof(KWAIT_BLOCK), OBP_WAIT_TAG);
        if (!Blocks)
            return STATUS_NO_MEMORY;
    }
    uint32_t Referenced = 0;
    for (; Referenced < Count; Referenced++) {
        Status = ObpReferenceWaitable(Handles[Referenced], &Objects[Referenced]);
        if (!HZ_SUCCESS(Status))
            break;
    }
    if (HZ_SUCCESS(Status))
        Status = KeWaitForMultipleObjectsEx(Count, Objects, WaitType == HZ_WAIT_ALL ? WaitAll : WaitAny,
                                            TimeoutPointer, Blocks, true);
    for (uint32_t i = 0; i < Referenced; i++)
        ObDereferenceObject(Objects[i]);
    if (Blocks)
        ExFreePoolWithTag(Blocks, OBP_WAIT_TAG);
    return Status;
}
