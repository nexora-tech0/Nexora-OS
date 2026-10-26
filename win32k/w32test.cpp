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
    DbgPrintEx(Clean ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "Window manager: %u windows and %lld surface sections before the desktop: %s\n",
               W32kQueryWindowCount(), (long long)(MmSectionObjectType->ObjectCount - SectionsBefore),
               Clean ? "OK" : "FAILED");
    return Clean;
}
