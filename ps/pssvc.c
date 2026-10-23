/*
 * Nexora OS - ps/pssvc.c
 * Process and thread system services. Arguments arrive as raw 64-bit values (see the service
 * list in uapi/hzapi.h); every user pointer goes through the Mm copy routines, and output
 * pointers are probed before anything irreversible (such as starting a thread) happens.
 */
#include "psp.h"

#define PSP_PATH_MAX 256

static STATUS PspProbeOutput(uint64_t Pointer, size_t Length)
{
    static const uint8_t Zero[8];
    if (!Pointer)
        return STATUS_SUCCESS; /* optional output */
    return MmCopyToUser((void *)Pointer, Zero, Length);
}

static STATUS PspReturnHandle(void *Object, ACCESS_MASK Access, uint64_t UserHandle)
{
    if (!UserHandle)
        return STATUS_SUCCESS;
    HANDLE Handle;
    STATUS Status = ObOpenObjectByPointer(Object, Access, NULL, &Handle);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = MmCopyToUser((void *)UserHandle, &Handle, sizeof(Handle));
    if (!HZ_SUCCESS(Status))
        ObCloseHandle(Handle, KernelMode);
    return Status;
}

/* TerminateProcess(HANDLE Process, STATUS ExitStatus) */
STATUS NtTerminateProcess(const uint64_t *Args)
{
    EPROCESS *Process;
    STATUS Status = ObReferenceObjectByHandle((HANDLE)Args[0], PsProcessType, PROCESS_TERMINATE, UserMode,
                                              (void **)&Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = PsTerminateProcess(Process, (STATUS)(uint32_t)Args[1]);
    bool Self = Process == PsGetCurrentProcess();
    ObDereferenceObject(Process);
    if (Self && Process->Terminating)
        PsExitThread(Process->ExitStatus); /* our own thread reference keeps Process alive */
    return Status;
}

/* TerminateThread(HANDLE Thread, STATUS ExitStatus) */
STATUS NtTerminateThread(const uint64_t *Args)
{
    ETHREAD *Thread;
    STATUS Status = ObReferenceObjectByHandle((HANDLE)Args[0], PsThreadType, THREAD_TERMINATE, UserMode,
                                              (void **)&Thread);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Thread == PsGetCurrentThread()) {
        ObDereferenceObject(Thread); /* the running reference keeps it alive */
        PsExitThread((STATUS)(uint32_t)Args[1]);
    }
    Status = PsTerminateThread(Thread, (STATUS)(uint32_t)Args[1]);
    ObDereferenceObject(Thread);
    return Status;
}

