/*
 * Nexora OS - ke/ketest.c
 * Boot-time scheduler and synchronization self-tests, run from the Phase1Init thread
 * (priority 8). Every wait has a timeout, so a broken primitive shows up as a failed test
 * rather than a hung boot. The last check verifies that all test threads were reaped (stacks
 * and pool returned).
 *
 * With several processors (M11) the time-slicing and preemption tests pin themselves (and, by
 * inheritance, their threads) to processor 0, where their single-processor expectations hold;
 * extra tests then run threads on every processor at once, wake threads across processors,
 * migrate a thread from processor to processor, and check TLB shootdowns reach every processor.
 */
#include "ki.h"

#define KT_PRIORITY KI_THREAD_PRIORITY_DEFAULT
#define KT_JOIN_TIMEOUT_US 5000000ull

static uint64_t KtTimeout = KT_JOIN_TIMEOUT_US;

static void KtReport(bool Ok, const char *Format, ...) HZ_PRINTF(2, 3);
static void KtReport(bool Ok, const char *Format, ...)
{
    char Text[160];
    va_list Args;
    va_start(Args, Format);
    RtlFormatStringV(Text, sizeof(Text), Format, Args);
    va_end(Args);
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL, "KeTest: %s: %s\n", Text, Ok ? "OK" : "FAILED");
}

static bool KtJoin(KTHREAD *Thread)
{
    STATUS Status = KeWaitForSingleObject(Thread, &KtTimeout);
    KeDereferenceThread(Thread);
    return Status == STATUS_WAIT_0;
}

/* ---- 1. create / join ------------------------------------------------------------------------ */

static volatile int64_t KtSum;

static void KtAddRoutine(void *Context)
{
    __atomic_add_fetch(&KtSum, (int64_t)(uintptr_t)Context, __ATOMIC_RELAXED);
}

static bool KtTestCreateJoin(void)
{
    enum { Count = 8 };
    KTHREAD *Threads[Count];
    void *Objects[Count];
    KWAIT_BLOCK Blocks[Count];
    KtSum = 0;
    for (int i = 0; i < Count; i++) {
        if (!HZ_SUCCESS(KeCreateThread(&Threads[i], KeSystemProcess, KtAddRoutine, (void *)(uintptr_t)(i + 1),
                                       KT_PRIORITY, "KtWorker")))
            return false;
        Objects[i] = Threads[i];
    }
    STATUS Status = KeWaitForMultipleObjects(Count, Objects, WaitAll, &KtTimeout, Blocks);
    for (int i = 0; i < Count; i++)
        KeDereferenceThread(Threads[i]);
    bool Ok = Status == STATUS_WAIT_0 && KtSum == Count * (Count + 1) / 2;
    KtReport(Ok, "%d threads created and joined with WaitAll", Count);
    return Ok;
}

/* ---- 2. event ping-pong ------------------------------------------------------------------------ */

enum { KtRounds = 1000 };
static KEVENT KtPing, KtPong;
static volatile uint32_t KtPongCount;

static void KtPongRoutine(void *Context)
{
    for (int i = 0; i < KtRounds; i++) {
        if (KeWaitForSingleObject(&KtPing, &KtTimeout) != STATUS_WAIT_0)
            return;
        KtPongCount++;
        KeSetEvent(&KtPong);
    }
}

static bool KtTestPingPong(uint64_t *NanosecondsPerRoundTrip)
{
    KeInitializeEvent(&KtPing, SynchronizationEvent, false);
    KeInitializeEvent(&KtPong, SynchronizationEvent, false);
    KtPongCount = 0;
    KTHREAD *Thread;
    if (!HZ_SUCCESS(KeCreateThread(&Thread, KeSystemProcess, KtPongRoutine, NULL, KT_PRIORITY, "KtPong")))
        return false;
    uint64_t Start = KeQueryUptimeMicroseconds();
    bool Ok = true;
    for (int i = 0; i < KtRounds && Ok; i++) {
        KeSetEvent(&KtPing);
        Ok = KeWaitForSingleObject(&KtPong, &KtTimeout) == STATUS_WAIT_0;
    }
    uint64_t Elapsed = KeQueryUptimeMicroseconds() - Start;
    Ok = KtJoin(Thread) && Ok && KtPongCount == KtRounds;
    *NanosecondsPerRoundTrip = Elapsed * 1000 / KtRounds;
    KtReport(Ok, "%d event ping-pong round trips (%llu ns each)", KtRounds, (unsigned long long)*NanosecondsPerRoundTrip);
    return Ok;
}

