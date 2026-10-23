/*
 * Nexora OS - ke/dispatch.c
 * The scheduler: 32 priority-ordered ready queues with a summary bitmap, round robin within a
 * priority, preemption by higher-priority threads, and quantum-based time slicing.
 *
 * Rescheduling is always deferred to the DISPATCH_LEVEL software interrupt (vector 0x2F): whoever
 * makes a better thread ready, and the clock when a quantum expires, only request it (a LAPIC
 * self-IPI, or a directed IPI on vector 0x2F to another processor). The APIC holds the request
 * until IRQL drops below DISPATCH_LEVEL, so a switch can never happen while code holds a spinlock
 * or runs at raised IRQL. Voluntary switches (waits, yields, termination) call KiSwapThread.
 *
 * Multiprocessor (M11): the ready queues are shared by every processor under the dispatcher lock
 * (one global queue scales well enough for a handful of processors and keeps priorities exact).
 * A processor takes the best thread whose affinity includes it. Readying a thread asks one
 * processor to reschedule: an idle one if possible (this one first), else the one running the
 * lowest-priority thread below the new one. A thread switched out by one processor may be made
 * ready and picked by another before its registers are saved: KiSwapThread sets its SwapBusy flag
 * under the lock and KiSwapContext clears it once it is off that stack; the processor switching
 * to it waits for the flag first.
 *
 * Invariant: KiSwapContext is only ever called at DISPATCH_LEVEL with interrupts disabled, so
 * every thread resumes at DISPATCH_LEVEL; new threads start in KiThreadStartup at that level.
 */
#include "ki.h"

KSPIN_LOCK KiDispatcherLock;
KPROCESS *KeSystemProcess;
KTHREAD KiIdleThread;
static LIST_ENTRY KiReadyQueue[32];
volatile uint32_t KiReadySummary;
static bool KiSchedulerReady;
uint64_t KiQuantumCycles;

extern uint8_t KiBootStackTop[];

void KiRequestDispatchInterrupt(void)
{
    HalRequestDispatchInterrupt();
}

static inline uint64_t KiProcessorBit(const KPRCB *Prcb)
{
    return 1ull << Prcb->Number;
}

static void KiInsertReadyQueue(KTHREAD *Thread, bool AtHead)
{
    LIST_ENTRY *Queue = &KiReadyQueue[Thread->Priority];
    if (AtHead)
        InsertHeadList(Queue, &Thread->QueueEntry);
    else
        InsertTailList(Queue, &Thread->QueueEntry);
    KiReadySummary |= 1u << Thread->Priority;
    Thread->State = ThreadReady;
}

static void KiRemoveFromReadyQueue(KTHREAD *Thread)
{
    if (RemoveEntryList(&Thread->QueueEntry))
        KiReadySummary &= ~(1u << Thread->Priority);
}

/* The best ready thread this processor may run (NULL if none), optionally removed. */
static KTHREAD *KiFindReadyThread(KPRCB *Prcb, bool Remove)
{
    uint64_t Bit = KiProcessorBit(Prcb);
    uint32_t Summary = KiReadySummary;
    while (Summary) {
        int Priority = 31 - __builtin_clz(Summary);
        Summary &= ~(1u << Priority);
        LIST_ENTRY *Queue = &KiReadyQueue[Priority];
        for (LIST_ENTRY *E = Queue->Flink; E != Queue; E = E->Flink) {
            KTHREAD *Thread = CONTAINING_RECORD(E, KTHREAD, QueueEntry);
            if (!(Thread->Affinity & Bit))
                continue;
            if (Remove)
                KiRemoveFromReadyQueue(Thread);
            return Thread;
        }
    }
    return NULL;
}

static int KiHighestReadyPriority(KPRCB *Prcb)
{
    KTHREAD *Thread = KiFindReadyThread(Prcb, false);
    return Thread ? Thread->Priority : -1;
}

/* For the clock interrupt on an idle processor. The queues may only be walked under the
   dispatcher lock: walked without it, a thread moved to another queue under the cursor leads the
   walk into that (circular) list for ever - an endless clock interrupt, found in M11 as a
   SPIN_LOCK_TIMEOUT when the stuck processor's idle thread was reaping under the kernel
   address-space lock. Only a try: this processor may hold the lock below CLOCK_LEVEL; then the
   next tick asks again. */
bool KiReadyThreadAvailable(KPRCB *Prcb)
{
    if (!KiReadySummary || !KeTryToAcquireSpinLockAtDpcLevel(&KiDispatcherLock))
        return false;
    bool Found = KiFindReadyThread(Prcb, false) != NULL;
    KeReleaseSpinLockFromDpcLevel(&KiDispatcherLock);
    return Found;
}

