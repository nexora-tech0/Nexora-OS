/*
 * Nexora OS - ke/bugcheck.c
 * Bug checks: stop the machine, report on COM1 and paint the stop screen.
 *
 * The stack walk follows the rbp chain (every language is built with frame pointers). Each
 * frame is checked against the live page tables before it is read, so a corrupt rbp ends the
 * walk instead of faulting inside the bug check. For traps the walk starts at the faulting
 * instruction and the interrupted rbp; addresses are symbolized from the symbols module.
 */
#include "bootinfo.h"
#include "hal.h"
#include "inbv.h"
#include "ke.h"
#include "rtl.h"

#define KI_MAX_STACK_FRAMES 12

static volatile int KiBugCheckActive;

bool KeIsBugCheckActive(void)
{
    return __atomic_load_n(&KiBugCheckActive, __ATOMIC_ACQUIRE) != 0;
}
static volatile uint32_t KiFirstBugCheckCode;

/* Walks the active page tables through the HHDM; true if Va is mapped. */
static bool KiIsAddressMapped(uint64_t Va)
{
    if (KeLoaderBlock.HhdmOffset == 0)
        return false;
    uint64_t Table = __readcr3() & PTE_ADDRESS_MASK;
    for (int Level = 3; Level >= 0; Level--) {
        uint64_t Index = (Va >> (12 + 9 * Level)) & 0x1FF;
        uint64_t Entry = ((uint64_t *)(Table + KeLoaderBlock.HhdmOffset))[Index];
        if (!(Entry & PTE_PRESENT))
            return false;
        if (Level > 0 && Level < 3 && (Entry & PTE_LARGE))
            return true; /* 1 GiB or 2 MiB page */
        Table = Entry & PTE_ADDRESS_MASK;
    }
    return true;
}

static bool KiIsPlausibleFrame(uint64_t Rbp, uint64_t Previous)
{
    return Rbp >= 0xffff800000000000ull && (Rbp & 7) == 0 && Rbp > Previous &&
           KiIsAddressMapped(Rbp) && KiIsAddressMapped(Rbp + 8);
}

static uint32_t KiWalkStack(uint64_t Rbp, uint64_t *Frames, uint32_t Count, uint32_t MaxFrames)
{
    uint64_t Previous = 0;
    while (Count < MaxFrames && KiIsPlausibleFrame(Rbp, Previous)) {
        uint64_t *Frame = (uint64_t *)Rbp;
        uint64_t ReturnAddress = Frame[1];
        if (ReturnAddress == 0)
            break;
        Frames[Count++] = ReturnAddress;
        Previous = Rbp;
        Rbp = Frame[0];
    }
    return Count;
}

static void KiSerialPrint(const char *Format, ...) HZ_PRINTF(1, 2);
static void KiSerialPrint(const char *Format, ...)
{
    char Buffer[320];
    va_list Args;
    va_start(Args, Format);
    int n = RtlFormatStringV(Buffer, sizeof(Buffer), Format, Args);
    va_end(Args);
    HalSerialWrite(Buffer, (size_t)HZ_MIN(n, (int)sizeof(Buffer) - 1));
}

static void KiSerialDumpTrapFrame(const KTRAP_FRAME *F, uint64_t Cr2, uint64_t Cr3)
{
    KiSerialPrint("    RAX %016llX  RBX %016llX  RCX %016llX\n", (unsigned long long)F->Rax,
                  (unsigned long long)F->Rbx, (unsigned long long)F->Rcx);
    KiSerialPrint("    RDX %016llX  RSI %016llX  RDI %016llX\n", (unsigned long long)F->Rdx,
                  (unsigned long long)F->Rsi, (unsigned long long)F->Rdi);
    KiSerialPrint("    RBP %016llX  RSP %016llX  R8  %016llX\n", (unsigned long long)F->Rbp,
                  (unsigned long long)F->Rsp, (unsigned long long)F->R8);
    KiSerialPrint("    R9  %016llX  R10 %016llX  R11 %016llX\n", (unsigned long long)F->R9,
                  (unsigned long long)F->R10, (unsigned long long)F->R11);
    KiSerialPrint("    R12 %016llX  R13 %016llX  R14 %016llX\n", (unsigned long long)F->R12,
                  (unsigned long long)F->R13, (unsigned long long)F->R14);
    KiSerialPrint("    R15 %016llX  RIP %016llX  RFL %016llX\n", (unsigned long long)F->R15,
                  (unsigned long long)F->Rip, (unsigned long long)F->RFlags);
    KiSerialPrint("    CS  %04llX  SS  %04llX  CR2 %016llX  CR3 %016llX\n", (unsigned long long)F->SegCs,
                  (unsigned long long)F->SegSs, (unsigned long long)Cr2, (unsigned long long)Cr3);
}

