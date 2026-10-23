/*
 * Nexora OS - ke/mp.c
 * Multiprocessor support (M11): bringing the application processors (APs) online, the processor
 * block array, interprocessor requests and TLB shootdown.
 *
 * Bring-up happens in two steps because Limine parks the APs in bootloader-reclaimable memory
 * (its code, page tables, GDT and stacks):
 *   1. KeParkApplicationProcessors (from MmInitializeSystem, once the kernel's page tables are
 *      active and before reclaiming): each AP jumps to KiApplicationProcessorEntry (apentry.asm),
 *      switches to the kernel's page tables and its own .bss stack, loads the boot processor's GDT
 *      and the IDT and spins. Processors beyond KI_MAX_PROCESSORS (or all, with hz.smp=off) are
 *      moved off the bootloader's memory too and halted for good.
 *   2. KeStartApplicationProcessors (Phase 1, scheduler and clock running): each AP gets a PRCB,
 *      its own GDT/TSS/IST stacks and an idle thread, is released, copies the boot processor's
 *      CR0/CR4/XCR0, programs its local APIC and clock, and enters its idle loop.
 *
 * TLB shootdown: KeFlushTb invalidates locally, then posts the range to every other online
 * processor (KI_IPI_FLUSH + an IPI on IPI_FLUSH_VECTOR) and waits for all of them. Senders are
 * serialized by a lock; anyone waiting (for that lock, for the acknowledgements, or for any
 * spinlock) keeps servicing its own requests, so two processors flushing at once, or one spinning
 * with interrupts masked, cannot deadlock.
 */
#include "bootinfo.h"
#include "ki.h"

#define KI_MP_TAG HZ_POOL_TAG('K', 'e', 'M', 'p')
#define KI_AP_STACK_SIZE (32 * 1024)
#define KI_FLUSH_ALL_THRESHOLD 32
#define CR4_PGE (1ull << 7)
#define CR4_OSXSAVE (1ull << 18)

KPRCB *KiProcessorBlock[KI_MAX_PROCESSORS];
volatile uint32_t KeNumberProcessors = 1;
volatile uint64_t KeActiveProcessors = 1;

/* Read by apentry.asm */
uint64_t KiApPml4;
uint64_t KiApPat;
volatile uint32_t KiApHaltedCount;
extern uint8_t KiApStacks[];
void KiApplicationProcessorEntry(void *Mailbox);

enum { KiApNone = 0, KiApParked, KiApGo };
static volatile uint32_t KiApState[KI_MAX_PROCESSORS];
static uint32_t KiApLapic[KI_MAX_PROCESSORS];
static uint32_t KiApCount; /* parked APs: processor numbers 1..KiApCount */
static HAL_PROCESSOR_TABLES *KiApTables[KI_MAX_PROCESSORS];
static uint64_t KiBspCr0, KiBspCr4;

/* ---- Step 1: parking --------------------------------------------------------------------------- */

static bool KiSpinUntil(volatile uint32_t *Value, uint32_t Wanted)
{
    for (uint64_t Spins = 0; Spins < (1ull << 27); Spins++) {
        if (__atomic_load_n(Value, __ATOMIC_ACQUIRE) == Wanted)
            return true;
        HalPause();
    }
    return false;
}

void KeParkApplicationProcessors(uint64_t KernelPml4)
{
    KiProcessorBlock[0] = KeGetCurrentPrcb();
    const LOADER_BLOCK *Lb = &KeLoaderBlock;
    if (Lb->ProcessorCount <= 1) {
        DbgPrint("MP: one processor\n");
        return;
    }
    bool Disabled = BiCommandLineHasOption("hz.smp=off");
    KiApPml4 = KernelPml4;
    KiApPat = __readmsr(MSR_IA32_PAT);
    uint32_t Next = 1, Halted = 0;
    for (uint32_t i = 0; i < Lb->ProcessorCount; i++) {
        const BOOT_PROCESSOR *P = &Lb->Processors[i];
        if (P->LapicId == Lb->BspLapicId)
            continue;
        if (!Disabled && Next < KI_MAX_PROCESSORS) {
            KiApLapic[Next] = P->LapicId;
            BiStartProcessor(P, KiApplicationProcessorEntry, Next);
            Next++;
        } else {
            BiStartProcessor(P, KiApplicationProcessorEntry, 0);
            Halted++;
        }
    }
    KiApCount = Next - 1;
    /* Nothing may reclaim the bootloader's memory before every AP has left it. */
    uint32_t Parked = 0;
    for (uint32_t n = 1; n <= KiApCount; n++)
        Parked += KiSpinUntil(&KiApState[n], KiApParked);
    if (Halted && !KiSpinUntil(&KiApHaltedCount, Halted))
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "MP: not every unused processor reached its halt loop\n");
    DbgPrint("MP: %u processors reported; %u application processor(s) parked%s, %u halted\n", Lb->ProcessorCount,
             Parked, Disabled ? " (hz.smp=off)" : "", Halted);
    if (Parked != KiApCount) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "MP: only %u of %u processors answered; using none\n", Parked, KiApCount);
        KiApCount = 0;
    }
}

