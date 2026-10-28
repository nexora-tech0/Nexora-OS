/*
 * Nexora OS - ps/process.c
 * Process objects: the System process, user process creation, termination requests, exit and
 * deletion, and the process list.
 *
 * Lifetime: a process object is referenced by its creator (until it closes its handle or
 * pointer), by each of its threads and by handles to it. It is signaled when its last thread
 * exits (PspProcessExited); its address space and image go when the last reference does.
 */
#include "psp.h"

OBJECT_TYPE *PsProcessType;
OBJECT_TYPE *PsThreadType;
EPROCESS *PsInitialSystemProcess;

KSPIN_LOCK PspLock;
LIST_ENTRY PspActiveProcessList;
static uint32_t PspNextProcessId = 8; /* System is 4; ids are multiples of 4 like handles */

const char *PspStatusName(STATUS Status)
{
    switch (Status) {
    case STATUS_SUCCESS: return "STATUS_SUCCESS";
    case STATUS_ACCESS_VIOLATION: return "STATUS_ACCESS_VIOLATION";
    case STATUS_STACK_OVERFLOW: return "STATUS_STACK_OVERFLOW";
    case STATUS_INTEGER_DIVIDE_BY_ZERO: return "STATUS_INTEGER_DIVIDE_BY_ZERO";
    case STATUS_PRIVILEGED_INSTRUCTION: return "STATUS_PRIVILEGED_INSTRUCTION";
    case STATUS_ILLEGAL_INSTRUCTION: return "STATUS_ILLEGAL_INSTRUCTION";
    case STATUS_FLOAT_INVALID_OPERATION: return "STATUS_FLOAT_INVALID_OPERATION";
    case STATUS_DATATYPE_MISALIGNMENT: return "STATUS_DATATYPE_MISALIGNMENT";
    case STATUS_BREAKPOINT: return "STATUS_BREAKPOINT";
    default: return "status";
    }
}

/* ---- Attach --------------------------------------------------------------------------------- */

MMADDRESS_SPACE *PsAttachProcess(EPROCESS *Process)
{
    MMADDRESS_SPACE *Previous = MmGetCurrentAddressSpace();
    MmSwitchAddressSpace(Process->AddressSpace);
    return Previous;
}

void PsDetachProcess(MMADDRESS_SPACE *Previous)
{
    MmSwitchAddressSpace(Previous);
}

/* ---- Object type procedures ------------------------------------------------------------------- */

void PspDeleteProcess(void *Object)
{
    EPROCESS *Process = Object;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    if (Process->ActiveProcessLinks.Flink)
        RemoveEntryList(&Process->ActiveProcessLinks);
    KeReleaseSpinLock(&PspLock, OldIrql);

    if (Process->ObjectTable) {
        /* Never ran a thread (creation failed or was abandoned). */
        HANDLE_TABLE *Table = Process->ObjectTable;
        Process->ObjectTable = NULL;
        ObKillHandleTable(Table, Process);
    }
    PspFreeWindowsHeapAllocations(Process);
    PspFreeWinModules(Process);
    if (Process->AddressSpace && !Process->IsSystem)
        MmDeleteAddressSpace(Process->AddressSpace);
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "Process: %s (PID %u) deleted\n", Process->ImageName, Process->ProcessId);
}