/* Every other online processor, as its freeze NMI left it (M11): the thread, IRQL and stack. A
   deadlock across processors shows both sides this way. Waits up to 50 ms for the reports. */
static void KiReportOtherProcessors(void)
{
    KPRCB *Self = KeGetCurrentPrcb();
    uint64_t Deadline = KeQueryUptimeMicroseconds() + 50000;
    for (uint32_t n = 0; n < KI_MAX_PROCESSORS; n++) {
        KPRCB *P = KiProcessorBlock[n];
        if (!P || P == Self || !P->Online)
            continue;
        while (!__atomic_load_n(&P->FrozenFrame, __ATOMIC_ACQUIRE) && KeQueryUptimeMicroseconds() < Deadline)
            HalPause();
        const KTRAP_FRAME *F = __atomic_load_n(&P->FrozenFrame, __ATOMIC_ACQUIRE);
        const KTHREAD *T = P->CurrentThread;
        if (!F) {
            KiSerialPrint("    Processor %u: did not report ('%s')\n", n, T ? T->Name : "?");
            continue;
        }
        KiSerialPrint("    Processor %u: thread '%s' (id %u) at IRQL %u, interrupts %s\n", n, T ? T->Name : "?",
                      T ? T->ThreadId : 0, P->FrozenIrql, (F->RFlags & RFLAGS_IF) ? "on" : "off");
        uint64_t Frames[KI_MAX_STACK_FRAMES];
        Frames[0] = F->Rip;
        uint32_t Count = KiWalkStack(F->Rbp, Frames, 1, 8);
        for (uint32_t i = 0; i < Count; i++) {
            char Symbol[160];
            KeFormatStackFrame(Symbol, sizeof(Symbol), Frames[i], i != 0);
            KiSerialPrint("      #%-2u 0x%016llX  %s\n", i, (unsigned long long)Frames[i], Symbol);
        }
    }
}

HZ_NORETURN static void KiBugCheckCommon(uint32_t Code, const uint64_t Params[4], const char *Message,
                                         const char *File, uint32_t Line, const KTRAP_FRAME *Frame)
{
    HalDisableInterrupts();

    if (__atomic_exchange_n(&KiBugCheckActive, 1, __ATOMIC_SEQ_CST) != 0) {
        /* A bug check while handling a bug check: keep it to the serial port. */
        KiSerialPrint("\n*** Nested bug check 0x%08X (%s) while handling 0x%08X - system halted.\n", Code,
                      KeBugCheckCodeName(Code), KiFirstBugCheckCode);
        HalHaltSystem();
    }
    KiFirstBugCheckCode = Code;
    KIRQL Irql = KeGetCurrentIrql();
    __writecr8(HIGH_LEVEL);
    if (KeNumberProcessors > 1)
        HalFreezeOtherProcessors(); /* an NMI: they halt (ke/trap.c) */

    /* GS base is only valid after HalInitializeProcessor; threads exist after M3 scheduler init. */
    const KTHREAD *Thread = __readmsr(MSR_GS_BASE) ? KeGetCurrentThread() : NULL;

    /* The on-screen boot log must not scribble over the stop screen. */
    InbvEnableDisplayString(false);

    uint64_t Cr2 = __readcr2();
    uint64_t Cr3 = __readcr3();
    uint64_t Frames[KI_MAX_STACK_FRAMES];
    uint32_t FrameCount;
    if (Frame) {
        Frames[0] = Frame->Rip;
        FrameCount = KiWalkStack(Frame->Rbp, Frames, 1, KI_MAX_STACK_FRAMES);
    } else {
        FrameCount = KiWalkStack((uint64_t)__builtin_frame_address(0), Frames, 0, KI_MAX_STACK_FRAMES);
    }
    const char *Name = KeBugCheckCodeName(Code);

    KiSerialPrint("\n*** STOP 0x%08X %s\n", Code, Name);
    KiSerialPrint("    Parameters: 0x%016llX 0x%016llX 0x%016llX 0x%016llX\n",
                  (unsigned long long)Params[0], (unsigned long long)Params[1],
                  (unsigned long long)Params[2], (unsigned long long)Params[3]);
    if (Message)
        KiSerialPrint("    Message: %s\n", Message);
    if (File)
        KiSerialPrint("    Location: %s:%u\n", File, Line);
    if (Thread)
        KiSerialPrint("    Thread: '%s' (id %u) at IRQL %u\n", Thread->Name, Thread->ThreadId, Irql);
    else
        KiSerialPrint("    Thread: none (phase 0) at IRQL %u\n", Irql);
    if (Frame) {
        KiSerialPrint("    Trap frame (vector %llu, error code 0x%llX):\n", (unsigned long long)Frame->Vector,
                      (unsigned long long)Frame->ErrorCode);
        KiSerialDumpTrapFrame(Frame, Cr2, Cr3);
    }
    KiSerialPrint("    Stack trace:\n");
    for (uint32_t i = 0; i < FrameCount; i++) {
        char Symbol[160];
        KeFormatStackFrame(Symbol, sizeof(Symbol), Frames[i], !(Frame && i == 0));
        KiSerialPrint("      #%-2u 0x%016llX  %s\n", i, (unsigned long long)Frames[i], Symbol);
    }
    if (KeNumberProcessors > 1 && Thread)
        KiReportOtherProcessors();
    KiSerialPrint("*** System halted.\n");

    if (InbvIsDisplayReady()) {
        STOP_SCREEN_INFO Info = {
            .Code = Code,
            .CodeName = Name,
            .Parameters = {Params[0], Params[1], Params[2], Params[3]},
            .Message = Message,
            .File = File,
            .Line = Line,
            .Frame = Frame,
            .Cr2 = Cr2,
            .Cr3 = Cr3,
            .Frames = Frames,
            .FrameCount = FrameCount,
            .ThreadName = Thread ? Thread->Name : NULL,
            .ThreadId = Thread ? Thread->ThreadId : 0,
            .Irql = Irql,
        };
        InbvDrawStopScreen(&Info);
    }

    HalHaltSystem();
}

