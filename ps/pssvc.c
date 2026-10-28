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

/* Win32 kernel32.dll compatibility: GetStdHandle(STD_*_HANDLE). The current Nexora handle
   encoding is a small, pointer-sized integer, so STATUS can carry it back in EAX. */
STATUS NtWinGetStdHandle(const uint64_t *Args)
{
    EPROCESS *Process = PsGetCurrentProcess();
    if (!Process || !Process->PebAddress)
        return STATUS_INVALID_PARAMETER;
    HZ_PEB Peb;
    STATUS Status = MmCopyFromUser(&Peb, (const void *)Process->PebAddress, sizeof(Peb));
    if (!HZ_SUCCESS(Status))
        return Status;
    switch ((int32_t)(uint32_t)Args[0]) {
    case -10: return (STATUS)(uintptr_t)Peb.StandardInput;
    case -11: return (STATUS)(uintptr_t)Peb.StandardOutput;
    case -12: return (STATUS)(uintptr_t)Peb.StandardError;
    default: return STATUS_INVALID_PARAMETER;
    }
}

/* Win32 WriteFile supports an output byte count and optional OVERLAPPED structure. Only its
   synchronous form is implemented; the native file service already validates and copies buffers. */
STATUS NtWinWriteFile(const uint64_t *Args)
{
    if (Args[4])
        return STATUS_NOT_SUPPORTED;
    uint64_t NativeArgs[5] = {Args[0], Args[1], (uint32_t)Args[2], 0, Args[3]};
    return NtWriteFile(NativeArgs);
}

/* Returns a handle only for a PE module actually recorded in this process. */
STATUS NtWinGetModuleHandle(const uint64_t *Args)
{
    enum { NameCapacity = 260 };
    EPROCESS *Process = PsGetCurrentProcess();
    if (!Process || !Args[2])
        return STATUS_INVALID_PARAMETER;

    uint64_t ImageHandle = Process->ImageBase;
    if (Args[0]) {
        char Requested[NameCapacity];
        STATUS Status;
        if (Args[1]) {
            size_t Index;
            for (Index = 0; Index < sizeof(Requested); ++Index) {
                if (Args[0] > UINTPTR_MAX - Index * sizeof(uint16_t))
                    return STATUS_INVALID_PARAMETER;
                uint16_t Character;
                Status = MmCopyFromUser(&Character, (const void *)(uintptr_t)(Args[0] +
                                                Index * sizeof(uint16_t)), sizeof(Character));
                if (!HZ_SUCCESS(Status))
                    return Status;
                if (!Character)
                    break;
                if (Character > 0x7Fu)
                    return STATUS_DLL_NOT_FOUND;
                Requested[Index] = (char)Character;
            }
            if (Index == sizeof(Requested))
                return STATUS_NAME_TOO_LONG;
            Requested[Index] = 0;
        } else {
            Status = MmCopyStringFromUser(Requested, sizeof(Requested),
                                          (const char *)(uintptr_t)Args[0]);
            if (!HZ_SUCCESS(Status))
                return Status;
        }

        Status = PspLookupWinModule(Process, Requested, &ImageHandle, NULL);
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    return MmCopyToUser((void *)(uintptr_t)Args[2], &ImageHandle, sizeof(ImageHandle));
}

/* GetProcAddress currently resolves exports from the main PE image. DLL module loading is the
   next loader layer; until then, a handle not present in the module table is rejected. */
STATUS NtWinGetProcAddress(const uint64_t *Args)
{
    EPROCESS *Process = PsGetCurrentProcess();
    if (!Process || !Args[0] || !Args[1] || !Args[2])
        return STATUS_INVALID_PARAMETER;
    STATUS Status = PspProbeOutput(Args[2], sizeof(uint64_t));
    if (!HZ_SUCCESS(Status))
        return Status;

    char ModuleName[PSP_WIN_MODULE_NAME_MAX];
    uint64_t ModuleSize = 0;
    uint32_t ModuleFlags = 0;
    Status = PspLookupWinModuleByBase(Process, Args[0], ModuleName, sizeof(ModuleName),
                                      &ModuleSize, &ModuleFlags);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!(ModuleFlags & PSP_WIN_MODULE_MAIN_IMAGE) || ModuleSize > PSP_MAX_IMAGE_FILE)
        return STATUS_NOT_SUPPORTED;

    char ExportName[PE_EXPORT_NAME_MAX];
    bool ByOrdinal = Args[1] <= UINT16_MAX;
    if (!ByOrdinal) {
        Status = MmCopyStringFromUser(ExportName, sizeof(ExportName), (const char *)(uintptr_t)Args[1]);
        if (!HZ_SUCCESS(Status))
            return Status;
    }

    uint8_t *Image = ExAllocatePoolZero(NonPagedPool, ModuleSize, PSP_IMAGE_TAG);
    if (!Image)
        return STATUS_NO_MEMORY;
    MMADDRESS_SPACE *Previous = PsAttachProcess(Process);
    Status = MmCopyFromUser(Image, (const void *)(uintptr_t)Args[0], ModuleSize);
    PsDetachProcess(Previous);
    if (!HZ_SUCCESS(Status)) {
        ExFreePoolWithTag(Image, PSP_IMAGE_TAG);
        return Status;
    }

    PE_HEADERS Headers;
    Status = PspLocateMappedPeHeaders(Image, ModuleSize, &Headers);
    uint32_t FunctionRva = 0;
    char Forwarder[PE_EXPORT_NAME_MAX];
    if (HZ_SUCCESS(Status))
        Status = PeFindImageExport(Image, ModuleSize, &Headers,
                                   ByOrdinal ? NULL : ExportName,
                                   (uint16_t)Args[1], ByOrdinal,
                                   &FunctionRva, Forwarder, sizeof(Forwarder));
    ExFreePoolWithTag(Image, PSP_IMAGE_TAG);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Forwarder[0])
        return STATUS_NOT_SUPPORTED;
    if ((uint64_t)FunctionRva >= ModuleSize || Args[0] > UINT64_MAX - FunctionRva)
        return STATUS_INVALID_IMAGE_FORMAT;
    uint64_t Address = Args[0] + FunctionRva;
    DbgPrintEx(DPFLTR_TRACE_LEVEL,
               "WinCompat: GetProcAddress %s module=0x%llx image=0x%llx rva=0x%x result=0x%llx\n",
               ExportName, (unsigned long long)Args[0], (unsigned long long)Process->ImageBase,
               FunctionRva, (unsigned long long)Address);
    return MmCopyToUser((void *)(uintptr_t)Args[2], &Address, sizeof(Address));
}

