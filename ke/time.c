/*
 * Nexora OS - ke/time.c
 * System time and the clock interrupt. Uptime is TSC-based, measured from kernel entry. The
 * clock interrupt (CLOCK_LEVEL, 1 kHz) counts ticks and, without taking any lock, requests the
 * DISPATCH software interrupt when the running thread has used its quantum (measured in TSC
 * cycles, so late or coalesced ticks under a hypervisor still slice correctly) and queues the
 * timer-expiration DPC when the earliest timer is due.
 */
#include "ki.h"

uint64_t KiBootTsc; /* TSC at kernel entry (set first thing in KiInitializeSystem) */
static volatile uint64_t KiTickCount;
static uint32_t KiClockRate;

/*
 * Clock watchdog (M11). Under VirtualBox's Hyper-V backend a processor that spins in guest code
 * can go without timer interrupts for as long as it spins (they are delivered on VM exits), while
 * IPIs still get through. Every processor's clock therefore watches the others: a busy processor
 * whose own clock has not ticked for KI_CLOCK_STALE_US of TSC time gets its quantum checked from
 * here (the TSC-based charge works remotely) and a DISPATCH IPI, which preempts it if its quantum
 * is over and takes it through the user-return path in any case. Two watchers racing on the same
 * processor at worst send it two IPIs.
 */
#define KI_CLOCK_STALE_US 10000
static volatile uint64_t KiWatchTicks[KI_MAX_PROCESSORS];
static volatile uint64_t KiWatchSinceUs[KI_MAX_PROCESSORS];
volatile uint64_t KiClockKicks; /* IPIs sent by the watchdog (reported with the heartbeat) */

static void KiWatchOtherClocks(KPRCB *Self)
{
    uint64_t Now = KeQueryUptimeMicroseconds();
    for (uint32_t n = 0; n < KI_MAX_PROCESSORS; n++) {
        KPRCB *P = KiProcessorBlock[n];
        if (!P || P == Self || !P->Online)
            continue;
        uint64_t Ticks = P->IdleTicks + P->BusyTicks;
        if (Ticks != KiWatchTicks[n] || KiWatchSinceUs[n] == 0) {
            KiWatchTicks[n] = Ticks;
            KiWatchSinceUs[n] = Now;
            continue;
        }
        if (Now - KiWatchSinceUs[n] < KI_CLOCK_STALE_US)
            continue;
        KiWatchSinceUs[n] = Now;
        KTHREAD *Thread = P->CurrentThread;
        if (!Thread || Thread == P->IdleThread)
            continue; /* halted: nothing to preempt */
        if ((int64_t)(HalReadTsc() - Thread->QuantumStart) >= Thread->QuantumRemaining)
            P->QuantumEnd = true;
        __atomic_add_fetch(&KiClockKicks, 1, __ATOMIC_RELAXED);
        HalSendIpi(P->ApicId, DISPATCH_VECTOR);
    }
}

bool KiClockInterrupt(KTRAP_FRAME *Frame, void *Context)
{
    /* Every processor runs a clock for its own quantum; only processor 0 counts ticks and
       expires timers (M11). */
    KPRCB *Prcb = KeGetCurrentPrcb();
    bool Boot = Prcb->Number == 0;
    /* A flush request whose IPI was missed is served within a tick (cheap when none is pending). */
    if (KeNumberProcessors > 1)
        KiServiceIpiRequests();
    if (Boot)
        KiTickCount++;
    KTHREAD *Thread = Prcb->CurrentThread;
    if (Thread) {
        if (Thread == Prcb->IdleThread) {
            Prcb->IdleTicks++;
            /* A thread may have become ready while another processor was asked to run it. */
            if (KiReadySummary && !Prcb->PreemptRequested && KiReadyThreadAvailable(Prcb)) {
                Prcb->PreemptRequested = true;
                HalRequestDispatchInterrupt();
            }
        } else {
            Prcb->BusyTicks++;
            if (!Prcb->QuantumEnd && (int64_t)(HalReadTsc() - Thread->QuantumStart) >= Thread->QuantumRemaining) {
                Prcb->QuantumEnd = true;
                HalRequestDispatchInterrupt();
            }
        }
    }
    /* Any processor may expire timers (M11; see ke/timer.c): each queues its own DPC. */
    if (KiNextTimerDueUs != UINT64_MAX && KeQueryUptimeMicroseconds() >= KiNextTimerDueUs)
        KeInsertQueueDpc(&KiTimerExpirationDpcs[Prcb->Number], NULL, NULL);
    if (KeNumberProcessors > 1)
        KiWatchOtherClocks(Prcb);
    return true;
}

void KiStartClock(uint32_t RateHz)
{
    KiClockRate = RateHz;
    KeConnectInterrupt(CLOCK_VECTOR, KiClockInterrupt, NULL);
    HalStartClockInterrupt(RateHz);
}

uint64_t KeQueryTickCount(void)
{
    return KiTickCount;
}

uint32_t KeQueryClockRate(void)
{
    return KiClockRate;
}

/* Uptime from the reference timer instead of the TSC (M11): chosen when the boot clock check
   finds the TSC running at the wrong rate, as VirtualBox's Hyper-V backend does with more than
   one processor (its guest TSC ran about a third slow and differed between processors, so a
   10 ms timer due on one processor looked minutes away on another). */
static volatile bool KiUptimeFromReference;
static int64_t KiReferenceOffsetUs;

bool KeUseReferenceClock(void)
{
    if (KiUptimeFromReference || !HalStartReferenceTime())
        return false;
    uint64_t Tsc = KeQueryUptimeMicroseconds();
    KiReferenceOffsetUs = (int64_t)Tsc - (int64_t)HalReadReferenceTimeUs(); /* continuous at the switch */
    __atomic_store_n(&KiUptimeFromReference, true, __ATOMIC_RELEASE);
    return true;
}

bool KeIsReferenceClockInUse(void)
{
    return KiUptimeFromReference;
}

uint64_t KeQueryUptimeMicroseconds(void)
{
    if (KiUptimeFromReference)
        return (uint64_t)((int64_t)HalReadReferenceTimeUs() + KiReferenceOffsetUs);
    uint64_t Frequency = HalQueryTscFrequency();
    if (Frequency == 0)
        return 0;
    /* Split into whole seconds and remainder so the multiply cannot overflow 64 bits. */
    uint64_t Ticks = HalReadTsc() - KiBootTsc;
    return (Ticks / Frequency) * 1000000u + ((Ticks % Frequency) * 1000000u) / Frequency;
}

void KeStallExecutionProcessor(uint32_t Microseconds)
{
    HalStallExecution(Microseconds);
}

/* ---- System time (wall clock) -------------------------------------------------------------- */

static uint64_t KiSystemTimeAtBoot; /* HZ time at uptime 0; 0 until set */

void KeSetSystemTime(uint64_t Time)
{
    KiSystemTimeAtBoot = Time - KeQueryUptimeMicroseconds() * 10;
}

uint64_t KeQuerySystemTime(void)
{
    if (!KiSystemTimeAtBoot)
        return 0;
    return KiSystemTimeAtBoot + KeQueryUptimeMicroseconds() * 10;
}