HZ_NORETURN void KeBugCheckEx(uint32_t Code, uint64_t Param1, uint64_t Param2, uint64_t Param3,
                              uint64_t Param4)
{
    uint64_t Params[4] = {Param1, Param2, Param3, Param4};
    KiBugCheckCommon(Code, Params, NULL, NULL, 0, NULL);
}

HZ_NORETURN void KeBugCheckExMessage(uint32_t Code, uint64_t Param1, uint64_t Param2, uint64_t Param3,
                                     uint64_t Param4, const char *Message)
{
    uint64_t Params[4] = {Param1, Param2, Param3, Param4};
    KiBugCheckCommon(Code, Params, Message, NULL, 0, NULL);
}

HZ_NORETURN void KeBugCheck(uint32_t Code)
{
    KeBugCheckEx(Code, 0, 0, 0, 0);
}

HZ_NORETURN void KeBugCheckWithMessage(uint32_t Code, const char *Message, const char *File,
                                       uint32_t Line)
{
    uint64_t Params[4] = {Line, 0, 0, 0};
    KiBugCheckCommon(Code, Params, Message, File, Line, NULL);
}

HZ_NORETURN void KeBugCheckTrap(uint32_t Code, uint64_t Param1, uint64_t Param2, uint64_t Param3,
                                uint64_t Param4, const KTRAP_FRAME *Frame, const char *Message)
{
    uint64_t Params[4] = {Param1, Param2, Param3, Param4};
    KiBugCheckCommon(Code, Params, Message, NULL, 0, Frame);
}

HZ_NORETURN void KeBugCheckRustPanic(const uint8_t *Message, size_t MessageLength,
                                     const uint8_t *File, size_t FileLength, uint32_t Line)
{
    static char MessageCopy[256];
    static char FileCopy[128];
    size_t m = HZ_MIN(MessageLength, sizeof(MessageCopy) - 1);
    size_t f = HZ_MIN(FileLength, sizeof(FileCopy) - 1);
    if (Message)
        memcpy(MessageCopy, Message, m);
    MessageCopy[Message ? m : 0] = '\0';
    if (File)
        memcpy(FileCopy, File, f);
    FileCopy[File ? f : 0] = '\0';

    uint64_t Params[4] = {Line, 0, 0, 0};
    KiBugCheckCommon(RUST_PANIC, Params, MessageCopy, File ? FileCopy : NULL, Line, NULL);
}
