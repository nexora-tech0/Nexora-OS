/*
 * Nexora OS - ke/trap.c
 * Trap dispatch. Exceptions raised in kernel mode are fatal (bug check with the trap frame),
 * except int3, which logs and resumes, and faults inside the user-copy routines, which resume at
 * their failure exit. Exceptions raised in ring 3 become an NT status that the user-exception
 * routine (Ps) handles by terminating the process. Hardware interrupts run their connected
 * service routine at the vector's IRQL with interrupts re-enabled, so higher-priority vectors
 * can nest; the dispatcher then sends EOI and restores the previous IRQL. Every return to ring 3
 * passes KiCheckUserReturn.
 */
#include "ki.h"

typedef struct _KINTERRUPT_ENTRY {
    KSERVICE_ROUTINE Routine;
    void *Context;
} KINTERRUPT_ENTRY;

static KINTERRUPT_ENTRY KiInterruptTable[256];
static KSPIN_LOCK KiInterruptTableLock; /* connecting and disconnecting (taken at HIGH_LEVEL) */
static volatile uint32_t KiUnexpectedInterrupts;
volatile uint64_t KiBreakpointCount;
volatile uint64_t KiUserBreakpointCount;
static KUSER_EXCEPTION_ROUTINE KiUserExceptionRoutine;

void KeSetUserExceptionRoutine(KUSER_EXCEPTION_ROUTINE Routine)
{
    KiUserExceptionRoutine = Routine;
}

static const char *const KiExceptionNames[32] = {
    "Divide error", "Debug exception", "Non-maskable interrupt", "Breakpoint", "Overflow",
    "Bound range exceeded", "Invalid opcode", "Device not available", "Double fault",
    "Coprocessor segment overrun", "Invalid TSS", "Segment not present", "Stack-segment fault",
    "General protection fault", "Page fault", "Reserved exception 15", "x87 floating-point error",
    "Alignment check", "Machine check", "SIMD floating-point error", "Virtualization exception",
    "Control protection exception", "Reserved exception 22", "Reserved exception 23",
    "Reserved exception 24", "Reserved exception 25", "Reserved exception 26",
    "Reserved exception 27", "Hypervisor injection exception", "VMM communication exception",
    "Security exception", "Reserved exception 31",
};

STATUS KeConnectInterrupt(uint8_t Vector, KSERVICE_ROUTINE Routine, void *Context)
{
    if (Vector < 32 || Vector == APIC_SPURIOUS_VECTOR || !Routine)
        return STATUS_INVALID_PARAMETER;
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&KiInterruptTableLock, HIGH_LEVEL, &OldIrql);
    STATUS Status = STATUS_SUCCESS;
    if (KiInterruptTable[Vector].Routine) {
        Status = STATUS_UNSUCCESSFUL; /* vector sharing arrives with the I/O manager (M5) */
    } else {
        KiInterruptTable[Vector].Context = Context;
        KiInterruptTable[Vector].Routine = Routine;
    }
    KeReleaseSpinLock(&KiInterruptTableLock, OldIrql);
    return Status;
}

STATUS KeConnectInterruptAnyVector(KIRQL Irql, KSERVICE_ROUTINE Routine, void *Context, uint8_t *Vector)
{
    if (Irql < DEVICE_LEVEL_LOW || Irql > DEVICE_LEVEL_HIGH || !Routine)
        return STATUS_INVALID_PARAMETER;
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&KiInterruptTableLock, HIGH_LEVEL, &OldIrql);
    STATUS Status = STATUS_INSUFFICIENT_RESOURCES;
    /* Vector = IRQL * 16 + n; the last slot of the band (0x?F) is left free, like 0x2F. */
    for (unsigned v = (unsigned)Irql << 4; v < ((unsigned)Irql << 4) + 15; v++) {
        if (v < KI_FIRST_DEVICE_VECTOR || v > KI_LAST_DEVICE_VECTOR || KiInterruptTable[v].Routine)
            continue;
        KiInterruptTable[v].Context = Context;
        KiInterruptTable[v].Routine = Routine;
        *Vector = (uint8_t)v;
        Status = STATUS_SUCCESS;
        break;
    }
    KeReleaseSpinLock(&KiInterruptTableLock, OldIrql);
    return Status;
}

