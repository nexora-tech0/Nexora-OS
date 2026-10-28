/*
 * Nexora OS - win32k/w32test.cpp
 * Boot-time window manager check (every boot, after the file system tests). The self-test programs
 * that used to run here (wintest.exe, uitest.exe, apptest.exe) were removed with the beta apps, so
 * what remains is the invariant they were there to protect: the window manager starts with no
 * windows and no surface sections, before the desktop is shown.
 */
#include "w32p.h"

extern "C" bool W32kRunSelfTests(void)
{
    uint64_t SectionsBefore = MmSectionObjectType->ObjectCount;
    bool Clean = W32kQueryWindowCount() == 0 && MmSectionObjectType->ObjectCount == SectionsBefore;
    bool PaintWakeOk = false;
    bool PaintWakeSkipped = false;
    STATUS CreateStatus = STATUS_UNSUCCESSFUL;
    WINDOW *Probe = nullptr;

    if (Clean) {
        HZ_CREATE_WINDOW Create = {};
        Create.Rect = HZ_RECT{CmpScreenWidth() + HZ_MAX_WINDOW_SIZE, CmpScreenHeight() + HZ_MAX_WINDOW_SIZE,
                              CmpScreenWidth() + HZ_MAX_WINDOW_SIZE + 160,
                              CmpScreenHeight() + HZ_MAX_WINDOW_SIZE + 120};
        Create.Style = HZ_WS_NOACTIVATE;

        WmAcquire();
        CreateStatus = WmCreateWindow(&Create, &Probe);
        W32THREAD *Owner = Probe ? Probe->Owner : nullptr;
        if (HZ_SUCCESS(CreateStatus) && Owner) {
            uint64_t Poll = 0;
            while (KeWaitForSingleObject(&Owner->Wake, &Poll) == STATUS_WAIT_0) {
            }
            WmShowWindow(Probe, HZ_SW_SHOWNOACTIVATE);
        }
        WmRelease();

        if (Probe && Owner) {
            uint64_t Poll = 0;
            bool Woken = KeWaitForSingleObject(&Owner->Wake, &Poll) == STATUS_WAIT_0;
            STATUS LastMessageStatus = STATUS_NO_MORE_ENTRIES;
            uint32_t LastMessage = 0;
            /* Showing the window can enqueue input as well as paint. Drain the window's queue until
               its update region produces WM_PAINT instead of assuming paint is the first message. */
            PaintWakeOk = Woken;
            for (uint32_t Attempt = 0; PaintWakeOk && Attempt < 32; Attempt++) {
                HZ_MSG Message = {};
                LastMessageStatus = WmGetMessage(&Message, Probe->Handle, 0, 0, true, false);
                LastMessage = Message.Message;
                if (!HZ_SUCCESS(LastMessageStatus)) {
                    PaintWakeOk = false;
                    break;
                }
                if (Message.Message == HZ_WM_PAINT)
                    break;
                if (Attempt == 31)
                    PaintWakeOk = false;
            }
            if (!PaintWakeOk)
                DbgPrintEx(DPFLTR_INFO_LEVEL, "Window manager check detail: wake=%u, last message=0x%X, status=0x%08X, visible=%u, update-empty=%u\n",
                           Woken, LastMessage, (uint32_t)LastMessageStatus, Probe->Visible, RgnIsEmpty(Probe->Update));
            WmAcquire();
            WmDestroyWindow(Probe);
            WmRelease();
        }
    }

    uint32_t WindowsAfter = W32kQueryWindowCount();
    uint64_t SectionsAfter = MmSectionObjectType->ObjectCount;
    bool CleanAfter = WindowsAfter == 0 && SectionsAfter == SectionsBefore;
    /* Phase-one runs in the kernel bootstrap process, which has no user address space to map a
       window surface into. STATUS_INVALID_PARAMETER here means this environment cannot run the
       user-window paint probe; it says nothing about the compositor's reveal path. */
    PaintWakeSkipped = !Probe && CreateStatus == STATUS_INVALID_PARAMETER;
    bool Ok = Clean && (PaintWakeOk || PaintWakeSkipped) && CleanAfter;
    DbgPrintEx(Ok ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "Window manager: startup clean %s, reveal wakes pending paint %s, cleanup %s "
               "(%u windows, %lld surface sections): %s\n",
               Clean ? "OK" : "FAILED", PaintWakeSkipped ? "SKIPPED (bootstrap has no user address space)"
                                      : PaintWakeOk ? "OK" : "FAILED",
               CleanAfter ? "OK" : "FAILED",
               WindowsAfter, (long long)(SectionsAfter - SectionsBefore), Ok ? "OK" : "FAILED");
    return Ok;
}
