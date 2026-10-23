/*
 * Nexora OS - ps/pstest.c
 * User-mode self-tests, run in Phase 1 once \??\C: exists: start the test programs from the
 * ramdisk, wait for them and compare their exit status with the expected one.
 *
 *   hello.exe     the first user program: prints through NtDisplayString and returns 42; run at
 *                 its preferred base and once more with the loader forced to relocate it
 *   usertest.exe  system calls, bad pointers and handles, events, waits, threads, termination,
 *                 virtual memory, FPU/SSE/AVX state across switches, child processes (exit 0)
 *   crash.exe     one fault per run (null pointer, kernel address, cli, divide, ud2, stack
 *                 overflow, execute data); each must end the process, not the system
 *
 * Afterwards every process and thread object must be gone and the pool back where it was.
 */
#include "psp.h"

#define PST_TIMEOUT_US (30ull * 1000 * 1000)
#define PST_SYSTEM_DIR "\\??\\C:\\Nexora\\System\\"
#define PST_PREFERRED_BASE 0x140000000ull /* lld-link's default for x64 executables */

typedef struct _PST_CASE {
    const char *Image;
    const char *CommandLine;
    uint32_t Flags;
    STATUS Expected;
    const char *Description;
} PST_CASE;

static const PST_CASE PstCases[] = {
    {"hello.exe", "hello.exe", 0, 42, "first user program at its preferred base"},
    {"hello.exe", "hello.exe relocated", PS_CREATE_FORCE_RELOCATION, 42, "same image relocated by the loader"},
    {"usertest.exe", "usertest.exe", 0, STATUS_SUCCESS, "system calls, handles, threads, memory, FPU state"},
    {"crash.exe", "crash.exe null", 0, STATUS_ACCESS_VIOLATION, "write through a null pointer"},
    {"crash.exe", "crash.exe kernel", 0, STATUS_ACCESS_VIOLATION, "read of kernel memory"},
    {"crash.exe", "crash.exe cli", 0, STATUS_PRIVILEGED_INSTRUCTION, "cli in ring 3"},
    {"crash.exe", "crash.exe divide", 0, STATUS_INTEGER_DIVIDE_BY_ZERO, "integer division by zero"},
    {"crash.exe", "crash.exe ud2", 0, STATUS_ILLEGAL_INSTRUCTION, "undefined instruction"},
    {"crash.exe", "crash.exe stack", 0, STATUS_STACK_OVERFLOW, "user stack overflow into the guard page"},
    {"crash.exe", "crash.exe nx", 0, STATUS_ACCESS_VIOLATION, "execute from a data page"},
    {"crash.exe", "crash.exe int3", 0, 3, "int3 is logged and execution continues"},
};

static bool PstRunCase(const PST_CASE *Case)
{
    char Path[128];
    RtlFormatString(Path, sizeof(Path), PST_SYSTEM_DIR "%s", Case->Image);
    EPROCESS *Process;
    STATUS Status = PsCreateProcess(Path, Case->CommandLine, Case->Flags, &Process);
    if (!HZ_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_ERROR_LEVEL, "PsTest: %s: could not start %s (0x%08X)\n", Case->Description, Path,
                   (uint32_t)Status);
        return false;
    }
    uint64_t Timeout = PST_TIMEOUT_US;
    Status = KeWaitForSingleObject(Process, &Timeout);
    if (Status == STATUS_TIMEOUT) {
        PsTerminateProcess(Process, STATUS_TIMEOUT);
        KeWaitForSingleObject(Process, NULL);
    }
    bool Relocated = Process->ImageBase != PST_PREFERRED_BASE;
    bool Ok = Status != STATUS_TIMEOUT && Process->ExitStatus == Case->Expected &&
              Relocated == ((Case->Flags & PS_CREATE_FORCE_RELOCATION) != 0);
    DbgPrintEx(Ok ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "PsTest: %-48s exit 0x%08X (expected 0x%08X) at 0x%llx: %s\n", Case->Description,
               (uint32_t)Process->ExitStatus, (uint32_t)Case->Expected, (unsigned long long)Process->ImageBase,
               Status == STATUS_TIMEOUT ? "TIMED OUT" : Ok ? "OK" : "FAILED");
    ObDereferenceObject(Process);
    return Ok;
}

bool PsRunSelfTests(void)
{
    POOL_STATISTICS PoolBefore, PoolAfter;
    ExQueryPoolStatistics(&PoolBefore);
    uint64_t ProcessesBefore = PsProcessType->ObjectCount, ThreadsBefore = PsThreadType->ObjectCount;
    uint64_t EventsBefore = ExEventObjectType->ObjectCount, FilesBefore = IoFileObjectType->ObjectCount;
    uint64_t SystemCallsBefore = KeGetCurrentPrcb()->SystemCalls;

    uint32_t Passed = 0;
    for (size_t i = 0; i < HZ_ARRAYSIZE(PstCases); i++)
        Passed += PstRunCase(&PstCases[i]);

    /* Thread objects are deleted by the worker thread after the reaper frees their stacks. */
    for (int Wait = 0; Wait < 200; Wait++) {
        if (PsProcessType->ObjectCount == ProcessesBefore && PsThreadType->ObjectCount == ThreadsBefore)
            break;
        KeDelayExecutionThread(10000);
    }
    ExQueryPoolStatistics(&PoolAfter);
    bool NoLeaks = PsProcessType->ObjectCount == ProcessesBefore && PsThreadType->ObjectCount == ThreadsBefore &&
                   ExEventObjectType->ObjectCount == EventsBefore && IoFileObjectType->ObjectCount == FilesBefore &&
                   PoolAfter.BytesInUse == PoolBefore.BytesInUse;
    DbgPrintEx(NoLeaks ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL,
               "PsTest: objects left: %lld processes, %lld threads, %lld events, %lld files; pool %+lld bytes: %s\n",
               (long long)(PsProcessType->ObjectCount - ProcessesBefore),
               (long long)(PsThreadType->ObjectCount - ThreadsBefore),
               (long long)(ExEventObjectType->ObjectCount - EventsBefore),
               (long long)(IoFileObjectType->ObjectCount - FilesBefore),
               (long long)(PoolAfter.BytesInUse - PoolBefore.BytesInUse), NoLeaks ? "OK" : "LEAK");

    uint32_t Total = HZ_ARRAYSIZE(PstCases) + 1;
    Passed += NoLeaks;
    DbgPrintEx(Passed == Total ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "Process self-tests: %u/%u passed (%llu system calls)\n", Passed, Total,
               (unsigned long long)(KeGetCurrentPrcb()->SystemCalls - SystemCallsBefore));
    return Passed == Total;
}
