/*
 * Nexora OS - ps/thread.c
 * Thread objects: system threads, user threads (stack, TEB, extended state, first entry into
 * ring 3), exit, termination requests, and the handling of fatal user-mode exceptions.
 *
 * User stack layout (one reservation): [guard page (reserved, never committed)][committed stack].
 * Running off the bottom faults on the guard page, which PspUserExceptionRoutine reports as
 * STATUS_STACK_OVERFLOW. The committed part is demand-zero, so only touched pages cost memory.
 */
#include "psp.h"
#include "w32k.h"

/* ---- Object type procedures ------------------------------------------------------------------- */

void PspDeleteThread(void *Object)
{
    ETHREAD *Thread = Object;
    if (Thread->Tcb.XSaveArea) {
        KeFreeExtendedState(Thread->Tcb.XSaveArea);
        Thread->Tcb.XSaveArea = NULL;
    }
    if (Thread->Process)
        ObDereferenceObject(Thread->Process);
}

/* Runs at DISPATCH_LEVEL from the thread reaper, after the kernel stack was freed. */
static void PspReleaseThread(KTHREAD *Tcb)
{
    ObDereferenceObject(CONTAINING_RECORD(Tcb, ETHREAD, Tcb));
}

static STATUS PspAllocateThreadObject(EPROCESS *Process, ETHREAD **Out)
{
    ETHREAD *Thread;
    STATUS Status = ObCreateObject(PsThreadType, sizeof(ETHREAD), NULL, (void **)&Thread);
    if (!HZ_SUCCESS(Status))
        return Status;
    ObReferenceObject(Process);
    Thread->Process = Process;
    Thread->ExitStatus = STATUS_PENDING;
    *Out = Thread;
    return STATUS_SUCCESS;
}

/* Links an initialized thread into its process and starts it; fails if the process is exiting. */
static STATUS PspInsertAndStartThread(ETHREAD *Thread)
{
    EPROCESS *Process = Thread->Process;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    if (Process->Terminating || Process->Exited) {
        /* Started anyway (KeInitializeThread cannot be undone) but exits before doing anything. */
        Thread->Terminating = true;
        Thread->ExitStatus = STATUS_PROCESS_IS_TERMINATING;
    }
    InsertTailList(&Process->ThreadListHead, &Thread->ThreadListEntry);
    Process->ActiveThreads++;
    KeReleaseSpinLock(&PspLock, OldIrql);

    ObReferenceObject(Thread); /* the running reference, dropped by PspReleaseThread */
    KeStartThread(&Thread->Tcb);
    return Thread->Terminating ? STATUS_PROCESS_IS_TERMINATING : STATUS_SUCCESS;
}

/* ---- System threads ------------------------------------------------------------------------------- */

static void PspSystemThreadStartup(void *Context)
{
    ETHREAD *Thread = Context;
    if (!Thread->Terminating)
        Thread->SystemRoutine(Thread->SystemContext);
    PsExitThread(STATUS_SUCCESS);
}

STATUS PsCreateSystemThread(KSTART_ROUTINE Routine, void *Context, KPRIORITY Priority, const char *Name,
                            ETHREAD **Out)
{
    ETHREAD *Thread;
    STATUS Status = PspAllocateThreadObject(PsInitialSystemProcess, &Thread);
    if (!HZ_SUCCESS(Status))
        return Status;
    Thread->SystemRoutine = Routine;
    Thread->SystemContext = Context;
    Status = KeInitializeThread(&Thread->Tcb, &PsInitialSystemProcess->Pcb, PspSystemThreadStartup, Thread, Priority,
                                Name, PspReleaseThread);
    if (!HZ_SUCCESS(Status)) {
        ObDereferenceObject(Thread);
        return Status;
    }
    PspInsertAndStartThread(Thread);
    if (Out)
        *Out = Thread;
    else
        ObDereferenceObject(Thread);
    return STATUS_SUCCESS;
}

/* ---- User threads ---------------------------------------------------------------------------------- */

static void PspUserThreadStartup(void *Context)
{
    ETHREAD *Thread = Context;
    if (Thread->Terminating || Thread->Process->Terminating)
        PsExitThread(Thread->Process->Terminating ? Thread->Process->ExitStatus : Thread->ExitStatus);

    /* Microsoft x64 convention at a function's first instruction: rsp + 8 is 16-byte aligned,
       [rsp] is the return address (0: returning from the start routine faults) and 32 bytes of
       home space lie above it. */
    KTRAP_FRAME Frame;
    memset(&Frame, 0, sizeof(Frame));
    Frame.Rip = Thread->UserStartAddress;
    Frame.Rcx = Thread->UserArgument1;
    Frame.Rdx = Thread->UserArgument2;
    Frame.Rsp = Thread->UserStackBase - 40;
    Frame.RFlags = RFLAGS_IF | 0x2;
    Frame.SegCs = KGDT_USER_CODE | 3;
    Frame.SegSs = KGDT_USER_DATA | 3;

    HalDisableInterrupts();
    KiCheckUserReturn(&Frame); /* a termination request may have arrived meanwhile */
    KiEnterUserMode(&Frame);
}

