/*
 * Nexora OS - ob/handle.c
 * Per-process handle tables.
 *
 * A handle is (index + 1) * 4 into the process's entry array, so 0 is never valid and the low
 * two bits are free (NT convention). Each entry holds a pointer reference to its object and the
 * access granted when the handle was created. The table grows by doubling; freed slots are kept
 * on a free list. HZ_CURRENT_PROCESS and HZ_CURRENT_THREAD are pseudo handles resolved without
 * the table.
 */
#include "obp.h"

#define OBP_HANDLE_INITIAL 32u
#define OBP_HANDLE_MAXIMUM 16384u

typedef struct _HANDLE_TABLE_ENTRY {
    void *Object;            /* NULL when free */
    ACCESS_MASK GrantedAccess;
    uint32_t NextFree;       /* index + 1 of the next free entry, 0 = end */
} HANDLE_TABLE_ENTRY;

struct _HANDLE_TABLE {
    KSPIN_LOCK Lock;
    HANDLE_TABLE_ENTRY *Entries;
    uint32_t Capacity;
    uint32_t Count;
    uint32_t FreeHead;       /* index + 1, 0 = none */
    bool Dead;               /* process exiting: no new handles */
};

static inline HANDLE ObpIndexToHandle(uint32_t Index)
{
    return (HANDLE)(uintptr_t)((Index + 1) * 4);
}

static inline bool ObpHandleToIndex(HANDLE Handle, uint32_t *Index)
{
    uintptr_t Value = (uintptr_t)Handle;
    if (Value == 0 || (Value & 3) || Value / 4 > OBP_HANDLE_MAXIMUM)
        return false;
    *Index = (uint32_t)(Value / 4 - 1);
    return true;
}

static HANDLE_TABLE *ObpCurrentHandleTable(void)
{
    EPROCESS *Process = PsGetCurrentProcess();
    return Process ? Process->ObjectTable : NULL;
}

HANDLE_TABLE *ObCreateHandleTable(void)
{
    HANDLE_TABLE *Table = ExAllocatePoolZero(NonPagedPool, sizeof(HANDLE_TABLE), OBP_HANDLE_TAG);
    if (!Table)
        return NULL;
    Table->Entries = ExAllocatePoolZero(NonPagedPool, OBP_HANDLE_INITIAL * sizeof(HANDLE_TABLE_ENTRY), OBP_HANDLE_TAG);
    if (!Table->Entries) {
        ExFreePoolWithTag(Table, OBP_HANDLE_TAG);
        return NULL;
    }
    KeInitializeSpinLock(&Table->Lock);
    Table->Capacity = OBP_HANDLE_INITIAL;
    /* Chain all slots, lowest index first, so handles are handed out in ascending order. */
    for (uint32_t i = 0; i < OBP_HANDLE_INITIAL; i++)
        Table->Entries[i].NextFree = i + 1 < OBP_HANDLE_INITIAL ? i + 2 : 0;
    Table->FreeHead = 1;
    return Table;
}

uint32_t ObHandleCount(HANDLE_TABLE *Table)
{
    return Table ? Table->Count : 0;
}

static ACCESS_MASK ObpMapAccess(OBJECT_TYPE *Type, ACCESS_MASK Desired)
{
    if (Desired & GENERIC_ALL)
        Desired = (Desired & ~GENERIC_ALL) | Type->Info.ValidAccessMask;
    return Desired & (Type->Info.ValidAccessMask | SYNCHRONIZE);
}

