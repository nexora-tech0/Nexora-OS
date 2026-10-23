/*
 * Nexora OS - init/diag.c
 * Diagnostics selected with `hz.test=<name>` on the kernel command line (see the Diagnostics
 * directory in boot/limine.conf). Each one provokes a specific failure so the trap path, the
 * stop screen and symbolization can be verified on real firmware and in scripts/smoke-test.py.
 * The routines are noinline so they show up by name in stack traces.
 */
#include "bootinfo.h"
#include "ex.h"
#include "ke.h"
#include "mm.h"
#include "w32k.h"

#define HZ_NOINLINE __attribute__((noinline))

HZ_NOINLINE static void KiTestPageFault(void)
{
    *(volatile uint64_t *)0x1000 = 0x484F52495A4F4Eull; /* lower half is unmapped */
}

HZ_NOINLINE static void KiTestDivideError(void)
{
    volatile int Dividend = 1, Divisor = 0;
    volatile int Quotient = Dividend / Divisor;
    (void)Quotient;
}

HZ_NOINLINE static void KiTestInvalidOpcode(void)
{
    __asm__ volatile("ud2");
}

HZ_NOINLINE static void KiTestGeneralProtection(void)
{
    *(volatile uint64_t *)0x8000000000000000ull = 1; /* non-canonical address */
}

HZ_NOINLINE static void KiTestDoubleFault(void)
{
    /* The push faults (#PF); delivering #PF needs the same unmapped stack, so the CPU raises
       #DF, which runs on its own IST stack. */
    __asm__ volatile("mov $0x1000, %%rsp\n\tpush %%rax" : : : "memory");
}

HZ_NOINLINE static void KiTestAssertion(void)
{
    HZ_ASSERT(KeGetCurrentIrql() == HIGH_LEVEL);
}

HZ_NOINLINE static void KiTestIrqlViolation(void)
{
    KIRQL Old;
    KeRaiseIrql(DISPATCH_LEVEL, &Old);
    KeRaiseIrql(PASSIVE_LEVEL, &Old); /* raising to a lower IRQL is a bug */
}

/* ---- Memory manager diagnostics (M2) ------------------------------------------------------ */

#define KI_DIAG_TAG HZ_POOL_TAG('D', 'i', 'a', 'g')

HZ_NOINLINE static uint64_t KiRecurseForever(volatile uint64_t *Depth)
{
    volatile uint8_t Frame[512];
    Frame[0] = (uint8_t)*Depth;
    (*Depth)++;
    if (*Depth > (1ull << 40)) /* never true; keeps the compiler from proving infinite recursion */
        return 0;
    return KiRecurseForever(Depth) + Frame[0];
}

static void KiStackOverflowRoutine(void *Context)
{
    KiRecurseForever((volatile uint64_t *)Context);
}

HZ_NOINLINE static void KiTestStackOverflow(void)
{
    static volatile uint64_t Depth;
    void *Stack = MmCreateKernelStack();
    KiCallOnStack(KiStackOverflowRoutine, (void *)&Depth, Stack);
}

HZ_NOINLINE static void KiTestPoolCorruption(void)
{
    uint8_t *Block = ExAllocatePoolWithTag(NonPagedPool, 64, KI_DIAG_TAG);
    Block[-12] ^= 0xFF; /* overwrite the header's Size field, as a buffer underrun would */
    ExFreePoolWithTag(Block, KI_DIAG_TAG);
}

HZ_NOINLINE static void KiTestDoubleFree(void)
{
    void *Block = ExAllocatePoolWithTag(NonPagedPool, 64, KI_DIAG_TAG);
    void *Neighbour = ExAllocatePoolWithTag(NonPagedPool, 64, KI_DIAG_TAG); /* keeps the slab alive */
    ExFreePoolWithTag(Block, KI_DIAG_TAG);
    ExFreePoolWithTag(Block, KI_DIAG_TAG);
    ExFreePoolWithTag(Neighbour, KI_DIAG_TAG);
}

static const uint32_t KiReadOnlyValue = 0x484F525A;

HZ_NOINLINE static void KiTestWriteReadOnly(void)
{
    *(volatile uint32_t *)&KiReadOnlyValue = 0; /* .rodata is mapped read-only (CR0.WP) */
}

static uint8_t KiDataCode[16] = {0xC3}; /* "ret", in a non-executable data page */

HZ_NOINLINE static void KiTestExecuteData(void)
{
    void (*Routine)(void) = (void (*)(void))(uintptr_t)KiDataCode;
    Routine(); /* NX: instruction fetch from .data faults */
}

/* ---- Scheduler diagnostics (M3) --------------------------------------------------------------- */