/* Synchronous CreateFileW bridge for ASCII drive-letter paths. Share mode, security attributes,
   template handles, and asynchronous I/O are not implemented in this compatibility layer.
   Handle-table values are bounded 32-bit integers, so a successful handle can be returned in EAX. */
STATUS NtWinCreateFile(const uint64_t *Args)
{
    enum { PathCapacity = 256 };
    if (!Args[0])
        return STATUS_INVALID_PARAMETER;
    STATUS Status;

    char Path[PathCapacity + 8] = {0};
    size_t Index = 0;
    for (; Index + 1 < PathCapacity; ++Index) {
        if (Args[0] > UINTPTR_MAX - Index * sizeof(uint16_t))
            return STATUS_INVALID_PARAMETER;
        uint16_t Character;
        Status = MmCopyFromUser(&Character, (const void *)(uintptr_t)(Args[0] + Index * 2),
                                sizeof(Character));
        if (!HZ_SUCCESS(Status))
            return Status;
        if (!Character)
            break;
        if (Character > 0x7Fu)
            return STATUS_NOT_SUPPORTED;
        Path[Index] = (char)Character;
    }
    if (Index == PathCapacity - 1 || Index == 0)
        return STATUS_NAME_TOO_LONG;

    char ObjectPath[PathCapacity + 8];
    if (Path[0] == '\\') {
        RtlStringCopy(ObjectPath, sizeof(ObjectPath), Path);
    } else if (((Path[0] >= 'A' && Path[0] <= 'Z') || (Path[0] >= 'a' && Path[0] <= 'z')) &&
               Path[1] == ':') {
        char Drive = Path[0] >= 'a' && Path[0] <= 'z' ? (char)(Path[0] - ('a' - 'A')) : Path[0];
        RtlFormatString(ObjectPath, sizeof(ObjectPath), "\\??\\%c%s", Drive, Path + 1);
    } else {
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    }

    uint32_t Disposition;
    switch ((uint32_t)Args[2]) {
    case 1: Disposition = FILE_CREATE; break;          /* CREATE_NEW */
    case 2: Disposition = FILE_OVERWRITE_IF; break;     /* CREATE_ALWAYS */
    case 3: Disposition = FILE_OPEN; break;             /* OPEN_EXISTING */
    case 4: Disposition = FILE_OPEN_IF; break;          /* OPEN_ALWAYS */
    case 5: Disposition = FILE_OVERWRITE; break;        /* TRUNCATE_EXISTING */
    default: return STATUS_INVALID_PARAMETER;
    }
    uint32_t Options = FILE_NON_DIRECTORY_FILE;
    if ((uint32_t)Args[3] & FILE_ATTRIBUTE_DIRECTORY)
        Options = FILE_DIRECTORY_FILE;

    ACCESS_MASK Access = (ACCESS_MASK)Args[1];
    if (Access & GENERIC_READ)
        Access = (Access & ~GENERIC_READ) | FILE_READ_DATA | FILE_READ_ATTRIBUTES;
    if (Access & GENERIC_WRITE)
        Access = (Access & ~GENERIC_WRITE) | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES;
    if (!Access)
        Access = FILE_READ_ATTRIBUTES;

    FILE_OBJECT *File = NULL;
    Status = IoCreateFile(ObjectPath, Access, Disposition, Options, UserMode, &File, NULL);
    if (!HZ_SUCCESS(Status))
        return Status;
    HANDLE Handle = 0;
    Status = ObOpenObjectByPointer(File, File->GrantedAccess | SYNCHRONIZE, NULL, &Handle);
    ObDereferenceObject(File);
    if (!HZ_SUCCESS(Status))
        return Status;
    if ((uint64_t)(uintptr_t)Handle > INT32_MAX) {
        ObCloseHandle(Handle, KernelMode);
        return STATUS_NOT_SUPPORTED;
    }
    return (STATUS)(uint32_t)(uintptr_t)Handle;
}

