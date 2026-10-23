/*
 * Nexora OS - ke/dpc.c
 * Deferred procedure calls. Interrupt service routines do the minimum at device IRQL and queue
 * a DPC; queued DPCs run at DISPATCH_LEVEL (interrupts enabled) when the DISPATCH software
 * interrupt fires, i.e. as soon as IRQL drops below DISPATCH_LEVEL. The per-CPU queue lock is
 * taken at HIGH_LEVEL because DPCs may be queued from any interrupt.
 */
#include "ki.h"

void KeInitializeDpc(KDPC *Dpc, KDEFERRED_ROUTINE Routine, void *Context)
{
    InitializeListHead(&Dpc->DpcListEntry);
    Dpc->DeferredRoutine = Routine;
    Dpc->DeferredContext = Context;
    Dpc->SystemArgument1 = Dpc->SystemArgument2 = NULL;
    Dpc->Inserted = false;
}

bool KeInsertQueueDpc(KDPC *Dpc, void *SystemArgument1, void *SystemArgument2)
{
    /* The claim is global (M11): two processors may try to queue the same DPC at once (a
       periodic timer's DPC while its last run is still queued elsewhere). */
    KPRCB *Prcb = KeGetCurrentPrcb();
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&Prcb->DpcLock, HIGH_LEVEL, &OldIrql);
    bool Queued = !__atomic_exchange_n(&Dpc->Inserted, true, __ATOMIC_ACQ_REL);
    if (Queued) {
        Dpc->SystemArgument1 = SystemArgument1;
        Dpc->SystemArgument2 = SystemArgument2;
        Dpc->Queue = Prcb;
        InsertTailList(&Prcb->DpcListHead, &Dpc->DpcListEntry);
    }
    KeReleaseSpinLock(&Prcb->DpcLock, OldIrql);
    if (Queued)
        HalRequestDispatchInterrupt();
    return Queued;
}

bool KeRemoveQueueDpc(KDPC *Dpc)
{
    KPRCB *Prcb = Dpc->Queue;
    if (!Prcb || !Dpc->Inserted)
        return false;
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&Prcb->DpcLock, HIGH_LEVEL, &OldIrql);
    bool WasQueued = Dpc->Inserted && Dpc->Queue == Prcb;
    if (WasQueued) {
        RemoveEntryList(&Dpc->DpcListEntry);
        Dpc->Queue = NULL;
        __atomic_store_n(&Dpc->Inserted, false, __ATOMIC_RELEASE);
    }
    KeReleaseSpinLock(&Prcb->DpcLock, OldIrql);
    return WasQueued;
}

void KiRetireDpcList(KPRCB *Prcb)
{
    for (;;) {
        KIRQL OldIrql;
        KeAcquireSpinLockRaiseToLevel(&Prcb->DpcLock, HIGH_LEVEL, &OldIrql);
        if (IsListEmpty(&Prcb->DpcListHead)) {
            KeReleaseSpinLock(&Prcb->DpcLock, OldIrql);
            return;
        }
        KDPC *Dpc = CONTAINING_RECORD(RemoveHeadList(&Prcb->DpcListHead), KDPC, DpcListEntry);
        KDEFERRED_ROUTINE Routine = Dpc->DeferredRoutine;
        void *Context = Dpc->DeferredContext;
        void *Argument1 = Dpc->SystemArgument1;
        void *Argument2 = Dpc->SystemArgument2;
        Dpc->Queue = NULL;
        __atomic_store_n(&Dpc->Inserted, false, __ATOMIC_RELEASE);
        KeReleaseSpinLock(&Prcb->DpcLock, OldIrql);

        Prcb->DpcCount++;
        Routine(Dpc, Context, Argument1, Argument2);
    }
}