/* Asks Target to reschedule (this processor through a self-IPI, another through an IPI). */
static void KiRequestReschedule(KPRCB *Target)
{
    Target->PreemptRequested = true;
    if (Target == KeGetCurrentPrcb())
        KiRequestDispatchInterrupt();
    else
        HalSendIpi(Target->ApicId, DISPATCH_VECTOR);
}

void KiReadyThreadLocked(KTHREAD *Thread)
{
    KiInsertReadyQueue(Thread, false);
    KPRCB *Self = KeGetCurrentPrcb();
    uint64_t Allowed = Thread->Affinity & KeActiveProcessors;
    if (!Allowed)
        return; /* nowhere to run (cannot happen: affinity is checked when set) */

    /* An idle processor, this one first. */
    KPRCB *Target = NULL;
    if ((Allowed & KiProcessorBit(Self)) && Self->CurrentThread == Self->IdleThread)
        Target = Self;
    for (uint32_t n = 0; !Target && n < KI_MAX_PROCESSORS; n++) {
        KPRCB *P = KiProcessorBlock[n];
        if (P && P->Online && (Allowed & KiProcessorBit(P)) && P->CurrentThread == P->IdleThread && !P->PreemptRequested)
            Target = P;
    }
    /* Else the processor running the lowest-priority thread below this one (this one on ties). */
    if (!Target) {
        int Lowest = Thread->Priority;
        if ((Allowed & KiProcessorBit(Self)) && Self->CurrentThread->Priority < Lowest) {
            Target = Self;
            Lowest = Self->CurrentThread->Priority;
        }
        for (uint32_t n = 0; n < KI_MAX_PROCESSORS; n++) {
            KPRCB *P = KiProcessorBlock[n];
            if (!P || P == Self || !P->Online || !(Allowed & KiProcessorBit(P)))
                continue;
            KTHREAD *Current = P->CurrentThread;
            if (Current && Current->Priority < Lowest) {
                Target = P;
                Lowest = Current->Priority;
            }
        }
    }
    if (Target)
        KiRequestReschedule(Target);
}

static void KiSwitchTo(KTHREAD *Old, KTHREAD *New)
{
    KPRCB *Prcb = KeGetCurrentPrcb();
    uint64_t Flags = __readrflags();
    HalDisableInterrupts();

    /* The processor that last ran New may still be saving it (see the file comment). */
    while (__atomic_load_n(&New->SwapBusy, __ATOMIC_ACQUIRE)) {
        if (KeNumberProcessors > 1)
            KiServiceIpiRequests();
        HalPause();
    }

    /* Charge the outgoing thread in TSC cycles; an expired quantum is refilled for its next turn. */
    uint64_t Now = HalReadTsc();
    uint64_t Ran = Now - Old->QuantumStart;
    Old->CycleTime += Ran;
    if (Old != Prcb->IdleThread) {
        Old->QuantumRemaining -= (int64_t)Ran;
        if (Old->QuantumRemaining <= 0)
            KiResetQuantum(Old);
    }
    New->QuantumStart = Now;

    /* User FPU/SSE/AVX state. Kernel code never touches those registers (it is built
       general-registers-only), so only threads that run user code carry an XSAVE area. */
    if (Old->XSaveArea)
        KiSaveExtendedState(Old->XSaveArea);
    if (New->XSaveArea)
        KiRestoreExtendedState(New->XSaveArea);
    /* While in kernel mode, KERNEL_GS_BASE holds the user GS base (the TEB); swapgs swaps. */
    __writemsr(MSR_KERNEL_GS_BASE, New->UserGsBase);

    Prcb->CurrentThread = New;
    Prcb->ContextSwitches++;
    New->ContextSwitches++;
    Prcb->KernelStackTop = (uint64_t)New->StackTop;
    HalSetKernelStack((uint64_t)New->StackTop);
    uint64_t Pml4 = ((MMADDRESS_SPACE *)New->AddressSpace)->Pml4;
    if ((__readcr3() & PTE_ADDRESS_MASK) != Pml4)
        __writecr3(Pml4);

    KiSwapContext(&Old->KernelStackPointer, New->KernelStackPointer, &Old->SwapBusy);

    /* Resumed: this is Old again, on its own stack. */
    if (Flags & RFLAGS_IF)
        HalEnableInterrupts();
}