HZ_NORETURN static void KiInitializeProcessor(uint32_t Number);

HZ_NORETURN void KiParkProcessor(uint64_t Number)
{
    HalLoadBootProcessorTables();
    __atomic_store_n(&KiApState[Number], KiApParked, __ATOMIC_RELEASE);
    while (__atomic_load_n(&KiApState[Number], __ATOMIC_ACQUIRE) != KiApGo)
        HalPause();
    KiInitializeProcessor((uint32_t)Number);
}

/* ---- Step 2: bringing APs online ---------------------------------------------------------------- */

HZ_NORETURN static void KiInitializeProcessor(uint32_t Number)
{
    KPRCB *Prcb = KiProcessorBlock[Number];
    __writecr0(KiBspCr0);
    __writecr4(KiBspCr4 & ~CR4_PGE); /* clearing PGE flushes the TLB, global entries included */
    __writecr4(KiBspCr4);
    if (KiBspCr4 & CR4_OSXSAVE)
        KiLoadExtendedStateControl();
    HalInitializeApplicationProcessor(Prcb, KiApTables[Number], Prcb->KernelStackTop);
    __writecr8(PASSIVE_LEVEL);
    Prcb->IdleThread->QuantumStart = HalReadTsc();
    HalStartClockInterrupt(KeQueryClockRate());
    __atomic_or_fetch(&KeActiveProcessors, 1ull << Number, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&KeNumberProcessors, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&Prcb->Online, 1, __ATOMIC_RELEASE);
    for (;;)
        HalEnableInterruptsAndHalt(); /* the idle loop: DISPATCH interrupts switch to ready threads */
}

static bool KiFlushInterrupt(KTRAP_FRAME *Frame, void *Context)
{
    KiServiceIpiRequests();
    return true;
}

uint32_t KeStartApplicationProcessors(void)
{
    if (!KiApCount)
        return KeNumberProcessors;
    KiBspCr0 = __readcr0();
    KiBspCr4 = __readcr4();
    KeConnectInterrupt(IPI_FLUSH_VECTOR, KiFlushInterrupt, NULL);
    uint64_t Start = KeQueryUptimeMicroseconds();
    for (uint32_t n = 1; n <= KiApCount; n++) {
        KPRCB *Prcb = ExAllocatePoolZero(NonPagedPool, sizeof(KPRCB), KI_MP_TAG);
        HAL_PROCESSOR_TABLES *Tables = ExAllocatePoolZero(NonPagedPool, sizeof(HAL_PROCESSOR_TABLES), KI_MP_TAG);
        KTHREAD *Idle = ExAllocatePoolZero(NonPagedPool, sizeof(KTHREAD), KI_MP_TAG);
        if (!Prcb || !Tables || !Idle) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "MP: no memory for processor %u\n", n);
            break;
        }
        uint64_t StackTop = (uint64_t)(KiApStacks + (uint64_t)n * KI_AP_STACK_SIZE);
        Prcb->Self = Prcb;
        Prcb->Number = n;
        Prcb->ApicId = KiApLapic[n];
        InitializeListHead(&Prcb->DpcListHead);
        char Name[24];
        RtlFormatString(Name, sizeof(Name), "Idle %u", n);
        KiInitializeThreadObject(Idle, KeSystemProcess, LOW_PRIORITY, Name);
        Idle->StackTop = (void *)StackTop;
        Idle->ReferenceCount = 1;
        Idle->State = ThreadRunning;
        Idle->Processor = (uint8_t)n;
        Idle->Affinity = 1ull << n;
        Prcb->IdleThread = Prcb->CurrentThread = Idle;
        Prcb->KernelStackTop = StackTop;
        KiProcessorBlock[n] = Prcb;
        KiApTables[n] = Tables;
        __atomic_store_n(&KiApState[n], KiApGo, __ATOMIC_RELEASE);
        for (int Waited = 0; Waited < 1000 && !__atomic_load_n(&Prcb->Online, __ATOMIC_ACQUIRE); Waited++)
            KeDelayExecutionThread(1000);
        if (Prcb->Online)
            DbgPrintEx(DPFLTR_TRACE_LEVEL, "MP: processor %u (local APIC %u) online\n", n, Prcb->ApicId);
        else
            DbgPrintEx(DPFLTR_ERROR_LEVEL, "MP: processor %u (local APIC %u) did not come online\n", n, Prcb->ApicId);
    }
    DbgPrint("MP: %u processors online (%llu ms)\n", KeNumberProcessors,
             (unsigned long long)((KeQueryUptimeMicroseconds() - Start) / 1000));
    return KeNumberProcessors;
}

