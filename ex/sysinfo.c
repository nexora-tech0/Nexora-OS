/*
 * Nexora OS - ex/sysinfo.c
 * Miscellaneous system services: debug output, delays, uptime and system information.
 */
#include "ex.h"
#include "hal.h"
#include "mm.h"
#include "netif.h"
#include "ps.h"
#include "rtl.h"

#define EXP_DISPLAY_MAX 1024
#define EXP_PROCESS_INFO_MAX 64
#define EXP_INFO_TAG HZ_POOL_TAG('E', 'x', 'I', 'n')

/* DisplayString(const char *Text, uint64_t Length): one log line per call, prefixed by the
   caller's image name; a trailing newline is optional. */
STATUS NtDisplayString(const uint64_t *Args)
{
    uint64_t Length = Args[1];
    if (Length > EXP_DISPLAY_MAX)
        return STATUS_INVALID_PARAMETER;
    char Text[EXP_DISPLAY_MAX + 1];
    STATUS Status = MmCopyFromUser(Text, (const void *)Args[0], Length);
    if (!HZ_SUCCESS(Status))
        return Status;
    Text[Length] = '\0';
    size_t Visible = strlen(Text);
    while (Visible && (Text[Visible - 1] == '\n' || Text[Visible - 1] == '\r'))
        Text[--Visible] = '\0';
    for (size_t i = 0; i < Visible; i++) {
        if ((uint8_t)Text[i] < 0x20 && Text[i] != '\n' && Text[i] != '\t')
            Text[i] = '?'; /* keep the serial log readable */
    }
    EPROCESS *Process = PsGetCurrentProcess();
    DbgPrint("[%s:%u] %s\n", Process->ImageName, Process->ProcessId, Text);
    return STATUS_SUCCESS;
}

/* DelayExecution(uint64_t Microseconds) */
STATUS NtDelayExecution(const uint64_t *Args)
{
    STATUS Status = KeDelayExecutionThreadEx(Args[0], true);
    return Status == STATUS_TIMEOUT ? STATUS_SUCCESS : Status; /* the delay elapsed (or STATUS_ALERTED) */
}

/* YieldExecution() */
STATUS NtYieldExecution(const uint64_t *Args)
{
    KeYieldThread();
    return STATUS_SUCCESS;
}

/* QueryUptime(uint64_t *Microseconds) */
STATUS NtQueryUptime(const uint64_t *Args)
{
    uint64_t Uptime = KeQueryUptimeMicroseconds();
    return MmCopyToUser((void *)Args[0], &Uptime, sizeof(Uptime));
}

