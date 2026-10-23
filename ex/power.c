/*
 * Nexora OS - ex/power.c
 * Shutdown and system time services.
 */
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "mm.h"
#include "ps.h"
#include "rtl.h"

HZ_NORETURN void ExShutdownSystem(uint32_t Action)
{
    EPROCESS *Caller = PsGetCurrentProcess();
    DbgPrint("Power: %s requested by %s\n", Action == HZ_SHUTDOWN_REBOOT ? "restart" : "shutdown",
             Caller ? Caller->ImageName : "the kernel");
    IoShutdownSystem();
    if (Action == HZ_SHUTDOWN_REBOOT) {
        DbgPrint("Power: resetting the machine\n");
        HalReboot();
    }
    DbgPrint("Power: powering off (ACPI S5)\n");
    HalPowerOff();
}

/* ShutdownSystem(uint32_t Action) */
STATUS NtShutdownSystem(const uint64_t *Args)
{
    uint32_t Action = (uint32_t)Args[0];
    if (Action != HZ_SHUTDOWN_POWER_OFF && Action != HZ_SHUTDOWN_REBOOT)
        return STATUS_INVALID_PARAMETER;
    ExShutdownSystem(Action);
}

/* QuerySystemTime(uint64_t *Time) */
STATUS NtQuerySystemTime(const uint64_t *Args)
{
    uint64_t Time = KeQuerySystemTime();
    return MmCopyToUser((void *)Args[0], &Time, sizeof(Time));
}