/* ---- 3. semaphores: bounded buffer -------------------------------------------------------------- */

enum { KtSlots = 8, KtItems = 2000 };
static uint32_t KtBuffer[KtSlots];
static uint32_t KtHead, KtTail;
static KSEMAPHORE KtEmpty, KtFull;
static KMUTEX KtBufferMutex;
static volatile uint64_t KtConsumed;

static void KtProducer(void *Context)
{
    for (uint32_t v = 1; v <= KtItems; v++) {
        if (KeWaitForSingleObject(&KtEmpty, &KtTimeout) != STATUS_WAIT_0)
            return;
        KeWaitForSingleObject(&KtBufferMutex, NULL);
        KtBuffer[KtHead++ % KtSlots] = v;
        KeReleaseMutex(&KtBufferMutex);
        KeReleaseSemaphore(&KtFull, 1, NULL);
    }
}

static void KtConsumer(void *Context)
{
    for (uint32_t n = 0; n < KtItems; n++) {
        if (KeWaitForSingleObject(&KtFull, &KtTimeout) != STATUS_WAIT_0)
            return;
        KeWaitForSingleObject(&KtBufferMutex, NULL);
        uint32_t v = KtBuffer[KtTail++ % KtSlots];
        KeReleaseMutex(&KtBufferMutex);
        KeReleaseSemaphore(&KtEmpty, 1, NULL);
        KtConsumed += v;
    }
}

static bool KtTestSemaphores(void)
{
    KeInitializeSemaphore(&KtEmpty, KtSlots, KtSlots);
    KeInitializeSemaphore(&KtFull, 0, KtSlots);
    KeInitializeMutex(&KtBufferMutex);
    KtHead = KtTail = 0;
    KtConsumed = 0;
    KTHREAD *Producer, *Consumer;
    if (!HZ_SUCCESS(KeCreateThread(&Producer, KeSystemProcess, KtProducer, NULL, KT_PRIORITY, "KtProducer")) ||
        !HZ_SUCCESS(KeCreateThread(&Consumer, KeSystemProcess, KtConsumer, NULL, KT_PRIORITY, "KtConsumer")))
        return false;
    bool Ok = KtJoin(Producer);
    Ok = KtJoin(Consumer) && Ok;
    int32_t Previous = 0;
    bool LimitOk = KeReleaseSemaphore(&KtEmpty, 1, &Previous) == STATUS_SEMAPHORE_LIMIT_EXCEEDED;
    Ok = Ok && LimitOk && KtConsumed == (uint64_t)KtItems * (KtItems + 1) / 2;
    KtReport(Ok, "producer/consumer through an %d-slot buffer, %d items, limit enforced", KtSlots, KtItems);
    return Ok;
}

/* ---- 4. mutex recursion and contention -------------------------------------------------------- */

static KMUTEX KtMutex;
static volatile uint32_t KtCounter;

static void KtMutexRoutine(void *Context)
{
    for (int i = 0; i < 300; i++) {
        KeWaitForSingleObject(&KtMutex, NULL);
        uint32_t Value = KtCounter;
        KeYieldThread(); /* without the mutex this would lose updates */
        KtCounter = Value + 1;
        KeReleaseMutex(&KtMutex);
    }
}