/* GetFileAttributesW returns Nexora's basic directory/normal attributes in EAX. */
STATUS NtWinGetFileAttributes(const uint64_t *Args)
{
    enum { PathCapacity = 256 };
    if (!Args[0])
        return STATUS_INVALID_PARAMETER;

    char Path[PathCapacity] = {0};
    size_t Index = 0;
    for (; Index + 1 < sizeof(Path); ++Index) {
        if (Args[0] > UINTPTR_MAX - Index * sizeof(uint16_t))
            return STATUS_INVALID_PARAMETER;
        uint16_t Character;
        STATUS Status = MmCopyFromUser(&Character,
                                       (const void *)(uintptr_t)(Args[0] + Index * sizeof(uint16_t)),
                                       sizeof(Character));
        if (!HZ_SUCCESS(Status))
            return Status;
        if (!Character)
            break;
        if (Character > 0x7Fu)
            return STATUS_NOT_SUPPORTED;
        Path[Index] = (char)Character;
    }
    if (Index == 0 || Index == sizeof(Path) - 1)
        return STATUS_NAME_TOO_LONG;

    char ObjectPath[sizeof(Path) + 8];
    if (Path[0] == '\\') {
        RtlStringCopy(ObjectPath, sizeof(ObjectPath), Path);
    } else if (((Path[0] >= 'A' && Path[0] <= 'Z') || (Path[0] >= 'a' && Path[0] <= 'z')) &&
               Path[1] == ':') {
        char Drive = Path[0] >= 'a' && Path[0] <= 'z' ? (char)(Path[0] - ('a' - 'A')) : Path[0];
        RtlFormatString(ObjectPath, sizeof(ObjectPath), "\\??\\%c%s", Drive, Path + 1);
    } else {
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    }

    FILE_OBJECT *File = NULL;
    STATUS Status = IoCreateFile(ObjectPath, FILE_READ_ATTRIBUTES, FILE_OPEN, 0,
                                 UserMode, &File, NULL);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint32_t Attributes = (File->Flags & FO_DIRECTORY) ? FILE_ATTRIBUTE_DIRECTORY : 0x80u;
    ObDereferenceObject(File);
    return (STATUS)Attributes;
}

/* Win32 ExitProcess(exitCode): terminate this process and never return to the caller. */
STATUS NtWinExitProcess(const uint64_t *Args)
{
    EPROCESS *Process = PsGetCurrentProcess();
    if (!Process)
        return STATUS_INVALID_PARAMETER;
    STATUS ExitStatus = (STATUS)(uint32_t)Args[0];
    DbgPrintEx(DPFLTR_INFO_LEVEL, "WinCompat: %s ExitProcess(%u)\n", Process->ImageName,
               (uint32_t)ExitStatus);
    STATUS Status = PsTerminateProcess(Process, ExitStatus);
    PsExitThread(HZ_SUCCESS(Status) ? ExitStatus : Status);
}

/* QueryWinCompatReport(ImagePath, Buffer, Capacity): format the current loader's verdict without
   creating a process or modifying the image. */
STATUS NtQueryWinCompatReport(const uint64_t *Args)
{
    enum { PathCapacity = 512, ReportCapacity = 2048 };
    if (!Args[0] || !Args[1] || Args[2] == 0)
        return STATUS_INVALID_PARAMETER;

    char UserPath[PathCapacity];
    char Path[PathCapacity + 8];
    STATUS Status = MmCopyStringFromUser(UserPath, sizeof(UserPath), (const char *)Args[0]);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (UserPath[0] == '\\')
        RtlStringCopy(Path, sizeof(Path), UserPath);
    else
        RtlFormatString(Path, sizeof(Path), "\\??\\%s", UserPath);

    void *File = NULL;
    uint64_t FileSize = 0;
    Status = IoReadWholeFile(Path, &File, &FileSize, PSP_MAX_IMAGE_FILE);
    if (!HZ_SUCCESS(Status))
        return Status;

    char Report[ReportCapacity] = {0};
    Status = PspBuildWindowsImportReport(File, FileSize, Report, sizeof(Report));
    ExFreePool(File);
    if (!Report[0])
        return Status;

    size_t Length = strlen(Report) + 1;
    if (Args[2] < Length)
        return STATUS_BUFFER_TOO_SMALL;
    STATUS CopyStatus = MmCopyToUser((void *)Args[1], Report, Length);
    if (!HZ_SUCCESS(CopyStatus))
        return CopyStatus;
    return Status == STATUS_BUFFER_TOO_SMALL ? Status : STATUS_SUCCESS;
}
