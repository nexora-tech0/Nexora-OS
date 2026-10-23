/*
 * Nexora OS - ke/timer.c
 * Kernel timers. Due times are absolute microseconds of TSC uptime, kept in one list sorted by
 * due time. The clock interrupt only compares the uptime with KiNextTimerDueUs (no lock: it runs
 * above SYNCH_LEVEL) and queues its processor's expiration DPC; the DPC expires timers under the
 * dispatcher lock: it signals them, wakes their waiters, queues their DPCs and re-arms
 * periodic ones. Every processor's clock does this (M11): a hypervisor can starve one
 * processor's timer while it spins, and processor 0 must not be the only one keeping time.
 */
#include "ki.h"

static LIST_ENTRY KiTimerListHead = {&KiTimerListHead, &KiTimerListHead};
volatile uint64_t KiNextTimerDueUs = UINT64_MAX;
KDPC KiTimerExpirationDpcs[KI_MAX_PROCESSORS];

static void KiUpdateNextDue(void)
{
    KiNextTimerDueUs = IsListEmpty(&KiTimerListHead)
                           ? UINT64_MAX
                           : CONTAINING_RECORD(KiTimerListHead.Flink, KTIMER, TimerListEntry)->DueTimeUs;
}

void KeInitializeTimerEx(KTIMER *Timer, TIMER_TYPE Type)
{
    KiInitializeDispatcherHeader(&Timer->Header,
                                 Type == NotificationTimer ? KObjectTimerNotification : KObjectTimerSynchronization, 0);
    InitializeListHead(&Timer->TimerListEntry);
    Timer->DueTimeUs = 0;
    Timer->PeriodUs = 0;
    Timer->Dpc = NULL;
    Timer->Inserted = false;
}

bool KiRemoveTimerLocked(KTIMER *Timer)
{
    if (!Timer->Inserted)
        return false;
    RemoveEntryList(&Timer->TimerListEntry);
    Timer->Inserted = false;
    KiUpdateNextDue();
    return true;
}

void KiInsertTimerLocked(KTIMER *Timer, uint64_t DueTimeUs)
{
    if (Timer->Inserted)
        KiRemoveTimerLocked(Timer);
    Timer->DueTimeUs = DueTimeUs;
    Timer->Header.SignalState = 0;
    /* Most timers are later than the ones already queued: search from the tail. */
    LIST_ENTRY *After = KiTimerListHead.Blink;
    while (After != &KiTimerListHead && CONTAINING_RECORD(After, KTIMER, TimerListEntry)->DueTimeUs > DueTimeUs)
        After = After->Blink;
    InsertHeadList(After, &Timer->TimerListEntry);
    Timer->Inserted = true;
    KiUpdateNextDue();
}

bool KeSetTimerEx(KTIMER *Timer, uint64_t DueInUs, uint64_t PeriodUs, KDPC *Dpc)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    bool WasInserted = Timer->Inserted;
    Timer->PeriodUs = PeriodUs;
    Timer->Dpc = Dpc;
    KiInsertTimerLocked(Timer, KeQueryUptimeMicroseconds() + DueInUs);
    KiReleaseDispatcherLock(OldIrql);
    return WasInserted;
}

bool KeCancelTimer(KTIMER *Timer)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    bool WasInserted = KiRemoveTimerLocked(Timer);
    KiReleaseDispatcherLock(OldIrql);
    return WasInserted;
}

bool KeReadStateTimer(KTIMER *Timer)
{
    return Timer->Header.SignalState > 0;
}

static void KiTimerExpirationRoutine(KDPC *Dpc, void *Context, void *Argument1, void *Argument2)
{
    KIRQL OldIrql = KiAcquireDispatcherLock();
    uint64_t Now = KeQueryUptimeMicroseconds();
    while (!IsListEmpty(&KiTimerListHead)) {
        KTIMER *Timer = CONTAINING_RECORD(KiTimerListHead.Flink, KTIMER, TimerListEntry);
        if (Timer->DueTimeUs > Now)
            break;
        RemoveEntryList(&Timer->TimerListEntry);
        Timer->Inserted = false;
        Timer->Header.SignalState = 1;
        KiWaitTestLocked(&Timer->Header);
        if (Timer->Dpc)
            KeInsertQueueDpc(Timer->Dpc, Timer, NULL);
        if (Timer->PeriodUs) {
            uint64_t Next = Timer->DueTimeUs + Timer->PeriodUs;
            if (Next <= Now)
                Next = Now + Timer->PeriodUs; /* fell behind: skip missed periods */
            int32_t Saved = Timer->Header.SignalState;
            KiInsertTimerLocked(Timer, Next);
            Timer->Header.SignalState = Saved; /* stays signaled until consumed/re-armed */
        }
    }
    KiUpdateNextDue();
    KiReleaseDispatcherLock(OldIrql);
}

void KiInitializeTimers(void)
{
    for (uint32_t n = 0; n < KI_MAX_PROCESSORS; n++)
        KeInitializeDpc(&KiTimerExpirationDpcs[n], KiTimerExpirationRoutine, NULL);
}