static void PspInitializeTypes(void)
{
    OBJECT_TYPE_INITIALIZER Init = {0};
    Init.PoolTag = PSP_PROCESS_TAG;
    Init.ValidAccessMask = PROCESS_ALL_ACCESS;
    Init.Waitable = true;
    Init.DeleteProcedure = PspDeleteProcess;
    if (!HZ_SUCCESS(ObCreateObjectType("Process", &Init, &PsProcessType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0x20, 0, 0);

    Init.PoolTag = PSP_THREAD_TAG;
    Init.ValidAccessMask = THREAD_ALL_ACCESS;
    Init.DeleteProcedure = PspDeleteThread;
    if (!HZ_SUCCESS(ObCreateObjectType("Thread", &Init, &PsThreadType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0x21, 0, 0);
}

void PsInitializeSystem(void)
{
    KeInitializeSpinLock(&PspLock);
    InitializeListHead(&PspActiveProcessList);
    PspInitializeTypes();

    EPROCESS *System;
    STATUS Status = ObCreateObject(PsProcessType, sizeof(EPROCESS), NULL, (void **)&System);
    if (!HZ_SUCCESS(Status))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_SYSTEM_PROCESS, (uint64_t)Status, 0, 0);
    System->IsSystem = true;
    System->AddressSpace = &MmKernelAddressSpace;
    System->ObjectTable = ObCreateHandleTable();
    if (!System->ObjectTable)
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_SYSTEM_PROCESS, (uint64_t)STATUS_NO_MEMORY, 1, 0);
    System->ProcessId = 4;
    System->ExitStatus = STATUS_PENDING;
    InitializeListHead(&System->ThreadListHead);
    InitializeListHead(&System->WinCompatHeapAllocations);
    InitializeListHead(&System->WinCompatModules);
    RtlStringCopy(System->ImageName, sizeof(System->ImageName), "System");
    KeInitializeProcess(&System->Pcb, &MmKernelAddressSpace, KI_THREAD_PRIORITY_DEFAULT, "System", 4);
    InsertTailList(&PspActiveProcessList, &System->ActiveProcessLinks);
    PsInitialSystemProcess = System; /* the creation reference is never dropped */

    KeSetUserReturnCallout(PspUserReturnCallout);
    KeSetUserExceptionRoutine(PspUserExceptionRoutine);
}

/* ---- Creation ----------------------------------------------------------------------------------- */

static void PspImageNameFromPath(char *Name, size_t Size, const char *Path)
{
    const char *Leaf = Path;
    for (const char *p = Path; *p; p++) {
        if (*p == '\\' || *p == '/')
            Leaf = p + 1;
    }
    RtlStringCopy(Name, Size, Leaf);
}

static STATUS PspCreatePeb(EPROCESS *Process, const char *CommandLine, const HANDLE Standard[3])
{
    void *Peb = NULL;
    STATUS Status = MmAllocateVirtualMemory(Process->AddressSpace, &Peb, sizeof(HZ_PEB), MEM_RESERVE | MEM_COMMIT,
                                            MM_PROT_READWRITE);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_PEB Block;
    memset(&Block, 0, sizeof(Block));
    Block.ImageBase = Process->ImageBase;
    Block.ImageSize = Process->ImageSize;
    Block.ProcessId = Process->ProcessId;
    Block.ParentProcessId = Process->ParentProcessId;
    RtlStringCopy(Block.ImageName, sizeof(Block.ImageName), Process->ImageName);
    const char *Command = CommandLine ? CommandLine : Process->ImageName;
    RtlStringCopy(Block.CommandLine, sizeof(Block.CommandLine), Command);
    size_t CommandIndex = 0;
    while (Command[CommandIndex] &&
           CommandIndex + 1 < sizeof(Block.CommandLineW) / sizeof(Block.CommandLineW[0])) {
        Block.CommandLineW[CommandIndex] = (uint8_t)Command[CommandIndex];
        ++CommandIndex;
    }
    Block.CommandLineW[CommandIndex] = 0;
    Block.StandardInput = (uint64_t)Standard[0];
    Block.StandardOutput = (uint64_t)Standard[1];
    Block.StandardError = (uint64_t)Standard[2];
    MMADDRESS_SPACE *Previous = PsAttachProcess(Process);
    Status = MmCopyToUser(Peb, &Block, sizeof(Block));
    PsDetachProcess(Previous);
    if (HZ_SUCCESS(Status))
        Process->PebAddress = (uint64_t)Peb;
    return Status;
}

STATUS PsCreateProcess(const char *ImagePath, const char *CommandLine, uint32_t Flags, EPROCESS **Out)
{
    return PsCreateProcessEx(ImagePath, CommandLine, Flags, NULL, Out);
}

STATUS PsLookupProcessById(uint32_t ProcessId, EPROCESS **Out)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    for (LIST_ENTRY *E = PspActiveProcessList.Flink; E != &PspActiveProcessList; E = E->Flink) {
        EPROCESS *Process = CONTAINING_RECORD(E, EPROCESS, ActiveProcessLinks);
        if (Process->ProcessId == ProcessId && !Process->Exited) {
            ObReferenceObject(Process);
            KeReleaseSpinLock(&PspLock, OldIrql);
            *Out = Process;
            return STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&PspLock, OldIrql);
    return STATUS_INVALID_CID;
}

STATUS PsCreateProcessEx(const char *ImagePath, const char *CommandLine, uint32_t Flags,
                         const PS_STANDARD_HANDLES *Standard, EPROCESS **Out)
{
    /* The image is read through the I/O manager (any mounted volume). */
    void *Image;
    uint64_t ImageSize;
    STATUS Status = IoReadWholeFile(ImagePath, &Image, &ImageSize, PSP_MAX_IMAGE_FILE);
    if (!HZ_SUCCESS(Status))
        return Status;

    EPROCESS *Process;
    Status = ObCreateObject(PsProcessType, sizeof(EPROCESS), NULL, (void **)&Process);
    if (!HZ_SUCCESS(Status)) {
        ExFreePool(Image);
        return Status;
    }
    InitializeListHead(&Process->ThreadListHead);
    InitializeListHead(&Process->WinCompatHeapAllocations);
    InitializeListHead(&Process->WinCompatModules);
    Process->ExitStatus = STATUS_PENDING;
    Process->CreateTimeUs = KeQueryUptimeMicroseconds();
    EPROCESS *Parent = PsGetCurrentProcess();
    Process->ParentProcessId = Parent ? Parent->ProcessId : 0;
    PspImageNameFromPath(Process->ImageName, sizeof(Process->ImageName), ImagePath);

    Status = MmCreateAddressSpace(&Process->AddressSpace);
    if (HZ_SUCCESS(Status)) {
        Process->ObjectTable = ObCreateHandleTable();
        if (!Process->ObjectTable)
            Status = STATUS_NO_MEMORY;
    }
    if (HZ_SUCCESS(Status)) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&PspLock, &OldIrql);
        Process->ProcessId = PspNextProcessId;
        PspNextProcessId += 4;
        InsertTailList(&PspActiveProcessList, &Process->ActiveProcessLinks);
        KeReleaseSpinLock(&PspLock, OldIrql);
        KeInitializeProcess(&Process->Pcb, Process->AddressSpace, PSP_USER_THREAD_PRIORITY, Process->ImageName,
                            Process->ProcessId);
        Status = PspMapImage(Process, Image, ImageSize, Flags);
    }
    ExFreePool(Image);
    HANDLE StandardHandles[3] = {0, 0, 0};
    for (int i = 0; i < 3 && HZ_SUCCESS(Status) && Standard; i++) {
        if (Standard->Object[i])
            Status = ObOpenObjectByPointer(Standard->Object[i], Standard->Access[i], Process, &StandardHandles[i]);
    }
    if (HZ_SUCCESS(Status))
        Status = PspCreatePeb(Process, CommandLine, StandardHandles);

    ETHREAD *Thread = NULL;
    if (HZ_SUCCESS(Status))
        Status = PsCreateUserThread(Process, Process->TlsStartupThunk ? Process->TlsStartupThunk : Process->EntryPoint,
                                    Process->PebAddress, 0, &Thread);
    if (!HZ_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_INFO_LEVEL, "Process: could not create %s from %s (0x%08X)\n", Process->ImageName,
                   ImagePath, (uint32_t)Status);
        ObDereferenceObject(Process); /* deletes the address space and handle table */
        return Status;
    }
    DbgPrint("Process: started %s (PID %u, thread %u) from %s\n", Process->ImageName, Process->ProcessId,
             Thread->Tcb.ThreadId, ImagePath);
    ObDereferenceObject(Thread);
    *Out = Process;
    return STATUS_SUCCESS;
}