void KiSwapThread(void)
{
    KPRCB *Prcb = KeGetCurrentPrcb();
    KTHREAD *Old = Prcb->CurrentThread;
    KTHREAD *New = KiFindReadyThread(Prcb, true);
    if (!New)
        New = Prcb->IdleThread;
    if (Old->State == ThreadRunning && Old != Prcb->IdleThread)
        KeBugCheckEx(THREAD_STATE_CORRUPT, (uint64_t)Old, Old->State, 1, 0);
    New->State = ThreadRunning;
    New->Processor = (uint8_t)Prcb->Number;
    Prcb->PreemptRequested = false;
    if (New != Old)
        Old->SwapBusy = 1; /* until KiSwapContext has left its stack */

    KeReleaseSpinLockFromDpcLevel(&KiDispatcherLock);
    KeLowerIrql(DISPATCH_LEVEL);
    if (New != Old)
        KiSwitchTo(Old, New);
    KiReapTerminatedThreads();
}

void KiDispatchSoftwareInterrupt(void)
{
    if (!KiSchedulerReady)
        return;
    KPRCB *Prcb = KeGetCurrentPrcb();
    if (!IsListEmpty(&Prcb->DpcListHead))
        KiRetireDpcList(Prcb);
    if (!Prcb->QuantumEnd && !Prcb->PreemptRequested)
        return;

    KIRQL OldIrql = KiAcquireDispatcherLock();
    bool QuantumEnd = Prcb->QuantumEnd;
    Prcb->QuantumEnd = false;
    Prcb->PreemptRequested = false;
    KTHREAD *Current = Prcb->CurrentThread;
    int Best = KiHighestReadyPriority(Prcb);

    if (Current == Prcb->IdleThread) {
        if (Best >= 0) {
            KiSwapThread();
            return;
        }
    } else if (!(Current->Affinity & KiProcessorBit(Prcb))) {
        /* Its affinity no longer includes this processor: give it up. */
        KiReadyThreadLocked(Current);
        KiSwapThread();
        return;
    } else if (Best >= 0 && (Best > Current->Priority || (QuantumEnd && Best >= Current->Priority))) {
        /* Preempted threads keep their place at the head; expired ones go to the tail (their
           quantum is refilled when KiSwitchTo charges them). */
        KiInsertReadyQueue(Current, !QuantumEnd);
        KiSwapThread();
        return;
    } else if (QuantumEnd) {
        /* Nobody else to run: account the expired slice and start a new quantum. */
        uint64_t Now = HalReadTsc();
        Current->CycleTime += Now - Current->QuantumStart;
        Current->QuantumStart = Now;
        KiResetQuantum(Current);
    }
    KiReleaseDispatcherLock(OldIrql);
}

void KeYieldThread(void)
{
    /* Lock-free first (M11): with nothing ready at this priority or above, yielding is a no-op.
       A thread yielding in a loop would otherwise hold the dispatcher lock most of the time, and
       a processor waiting for it (a timer DPC, a wake-up) could starve: found in VirtualBox,
       whose Hyper-V backend deschedules a virtual processor that spins on PAUSE. */
    if ((KiReadySummary >> KeGetCurrentThread()->Priority) == 0) {
        /* A thread yielding in a loop never leaves guest code. VirtualBox's Hyper-V backend runs
           every virtual timer on the first processor's thread, so such a loop there stopped all
           clocks (both processors' timers stood still for minutes). Touch the local APIC (a VM
           exit) at most once per millisecond of TSC time; it costs nothing measurable elsewhere. */
        KPRCB *Prcb = KeGetCurrentPrcb();
        uint64_t Now = HalReadTsc(), PerMs = HalQueryTscFrequency() / 1000u;
        if (Now - Prcb->LastApicTouchTsc >= PerMs) {
            Prcb->LastApicTouchTsc = Now;
            HalTouchLocalApic();
        }
        return;
    }
    KIRQL OldIrql = KiAcquireDispatcherLock();
    KTHREAD *Current = KeGetCurrentThread();
    if (KiHighestReadyPriority(KeGetCurrentPrcb()) >= Current->Priority) {
        KiInsertReadyQueue(Current, false);
        KiSwapThread();
        KeLowerIrql(OldIrql);
        return;
    }
    KiReleaseDispatcherLock(OldIrql);
}

