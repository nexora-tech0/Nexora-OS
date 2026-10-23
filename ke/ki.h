/*
 * Nexora OS - ke/ki.h
 * Kernel-core internals shared by the dispatcher files in ke/.
 *
 * The dispatcher database (ready queues, thread states, object wait lists, the timer list) is
 * protected by KiDispatcherLock, taken at SYNCH_LEVEL. Context switches always happen at
 * DISPATCH_LEVEL with the lock released: KiSwapThread is entered holding the lock and returns
 * (in the resumed thread) at DISPATCH_LEVEL without it.
 */
#ifndef KI_H
#define KI_H

#include "ex.h"
#include "hal.h"
#include "ke.h"
#include "mm.h"
#include "rtl.h"

extern KSPIN_LOCK KiDispatcherLock;
extern LIST_ENTRY KiThreadListHead;
extern uint32_t KiThreadCount;
extern KTHREAD KiIdleThread;
extern uint64_t KiQuantumCycles; /* KI_QUANTUM_US in TSC cycles */

static inline void KiResetQuantum(KTHREAD *Thread)
{
    Thread->QuantumRemaining = (int64_t)KiQuantumCycles;
}

static inline KIRQL KiAcquireDispatcherLock(void)
{
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&KiDispatcherLock, SYNCH_LEVEL, &OldIrql);
    return OldIrql;
}

static inline void KiReleaseDispatcherLock(KIRQL OldIrql)
{
    KeReleaseSpinLock(&KiDispatcherLock, OldIrql);
}

/* ke/dispatch.c (all "Locked" routines require the dispatcher lock) */
extern volatile uint32_t KiReadySummary;
void KiReadyThreadLocked(KTHREAD *Thread);
/* A ready thread exists that this processor may run (unlocked hint for idle processors). */
bool KiReadyThreadAvailable(KPRCB *Prcb);
/* Switches to the best ready thread (or idle). Entered holding the dispatcher lock; returns at
   DISPATCH_LEVEL without it, once the current thread runs again. */
void KiSwapThread(void);
void KiDispatchSoftwareInterrupt(void);
void KiRequestDispatchInterrupt(void);

/* ke/thread.c */
void KiReapTerminatedThreads(void);
void KiInitializeThreadObject(KTHREAD *Thread, KPROCESS *Process, KPRIORITY Priority, const char *Name);
/* hal-independent stub in ke/ctxswap.asm; switches stacks, then clears *OldSwapBusy */
void KiSwapContext(uint64_t *OldStackPointer, uint64_t NewStackPointer, volatile uint8_t *OldSwapBusy);
void KiThreadStartupStub(void);
void KiSaveExtendedState(void *Area);    /* ke/ctxswap.asm: XSAVE64 or FXSAVE64 */
void KiRestoreExtendedState(void *Area); /* XRSTOR64 or FXRSTOR64 */
void KiLoadExtendedStateControl(void);   /* ke/usermode.c: XCR0 on an application processor */

/* ke/wait.c */
void KiWaitTestLocked(DISPATCHER_HEADER *Object);
void KiUnwaitThreadLocked(KTHREAD *Thread, STATUS Status);
void KiAbandonMutexesLocked(KTHREAD *Thread);
static inline void KiInitializeDispatcherHeader(DISPATCHER_HEADER *Header, KOBJECT_TYPE Type, int32_t State)
{
    Header->Type = (uint8_t)Type;
    Header->SignalState = State;
    InitializeListHead(&Header->WaitListHead);
}

/* ke/timer.c */
void KiInitializeTimers(void);
void KiInsertTimerLocked(KTIMER *Timer, uint64_t DueTimeUs);
bool KiRemoveTimerLocked(KTIMER *Timer);
extern volatile uint64_t KiNextTimerDueUs;
extern KDPC KiTimerExpirationDpcs[KI_MAX_PROCESSORS]; /* one per processor (M11) */

/* ke/dpc.c */
void KiRetireDpcList(KPRCB *Prcb);

#endif /* KI_H */