/* ---- Termination ---------------------------------------------------------------------------- */

STATUS PsTerminateProcess(EPROCESS *Process, STATUS ExitStatus)
{
    if (Process->IsSystem)
        return STATUS_ACCESS_DENIED;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    if (Process->Terminating || Process->Exited) {
        KeReleaseSpinLock(&PspLock, OldIrql);
        return STATUS_PROCESS_IS_TERMINATING;
    }
    Process->Terminating = true;
    Process->ExitStatus = ExitStatus;
    ETHREAD *Current = PsGetCurrentThread();
    for (LIST_ENTRY *E = Process->ThreadListHead.Flink; E != &Process->ThreadListHead; E = E->Flink) {
        ETHREAD *Thread = CONTAINING_RECORD(E, ETHREAD, ThreadListEntry);
        Thread->Terminating = true;
        Thread->ExitStatus = ExitStatus;
        if (Thread != Current)
            KeRequestUserReturnCallout(&Thread->Tcb);
    }
    KeReleaseSpinLock(&PspLock, OldIrql);
    return STATUS_SUCCESS;
}

void PspProcessExited(EPROCESS *Process)
{
    ExCleanupWinSocketsForCurrentProcess();
    HANDLE_TABLE *Table = Process->ObjectTable;
    Process->ObjectTable = NULL;
    if (Table)
        ObKillHandleTable(Table, Process);
    Process->ExitTimeUs = KeQueryUptimeMicroseconds();
    uint64_t Ms = (Process->ExitTimeUs - Process->CreateTimeUs) / 1000;
    DbgPrint("Process: %s (PID %u) exited with status 0x%08X%s%s after %llu ms\n", Process->ImageName,
             Process->ProcessId, (uint32_t)Process->ExitStatus,
             (uint32_t)Process->ExitStatus >= 0xC0000000u ? " " : "",
             (uint32_t)Process->ExitStatus >= 0xC0000000u ? PspStatusName(Process->ExitStatus) : "",
             (unsigned long long)Ms);
    KeSignalObject(&Process->Pcb.Header);
}