KPRIORITY KeSetPriorityThread(KTHREAD *Thread, KPRIORITY Priority)
{
    if (Priority < LOW_PRIORITY + 1)
        Priority = LOW_PRIORITY + 1; /* priority 0 belongs to the idle threads */
    if (Priority > HIGH_PRIORITY)
        Priority = HIGH_PRIORITY;
    KIRQL OldIrql = KiAcquireDispatcherLock();
    KPRIORITY Old = Thread->Priority;
    if (Thread->State == ThreadReady) {
        KiRemoveFromReadyQueue(Thread);
        Thread->Priority = Thread->BasePriority = Priority;
        KiReadyThreadLocked(Thread);
    } else {
        Thread->Priority = Thread->BasePriority = Priority;
        if (Thread->State == ThreadRunning && Priority < Old) {
            KPRCB *P = KiProcessorBlock[Thread->Processor];
            if (P && P->CurrentThread == Thread && KiHighestReadyPriority(P) > Priority)
                KiRequestReschedule(P);
        }
    }
    KiReleaseDispatcherLock(OldIrql);
    return Old;
}

uint64_t KeSetAffinityThread(KTHREAD *Thread, uint64_t Affinity)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    uint64_t Old = Thread->Affinity;
    if (!(Affinity & KeActiveProcessors)) {
        KiReleaseDispatcherLock(OldIrql);
        return Old; /* would leave it nowhere to run: unchanged */
    }
    Thread->Affinity = Affinity;
    if (Thread->State == ThreadReady) {
        KiRemoveFromReadyQueue(Thread);
        KiReadyThreadLocked(Thread);
    } else if (Thread->State == ThreadRunning && !(Affinity & (1ull << Thread->Processor))) {
        KPRCB *P = KiProcessorBlock[Thread->Processor];
        if (P)
            KiRequestReschedule(P); /* it moves at its next dispatch interrupt */
    }
    KiReleaseDispatcherLock(OldIrql);
    /* The caller moving itself waits until it runs on an allowed processor. */
    while (Thread == KeGetCurrentThread() && !(Affinity & (1ull << KeGetCurrentPrcb()->Number)))
        KeYieldThread();
    return Old;
}

uint32_t KeQueryThreadCount(void)
{
    return KiThreadCount;
}

uint64_t KeQueryThreadCpuTimeUs(KTHREAD *Thread)
{
    uint64_t Cycles = Thread->CycleTime;
    if (Thread->State == ThreadRunning) {
        uint64_t Now = HalReadTsc(), Start = Thread->QuantumStart;
        if (Now > Start)
            Cycles += Now - Start;
    }
    uint64_t PerUs = HalQueryTscFrequency() / 1000000u;
    return PerUs ? Cycles / PerUs : 0;
}

void KeInitializeProcess(KPROCESS *Process, struct _MMADDRESS_SPACE *AddressSpace, KPRIORITY BasePriority,
                         const char *Name, uint32_t ProcessId)
{
    KiInitializeDispatcherHeader(&Process->Header, KObjectProcess, 0);
    Process->AddressSpace = AddressSpace;
    InitializeListHead(&Process->ThreadListHead);
    Process->ProcessId = ProcessId;
    Process->ThreadCount = 0;
    Process->BasePriority = BasePriority;
    RtlStringCopy(Process->Name, sizeof(Process->Name), Name);
}

void KeInitializeScheduler(KPROCESS *SystemProcess)
{
    for (int i = 0; i < 32; i++)
        InitializeListHead(&KiReadyQueue[i]);
    InitializeListHead(&KiThreadListHead);
    KPRCB *Prcb = KeGetCurrentPrcb();
    InitializeListHead(&Prcb->DpcListHead);
    KiInitializeTimers();

    KiQuantumCycles = HalQueryTscFrequency() / 1000000u * KI_QUANTUM_US;

    KeSystemProcess = SystemProcess;
    KiProcessorBlock[0] = Prcb;

    /* The boot context (on the .bss boot stack) becomes this CPU's idle thread. */
    KiInitializeThreadObject(&KiIdleThread, KeSystemProcess, LOW_PRIORITY, "Idle");
    KiIdleThread.StackTop = KiBootStackTop;
    KiIdleThread.ReferenceCount = 1;
    KiIdleThread.State = ThreadRunning;
    KiIdleThread.QuantumStart = HalReadTsc();
    KiIdleThread.Affinity = 1;
    Prcb->IdleThread = &KiIdleThread;
    Prcb->CurrentThread = &KiIdleThread;
    Prcb->KernelStackTop = (uint64_t)KiBootStackTop;
    Prcb->Online = 1;
    HalSetKernelStack((uint64_t)KiBootStackTop);
    KiSchedulerReady = true;
}