static bool KtTestMutex(void)
{
    KeInitializeMutex(&KtMutex);
    bool Ok = true;
    for (int i = 0; i < 3; i++)
        Ok = Ok && KeWaitForSingleObject(&KtMutex, &KtTimeout) == STATUS_WAIT_0;
    Ok = Ok && !KeReadStateMutex(&KtMutex);
    for (int i = 0; i < 3; i++)
        KeReleaseMutex(&KtMutex);
    Ok = Ok && KeReadStateMutex(&KtMutex);

    enum { Count = 4 };
    KTHREAD *Threads[Count];
    KtCounter = 0;
    for (int i = 0; i < Count; i++) {
        if (!HZ_SUCCESS(KeCreateThread(&Threads[i], KeSystemProcess, KtMutexRoutine, NULL, KT_PRIORITY, "KtMutex")))
            return false;
    }
    for (int i = 0; i < Count; i++)
        Ok = KtJoin(Threads[i]) && Ok;
    Ok = Ok && KtCounter == Count * 300;
    KtReport(Ok, "mutex recursion x3 and %d contending threads (%u increments)", Count, KtCounter);
    return Ok;
}

/* ---- 5. time slicing ---------------------------------------------------------------------------- */

static volatile uint64_t KtSpinDeadline;
static volatile uint64_t KtSpins[2];

static void KtSpinRoutine(void *Context)
{
    volatile uint64_t *Counter = &KtSpins[(uintptr_t)Context];
    while (KeQueryUptimeMicroseconds() < KtSpinDeadline)
        (*Counter)++;
}

static bool KtTestTimeSlicing(void)
{
    uint64_t Affinity = KeSetAffinityThread(KeGetCurrentThread(), 1); /* processor 0 only */
    KtSpins[0] = KtSpins[1] = 0;
    KtSpinDeadline = KeQueryUptimeMicroseconds() + 200000;
    uint64_t SwitchesBefore = KeGetCurrentPrcb()->ContextSwitches;
    uint64_t TicksBefore = KeQueryTickCount();
    KTHREAD *A, *B;
    if (!HZ_SUCCESS(KeCreateThread(&A, KeSystemProcess, KtSpinRoutine, (void *)0, KT_PRIORITY, "KtSpinA")) ||
        !HZ_SUCCESS(KeCreateThread(&B, KeSystemProcess, KtSpinRoutine, (void *)1, KT_PRIORITY, "KtSpinB")))
        return false;
    bool Ok = KtJoin(A);
    Ok = KtJoin(B) && Ok;
    uint64_t Ticks = KeQueryTickCount() - TicksBefore;
    uint64_t Lo = HZ_MIN(KtSpins[0], KtSpins[1]), Hi = HZ_MAX(KtSpins[0], KtSpins[1]);
    uint64_t Switches = KeGetCurrentPrcb()->ContextSwitches - SwitchesBefore;
    Ok = Ok && Lo > 0 && Lo * 4 >= Hi && Switches >= 10;
    KtReport(Ok, "two CPU-bound threads shared 200 ms (%llu clock ticks, %llu context switches, ratio %llu%%)",
             (unsigned long long)Ticks, (unsigned long long)Switches, (unsigned long long)(Hi ? Lo * 100 / Hi : 0));
    KeSetAffinityThread(KeGetCurrentThread(), Affinity);
    return Ok;
}

/* ---- 6. priority preemption --------------------------------------------------------------------- */

static KEVENT KtHighGo;
static volatile int KtOrder[2];
static volatile int KtOrderCount;

static void KtHighRoutine(void *Context)
{
    if (KeWaitForSingleObject(&KtHighGo, &KtTimeout) == STATUS_WAIT_0)
        KtOrder[KtOrderCount++] = 1;
}

