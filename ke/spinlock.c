/*
 * Nexora OS - ke/spinlock.c
 * Spinlocks and IRQL checks. Locks are real atomic test-and-test-and-set locks even while the
 * system runs on one CPU, so SMP (M11) needs no retrofit. A lock that cannot be taken within
 * ~2^28 spins is a deadlock on this design and stops the system with SPIN_LOCK_TIMEOUT.
 */
#include "hal.h"
#include "ke.h"

#define KI_SPIN_LIMIT (1ull << 28)

HZ_NORETURN void KiIrqlViolation(KIRQL Current, KIRQL Requested, bool Raising)
{
    KeBugCheckEx(IRQL_INVALID_TRANSITION, Current, Requested, Raising ? 1 : 0,
                 (uint64_t)__builtin_return_address(0));
}

bool KeTryToAcquireSpinLockAtDpcLevel(KSPIN_LOCK *Lock)
{
    return __atomic_exchange_n(Lock, 1, __ATOMIC_ACQUIRE) == 0;
}

void KeAcquireSpinLockAtDpcLevel(KSPIN_LOCK *Lock)
{
    uint64_t Spins = 0;
    while (!KeTryToAcquireSpinLockAtDpcLevel(Lock)) {
        while (__atomic_load_n(Lock, __ATOMIC_RELAXED) != 0) {
            HalPause();
            /* The holder may be waiting for this processor to acknowledge a TLB shootdown. */
            if (KeNumberProcessors > 1)
                KiServiceIpiRequests();
            if (++Spins > KI_SPIN_LIMIT)
                KeBugCheckEx(SPIN_LOCK_TIMEOUT, (uint64_t)Lock, (uint64_t)__builtin_return_address(0),
                             KeGetCurrentIrql(), 0);
        }
    }
}

void KeReleaseSpinLockFromDpcLevel(KSPIN_LOCK *Lock)
{
    __atomic_store_n(Lock, 0, __ATOMIC_RELEASE);
}

void KeAcquireSpinLockRaiseToLevel(KSPIN_LOCK *Lock, KIRQL Level, KIRQL *OldIrql)
{
    KeRaiseIrql(Level, OldIrql);
    KeAcquireSpinLockAtDpcLevel(Lock);
}

void KeAcquireSpinLock(KSPIN_LOCK *Lock, KIRQL *OldIrql)
{
    KeAcquireSpinLockRaiseToLevel(Lock, DISPATCH_LEVEL, OldIrql);
}

void KeReleaseSpinLock(KSPIN_LOCK *Lock, KIRQL OldIrql)
{
    KeReleaseSpinLockFromDpcLevel(Lock);
    KeLowerIrql(OldIrql);
}