void KeDisconnectInterrupt(uint8_t Vector)
{
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&KiInterruptTableLock, HIGH_LEVEL, &OldIrql);
    KiInterruptTable[Vector].Routine = NULL;
    KiInterruptTable[Vector].Context = NULL;
    KeReleaseSpinLock(&KiInterruptTableLock, OldIrql);
}

static void KiDescribePageFault(char *Buffer, size_t Size, uint64_t Address, uint64_t Error, uint64_t Rip)
{
    char Where[128];
    KeFormatAddress(Where, sizeof(Where), Rip);
    const char *Access = (Error & 0x10) ? "executing" : (Error & 0x02) ? "writing" : "reading";
    const char *Cause = (Error & 0x08) ? "reserved bit set"
                        : (Error & 0x01) ? "protection violation"
                                         : "page not present";
    RtlFormatString(Buffer, Size, "Page fault %s 0x%016llX (%s) in %s", Access,
                    (unsigned long long)Address, Cause, Where);
}

/*
 * #GP in ring 3: privileged instructions (cli, hlt, in/out, mov cr, ...) report
 * STATUS_PRIVILEGED_INSTRUCTION; anything else (non-canonical address, bad segment) is an
 * access violation. The opcode is read through the checked user-copy path.
 */
static STATUS KiClassifyUserGeneralProtection(const KTRAP_FRAME *Frame)
{
    uint8_t Code[5] = {0};
    size_t Length = sizeof(Code);
    if (Frame->Rip >= MM_USER_TOP)
        return STATUS_ACCESS_VIOLATION;
    if (MM_USER_TOP - Frame->Rip < Length)
        Length = MM_USER_TOP - Frame->Rip;
    if (!HZ_SUCCESS(MmCopyFromUser(Code, (const void *)Frame->Rip, Length)))
        return STATUS_ACCESS_VIOLATION;
    unsigned i = 0;
    while (i < 3 && (Code[i] == 0x66 || Code[i] == 0xF2 || Code[i] == 0xF3 || (Code[i] & 0xF0) == 0x40))
        i++; /* operand-size, rep and REX prefixes */
    uint8_t Op = Code[i];
    if (Op == 0xFA || Op == 0xFB || Op == 0xF4 || (Op >= 0xE4 && Op <= 0xE7) || (Op >= 0xEC && Op <= 0xEF) ||
        (Op >= 0x6C && Op <= 0x6F) || Op == 0xCF)
        return STATUS_PRIVILEGED_INSTRUCTION; /* cli sti hlt in out ins outs iretq */
    if (Op == 0x0F) {
        uint8_t Op2 = Code[i + 1];
        if (Op2 == 0x00 || Op2 == 0x01 || Op2 == 0x06 || Op2 == 0x07 || Op2 == 0x08 || Op2 == 0x09 ||
            (Op2 >= 0x20 && Op2 <= 0x23) || Op2 == 0x30 || Op2 == 0x32)
            return STATUS_PRIVILEGED_INSTRUCTION; /* lgdt.., clts, sysret, invd, wbinvd, mov cr/dr, wrmsr, rdmsr */
    }
    return STATUS_ACCESS_VIOLATION;
}

static void KiDispatchUserException(KTRAP_FRAME *Frame)
{
    uint64_t Vector = Frame->Vector;
    uint64_t Address = 0;
    STATUS Status;
    switch (Vector) {
    case 3:
        KiUserBreakpointCount++;
        DbgPrintEx(DPFLTR_INFO_LEVEL, "User breakpoint at 0x%016llX (thread %u)\n",
                   (unsigned long long)Frame->Rip - 1, KeGetCurrentThread()->ThreadId);
        return;
    case 14:
        Address = __readcr2();
        if (HZ_SUCCESS(MmAccessFault(Address, Frame->ErrorCode, Frame)))
            return;
        Status = STATUS_ACCESS_VIOLATION;
        break;
    case 0: Status = STATUS_INTEGER_DIVIDE_BY_ZERO; break;
    case 6: Status = STATUS_ILLEGAL_INSTRUCTION; break;
    case 13: Status = KiClassifyUserGeneralProtection(Frame); break;
    case 16:
    case 19: Status = STATUS_FLOAT_INVALID_OPERATION; break;
    case 17: Status = STATUS_DATATYPE_MISALIGNMENT; break;
    default: Status = STATUS_ILLEGAL_INSTRUCTION; break;
    }
    if (!KiUserExceptionRoutine) {
        KeBugCheckTrap(UNEXPECTED_KERNEL_TRAP, Vector, Frame->ErrorCode, Frame->Rip, Frame->Rsp, Frame,
                       "User-mode exception before the process manager was initialized");
    }
    HalEnableInterrupts();
    KiUserExceptionRoutine(Frame, Status, Address);
    HalDisableInterrupts();
}