static bool KtTestPriority(void)
{
    uint64_t Affinity = KeSetAffinityThread(KeGetCurrentThread(), 1); /* processor 0 only */
    KeInitializeEvent(&KtHighGo, NotificationEvent, false);
    KtOrderCount = 0;
    KTHREAD *High;
    if (!HZ_SUCCESS(KeCreateThread(&High, KeSystemProcess, KtHighRoutine, NULL, KT_PRIORITY + 4, "KtHigh")))
        return false;
    /* KtHigh preempted us when it was created and is now blocked on the event. */
    KeSetEvent(&KtHighGo); /* readies it: it must run before the next line */
    KtOrder[KtOrderCount++] = 2;
    bool Ok = KtJoin(High) && KtOrderCount == 2 && KtOrder[0] == 1 && KtOrder[1] == 2;
    KtReport(Ok, "higher-priority thread preempts its waker immediately");
    KeSetAffinityThread(KeGetCurrentThread(), Affinity);
    return Ok;
}

/* ---- 6b. multiprocessor (M11) --------------------------------------------------------------------- */

static volatile uint64_t KtParallelStart[KI_MAX_PROCESSORS], KtParallelEnd[KI_MAX_PROCESSORS];
static volatile uint32_t KtParallelCpu[KI_MAX_PROCESSORS];
static volatile uint64_t KtParallelWork;

static void KtParallelRoutine(void *Context)
{
    uint32_t Index = (uint32_t)(uintptr_t)Context;
    KtParallelStart[Index] = KeQueryUptimeMicroseconds();
    KtParallelCpu[Index] = KeGetCurrentPrcb()->Number;
    uint64_t Deadline = KtParallelStart[Index] + 100000;
    while (KeQueryUptimeMicroseconds() < Deadline)
        __atomic_add_fetch(&KtParallelWork, 1, __ATOMIC_RELAXED);
    KtParallelEnd[Index] = KeQueryUptimeMicroseconds();
}

/* Starts Routine on processor Number (the new thread inherits the creator's affinity). */
static KTHREAD *KtStartOn(uint32_t Number, KSTART_ROUTINE Routine, void *Context, const char *Name)
{
    uint64_t Affinity = KeSetAffinityThread(KeGetCurrentThread(), 1ull << Number);
    KTHREAD *Thread = NULL;
    if (!HZ_SUCCESS(KeCreateThread(&Thread, KeSystemProcess, Routine, Context, KT_PRIORITY, Name)))
        Thread = NULL;
    KeSetAffinityThread(KeGetCurrentThread(), Affinity);
    return Thread;
}

static bool KtTestParallel(void)
{
    uint32_t Count = KeNumberProcessors;
    KTHREAD *Threads[KI_MAX_PROCESSORS];
    uint64_t TicksBefore[KI_MAX_PROCESSORS];
    for (uint32_t n = 0; n < Count; n++)
        TicksBefore[n] = KiProcessorBlock[n]->IdleTicks + KiProcessorBlock[n]->BusyTicks;
    KtParallelWork = 0;
    for (uint32_t n = 0; n < Count; n++) {
        KtParallelCpu[n] = UINT32_MAX;
        Threads[n] = KtStartOn(n, KtParallelRoutine, (void *)(uintptr_t)n, "KtParallel");
        if (!Threads[n])
            return false;
    }
    bool Ok = true;
    for (uint32_t n = 0; n < Count; n++)
        Ok = KtJoin(Threads[n]) && Ok;
    uint64_t LatestStart = 0, EarliestEnd = UINT64_MAX;
    for (uint32_t n = 0; n < Count; n++) {
        Ok = Ok && KtParallelCpu[n] == n;
        LatestStart = HZ_MAX(LatestStart, KtParallelStart[n]);
        EarliestEnd = HZ_MIN(EarliestEnd, KtParallelEnd[n]);
    }
    Ok = Ok && LatestStart < EarliestEnd; /* all of them ran at the same time */
    /* Informational: clock ticks each processor received meanwhile (a hypervisor may starve
       a spinning processor's timer; ke/time.c's watchdog covers for it). */
    char Ticks[96] = "";
    size_t Used = 0;
    for (uint32_t n = 0; n < Count && Used < sizeof(Ticks) - 12; n++)
        Used += (size_t)RtlFormatString(Ticks + Used, sizeof(Ticks) - Used, "%s%llu", n ? " " : "",
                                        (unsigned long long)(KiProcessorBlock[n]->IdleTicks +
                                                             KiProcessorBlock[n]->BusyTicks - TicksBefore[n]));
    DbgPrint("KeTest: clock ticks per processor during the parallel run: %s\n", Ticks);
    KtReport(Ok, "%u threads ran on %u processors at once (%llu ms overlap)", Count, Count,
             (unsigned long long)(EarliestEnd > LatestStart ? (EarliestEnd - LatestStart) / 1000 : 0));
    return Ok;
}

