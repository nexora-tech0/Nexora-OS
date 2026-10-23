/*
 * Nexora OS - ke/wait.c
 * Waiting and dispatcher objects: events, semaphores, mutexes (timers and threads are waitable
 * too). The model follows NT: a waiting thread links one KWAIT_BLOCK per object into the
 * objects' wait lists (plus one into its private timer for a timeout) and leaves the CPU.
 * Whoever signals an object runs KiWaitTestLocked, which satisfies waiters in FIFO order and
 * readies them with their completion status.
 */
#include "ki.h"

static bool KiIsSignaled(DISPATCHER_HEADER *Header, KTHREAD *Thread)
{
    if (Header->Type == KObjectMutex)
        return Header->SignalState > 0 || ((KMUTEX *)Header)->OwnerThread == Thread;
    return Header->SignalState > 0;
}

/* Consumes the object on behalf of Thread; returns true if it was an abandoned mutex. */
static bool KiSatisfy(DISPATCHER_HEADER *Header, KTHREAD *Thread)
{
    switch (Header->Type) {
    case KObjectEventSynchronization:
    case KObjectTimerSynchronization:
        Header->SignalState = 0;
        return false;
    case KObjectSemaphore:
        Header->SignalState--;
        return false;
    case KObjectMutex: {
        KMUTEX *Mutex = (KMUTEX *)Header;
        Header->SignalState--;
        if (!Mutex->OwnerThread) {
            Mutex->OwnerThread = Thread;
            InsertTailList(&Thread->MutantListHead, &Mutex->MutantListEntry);
        }
        bool Abandoned = Mutex->Abandoned;
        Mutex->Abandoned = false;
        return Abandoned;
    }
    default:
        return false; /* notification objects stay signaled */
    }
}

static bool KiAllSignaled(KTHREAD *Thread)
{
    for (uint32_t i = 0; i < Thread->WaitCount; i++) {
        if (!KiIsSignaled(Thread->WaitBlockList[i].Object, Thread))
            return false;
    }
    return true;
}

static STATUS KiSatisfyAll(KTHREAD *Thread, KWAIT_BLOCK *Blocks, uint32_t Count, DISPATCHER_HEADER *const *Objects)
{
    STATUS Status = STATUS_WAIT_0;
    for (uint32_t i = 0; i < Count; i++) {
        DISPATCHER_HEADER *Object = Blocks ? Blocks[i].Object : Objects[i];
        if (KiSatisfy(Object, Thread) && Status == STATUS_WAIT_0)
            Status = STATUS_ABANDONED_WAIT_0 + (STATUS)i;
    }
    return Status;
}

void KiUnwaitThreadLocked(KTHREAD *Thread, STATUS Status)
{
    for (uint32_t i = 0; i < Thread->WaitCount; i++) {
        KWAIT_BLOCK *Block = &Thread->WaitBlockList[i];
        if (Block->Linked) {
            RemoveEntryList(&Block->WaitListEntry);
            Block->Linked = false;
        }
    }
    if (Thread->TimerWaitBlock.Linked) {
        RemoveEntryList(&Thread->TimerWaitBlock.WaitListEntry);
        Thread->TimerWaitBlock.Linked = false;
    }
    KiRemoveTimerLocked(&Thread->Timer);
    Thread->WaitStatus = Status;
    KiResetQuantum(Thread); /* a fresh quantum after every wait, as in NT */
    KiReadyThreadLocked(Thread);
}

void KiWaitTestLocked(DISPATCHER_HEADER *Object)
{
    LIST_ENTRY *Entry = Object->WaitListHead.Flink;
    while (Entry != &Object->WaitListHead) {
        LIST_ENTRY *Next = Entry->Flink;
        KWAIT_BLOCK *Block = CONTAINING_RECORD(Entry, KWAIT_BLOCK, WaitListEntry);
        KTHREAD *Thread = Block->Thread;
        if (Block->WaitKey == KI_TIMEOUT_WAIT_KEY) {
            KiUnwaitThreadLocked(Thread, STATUS_TIMEOUT);
        } else if (Block->WaitType == WaitAny) {
            if (!KiIsSignaled(Object, Thread))
                break; /* consumed: nobody further down can be satisfied either */
            bool Abandoned = KiSatisfy(Object, Thread);
            KiUnwaitThreadLocked(Thread, (Abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0) + Block->WaitKey);
        } else if (KiAllSignaled(Thread)) {
            KiUnwaitThreadLocked(Thread, KiSatisfyAll(Thread, Thread->WaitBlockList, Thread->WaitCount, NULL));
        }
        Entry = Next;
    }
}