static void KiDispatchException(KTRAP_FRAME *Frame)
{
    static char Message[224];
    char Where[128];
    uint64_t Vector = Frame->Vector;

    /* A frozen processor (another one is bug-checking) records where it was and stops; the
       bug-checking processor reports it. */
    if (Vector == 2 && KeIsBugCheckActive()) {
        KPRCB *Prcb = KeGetCurrentPrcb();
        Prcb->FrozenIrql = KeGetCurrentIrql();
        __atomic_store_n(&Prcb->FrozenFrame, Frame, __ATOMIC_RELEASE);
        HalHaltSystem();
    }
    /* NMI, #DF and #MC are machine-level events: fatal whatever mode was interrupted. */
    if (KiIsUserModeFrame(Frame) && Vector != 2 && Vector != 8 && Vector != 18) {
        KiDispatchUserException(Frame);
        return;
    }
    KeFormatAddress(Where, sizeof(Where), Frame->Rip);

    switch (Vector) {
    case 3: /* int3: RIP already points past the instruction */
        KiBreakpointCount++;
        DbgPrintEx(DPFLTR_TRACE_LEVEL, "Breakpoint at %s\n", Where);
        return;

    case 2:
        if (KeIsBugCheckActive())
            HalHaltSystem(); /* another processor is stopping the system (HalFreezeOtherProcessors) */
        RtlFormatString(Message, sizeof(Message), "Non-maskable interrupt while executing %s", Where);
        KeBugCheckTrap(NMI_HARDWARE_FAILURE, Frame->Rip, 0, 0, 0, Frame, Message);

    case 8: {
        /* Running off a kernel stack faults on its guard page; delivering that #PF needs the same
           (unusable) stack, so the CPU escalates to #DF, which runs on its own IST stack. */
        uint64_t Address = __readcr2();
        if (MmIsKernelStackGuard(Address)) {
            RtlFormatString(Message, sizeof(Message), "Kernel stack overflow: guard page 0x%016llX hit in %s",
                            (unsigned long long)Address, Where);
            KeBugCheckTrap(KERNEL_STACK_OVERFLOW, Address, Frame->Rsp, Frame->Rip, 0, Frame, Message);
        }
        RtlFormatString(Message, sizeof(Message), "Double fault in %s (RSP 0x%016llX)", Where,
                        (unsigned long long)Frame->Rsp);
        KeBugCheckTrap(DOUBLE_FAULT, Frame->Rip, Frame->Rsp, 0, 0, Frame, Message);
    }

    case 14: {
        uint64_t Address = __readcr2();
        STATUS Status = MmAccessFault(Address, Frame->ErrorCode, Frame);
        if (HZ_SUCCESS(Status))
            return; /* demand-zero page mapped: retry the instruction */
        if (Status == STATUS_IRQL_TOO_HIGH) {
            RtlFormatString(Message, sizeof(Message), "Page fault on 0x%016llX at IRQL %u in %s",
                            (unsigned long long)Address, KeGetCurrentIrql(), Where);
            KeBugCheckTrap(IRQL_NOT_LESS_OR_EQUAL, Address, KeGetCurrentIrql(), Frame->ErrorCode, Frame->Rip, Frame,
                           Message);
        }
        if (MmIsUserCopyFault(Frame->Rip)) {
            Frame->Rip = MmUserCopyFaultResume(); /* bad user buffer: the copy returns failure */
            return;
        }
        if (MmIsKernelStackGuard(Address)) {
            RtlFormatString(Message, sizeof(Message), "Kernel stack overflow: guard page 0x%016llX hit in %s",
                            (unsigned long long)Address, Where);
            KeBugCheckTrap(KERNEL_STACK_OVERFLOW, Address, Frame->Rsp, Frame->Rip, 0, Frame, Message);
        }
        if (Address < MM_USER_TOP && (Frame->ErrorCode & 0x11) == 0x11) {
            RtlFormatString(Message, sizeof(Message), "SMEP violation: kernel executed user page 0x%016llX from %s",
                            (unsigned long long)Address, Where);
            KeBugCheckTrap(SUPERVISOR_MODE_USER_EXECUTE, Address, Frame->ErrorCode, Frame->Rip, 0, Frame, Message);
        }
        if (Address < MM_USER_TOP && (Frame->ErrorCode & 0x1) && MmSmapEnabled && !(Frame->RFlags & (1ull << 18))) {
            RtlFormatString(Message, sizeof(Message),
                            "SMAP violation: kernel %s user address 0x%016llX directly in %s",
                            (Frame->ErrorCode & 0x2) ? "wrote" : "read", (unsigned long long)Address, Where);
            KeBugCheckTrap(SUPERVISOR_MODE_USER_ACCESS, Address, Frame->ErrorCode, Frame->Rip, 0, Frame, Message);
        }
        KiDescribePageFault(Message, sizeof(Message), Address, Frame->ErrorCode, Frame->Rip);
        KeBugCheckTrap(KERNEL_PAGE_FAULT, Address, Frame->ErrorCode, Frame->Rip, 0, Frame, Message);
    }

    case 18:
        RtlFormatString(Message, sizeof(Message), "Machine check while executing %s", Where);
        KeBugCheckTrap(MACHINE_CHECK_EXCEPTION, Frame->Rip, 0, 0, 0, Frame, Message);

    default:
        RtlFormatString(Message, sizeof(Message), "%s (error code 0x%llX) in %s", KiExceptionNames[Vector],
                        (unsigned long long)Frame->ErrorCode, Where);
        KeBugCheckTrap(UNEXPECTED_KERNEL_TRAP, Vector, Frame->ErrorCode, Frame->Rip, Frame->Rsp, Frame,
                       Message);
    }
}