STATUS PsCreateUserThread(EPROCESS *Process, uint64_t StartAddress, uint64_t Argument1, uint64_t Argument2,
                          ETHREAD **Out)
{
    if (Process->IsSystem || StartAddress >= MM_USER_TOP)
        return STATUS_INVALID_PARAMETER;
    if (Process->Terminating || Process->Exited)
        return STATUS_PROCESS_IS_TERMINATING;

    ETHREAD *Thread;
    STATUS Status = PspAllocateThreadObject(Process, &Thread);
    if (!HZ_SUCCESS(Status))
        return Status;
    Thread->IsUserThread = true;
    Thread->UserStartAddress = StartAddress;
    Thread->UserArgument1 = Argument1;
    Thread->UserArgument2 = Argument2;

    /* Everything that can fail happens before KeInitializeThread. */
    MMADDRESS_SPACE *As = Process->AddressSpace;
    void *Reservation = NULL, *Committed = NULL, *Teb = NULL;
    uint64_t StackReserve = Process->StackReserve ? Process->StackReserve : PSP_DEFAULT_STACK_RESERVE;
    Status = MmAllocateVirtualMemory(As, &Reservation, StackReserve + PAGE_SIZE, MEM_RESERVE, MM_PROT_READWRITE);
    if (HZ_SUCCESS(Status)) {
        Committed = (uint8_t *)Reservation + PAGE_SIZE;
        Status = MmAllocateVirtualMemory(As, &Committed, StackReserve, MEM_COMMIT, MM_PROT_READWRITE);
    }
    if (HZ_SUCCESS(Status))
        Status = MmAllocateVirtualMemory(As, &Teb, sizeof(HZ_TEB), MEM_RESERVE | MEM_COMMIT, MM_PROT_READWRITE);
    if (HZ_SUCCESS(Status)) {
        Thread->Tcb.XSaveArea = KeAllocateExtendedState();
        if (!Thread->Tcb.XSaveArea)
            Status = STATUS_NO_MEMORY;
    }
    if (HZ_SUCCESS(Status)) {
        char Name[24];
        RtlFormatString(Name, sizeof(Name), "%s", Process->ImageName);
        Status = KeInitializeThread(&Thread->Tcb, &Process->Pcb, PspUserThreadStartup, Thread,
                                    PSP_USER_THREAD_PRIORITY, Name, PspReleaseThread);
    }
    if (!HZ_SUCCESS(Status)) {
        if (Teb)
            MmFreeVirtualMemory(As, Teb);
        if (Reservation)
            MmFreeVirtualMemory(As, Reservation);
        ObDereferenceObject(Thread); /* frees the extended state */
        return Status;
    }

    Thread->UserStackReservation = (uint64_t)Reservation;
    Thread->UserStackLimit = (uint64_t)Reservation + PAGE_SIZE;
    Thread->UserStackBase = (uint64_t)Reservation + PAGE_SIZE + StackReserve;
    Thread->TebAddress = (uint64_t)Teb;
    Thread->Tcb.UserGsBase = (uint64_t)Teb;

    HZ_TEB Block;
    memset(&Block, 0, sizeof(Block));
    Block.Self = (HZ_TEB *)Teb;
    Block.Peb = (HZ_PEB *)Process->PebAddress;
    Block.ProcessId = Process->ProcessId;
    Block.ThreadId = Thread->Tcb.ThreadId;
    Block.StackBase = Thread->UserStackBase;
    Block.StackLimit = Thread->UserStackLimit;
    MMADDRESS_SPACE *Previous = PsAttachProcess(Process);
    Status = MmCopyToUser(Teb, &Block, sizeof(Block));
    PsDetachProcess(Previous);
    if (!HZ_SUCCESS(Status)) {
        Thread->Terminating = true; /* cannot happen for a freshly committed page; exit cleanly */
        Thread->ExitStatus = Status;
    }

    Status = PspInsertAndStartThread(Thread);
    if (!HZ_SUCCESS(Status)) {
        ObDereferenceObject(Thread);
        return Status;
    }
    if (Out)
        *Out = Thread;
    else
        ObDereferenceObject(Thread);
    return STATUS_SUCCESS;
}

