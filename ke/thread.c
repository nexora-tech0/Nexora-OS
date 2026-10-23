/*
 * Nexora OS - ke/thread.c
 * Thread initialization, start-up, termination and reaping.
 *
 * A KTHREAD lives inside an owning object: an ETHREAD (Ps, reference counted by the object
 * manager) or, for bare kernel threads created with KeCreateThread, a pool block with its own
 * reference count. KeInitializeThread gives it a guarded kernel stack whose top holds a frame that
 * KiSwapContext "returns" into: KiThreadStartupStub -> KiThreadStartup -> StartRoutine(Context)
 * -> KeTerminateThread. A terminating thread cannot free the stack it runs on, so it parks itself
 * on the reaper list; the next thread to run (in KiSwapThread or KiThreadStartup) frees the
 * stack and calls the thread's ReleaseRoutine to drop the running reference.
 */
#include "ki.h"

#define KI_THREAD_TAG HZ_POOL_TAG('K', 'e', 'T', 'h')

LIST_ENTRY KiThreadListHead;
uint32_t KiThreadCount;
static LIST_ENTRY KiReaperListHead = {&KiReaperListHead, &KiReaperListHead};
static volatile uint32_t KiNextThreadId = 1;

HZ_NORETURN void KiThreadStartup(KTHREAD *Thread);

void KiInitializeThreadObject(KTHREAD *Thread, KPROCESS *Process, KPRIORITY Priority, const char *Name)
{
    KiInitializeDispatcherHeader(&Thread->Header, KObjectThread, 0);
    Thread->Process = Process;
    Thread->AddressSpace = Process->AddressSpace;
    Thread->Priority = Thread->BasePriority = Priority;
    KiResetQuantum(Thread);
    InitializeListHead(&Thread->MutantListHead);
    InitializeListHead(&Thread->QueueEntry);
    KeInitializeTimerEx(&Thread->Timer, NotificationTimer);
    /* Affinity: the creator's, except that threads made by an idle thread (the boot context) may
       run anywhere. */
    KPRCB *Prcb = KeGetCurrentPrcb();
    KTHREAD *Creator = Prcb ? Prcb->CurrentThread : NULL;
    Thread->Affinity = (Creator && Creator != Prcb->IdleThread) ? Creator->Affinity : ~0ull;
    Thread->ThreadId = Priority == LOW_PRIORITY && !KiThreadCount ? 0 : __atomic_fetch_add(&KiNextThreadId, 1, __ATOMIC_RELAXED);
    RtlStringCopy(Thread->Name, sizeof(Thread->Name), Name ? Name : "Thread");

    KIRQL OldIrql = KiAcquireDispatcherLock();
    InsertTailList(&Process->ThreadListHead, &Thread->ThreadListEntry);
    Process->ThreadCount++;
    InsertTailList(&KiThreadListHead, &Thread->GlobalListEntry);
    KiThreadCount++;
    KiReleaseDispatcherLock(OldIrql);
}

STATUS KeInitializeThread(KTHREAD *Thread, KPROCESS *Process, KSTART_ROUTINE Routine, void *Context,
                          KPRIORITY Priority, const char *Name, void (*ReleaseRoutine)(KTHREAD *Thread))
{
    if (!Routine || !ReleaseRoutine || Priority <= LOW_PRIORITY || Priority > HIGH_PRIORITY)
        return STATUS_INVALID_PARAMETER;
    void *Stack = MmCreateKernelStack();
    if (!Stack)
        return STATUS_NO_MEMORY;
    Thread->StackTop = Stack;
    Thread->StartRoutine = Routine;
    Thread->StartContext = Context;
    Thread->ReleaseRoutine = ReleaseRoutine;

    /* Initial frame consumed by KiSwapContext: r15 r14 r13 r12 rbx rbp, then the return address. */
    uint64_t *Sp = (uint64_t *)Stack;
    *--Sp = 0;                                  /* terminates the frame chain; keeps alignment */
    *--Sp = (uint64_t)KiThreadStartupStub;
    *--Sp = 0;                                  /* rbp */
    *--Sp = 0;                                  /* rbx */
    *--Sp = (uint64_t)Thread;                   /* r12: handed to KiThreadStartup */
    *--Sp = 0;                                  /* r13 */
    *--Sp = 0;                                  /* r14 */
    *--Sp = 0;                                  /* r15 */
    Thread->KernelStackPointer = (uint64_t)Sp;

    KiInitializeThreadObject(Thread, Process, Priority, Name);
    return STATUS_SUCCESS;
}

