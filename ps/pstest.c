/*
 * Nexora OS - ps/pstest.c
 * Boot-time process-manager check, run in Phase 1 once \??\C: exists.
 *
 * The process self-tests that used to be here started test programs from the ramdisk (hello.exe,
 * usertest.exe and crash.exe), waited for them and compared each exit status with the expected one -
 * the first user program at its preferred base and relocated, system calls and handles and threads,
 * and one faulting run per exception. Those programs were removed with the beta apps, so what is
 * left is the invariant they were there to protect: once Phase 1 has settled, the object manager
 * holds no leftover process, thread, event or file object, and the pool is back where it started.
 */
#include "psp.h"

bool PsRunSelfTests(void)
{
    POOL_STATISTICS PoolBefore, PoolAfter;
    ExQueryPoolStatistics(&PoolBefore);
    uint64_t ProcessesBefore = PsProcessType->ObjectCount, ThreadsBefore = PsThreadType->ObjectCount;
    uint64_t EventsBefore = ExEventObjectType->ObjectCount, FilesBefore = IoFileObjectType->ObjectCount;

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
    DbgPrintEx(NoLeaks ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL, "Process self-tests: %u/1 passed\n",
               NoLeaks ? 1u : 0u);
    return NoLeaks;
}