static void KtCrossPongRoutine(void *Context) /* KtPongRoutine twin, named apart for the logs */
{
    for (int i = 0; i < KtRounds; i++) {
        if (KeWaitForSingleObject(&KtPing, &KtTimeout) != STATUS_WAIT_0)
            return;
        KtPongCount++;
        KeSetEvent(&KtPong);
    }
}

static bool KtTestCrossProcessorWake(void)
{
    KeInitializeEvent(&KtPing, SynchronizationEvent, false);
    KeInitializeEvent(&KtPong, SynchronizationEvent, false);
    KtPongCount = 0;
    uint64_t Affinity = KeSetAffinityThread(KeGetCurrentThread(), 1); /* this one on processor 0 */
    KTHREAD *Thread = KtStartOn(KeNumberProcessors - 1, KtCrossPongRoutine, NULL, "KtCrossPong");
    bool Ok = Thread != NULL;
    uint64_t Start = KeQueryUptimeMicroseconds();
    for (int i = 0; i < KtRounds && Ok; i++) {
        KeSetEvent(&KtPing);
        Ok = KeWaitForSingleObject(&KtPong, &KtTimeout) == STATUS_WAIT_0;
    }
    uint64_t Elapsed = KeQueryUptimeMicroseconds() - Start;
    Ok = Thread && KtJoin(Thread) && Ok && KtPongCount == KtRounds;
    KeSetAffinityThread(KeGetCurrentThread(), Affinity);
    KtReport(Ok, "%d wake-ups between processors 0 and %u (%llu ns each round trip)", KtRounds,
             KeNumberProcessors - 1, (unsigned long long)(Elapsed * 1000 / KtRounds));
    return Ok;
}

static bool KtTestMigration(void)
{
    uint64_t Affinity = KeGetCurrentThread()->Affinity;
    bool Ok = true;
    for (uint32_t Round = 0; Round < 2; Round++) {
        for (uint32_t n = 0; n < KeNumberProcessors; n++) {
            uint32_t Target = Round ? KeNumberProcessors - 1 - n : n;
            KeSetAffinityThread(KeGetCurrentThread(), 1ull << Target);
            Ok = Ok && KeGetCurrentPrcb()->Number == Target;
        }
    }
    KeSetAffinityThread(KeGetCurrentThread(), Affinity);
    KtReport(Ok, "a thread moved itself across all %u processors and back", KeNumberProcessors);
    return Ok;
}

static bool KtTestShootdown(void)
{
    uint64_t Before[KI_MAX_PROCESSORS];
    for (uint32_t n = 0; n < KeNumberProcessors; n++)
        Before[n] = KiProcessorBlock[n]->TlbShootdowns;
    KIRQL OldIrql;
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql); /* stay on this processor */
    uint32_t Self = KeGetCurrentPrcb()->Number;
    static const uint8_t Probe = 0;
    KeFlushTb(0, (uint64_t)&Probe & ~(uint64_t)(PAGE_SIZE - 1), 1);
    KeFlushTb(0, (uint64_t)&Probe & ~(uint64_t)(PAGE_SIZE - 1), 100); /* a full flush */
    KeLowerIrql(OldIrql);
    bool Ok = true;
    for (uint32_t n = 0; n < KeNumberProcessors; n++)
        Ok = Ok && (n == Self ? KiProcessorBlock[n]->TlbShootdowns == Before[n]
                              : KiProcessorBlock[n]->TlbShootdowns >= Before[n] + 2);
    KtReport(Ok, "kernel TLB shootdowns reached the other %u processors", KeNumberProcessors - 1);
    return Ok;
}