/* QuerySystemInformation(uint32_t Class, void *Buffer, uint32_t Length, uint32_t *ReturnLength) */
STATUS NtQuerySystemInformation(const uint64_t *Args)
{
    uint32_t Class = (uint32_t)Args[0];
    uint32_t Length = (uint32_t)Args[2];
    void *Buffer = (void *)Args[1];
    uint32_t Returned = 0;
    STATUS Status;

    if (Class == HZ_SYSTEM_BASIC_INFORMATION_CLASS) {
        if (Length != sizeof(HZ_SYSTEM_BASIC_INFORMATION))
            return STATUS_INFO_LENGTH_MISMATCH;
        HZ_SYSTEM_BASIC_INFORMATION Info;
        memset(&Info, 0, sizeof(Info));
        MM_STATISTICS Mm;
        MmQueryStatistics(&Mm);
        Info.PageSize = PAGE_SIZE;
        Info.ProcessorCount = KeNumberProcessors;
        Info.TotalPhysicalPages = Mm.TotalPages;
        Info.AvailablePages = Mm.AvailablePages;
        Info.UptimeUs = KeQueryUptimeMicroseconds();
        for (uint32_t n = 0; n < KI_MAX_PROCESSORS; n++) { /* totals over the online processors (M11) */
            KPRCB *Prcb = KiProcessorBlock[n];
            if (!Prcb || !Prcb->Online)
                continue;
            Info.IdleTimeUs += KeQueryThreadCpuTimeUs(Prcb->IdleThread);
            Info.ContextSwitches += Prcb->ContextSwitches;
            Info.InterruptCount += Prcb->InterruptCount;
        }
        Info.ProcessCount = PsQueryProcessCount();
        Info.ThreadCount = KeQueryThreadCount();
        Returned = sizeof(Info);
        Status = MmCopyToUser(Buffer, &Info, sizeof(Info));
    } else if (Class == HZ_SYSTEM_PROCESS_INFORMATION_CLASS) {
        HZ_SYSTEM_PROCESS_INFORMATION *Entries =
            ExAllocatePoolWithTag(NonPagedPool, EXP_PROCESS_INFO_MAX * sizeof(*Entries), EXP_INFO_TAG);
        if (!Entries)
            return STATUS_NO_MEMORY;
        uint32_t Count = PsQueryProcessList(Entries, EXP_PROCESS_INFO_MAX);
        uint32_t Available = HZ_MIN(Count, EXP_PROCESS_INFO_MAX);
        uint32_t Fit = Length / sizeof(*Entries);
        Returned = Count * sizeof(*Entries);
        Status = MmCopyToUser(Buffer, Entries, HZ_MIN(Fit, Available) * sizeof(*Entries));
        if (HZ_SUCCESS(Status) && Fit < Count)
            Status = STATUS_BUFFER_TOO_SMALL;
        ExFreePoolWithTag(Entries, EXP_INFO_TAG);
    } else if (Class == HZ_SYSTEM_PROCESSOR_INFORMATION_CLASS) {
        HZ_SYSTEM_PROCESSOR_INFORMATION Entries[KI_MAX_PROCESSORS];
        uint32_t Count = 0;
        for (uint32_t n = 0; n < KI_MAX_PROCESSORS; n++) {
            KPRCB *Prcb = KiProcessorBlock[n];
            if (!Prcb || !Prcb->Online)
                continue;
            HZ_SYSTEM_PROCESSOR_INFORMATION *E = &Entries[Count++];
            memset(E, 0, sizeof(*E));
            E->Number = Prcb->Number;
            E->ApicId = Prcb->ApicId;
            E->IdleTimeUs = KeQueryThreadCpuTimeUs(Prcb->IdleThread);
            E->ContextSwitches = Prcb->ContextSwitches;
            E->Interrupts = Prcb->InterruptCount;
            E->TlbShootdowns = Prcb->TlbShootdowns;
        }
        uint32_t Fit = Length / sizeof(Entries[0]);
        Returned = Count * sizeof(Entries[0]);
        Status = MmCopyToUser(Buffer, Entries, HZ_MIN(Fit, Count) * sizeof(Entries[0]));
        if (HZ_SUCCESS(Status) && Fit < Count)
            Status = STATUS_BUFFER_TOO_SMALL;
    } else if (Class == HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS) {
        HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION Entries[HZ_MAX_NETWORK_INTERFACES];
        uint32_t Count = 0;
        for (NET_INTERFACE *Interface = NetGetInterfaceList();
             Interface && Count < HZ_MAX_NETWORK_INTERFACES; Interface = Interface->Next) {
            HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION *Entry = &Entries[Count++];
            memset(Entry, 0, sizeof(*Entry));
            RtlStringCopy(Entry->Name, sizeof(Entry->Name), Interface->Name);
            memcpy(Entry->MacAddress, Interface->MacAddress, sizeof(Entry->MacAddress));
            Entry->LinkUp = Interface->QueryLink(Interface) ? 1 : 0;
            Entry->Mtu = Interface->Mtu;
        }
        uint32_t Fit = Length / sizeof(Entries[0]);
        Returned = Count * sizeof(Entries[0]);
        Status = MmCopyToUser(Buffer, Entries, HZ_MIN(Fit, Count) * sizeof(Entries[0]));
        if (HZ_SUCCESS(Status) && Fit < Count)
            Status = STATUS_BUFFER_TOO_SMALL;
    } else {
        return STATUS_INVALID_INFO_CLASS;
    }

    if (Args[3]) {
        STATUS CopyStatus = MmCopyToUser((void *)Args[3], &Returned, sizeof(Returned));
        if (!HZ_SUCCESS(CopyStatus))
            return CopyStatus;
    }
    return Status;
}