/* CreateThread(HANDLE Process, void *Start, uint64_t Arg1, uint64_t Arg2, HANDLE *Thread, uint32_t *ThreadId) */
STATUS NtCreateThread(const uint64_t *Args)
{
    STATUS Status = PspProbeOutput(Args[4], sizeof(HANDLE));
    if (HZ_SUCCESS(Status))
        Status = PspProbeOutput(Args[5], sizeof(uint32_t));
    if (!HZ_SUCCESS(Status))
        return Status;
    EPROCESS *Process;
    Status = ObReferenceObjectByHandle((HANDLE)Args[0], PsProcessType, PROCESS_CREATE_THREAD, UserMode,
                                       (void **)&Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Process != PsGetCurrentProcess()) {
        ObDereferenceObject(Process);
        return STATUS_NOT_SUPPORTED; /* remote threads need the target's address space */
    }
    ETHREAD *Thread;
    Status = PsCreateUserThread(Process, Args[1], Args[2], Args[3], &Thread);
    ObDereferenceObject(Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = PspReturnHandle(Thread, THREAD_ALL_ACCESS, Args[4]);
    if (HZ_SUCCESS(Status) && Args[5]) {
        uint32_t Id = Thread->Tcb.ThreadId;
        Status = MmCopyToUser((void *)Args[5], &Id, sizeof(Id));
    }
    ObDereferenceObject(Thread);
    return Status;
}

/* CreateProcess(const char *ImagePath, const char *CommandLine, HANDLE *Process, uint32_t *ProcessId) */
STATUS NtCreateProcess(const uint64_t *Args)
{
    char UserPath[PSP_PATH_MAX], Path[PSP_PATH_MAX + 8], CommandLine[PSP_PATH_MAX];
    STATUS Status = MmCopyStringFromUser(UserPath, sizeof(UserPath), (const char *)Args[0]);
    if (HZ_SUCCESS(Status) && Args[1])
        Status = MmCopyStringFromUser(CommandLine, sizeof(CommandLine), (const char *)Args[1]);
    if (HZ_SUCCESS(Status))
        Status = PspProbeOutput(Args[2], sizeof(HANDLE));
    if (HZ_SUCCESS(Status))
        Status = PspProbeOutput(Args[3], sizeof(uint32_t));
    if (!HZ_SUCCESS(Status))
        return Status;
    /* "C:\x.exe" is a DOS path: resolve it through \??\. */
    if (UserPath[0] == '\\')
        RtlStringCopy(Path, sizeof(Path), UserPath);
    else
        RtlFormatString(Path, sizeof(Path), "\\??\\%s", UserPath);

    EPROCESS *Process;
    Status = PsCreateProcess(Path, Args[1] ? CommandLine : NULL, 0, &Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = PspReturnHandle(Process, PROCESS_ALL_ACCESS, Args[2]);
    if (HZ_SUCCESS(Status) && Args[3]) {
        uint32_t Id = Process->ProcessId;
        Status = MmCopyToUser((void *)Args[3], &Id, sizeof(Id));
    }
    ObDereferenceObject(Process);
    return Status;
}

/* CreateProcessEx(const char *ImagePath, const char *CommandLine, const HZ_PROCESS_PARAMETERS *Parameters,
                   HANDLE *Process, uint32_t *ProcessId) */
STATUS NtCreateProcessEx(const uint64_t *Args)
{
    char UserPath[PSP_PATH_MAX], Path[PSP_PATH_MAX + 8], CommandLine[PSP_PATH_MAX];
    HZ_PROCESS_PARAMETERS Parameters;
    memset(&Parameters, 0, sizeof(Parameters));
    STATUS Status = MmCopyStringFromUser(UserPath, sizeof(UserPath), (const char *)Args[0]);
    if (HZ_SUCCESS(Status) && Args[1])
        Status = MmCopyStringFromUser(CommandLine, sizeof(CommandLine), (const char *)Args[1]);
    if (HZ_SUCCESS(Status) && Args[2])
        Status = MmCopyFromUser(&Parameters, (const void *)Args[2], sizeof(Parameters));
    if (HZ_SUCCESS(Status))
        Status = PspProbeOutput(Args[3], sizeof(HANDLE));
    if (HZ_SUCCESS(Status))
        Status = PspProbeOutput(Args[4], sizeof(uint32_t));
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Parameters.Flags)
        return STATUS_INVALID_PARAMETER;

    /* The standard handles must be files (including pipe ends) of the caller. */
    PS_STANDARD_HANDLES Standard;
    memset(&Standard, 0, sizeof(Standard));
    HANDLE Given[3] = {Parameters.StandardInput, Parameters.StandardOutput, Parameters.StandardError};
    for (int i = 0; i < 3 && HZ_SUCCESS(Status); i++) {
        if (!Given[i])
            continue;
        Status = ObReferenceObjectByHandle(Given[i], IoFileObjectType, 0, UserMode, &Standard.Object[i]);
        if (HZ_SUCCESS(Status))
            Standard.Access[i] = ((FILE_OBJECT *)Standard.Object[i])->GrantedAccess | SYNCHRONIZE;
    }
    EPROCESS *Process = NULL;
    if (HZ_SUCCESS(Status)) {
        if (UserPath[0] == '\\')
            RtlStringCopy(Path, sizeof(Path), UserPath);
        else
            RtlFormatString(Path, sizeof(Path), "\\??\\%s", UserPath);
        Status = PsCreateProcessEx(Path, Args[1] ? CommandLine : NULL, 0, &Standard, &Process);
    }
    for (int i = 0; i < 3; i++)
        if (Standard.Object[i])
            ObDereferenceObject(Standard.Object[i]);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = PspReturnHandle(Process, PROCESS_ALL_ACCESS, Args[3]);
    if (HZ_SUCCESS(Status) && Args[4]) {
        uint32_t Id = Process->ProcessId;
        Status = MmCopyToUser((void *)Args[4], &Id, sizeof(Id));
    }
    ObDereferenceObject(Process);
    return Status;
}

/* OpenProcess(HANDLE *Process, ACCESS_MASK Access, uint32_t ProcessId): any process but the
   System process, which only allows queries. */
STATUS NtOpenProcess(const uint64_t *Args)
{
    STATUS Status = PspProbeOutput(Args[0], sizeof(HANDLE));
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!Args[0])
        return STATUS_ACCESS_VIOLATION;
    ACCESS_MASK Access = (ACCESS_MASK)Args[1];
    if (Access & ~PROCESS_ALL_ACCESS)
        return STATUS_INVALID_PARAMETER;
    EPROCESS *Process;
    Status = PsLookupProcessById((uint32_t)Args[2], &Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Process->IsSystem && (Access & ~(PROCESS_QUERY_INFORMATION | SYNCHRONIZE)))
        Status = STATUS_ACCESS_DENIED;
    else
        Status = PspReturnHandle(Process, Access, Args[0]);
    ObDereferenceObject(Process);
    return Status;
}