STATUS KeWaitForMultipleObjectsEx(uint32_t Count, void *const Objects[], WAIT_TYPE WaitType, const uint64_t *TimeoutUs,
                                  KWAIT_BLOCK *WaitBlockArray, bool Alertable)
{
    KTHREAD *Thread = KeGetCurrentThread();
    bool Poll = TimeoutUs && *TimeoutUs == 0;
    KIRQL Irql = KeGetCurrentIrql();
    if (!Thread || Thread == &KiIdleThread || (Irql >= DISPATCH_LEVEL && !Poll))
        KeBugCheckEx(WAIT_AT_RAISED_IRQL, Irql, Count ? (uint64_t)Objects[0] : 0, (uint64_t)__builtin_return_address(0),
                     0);
    if (Count > MAXIMUM_WAIT_OBJECTS || (Count > THREAD_WAIT_OBJECTS && !WaitBlockArray) || (Count == 0 && !TimeoutUs))
        return STATUS_INVALID_PARAMETER;
    for (uint32_t i = 0; i < Count; i++) {
        for (uint32_t j = i + 1; j < Count; j++) {
            if (Objects[i] == Objects[j])
                return STATUS_INVALID_PARAMETER; /* duplicates would corrupt the wait lists */
        }
    }
    DISPATCHER_HEADER *const *Headers = (DISPATCHER_HEADER *const *)Objects;

    KIRQL OldIrql = KiAcquireDispatcherLock();
    if (Alertable && Thread->Alerted) {
        Thread->Alerted = false;
        KiReleaseDispatcherLock(OldIrql);
        return STATUS_ALERTED;
    }
    if (WaitType == WaitAny) {
        for (uint32_t i = 0; i < Count; i++) {
            if (KiIsSignaled(Headers[i], Thread)) {
                bool Abandoned = KiSatisfy(Headers[i], Thread);
                KiReleaseDispatcherLock(OldIrql);
                return (Abandoned ? STATUS_ABANDONED_WAIT_0 : STATUS_WAIT_0) + (STATUS)i;
            }
        }
    } else if (Count > 0) {
        bool All = true;
        for (uint32_t i = 0; i < Count && All; i++)
            All = KiIsSignaled(Headers[i], Thread);
        if (All) {
            STATUS Status = KiSatisfyAll(Thread, NULL, Count, Headers);
            KiReleaseDispatcherLock(OldIrql);
            return Status;
        }
    }
    if (Poll) {
        KiReleaseDispatcherLock(OldIrql);
        return STATUS_TIMEOUT;
    }

    KWAIT_BLOCK *Blocks = WaitBlockArray ? WaitBlockArray : Thread->WaitBlock;
    Thread->WaitBlockList = Blocks;
    Thread->WaitCount = Count;
    Thread->WaitType = (uint8_t)WaitType;
    Thread->WaitStatus = STATUS_PENDING;
    for (uint32_t i = 0; i < Count; i++) {
        KWAIT_BLOCK *Block = &Blocks[i];
        Block->Thread = Thread;
        Block->Object = Headers[i];
        Block->WaitKey = (uint16_t)i;
        Block->WaitType = (uint8_t)WaitType;
        InsertTailList(&Headers[i]->WaitListHead, &Block->WaitListEntry);
        Block->Linked = true;
    }
    if (TimeoutUs) {
        KWAIT_BLOCK *Block = &Thread->TimerWaitBlock;
        Block->Thread = Thread;
        Block->Object = &Thread->Timer.Header;
        Block->WaitKey = KI_TIMEOUT_WAIT_KEY;
        Block->WaitType = WaitAny;
        InsertTailList(&Thread->Timer.Header.WaitListHead, &Block->WaitListEntry);
        Block->Linked = true;
        KiInsertTimerLocked(&Thread->Timer, KeQueryUptimeMicroseconds() + *TimeoutUs);
    }
    Thread->WaitIsAlertable = Alertable;
    Thread->State = ThreadWaiting;
    KiSwapThread();
    KeLowerIrql(OldIrql);
    if (Thread->WaitStatus == STATUS_ALERTED)
        Thread->Alerted = false;
    return Thread->WaitStatus;
}

STATUS KeWaitForMultipleObjects(uint32_t Count, void *const Objects[], WAIT_TYPE WaitType, const uint64_t *TimeoutUs,
                                KWAIT_BLOCK *WaitBlockArray)
{
    return KeWaitForMultipleObjectsEx(Count, Objects, WaitType, TimeoutUs, WaitBlockArray, false);
}

STATUS KeDelayExecutionThreadEx(uint64_t Microseconds, bool Alertable)
{
    return KeWaitForMultipleObjectsEx(0, NULL, WaitAny, &Microseconds, NULL, Alertable);
}