static void KiDispatchInterrupt(KTRAP_FRAME *Frame)
{
    uint8_t Vector = (uint8_t)Frame->Vector;
    KPRCB *Prcb = KeGetCurrentPrcb();
    KIRQL OldIrql = KeGetCurrentIrql();
    KIRQL NewIrql = KI_VECTOR_IRQL(Vector);
    if (NewIrql > OldIrql)
        __writecr8(NewIrql);

    Prcb->InterruptCount++;
    Prcb->NestedInterrupts++;

    KINTERRUPT_ENTRY Entry = KiInterruptTable[Vector];
    if (Entry.Routine) {
        HalEnableInterrupts();
        Entry.Routine(Frame, Entry.Context);
        HalDisableInterrupts();
    } else if (KiUnexpectedInterrupts++ < 8) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Unexpected interrupt on vector 0x%02X\n", Vector);
    }

    Prcb->NestedInterrupts--;
    HalEndOfInterrupt();
    __writecr8(OldIrql);
}

/*
 * The DISPATCH_LEVEL software interrupt (a LAPIC self-IPI). EOI goes out first: the handler may
 * switch threads, and an in-service vector 0x2F left behind by a switched-out thread would mask
 * every further DISPATCH interrupt on this CPU until that thread ran again.
 */
static void KiDispatchSoftwareInterruptTrap(void)
{
    KeGetCurrentPrcb()->InterruptCount++;
    HalEndOfInterrupt();
    KIRQL OldIrql = KeGetCurrentIrql();
    if (OldIrql >= DISPATCH_LEVEL)
        return; /* cannot be delivered at this IRQL; nothing to do */
    __writecr8(DISPATCH_LEVEL);
    HalEnableInterrupts();
    KiDispatchSoftwareInterrupt();
    HalDisableInterrupts();
    __writecr8(OldIrql);
}

void KiDispatchTrap(KTRAP_FRAME *Frame)
{
    if (Frame->Vector < 32) {
        KeGetCurrentPrcb()->ExceptionCount++;
        KiDispatchException(Frame);
    } else if (Frame->Vector == APIC_SPURIOUS_VECTOR) {
        /* spurious interrupts are not in service: no EOI */
    } else if (Frame->Vector == DISPATCH_VECTOR) {
        KiDispatchSoftwareInterruptTrap();
    } else {
        KiDispatchInterrupt(Frame);
    }
    if (KiIsUserModeFrame(Frame))
        KiCheckUserReturn(Frame);
}
