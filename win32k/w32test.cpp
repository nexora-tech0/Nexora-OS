/*
 * Nexora OS - win32k/w32test.cpp
 * Window manager self-tests (every boot, after the file system tests): runs wintest.exe (windows,
 * messages, painting, composition, input, window states), uitest.exe (the hzui toolkit, the
 * editor and the dialogs) and apptest.exe (the applications' engines, manifests, paths), each
 * returning its number of failed checks, then verifies that no window or surface section was
 * left behind.
 */
#include "w32p.h"

namespace {

STATUS RunTestProgram(const char *Path, const char *Name)
{
    EPROCESS *Process;
    STATUS Status = PsCreateProcess(Path, Name, 0, &Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint64_t Timeout = 60ull * 1000 * 1000;
    if (KeWaitForSingleObject(Process, &Timeout) == STATUS_TIMEOUT) {
        PsTerminateProcess(Process, STATUS_TIMEOUT);
        KeWaitForSingleObject(Process, NULL);
    }
    Status = Process->ExitStatus;
    ObDereferenceObject(Process);
    return Status;
}

}  // namespace

extern "C" bool W32kRunSelfTests(void)
{
    uint64_t Start = KeQueryUptimeMicroseconds();
    uint64_t SectionsBefore = MmSectionObjectType->ObjectCount;
    STATUS WinTest = RunTestProgram("\\??\\C:\\Nexora\\System\\wintest.exe", "wintest.exe");
    STATUS UiTest = RunTestProgram("\\??\\C:\\Nexora\\System\\uitest.exe", "uitest.exe");
    STATUS AppTest = RunTestProgram("\\??\\C:\\Nexora\\System\\apptest.exe", "apptest.exe");
    bool Ok = WinTest == STATUS_SUCCESS && UiTest == STATUS_SUCCESS && AppTest == STATUS_SUCCESS;
    /* Surfaces go when the compositor drops its last frame's references and the processes die. */
    for (int Wait = 0; Wait < 200 && MmSectionObjectType->ObjectCount != SectionsBefore; Wait++)
        KeDelayExecutionThread(10000);
    bool Clean = W32kQueryWindowCount() == 0 && MmSectionObjectType->ObjectCount == SectionsBefore;
    DbgPrintEx(Ok && Clean ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "Window manager self-tests: wintest.exe exit 0x%08X, uitest.exe exit 0x%08X, apptest.exe exit 0x%08X, "
               "%u windows and %lld surfaces left: %s (%llu ms)\n",
               (uint32_t)WinTest, (uint32_t)UiTest, (uint32_t)AppTest, W32kQueryWindowCount(),
               (long long)(MmSectionObjectType->ObjectCount - SectionsBefore), Ok && Clean ? "OK" : "FAILED",
               (unsigned long long)((KeQueryUptimeMicroseconds() - Start) / 1000));
    return Ok && Clean;
}