static void KiOverflowThreadRoutine(void *Context)
{
    static volatile uint64_t Depth;
    KiRecurseForever(&Depth);
}

HZ_NOINLINE static void KiTestThreadOverflow(void)
{
    KTHREAD *Thread;
    if (HZ_SUCCESS(KeCreateThread(&Thread, KeSystemProcess, KiOverflowThreadRoutine, NULL, KI_THREAD_PRIORITY_DEFAULT,
                                  "DiagOverflow")))
        KeWaitForSingleObject(Thread, NULL); /* the thread's guard page stops the system */
}

HZ_NOINLINE static void KiTestWaitAtRaisedIrql(void)
{
    static KEVENT Event;
    KeInitializeEvent(&Event, NotificationEvent, false);
    KIRQL OldIrql;
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    KeWaitForSingleObject(&Event, NULL); /* blocking at DISPATCH_LEVEL is illegal */
}

HZ_NOINLINE static void KiTestMutexNotOwned(void)
{
    static KMUTEX Mutex;
    KeInitializeMutex(&Mutex);
    KeReleaseMutex(&Mutex); /* releasing a mutex nobody owns */
}

/* ---- User-boundary diagnostics (M4) ------------------------------------------------------------ */

HZ_NOINLINE static void KiTestSmapViolation(void)
{
    MMADDRESS_SPACE *As;
    void *Base = NULL;
    if (!MmSmapEnabled)
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Diagnostics: this CPU has no SMAP; the access below will succeed\n");
    if (!HZ_SUCCESS(MmCreateAddressSpace(&As)) ||
        !HZ_SUCCESS(MmAllocateVirtualMemory(As, &Base, PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, MM_PROT_READWRITE)))
        return;
    MmSwitchAddressSpace(As);
    MmCopyToUser(Base, "H", 1); /* the legal path: materializes the page */
    volatile uint8_t Value = *(volatile uint8_t *)Base; /* a direct kernel read of a user page */
    (void)Value;
}

typedef struct _KI_DIAGNOSTIC {
    const char *Option;
    const char *Description;
    void (*Routine)(void);
} KI_DIAGNOSTIC;

static void KiTestManualCrash(void)
{
    KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0x484F52495A4F4Eull /* "HORIZON" */, 0, 0, 0);
}

static void KiTestRustPanic(void)
{
    RgnTriggerPanic();
}

static const KI_DIAGNOSTIC KiDiagnostics[] = {
    {"hz.test=bugcheck", "raising a manual bug check", KiTestManualCrash},
    {"hz.test=rustpanic", "triggering a Rust panic", KiTestRustPanic},
    {"hz.test=pagefault", "writing to an unmapped address", KiTestPageFault},
    {"hz.test=divide", "dividing by zero", KiTestDivideError},
    {"hz.test=invalidop", "executing ud2", KiTestInvalidOpcode},
    {"hz.test=gpf", "touching a non-canonical address", KiTestGeneralProtection},
    {"hz.test=doublefault", "faulting with an unusable stack", KiTestDoubleFault},
    {"hz.test=assert", "failing an assertion", KiTestAssertion},
    {"hz.test=irql", "raising IRQL to a lower level", KiTestIrqlViolation},
    {"hz.test=stackoverflow", "recursing on a guarded kernel stack", KiTestStackOverflow},
    {"hz.test=poolcorruption", "freeing a pool block with a damaged header", KiTestPoolCorruption},
    {"hz.test=doublefree", "freeing a pool block twice", KiTestDoubleFree},
    {"hz.test=writero", "writing to read-only kernel data", KiTestWriteReadOnly},
    {"hz.test=nxexec", "executing code from a data page", KiTestExecuteData},
    {"hz.test=threadoverflow", "overflowing a thread's kernel stack", KiTestThreadOverflow},
    {"hz.test=waitirql", "waiting at DISPATCH_LEVEL", KiTestWaitAtRaisedIrql},
    {"hz.test=mutexowner", "releasing a mutex the thread does not own", KiTestMutexNotOwned},
    {"hz.test=smap", "reading a user page directly from kernel mode", KiTestSmapViolation},
};

void KiRunCommandLineDiagnostics(void)
{
    for (size_t i = 0; i < HZ_ARRAYSIZE(KiDiagnostics); i++) {
        if (BiCommandLineHasOption(KiDiagnostics[i].Option)) {
            DbgPrint("Diagnostics: %s\n", KiDiagnostics[i].Description);
            KiDiagnostics[i].Routine();
            DbgPrintEx(DPFLTR_ERROR_LEVEL, "Diagnostics: %s did not stop the system\n", KiDiagnostics[i].Option);
        }
    }
}