void KeStartThread(KTHREAD *Thread)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    KiReadyThreadLocked(Thread);
    KiReleaseDispatcherLock(OldIrql);
}

/* ---- Bare pool-allocated threads ----------------------------------------------------------------- */

static void KiReleasePoolThread(KTHREAD *Thread)
{
    KeDereferenceThread(Thread);
}

STATUS KeCreateThread(KTHREAD **Out, KPROCESS *Process, KSTART_ROUTINE Routine, void *Context, KPRIORITY Priority,
                      const char *Name)
{
    KTHREAD *Thread = ExAllocatePoolZero(NonPagedPool, sizeof(KTHREAD), KI_THREAD_TAG);
    if (!Thread)
        return STATUS_NO_MEMORY;
    Thread->ReferenceCount = 2; /* one for running, one returned to the creator */
    STATUS Status = KeInitializeThread(Thread, Process, Routine, Context, Priority, Name, KiReleasePoolThread);
    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Thread, KI_THREAD_TAG);
        return Status;
    }
    KeStartThread(Thread);
    *Out = Thread;
    return STATUS_SUCCESS;
}

void KeReferenceThread(KTHREAD *Thread)
{
    __atomic_add_fetch(&Thread->ReferenceCount, 1, __ATOMIC_RELAXED);
}

void KeDereferenceThread(KTHREAD *Thread)
{
    int32_t Remaining = __atomic_sub_fetch(&Thread->ReferenceCount, 1, __ATOMIC_ACQ_REL);
    if (Remaining > 0)
        return;
    if (Remaining < 0 || Thread->State != ThreadTerminated || Thread->StackTop)
        KeBugCheckEx(THREAD_STATE_CORRUPT, (uint64_t)Thread, Thread->State, 4, (uint64_t)Remaining);
    ExFreePoolWithTag(Thread, KI_THREAD_TAG);
}

/* ---- Start-up and termination ------------------------------------------------------------------- */

HZ_NORETURN void KiThreadStartup(KTHREAD *Thread)
{
    /* Entered from KiSwapContext at DISPATCH_LEVEL with interrupts disabled. */
    KiReapTerminatedThreads();
    KeLowerIrql(PASSIVE_LEVEL);
    HalEnableInterrupts();
    Thread->StartRoutine(Thread->StartContext);
    KeTerminateThread();
}

HZ_NORETURN void KeTerminateThread(void)
{
    KTHREAD *Thread = KeGetCurrentThread();
    if (Thread == &KiIdleThread)
        KeBugCheckEx(THREAD_STATE_CORRUPT, (uint64_t)Thread, Thread->State, 2, 0);

    KIRQL OldIrql = KiAcquireDispatcherLock();
    (void)OldIrql;
    KiAbandonMutexesLocked(Thread);
    Thread->Header.SignalState = 1;
    KiWaitTestLocked(&Thread->Header); /* wake threads waiting for us to exit */
    Thread->State = ThreadTerminated;
    RemoveEntryList(&Thread->ThreadListEntry);
    Thread->Process->ThreadCount--;
    RemoveEntryList(&Thread->GlobalListEntry);
    KiThreadCount--;
    InsertTailList(&KiReaperListHead, &Thread->QueueEntry);
    KiSwapThread();
    KeBugCheckEx(THREAD_STATE_CORRUPT, (uint64_t)Thread, Thread->State, 3, 0);
}

void KiReapTerminatedThreads(void)
{
    for (;;) {
        KTHREAD *Victim = NULL;
        KIRQL OldIrql = KiAcquireDispatcherLock();
        for (LIST_ENTRY *E = KiReaperListHead.Flink; E != &KiReaperListHead; E = E->Flink) {
            KTHREAD *T = CONTAINING_RECORD(E, KTHREAD, QueueEntry);
            if (T != KeGetCurrentThread() && !__atomic_load_n(&T->SwapBusy, __ATOMIC_ACQUIRE)) {
                RemoveEntryList(E);
                Victim = T;
                break;
            }
        }
        KiReleaseDispatcherLock(OldIrql);
        if (!Victim)
            return;
        MmDeleteKernelStack(Victim->StackTop);
        Victim->StackTop = NULL;
        Victim->ReleaseRoutine(Victim); /* may free the thread (and, for Ps, its process) */
    }
}
