/*
 * Nexora OS - ke/usermode.c
 * Kernel side of the boundary to user mode: the user-return callout and per-thread extended
 * processor state (x87/SSE/AVX through XSAVE, or FXSAVE on CPUs without it).
 *
 * The user-return callout plays the part NT gives to APCs for thread termination: Ps marks a
 * thread (KeRequestUserReturnCallout), alerts it out of any alertable wait, and the thread runs
 * the callout in its own context the next time it is about to return to ring 3.
 */
#include "ki.h"

#define KI_XSTATE_TAG HZ_POOL_TAG('K', 'e', 'X', 's')
#define CR0_MP (1ull << 1)
#define CR0_EM (1ull << 2)
#define CR0_TS (1ull << 3)
#define CR0_NE (1ull << 5)
#define CR4_OSFXSR (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_OSXSAVE (1ull << 18)
#define XCR0_X87 1ull
#define XCR0_SSE 2ull
#define XCR0_AVX 4ull

static KUSER_RETURN_CALLOUT KiUserReturnCallout;
uint8_t KiXsaveEnabled; /* read by ke/ctxswap.asm */
static uint32_t KiExtendedStateSize = 512;
static uint64_t KiXcr0;

void KeSetUserReturnCallout(KUSER_RETURN_CALLOUT Callout)
{
    KiUserReturnCallout = Callout;
}

void KeRequestUserReturnCallout(KTHREAD *Thread)
{
    Thread->UserReturnPending = true;
    KeAlertThread(Thread); /* get it out of an alertable wait so it can reach the return path */
    /* Running on another processor (M11): interrupt it, so it passes the return path now instead
       of at its next clock tick (which a hypervisor may hold back while user code spins). */
    KIRQL OldIrql = KiAcquireDispatcherLock();
    if (Thread->State == ThreadRunning) {
        KPRCB *P = KiProcessorBlock[Thread->Processor];
        if (P && P != KeGetCurrentPrcb() && P->CurrentThread == Thread)
            HalSendIpi(P->ApicId, DISPATCH_VECTOR);
    }
    KiReleaseDispatcherLock(OldIrql);
}

void KiCheckUserReturn(KTRAP_FRAME *Frame)
{
    KTHREAD *Thread = KeGetCurrentThread();
    if (!Thread || !Thread->UserReturnPending || !KiUserReturnCallout)
        return;
    Thread->UserReturnPending = false;
    HalEnableInterrupts();
    KiUserReturnCallout(Frame);
    HalDisableInterrupts();
}

/* ---- Extended state ----------------------------------------------------------------------- */

void KeInitializeExtendedState(void)
{
    uint32_t Regs[4];
    __cpuid(1, 0, Regs);
    bool HasXsave = (Regs[2] & (1u << 26)) != 0;

    uint64_t Cr0 = __readcr0();
    Cr0 &= ~(CR0_EM | CR0_TS);
    Cr0 |= CR0_MP | CR0_NE;
    __writecr0(Cr0);
    uint64_t Cr4 = __readcr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (HasXsave)
        Cr4 |= CR4_OSXSAVE;
    __writecr4(Cr4);

    if (HasXsave) {
        __cpuid(0xD, 0, Regs);
        KiXcr0 = XCR0_X87 | XCR0_SSE | (Regs[0] & XCR0_AVX);
        /* xsetbv (encoded directly: the kernel is built without the XSAVE target feature) */
        __asm__ volatile(".byte 0x0f, 0x01, 0xd1" : : "c"(0), "a"((uint32_t)KiXcr0), "d"((uint32_t)(KiXcr0 >> 32)));
        __cpuid(0xD, 0, Regs);
        KiExtendedStateSize = Regs[1];
        KiXsaveEnabled = 1;
    }
    DbgPrint("CPU: user extended state via %s (%s), %u bytes per thread\n", HasXsave ? "XSAVE" : "FXSAVE",
             KiXcr0 & XCR0_AVX ? "x87, SSE, AVX" : "x87, SSE", KiExtendedStateSize);
}

void KiLoadExtendedStateControl(void)
{
    if (KiXsaveEnabled)
        __asm__ volatile(".byte 0x0f, 0x01, 0xd1" : : "c"(0), "a"((uint32_t)KiXcr0), "d"((uint32_t)(KiXcr0 >> 32)));
}

uint32_t KeExtendedStateSize(void)
{
    return KiExtendedStateSize;
}

void *KeAllocateExtendedState(void)
{
    uint8_t *Raw = ExAllocatePoolZero(NonPagedPool, KiExtendedStateSize + 64 + 8, KI_XSTATE_TAG);
    if (!Raw)
        return NULL;
    uint8_t *Area = (uint8_t *)HZ_ALIGN_UP((uint64_t)Raw + 8, 64); /* XSAVE needs 64-byte alignment */
    ((void **)Area)[-1] = Raw;
    /* Legacy region: FCW = 0x037F (all x87 exceptions masked), MXCSR = 0x1F80 (all SSE exceptions
       masked). The XSAVE header stays zero, so XRSTOR loads the init state for everything else. */
    *(uint16_t *)(Area + 0) = 0x037F;
    *(uint32_t *)(Area + 24) = 0x1F80;
    return Area;
}

void KeFreeExtendedState(void *Area)
{
    if (Area)
        ExFreePoolWithTag(((void **)Area)[-1], KI_XSTATE_TAG);
}