/* ---- Queries ------------------------------------------------------------------------------------ */

uint32_t PsQueryProcessCount(void)
{
    uint32_t Count = 0;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    for (LIST_ENTRY *E = PspActiveProcessList.Flink; E != &PspActiveProcessList; E = E->Flink) {
        if (!CONTAINING_RECORD(E, EPROCESS, ActiveProcessLinks)->Exited)
            Count++;
    }
    KeReleaseSpinLock(&PspLock, OldIrql);
    return Count;
}

uint32_t PsQueryProcessList(HZ_SYSTEM_PROCESS_INFORMATION *Entries, uint32_t Capacity)
{
    uint32_t Count = 0;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PspLock, &OldIrql);
    for (LIST_ENTRY *E = PspActiveProcessList.Flink; E != &PspActiveProcessList; E = E->Flink) {
        EPROCESS *Process = CONTAINING_RECORD(E, EPROCESS, ActiveProcessLinks);
        if (Process->Exited)
            continue;
        if (Count < Capacity) {
            HZ_SYSTEM_PROCESS_INFORMATION *Info = &Entries[Count];
            memset(Info, 0, sizeof(*Info));
            Info->ProcessId = Process->ProcessId;
            Info->ParentProcessId = Process->ParentProcessId;
            Info->ThreadCount = Process->ActiveThreads;
            Info->HandleCount = ObHandleCount(Process->ObjectTable);
            Info->ResidentPages = Process->IsSystem ? 0 : Process->AddressSpace->ResidentPages;
            Info->CreateTimeUs = Process->CreateTimeUs;
            /* CPU time of the live threads (exited threads' time is not accumulated yet). */
            for (LIST_ENTRY *T = Process->ThreadListHead.Flink; T != &Process->ThreadListHead; T = T->Flink)
                Info->CpuTimeUs += KeQueryThreadCpuTimeUs(&CONTAINING_RECORD(T, ETHREAD, ThreadListEntry)->Tcb);
            RtlStringCopy(Info->ImageName, sizeof(Info->ImageName), Process->ImageName);
        }
        Count++;
    }
    KeReleaseSpinLock(&PspLock, OldIrql);
    return Count;
}