/* QueryInformationProcess(HANDLE Process, uint32_t Class, void *Buffer, uint32_t Length) */
STATUS NtQueryInformationProcess(const uint64_t *Args)
{
    if ((uint32_t)Args[1] != HZ_PROCESS_BASIC_INFORMATION_CLASS)
        return STATUS_INVALID_INFO_CLASS;
    if ((uint32_t)Args[3] != sizeof(HZ_PROCESS_BASIC_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;
    EPROCESS *Process;
    STATUS Status = ObReferenceObjectByHandle((HANDLE)Args[0], PsProcessType, PROCESS_QUERY_INFORMATION, UserMode,
                                              (void **)&Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_PROCESS_BASIC_INFORMATION Info;
    memset(&Info, 0, sizeof(Info));
    Info.ExitStatus = Process->Exited ? Process->ExitStatus : STATUS_PENDING;
    Info.ProcessId = Process->ProcessId;
    Info.ParentProcessId = Process->ParentProcessId;
    Info.ThreadCount = Process->ActiveThreads;
    Info.HandleCount = ObHandleCount(Process->ObjectTable);
    Info.ImageBase = Process->ImageBase;
    ObDereferenceObject(Process);
    return MmCopyToUser((void *)Args[2], &Info, sizeof(Info));
}

/* QueryInformationThread(HANDLE Thread, uint32_t Class, void *Buffer, uint32_t Length) */
STATUS NtQueryInformationThread(const uint64_t *Args)
{
    if ((uint32_t)Args[1] != HZ_THREAD_BASIC_INFORMATION_CLASS)
        return STATUS_INVALID_INFO_CLASS;
    if ((uint32_t)Args[3] != sizeof(HZ_THREAD_BASIC_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;
    ETHREAD *Thread;
    STATUS Status = ObReferenceObjectByHandle((HANDLE)Args[0], PsThreadType, THREAD_QUERY_INFORMATION, UserMode,
                                              (void **)&Thread);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_THREAD_BASIC_INFORMATION Info;
    memset(&Info, 0, sizeof(Info));
    Info.ExitStatus = Thread->HasExited ? Thread->ExitStatus : STATUS_PENDING;
    Info.ThreadId = Thread->Tcb.ThreadId;
    Info.ProcessId = Thread->Process->ProcessId;
    Info.Priority = Thread->Tcb.Priority;
    Info.TebAddress = Thread->TebAddress;
    ObDereferenceObject(Thread);
    return MmCopyToUser((void *)Args[2], &Info, sizeof(Info));
}