void KeAlertThread(KTHREAD *Thread)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    if (Thread->State == ThreadWaiting && Thread->WaitIsAlertable)
        KiUnwaitThreadLocked(Thread, STATUS_ALERTED);
    else
        Thread->Alerted = true;
    KiReleaseDispatcherLock(OldIrql);
}

void KeSignalObject(DISPATCHER_HEADER *Header)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    Header->SignalState = 1;
    KiWaitTestLocked(Header);
    KiReleaseDispatcherLock(OldIrql);
}

STATUS KeWaitForSingleObject(void *Object, const uint64_t *TimeoutUs)
{
    void *Objects[1] = {Object};
    return KeWaitForMultipleObjects(1, Objects, WaitAny, TimeoutUs, NULL);
}

void KeDelayExecutionThread(uint64_t Microseconds)
{
    KeWaitForMultipleObjects(0, NULL, WaitAny, &Microseconds, NULL);
}

/* ---- Events ------------------------------------------------------------------------------- */

void KeInitializeEvent(KEVENT *Event, EVENT_TYPE Type, bool Signaled)
{
    KiInitializeDispatcherHeader(&Event->Header,
                                 Type == NotificationEvent ? KObjectEventNotification : KObjectEventSynchronization,
                                 Signaled ? 1 : 0);
}

int32_t KeSetEvent(KEVENT *Event)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    int32_t Previous = Event->Header.SignalState;
    if (Previous == 0) {
        Event->Header.SignalState = 1;
        KiWaitTestLocked(&Event->Header);
    }
    KiReleaseDispatcherLock(OldIrql);
    return Previous;
}

void KeResetEvent(KEVENT *Event)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    Event->Header.SignalState = 0;
    KiReleaseDispatcherLock(OldIrql);
}

int32_t KeReadStateEvent(KEVENT *Event)
{
    return Event->Header.SignalState;
}

/* ---- Semaphores -------------------------------------------------------------------------- */

void KeInitializeSemaphore(KSEMAPHORE *Semaphore, int32_t Count, int32_t Limit)
{
    KiInitializeDispatcherHeader(&Semaphore->Header, KObjectSemaphore, Count);
    Semaphore->Limit = Limit;
}

STATUS KeReleaseSemaphore(KSEMAPHORE *Semaphore, int32_t Adjustment, int32_t *PreviousCount)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    int32_t Previous = Semaphore->Header.SignalState;
    if (Adjustment <= 0 || Previous > Semaphore->Limit - Adjustment) {
        KiReleaseDispatcherLock(OldIrql);
        return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    }
    Semaphore->Header.SignalState = Previous + Adjustment;
    if (Previous == 0)
        KiWaitTestLocked(&Semaphore->Header);
    KiReleaseDispatcherLock(OldIrql);
    if (PreviousCount)
        *PreviousCount = Previous;
    return STATUS_SUCCESS;
}

/* ---- Mutexes ---------------------------------------------------------------------------- */

void KeInitializeMutex(KMUTEX *Mutex)
{
    KiInitializeDispatcherHeader(&Mutex->Header, KObjectMutex, 1);
    InitializeListHead(&Mutex->MutantListEntry);
    Mutex->OwnerThread = NULL;
    Mutex->Abandoned = false;
}

void KeReleaseMutex(KMUTEX *Mutex)
{
    KTHREAD *Thread = KeGetCurrentThread();
    KIRQL OldIrql = KiAcquireDispatcherLock();
    if (Mutex->OwnerThread != Thread) {
        KiReleaseDispatcherLock(OldIrql);
        KeBugCheckEx(MUTEX_NOT_OWNED, (uint64_t)Mutex, (uint64_t)Mutex->OwnerThread, (uint64_t)Thread,
                     (uint64_t)__builtin_return_address(0));
    }
    Mutex->Header.SignalState++;
    if (Mutex->Header.SignalState == 1) {
        Mutex->OwnerThread = NULL;
        RemoveEntryList(&Mutex->MutantListEntry);
        KiWaitTestLocked(&Mutex->Header);
    }
    KiReleaseDispatcherLock(OldIrql);
}

bool KeReadStateMutex(KMUTEX *Mutex)
{
    return Mutex->Header.SignalState > 0;
}

void KiAbandonMutexesLocked(KTHREAD *Thread)
{
    while (!IsListEmpty(&Thread->MutantListHead)) {
        KMUTEX *Mutex = CONTAINING_RECORD(RemoveHeadList(&Thread->MutantListHead), KMUTEX, MutantListEntry);
        Mutex->Header.SignalState = 1;
        Mutex->OwnerThread = NULL;
        Mutex->Abandoned = true;
        KiWaitTestLocked(&Mutex->Header);
    }
}