/* ---- Interprocessor requests and TLB shootdown ---------------------------------------------------- */

static KSPIN_LOCK KiFlushLock;
static volatile uint64_t KiFlushPml4, KiFlushVa, KiFlushPages;
static volatile uint32_t KiFlushPending;

static void KiFlushLocal(uint64_t Pml4, uint64_t Va, uint64_t Pages)
{
    if (Pml4 && (__readcr3() & PTE_ADDRESS_MASK) != Pml4)
        return; /* not loaded here: switching to it flushes anyway (no PCIDs) */
    if (Pages > KI_FLUSH_ALL_THRESHOLD) {
        if (Pml4 == 0) {
            uint64_t Cr4 = __readcr4();
            __writecr4(Cr4 & ~CR4_PGE);
            __writecr4(Cr4);
        } else {
            __writecr3(__readcr3());
        }
        return;
    }
    for (uint64_t i = 0; i < Pages; i++)
        __invlpg((void *)(Va + i * PAGE_SIZE));
}

void KiServiceIpiRequests(void)
{
    KPRCB *Prcb = KeGetCurrentPrcb();
    if (!Prcb->IpiRequests)
        return;
    uint32_t Requests = __atomic_exchange_n(&Prcb->IpiRequests, 0, __ATOMIC_ACQ_REL);
    if (Requests & KI_IPI_FLUSH) {
        Prcb->TlbShootdowns++;
        KiFlushLocal(KiFlushPml4, KiFlushVa, KiFlushPages);
        __atomic_sub_fetch(&KiFlushPending, 1, __ATOMIC_RELEASE);
    }
}

void KeFlushTb(uint64_t Pml4, uint64_t Va, uint64_t Pages)
{
    KiFlushLocal(Pml4, Va, Pages);
    if (__atomic_load_n(&KeNumberProcessors, __ATOMIC_ACQUIRE) <= 1)
        return;
    KIRQL OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL)
        __writecr8(DISPATCH_LEVEL); /* no rescheduling (and migration) in the middle */
    while (!KeTryToAcquireSpinLockAtDpcLevel(&KiFlushLock)) {
        KiServiceIpiRequests();
        HalPause();
    }
    KiFlushPml4 = Pml4;
    KiFlushVa = Va;
    KiFlushPages = Pages;
    /* A snapshot of the targets: a processor coming online meanwhile starts with a clean TLB.
       For a user address space only processors running it can cache its entries; one switching
       to it later loads CR3 after the page tables changed (the fence orders our page-table
       writes before reading what they run; they set CurrentThread before writing CR3). */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    KPRCB *Self = KeGetCurrentPrcb();
    KPRCB *Targets[KI_MAX_PROCESSORS];
    uint32_t Count = 0;
    for (uint32_t n = 0; n < KI_MAX_PROCESSORS; n++) {
        KPRCB *P = KiProcessorBlock[n];
        if (!P || P == Self || !__atomic_load_n(&P->Online, __ATOMIC_ACQUIRE))
            continue;
        KTHREAD *Running = __atomic_load_n(&P->CurrentThread, __ATOMIC_ACQUIRE);
        if (Pml4 && Running && ((MMADDRESS_SPACE *)Running->AddressSpace)->Pml4 != Pml4)
            continue;
        Targets[Count++] = P;
    }
    if (!Count) {
        KeReleaseSpinLockFromDpcLevel(&KiFlushLock);
        if (OldIrql < DISPATCH_LEVEL)
            KeLowerIrql(OldIrql);
        return;
    }
    __atomic_store_n(&KiFlushPending, Count, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < Count; i++) {
        __atomic_or_fetch(&Targets[i]->IpiRequests, KI_IPI_FLUSH, __ATOMIC_SEQ_CST);
        HalSendIpi(Targets[i]->ApicId, IPI_FLUSH_VECTOR);
    }
    uint64_t Spins = 0;
    while (__atomic_load_n(&KiFlushPending, __ATOMIC_ACQUIRE)) {
        KiServiceIpiRequests();
        HalPause();
        if (++Spins == (1ull << 30))
            KeBugCheckEx(SPIN_LOCK_TIMEOUT, (uint64_t)&KiFlushPending, KiFlushPending, Va, 1);
    }
    KeReleaseSpinLockFromDpcLevel(&KiFlushLock);
    if (OldIrql < DISPATCH_LEVEL)
        KeLowerIrql(OldIrql);
}