STATUS ObOpenObjectByPointer(void *Object, ACCESS_MASK DesiredAccess, EPROCESS *Process, HANDLE *Handle)
{
    if (!Process)
        Process = PsGetCurrentProcess();
    HANDLE_TABLE *Table = Process ? Process->ObjectTable : NULL;
    if (!Table)
        return STATUS_PROCESS_IS_TERMINATING;
    OBJECT_HEADER *Header = ObpHeader(Object);
    ObpCheckHeader(Header);
    ACCESS_MASK Granted = ObpMapAccess(Header->Type, DesiredAccess);
    ObReferenceObject(Object);

    HANDLE_TABLE_ENTRY *Spare = NULL;
    uint32_t SpareCapacity = 0;
    for (;;) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&Table->Lock, &OldIrql);
        if (Table->Dead) {
            KeReleaseSpinLock(&Table->Lock, OldIrql);
            if (Spare)
                ExFreePoolWithTag(Spare, OBP_HANDLE_TAG);
            ObDereferenceObject(Object);
            return STATUS_PROCESS_IS_TERMINATING;
        }
        if (!Table->FreeHead && Spare && SpareCapacity > Table->Capacity) {
            /* Install the grown array allocated outside the lock. */
            memcpy(Spare, Table->Entries, Table->Capacity * sizeof(HANDLE_TABLE_ENTRY));
            for (uint32_t i = Table->Capacity; i < SpareCapacity; i++)
                Spare[i].NextFree = i + 1 < SpareCapacity ? i + 2 : 0;
            HANDLE_TABLE_ENTRY *Old = Table->Entries;
            Table->FreeHead = Table->Capacity + 1;
            Table->Entries = Spare;
            Table->Capacity = SpareCapacity;
            Spare = Old; /* freed below, outside the lock */
            SpareCapacity = 0;
        }
        if (Table->FreeHead) {
            uint32_t Index = Table->FreeHead - 1;
            HANDLE_TABLE_ENTRY *Entry = &Table->Entries[Index];
            Table->FreeHead = Entry->NextFree;
            Entry->Object = Object;
            Entry->GrantedAccess = Granted;
            Entry->NextFree = 0;
            Table->Count++;
            __atomic_add_fetch(&Header->HandleCount, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&Header->Type->HandleCount, 1, __ATOMIC_RELAXED);
            KeReleaseSpinLock(&Table->Lock, OldIrql);
            if (Spare)
                ExFreePoolWithTag(Spare, OBP_HANDLE_TAG);
            *Handle = ObpIndexToHandle(Index);
            return STATUS_SUCCESS;
        }
        uint32_t Wanted = Table->Capacity * 2;
        KeReleaseSpinLock(&Table->Lock, OldIrql);

        if (Spare)
            ExFreePoolWithTag(Spare, OBP_HANDLE_TAG);
        Spare = NULL;
        if (Wanted > OBP_HANDLE_MAXIMUM) {
            ObDereferenceObject(Object);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Spare = ExAllocatePoolZero(NonPagedPool, Wanted * sizeof(HANDLE_TABLE_ENTRY), OBP_HANDLE_TAG);
        if (!Spare) {
            ObDereferenceObject(Object);
            return STATUS_NO_MEMORY;
        }
        SpareCapacity = Wanted;
    }
}