/* ---- Exit and termination ----------------------------------------------------------------------- */

static PS_WIN32_THREAD_CALLOUT PspWin32ThreadCallout;

void PsSetWin32ThreadCallout(PS_WIN32_THREAD_CALLOUT Callout)
{
    PspWin32ThreadCallout = Callout;
}

HZ_NORETURN void PsExitThread(STATUS ExitStatus)
{
    ETHREAD *Thread = PsGetCurrentThread();
    EPROCESS *Process = Thread->Process;
    if (Thread->Win32Thread && PspWin32ThreadCallout)
        PspWin32ThreadCallout(Thread); /* destroys its windows while the address space is intact */

    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    Thread->ExitStatus = ExitStatus;
    Thread->HasExited = true;
    RemoveEntryList(&Thread->ThreadListEntry);
    bool Last = --Process->ActiveThreads == 0 && !Process->IsSystem;
    if (Last) {
        Process->Exited = true;
        if (!Process->Terminating)
            Process->ExitStatus = ExitStatus;
    }
    KeReleaseSpinLock(&PspLock, OldIrql);

    if (Thread->IsUserThread) {
        DbgPrintEx(DPFLTR_TRACE_LEVEL, "Thread: %u of %s (PID %u) exited with status 0x%08X\n", Thread->Tcb.ThreadId,
                   Process->ImageName, Process->ProcessId, (uint32_t)ExitStatus);
        if (!Last) {
            /* The rest of the address space goes with the process; give back this thread's part. */
            MmFreeVirtualMemory(Process->AddressSpace, (void *)Thread->TebAddress);
            MmFreeVirtualMemory(Process->AddressSpace, (void *)Thread->UserStackReservation);
        }
    }
    if (Last)
        PspProcessExited(Process);
    KeTerminateThread();
}

STATUS PsTerminateThread(ETHREAD *Thread, STATUS ExitStatus)
{
    if (Thread == PsGetCurrentThread())
        PsExitThread(ExitStatus);
    if (!Thread->IsUserThread)
        return STATUS_ACCESS_DENIED; /* system threads end by returning */
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    if (!Thread->HasExited && !Thread->Terminating) {
        Thread->Terminating = true;
        Thread->ExitStatus = ExitStatus;
        KeRequestUserReturnCallout(&Thread->Tcb);
    }
    KeReleaseSpinLock(&PspLock, OldIrql);
    return STATUS_SUCCESS;
}

void PspUserReturnCallout(KTRAP_FRAME *Frame)
{
    ETHREAD *Thread = PsGetCurrentThread();
    if (!Thread->IsUserThread)
        return;
    EPROCESS *Process = Thread->Process;
    if (Process->Terminating)
        PsExitThread(Process->ExitStatus);
    if (Thread->Terminating)
        PsExitThread(Thread->ExitStatus);
}

void PspUserExceptionRoutine(KTRAP_FRAME *Frame, STATUS ExceptionStatus, uint64_t Address)
{
    ETHREAD *Thread = PsGetCurrentThread();
    EPROCESS *Process = Thread->Process;
    if (ExceptionStatus == STATUS_ACCESS_VIOLATION && Thread->UserStackReservation &&
        Address >= Thread->UserStackReservation && Address < Thread->UserStackLimit)
        ExceptionStatus = STATUS_STACK_OVERFLOW;

    char Detail[64] = "";
    if (Frame->Vector == 14)
        RtlFormatString(Detail, sizeof(Detail), ", %s 0x%016llX",
                        (Frame->ErrorCode & 0x10) ? "executing" : (Frame->ErrorCode & 0x2) ? "writing" : "reading",
                        (unsigned long long)Address);
    DbgPrintEx(DPFLTR_WARNING_LEVEL,
               "Process: %s (PID %u) thread %u raised %s (0x%08X) at RIP 0x%016llX (image+0x%llx)%s; terminating\n",
               Process->ImageName, Process->ProcessId, Thread->Tcb.ThreadId, PspStatusName(ExceptionStatus),
               (uint32_t)ExceptionStatus, (unsigned long long)Frame->Rip,
               (unsigned long long)(Frame->Rip - Process->ImageBase), Detail);
    W32kNotifyProcessCrash(Process->ProcessId, ExceptionStatus); /* the shell shows a dialog (M11) */
    PsTerminateProcess(Process, ExceptionStatus);
    PsExitThread(Process->ExitStatus);
}