/* ---- 7. timers, timeouts, DPCs ------------------------------------------------------------------- */

static volatile uint32_t KtDpcCount;
static volatile bool KtDpcIrqlOk;

static void KtDpcRoutine(KDPC *Dpc, void *Context, void *Argument1, void *Argument2)
{
    KtDpcCount++;
    if (KeGetCurrentIrql() != DISPATCH_LEVEL)
        KtDpcIrqlOk = false;
}

static bool KtTestTimers(void)
{
    /* sleep */
    uint64_t Start = KeQueryUptimeMicroseconds();
    KeDelayExecutionThread(30000);
    uint64_t Slept = KeQueryUptimeMicroseconds() - Start;
    bool SleepOk = Slept >= 30000 && Slept < 45000;

    /* wait timeout */
    KEVENT Never;
    KeInitializeEvent(&Never, NotificationEvent, false);
    uint64_t Timeout = 20000;
    Start = KeQueryUptimeMicroseconds();
    STATUS Status = KeWaitForSingleObject(&Never, &Timeout);
    uint64_t Waited = KeQueryUptimeMicroseconds() - Start;
    bool TimeoutOk = Status == STATUS_TIMEOUT && Waited >= 20000 && Waited < 35000;

    /* periodic timer with a DPC (queued from the clock interrupt via the expiration DPC) */
    KTIMER Periodic;
    KDPC Dpc;
    KtDpcCount = 0;
    KtDpcIrqlOk = true;
    KeInitializeTimerEx(&Periodic, NotificationTimer);
    KeInitializeDpc(&Dpc, KtDpcRoutine, NULL);
    KeSetTimerEx(&Periodic, 5000, 5000, &Dpc);
    KeDelayExecutionThread(52000);
    KeCancelTimer(&Periodic);
    KeRemoveQueueDpc(&Dpc);
    /* About 10 periods. Late clock ticks (VirtualBox NEM while the guest is busy) make the timer
       skip missed periods by design, so the lower bound only proves that it repeats. */
    bool PeriodicOk = KtDpcCount >= 5 && KtDpcCount <= 12 && KtDpcIrqlOk;

    /* waiting on a timer object */
    KTIMER OneShot;
    KeInitializeTimerEx(&OneShot, SynchronizationTimer);
    KeSetTimerEx(&OneShot, 10000, 0, NULL);
    Start = KeQueryUptimeMicroseconds();
    Status = KeWaitForSingleObject(&OneShot, &KtTimeout);
    uint64_t TimerWait = KeQueryUptimeMicroseconds() - Start;
    bool TimerOk = Status == STATUS_WAIT_0 && TimerWait >= 9000 && !KeReadStateTimer(&OneShot);

    bool Ok = SleepOk && TimeoutOk && PeriodicOk && TimerOk;
    KtReport(Ok, "sleep 30 ms -> %llu us, timeout 20 ms -> %llu us, 5 ms periodic DPC x%u, timer wait %llu us",
             (unsigned long long)Slept, (unsigned long long)Waited, KtDpcCount, (unsigned long long)TimerWait);
    return Ok;
}

/* ---- 8. WaitAny, polling ------------------------------------------------------------------------- */

static bool KtTestWaitAny(void)
{
    KEVENT A, B;
    KeInitializeEvent(&A, NotificationEvent, false);
    KeInitializeEvent(&B, NotificationEvent, false);
    void *Objects[2] = {&A, &B};
    uint64_t Poll = 0;
    bool Ok = KeWaitForMultipleObjects(2, Objects, WaitAny, &Poll, NULL) == STATUS_TIMEOUT;
    KeSetEvent(&B);
    Ok = Ok && KeWaitForMultipleObjects(2, Objects, WaitAny, &KtTimeout, NULL) == STATUS_WAIT_0 + 1;
    Ok = Ok && KeWaitForMultipleObjects(2, Objects, WaitAll, &Poll, NULL) == STATUS_TIMEOUT;
    KeSetEvent(&A);
    Ok = Ok && KeWaitForMultipleObjects(2, Objects, WaitAll, &Poll, NULL) == STATUS_WAIT_0;
    KtReport(Ok, "WaitAny returns the signaled index; WaitAll and polling");
    return Ok;
}