STATUS ObReferenceObjectByHandle(HANDLE Handle, OBJECT_TYPE *Type, ACCESS_MASK DesiredAccess, KPROCESSOR_MODE Mode,
                                 void **Object)
{
    void *Pseudo = NULL;
    if (Handle == HZ_CURRENT_PROCESS)
        Pseudo = PsGetCurrentProcess();
    else if (Handle == HZ_CURRENT_THREAD)
        Pseudo = PsGetCurrentThread();
    if (Pseudo) {
        if (Type && ObGetObjectType(Pseudo) != Type)
            return STATUS_OBJECT_TYPE_MISMATCH;
        ObReferenceObject(Pseudo);
        *Object = Pseudo;
        return STATUS_SUCCESS;
    }

    uint32_t Index;
    HANDLE_TABLE *Table = ObpCurrentHandleTable();
    if (!Table || !ObpHandleToIndex(Handle, &Index))
        return STATUS_INVALID_HANDLE;
    STATUS Status = STATUS_INVALID_HANDLE;
    KIRQL OldIrql;
    KeAcquireSpinLock(&Table->Lock, &OldIrql);
    if (Index < Table->Capacity && Table->Entries[Index].Object) {
        HANDLE_TABLE_ENTRY *Entry = &Table->Entries[Index];
        if (Type && ObGetObjectType(Entry->Object) != Type) {
            Status = STATUS_OBJECT_TYPE_MISMATCH;
        } else if (Mode == UserMode && (Entry->GrantedAccess & DesiredAccess) != DesiredAccess) {
            Status = STATUS_ACCESS_DENIED;
        } else {
            ObReferenceObject(Entry->Object);
            *Object = Entry->Object;
            Status = STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&Table->Lock, OldIrql);
    return Status;
}

/* Removes entry Index; returns its object (still referenced by the caller now) or NULL. */
static void *ObpRemoveHandleLocked(HANDLE_TABLE *Table, uint32_t Index)
{
    HANDLE_TABLE_ENTRY *Entry = &Table->Entries[Index];
    void *Object = Entry->Object;
    if (!Object)
        return NULL;
    Entry->Object = NULL;
    Entry->GrantedAccess = 0;
    Entry->NextFree = Table->FreeHead;
    Table->FreeHead = Index + 1;
    Table->Count--;
    return Object;
}

static void ObpCloseObject(void *Object, EPROCESS *Process)
{
    OBJECT_HEADER *Header = ObpHeader(Object);
    int64_t Remaining = __atomic_sub_fetch(&Header->HandleCount, 1, __ATOMIC_ACQ_REL);
    __atomic_sub_fetch(&Header->Type->HandleCount, 1, __ATOMIC_RELAXED);
    if (Remaining < 0)
        KeBugCheckEx(OBJECT_REFCOUNT_CORRUPT, (uint64_t)Object, (uint64_t)Header->PointerCount, (uint64_t)Remaining, 5);
    if (Header->Type->Info.CloseProcedure)
        Header->Type->Info.CloseProcedure(Object, Process, (uint64_t)Remaining);
    ObDereferenceObject(Object);
}

STATUS ObCloseHandle(HANDLE Handle, KPROCESSOR_MODE Mode)
{
    if (Handle == HZ_CURRENT_PROCESS || Handle == HZ_CURRENT_THREAD)
        return STATUS_SUCCESS; /* pseudo handles need no closing */
    uint32_t Index;
    EPROCESS *Process = PsGetCurrentProcess();
    HANDLE_TABLE *Table = Process ? Process->ObjectTable : NULL;
    if (!Table || !ObpHandleToIndex(Handle, &Index))
        return STATUS_INVALID_HANDLE;
    void *Object = NULL;
    KIRQL OldIrql;
    KeAcquireSpinLock(&Table->Lock, &OldIrql);
    if (Index < Table->Capacity)
        Object = ObpRemoveHandleLocked(Table, Index);
    KeReleaseSpinLock(&Table->Lock, OldIrql);
    if (!Object)
        return STATUS_INVALID_HANDLE;
    ObpCloseObject(Object, Process);
    return STATUS_SUCCESS;
}

void ObKillHandleTable(HANDLE_TABLE *Table, EPROCESS *Process)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&Table->Lock, &OldIrql);
    Table->Dead = true;
    KeReleaseSpinLock(&Table->Lock, OldIrql);

    /* Close outside the lock: close/delete procedures may use Ob (and this table is dead, so
       nothing can be added behind us). */
    for (uint32_t Index = 0;; Index++) {
        KeAcquireSpinLock(&Table->Lock, &OldIrql);
        if (Index >= Table->Capacity) {
            KeReleaseSpinLock(&Table->Lock, OldIrql);
            break;
        }
        void *Object = ObpRemoveHandleLocked(Table, Index);
        KeReleaseSpinLock(&Table->Lock, OldIrql);
        if (Object)
            ObpCloseObject(Object, Process);
    }
    ExFreePoolWithTag(Table->Entries, OBP_HANDLE_TAG);
    ExFreePoolWithTag(Table, OBP_HANDLE_TAG);
}