/* ---- 9. abandoned mutex ----------------------------------------------------------------------------- */

static KMUTEX KtAbandoned;

static void KtAbandonRoutine(void *Context)
{
    KeWaitForSingleObject(&KtAbandoned, NULL); /* exits while still owning it */
}

static bool KtTestAbandonedMutex(void)
{
    KeInitializeMutex(&KtAbandoned);
    KTHREAD *Thread;
    if (!HZ_SUCCESS(KeCreateThread(&Thread, KeSystemProcess, KtAbandonRoutine, NULL, KT_PRIORITY, "KtAbandon")))
        return false;
    bool Ok = KtJoin(Thread);
    Ok = Ok && KeWaitForSingleObject(&KtAbandoned, &KtTimeout) == STATUS_ABANDONED_WAIT_0;
    KeReleaseMutex(&KtAbandoned);
    Ok = Ok && KeReadStateMutex(&KtAbandoned);
    KtReport(Ok, "mutex owned by an exiting thread is reported abandoned");
    return Ok;
}

bool KeRunSchedulerSelfTests(void)
{
    MM_STATISTICS MemBefore, MemAfter;
    POOL_STATISTICS PoolBefore, PoolAfter;
    MmQueryStatistics(&MemBefore);
    ExQueryPoolStatistics(&PoolBefore);
    uint64_t SwitchesBefore = KeGetCurrentPrcb()->ContextSwitches;
    uint64_t Start = KeQueryUptimeMicroseconds();
    uint32_t ThreadsBefore = KeQueryThreadCount(); /* idle, Phase1Init, the worker */

    int Passed = 0, Total = 0;
    uint64_t PingPongNs = 0;
    Total++, Passed += KtTestCreateJoin();
    Total++, Passed += KtTestPingPong(&PingPongNs);
    Total++, Passed += KtTestSemaphores();
    Total++, Passed += KtTestMutex();
    Total++, Passed += KtTestTimeSlicing();
    Total++, Passed += KtTestPriority();
    if (KeNumberProcessors > 1) {
        Total++, Passed += KtTestParallel();
        Total++, Passed += KtTestCrossProcessorWake();
        Total++, Passed += KtTestMigration();
        Total++, Passed += KtTestShootdown();
    }
    Total++, Passed += KtTestTimers();
    Total++, Passed += KtTestWaitAny();
    Total++, Passed += KtTestAbandonedMutex();

    MmQueryStatistics(&MemAfter);
    ExQueryPoolStatistics(&PoolAfter);
    bool NoLeak = MemAfter.ActivePages == MemBefore.ActivePages && PoolAfter.BytesInUse == PoolBefore.BytesInUse &&
                  KeQueryThreadCount() == ThreadsBefore;
    KtReport(NoLeak, "all test threads reaped (stacks %lld pages, pool %lld bytes, %d threads left)",
             (long long)(MemAfter.ActivePages - MemBefore.ActivePages),
             (long long)(PoolAfter.BytesInUse - PoolBefore.BytesInUse), (int)(KeQueryThreadCount() - ThreadsBefore));
    Total++, Passed += NoLeak;

    DbgPrintEx(Passed == Total ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "Scheduler self-tests: %d/%d passed (%llu context switches, %llu ns per event round trip, %llu ms)\n",
               Passed, Total, (unsigned long long)(KeGetCurrentPrcb()->ContextSwitches - SwitchesBefore),
               (unsigned long long)PingPongNs, (unsigned long long)((KeQueryUptimeMicroseconds() - Start) / 1000));
    return Passed == Total;
}
