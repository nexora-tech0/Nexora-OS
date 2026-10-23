/*
 * Nexora OS - init/init.c
 * System initialization (entered from KiSystemStartup in entry.asm).
 *
 * Phase 0, on the boot stack, interrupts off:
 *   COM1 -> C++ constructors -> loader block -> processor tables (GDT/TSS/IDT/PRCB and the
 *   SYSCALL MSRs, so every later fault gets a stop screen) -> boot display -> kernel symbols ->
 *   memory manager (own page tables, PFN database, pool, SMEP/SMAP) -> object manager ->
 *   process structure (System process) -> I/O and executive object types, system-service
 *   table -> ACPI -> PIC/APICs -> timer calibration -> system time (CMOS) -> FPU/SSE/AVX state
 *   for user mode -> scheduler (the boot context becomes the idle thread) -> clock -> create
 *   Phase1Init -> interrupts on.
 * Phase 1, in the Phase1Init system thread (priority 8):
 *   worker thread -> application processors (M11; parked by the memory manager in phase 0) ->
 *   self-tests (traps, clock, I/O APIC, memory, scheduler) -> built-in drivers
 *   (PCI, file systems, disk class, AHCI, PS/2, ramdisk), mounting and drive letters -> language
 *   checks -> diagnostics -> user-mode self-tests (processes, PE loader, system calls, faults) ->
 *   file system self-tests -> start the zero-page and status threads -> power diagnostics.
 * Diagnostics options are handled by init/diag.c.
 */
#include "bootinfo.h"
#include "ex.h"
#include "hal.h"
#include "hzversion.h"
#include "inbv.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "netif.h"
#include "ob.h"
#include "ps.h"
#include "rtl.h"
#include "w32k.h"

#define KI_CLOCK_RATE_HZ 1000
#define KI_HEARTBEAT_SECONDS 3

HZ_NORETURN void KiInitializeSystem(void);
void KiRunCommandLineDiagnostics(void);

static const char *const KiWeekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static void KiReportLoaderBlock(void)
{
    const LOADER_BLOCK *Lb = &KeLoaderBlock;
    char Size[32];

    DbgPrint("Boot: %s %s, %s firmware, protocol base revision %llu\n", Lb->LoaderName,
             Lb->LoaderVersion, Lb->FirmwareIsUefi ? "UEFI" : "non-UEFI",
             (unsigned long long)Lb->BaseRevision);
    if (Lb->CommandLine[0])
        DbgPrint("Boot: command line \"%s\"\n", Lb->CommandLine);
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "Kernel: virtual 0x%llx, physical 0x%llx, HHDM offset 0x%llx\n",
               (unsigned long long)Lb->KernelVirtualBase, (unsigned long long)Lb->KernelPhysicalBase,
               (unsigned long long)Lb->HhdmOffset);

    if (Lb->HasFramebuffer) {
        const FRAMEBUFFER_INFO *Fb = &Lb->Framebuffer;
        DbgPrint("Display: %ux%u, %u bpp, pitch %u\n", Fb->Width, Fb->Height, Fb->Bpp, Fb->Pitch);
    } else {
        DbgPrint("Display: no usable 32-bpp linear framebuffer (serial only)\n");
    }

    RtlFormatByteSize(Size, sizeof(Size), Lb->UsableMemoryBytes);
    DbgPrint("Memory: %s usable in %u map entries, highest RAM address 0x%llx\n", Size,
             Lb->MemoryDescriptorCount, (unsigned long long)Lb->HighestPhysicalAddress);
    for (uint32_t i = 0; i < Lb->MemoryDescriptorCount; i++) {
        const MEMORY_DESCRIPTOR *D = &Lb->MemoryDescriptors[i];
        RtlFormatByteSize(Size, sizeof(Size), D->Length);
        DbgPrintEx(DPFLTR_TRACE_LEVEL, "  [%2u] 0x%016llx-0x%016llx %-18s %s\n", i, (unsigned long long)D->Base,
                   (unsigned long long)(D->Base + D->Length), BiMemoryTypeName(D->Type), Size);
    }

    if (Lb->HasBootTime) {
        TIME_FIELDS T;
        RtlUnixTimeToTimeFields(Lb->BootUnixTime, &T);
        DbgPrint("Clock: %s %04u-%02u-%02u %02u:%02u:%02u UTC (firmware RTC)\n", KiWeekdays[T.Weekday],
                 T.Year, T.Month, T.Day, T.Hour, T.Minute, T.Second);
    }
}

static void KiLoadKernelSymbols(void)
{
    const BOOT_MODULE *Module = BiFindModule("symbols");
    if (!Module) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Symbols: module not found; stack traces will be raw addresses\n");
        return;
    }
    if (KeLoadSymbols(Module->Address, Module->Size))
        DbgPrint("Symbols: %u kernel functions loaded from %s\n", KeSymbolCount(), Module->Path);
    else
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Symbols: %s does not match this kernel; ignored\n", Module->Path);
}

/* Wall-clock time: the CMOS clock, or the time the bootloader read at boot. */
static void KiInitializeSystemTime(void)
{
    TIME_FIELDS T;
    const char *Source = "CMOS RTC";
    if (HalQueryRealTimeClock(&T)) {
        KeSetSystemTime(RtlTimeFieldsToTime(&T));
    } else if (KeLoaderBlock.HasBootTime) {
        KeSetSystemTime((uint64_t)KeLoaderBlock.BootUnixTime * HZ_TIME_UNITS_PER_SECOND + HZ_TIME_UNIX_EPOCH);
        Source = "bootloader";
    } else {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Clock: no time source; file times start at 1980\n");
        return;
    }
    RtlTimeToTimeFields(KeQuerySystemTime(), &T);
    DbgPrint("Clock: system time %04u-%02u-%02u %02u:%02u:%02u UTC (%s)\n", T.Year, T.Month, T.Day, T.Hour, T.Minute,
             T.Second, Source);
}

static bool KiRunLanguageSelfTests(void)
{
    bool CppOk = W32kCppSelfTest();
    uint32_t RustFailure = RgnSelfTest();
    DbgPrint("Runtime: C++ %s, Rust %s\n", CppOk ? "OK" : "FAILED", RustFailure == 0 ? "OK" : "FAILED");
    if (RustFailure)
        DbgPrintEx(DPFLTR_ERROR_LEVEL, "Rust region self-test failed check %u\n", RustFailure);
    return CppOk && RustFailure == 0;
}

/* int3 must trap, be logged and resume right after the instruction. */
static bool KiBreakpointSelfTest(void)
{
    uint64_t Before = KiBreakpointCount;
    __asm__ volatile("int3");
    return KiBreakpointCount == Before + 1;
}

/*
 * Counts clock interrupts during a 200 ms wait on the reference timer. Under a hypervisor the
 * reference timer and the LAPIC timer share the virtual clock, which may run fast for a while
 * after boot to catch up with real time; comparing within that clock domain checks the LAPIC
 * programming itself (±10 %). The rate relative to the TSC (real time) is logged for information.
 * A hypervisor that deschedules the virtual CPU coalesces the periodic interrupts that fell due
 * meanwhile (QEMU TCG on a busy host lost a third of them in one window), so a window is retried
 * up to three times: a misprogrammed timer is wrong every time, lost ticks are not.
 */
static bool KiClockSelfTest(void)
{
    const uint32_t WindowUs = 200000;
    uint64_t Measured = 0, TscRelative = 0;
    bool Ok = false;
    int Attempt;
    for (Attempt = 1; Attempt <= 3 && !Ok; Attempt++) {
        uint64_t Start = KeQueryTickCount();
        uint64_t Tsc0 = HalReadTsc();
        HalReferenceStall(WindowUs);
        uint64_t Seen = KeQueryTickCount() - Start;
        uint64_t TscElapsed = HalReadTsc() - Tsc0;
        Measured = Seen * 1000000u / WindowUs;
        TscRelative = TscElapsed ? Seen * HalQueryTscFrequency() / TscElapsed : 0;
        Ok = Measured * 100 >= KI_CLOCK_RATE_HZ * 90ull && Measured * 100 <= KI_CLOCK_RATE_HZ * 110ull;
        if (!Ok && Attempt < 3)
            DbgPrintEx(DPFLTR_INFO_LEVEL, "Clock: window %d measured %llu Hz (lost ticks?), measuring again\n",
                       Attempt, (unsigned long long)Measured);
    }
    DbgPrintEx(Ok ? DPFLTR_INFO_LEVEL : DPFLTR_WARNING_LEVEL,
               "Clock: LAPIC timer at %u Hz, measured %llu Hz against the %s (%llu Hz against the TSC)%s\n",
               KI_CLOCK_RATE_HZ, (unsigned long long)Measured, HalGetReferenceTimerName(),
               (unsigned long long)TscRelative, Ok ? "" : " (out of tolerance)");
    /* The clock agrees with the reference timer but not with the TSC: the TSC runs at the wrong
       rate (M11: VirtualBox's Hyper-V backend with several processors), so uptime and timers
       must not come from it. */
    bool TscOk = TscRelative * 100 >= KI_CLOCK_RATE_HZ * 90ull && TscRelative * 100 <= KI_CLOCK_RATE_HZ * 110ull;
    if (Ok && !TscOk && KeUseReferenceClock())
        DbgPrint("Clock: the TSC runs at the wrong rate here; uptime and timers now come from the %s\n",
                 HalGetReferenceTimerName());
    return Ok;
}

static bool KiIoApicSelfTest(void)
{
    const uint32_t DurationMs = 50, RateHz = 1000;
    uint32_t Count = HalRunIoApicSelfTest(DurationMs, RateHz);
    uint32_t Expected = DurationMs * RateHz / 1000;
    bool Low, Level;
    uint32_t Gsi = HalTranslateIsaIrq(0, &Low, &Level);
    bool Ok = Count >= Expected / 2 && Count <= Expected * 2;
    DbgPrintEx(Ok ? DPFLTR_INFO_LEVEL : DPFLTR_WARNING_LEVEL,
               "I/O APIC: ISA IRQ 0 -> GSI %u delivered %u interrupts in %u ms (%s)\n", Gsi, Count, DurationMs,
               Ok ? "OK" : "unexpected");
    return Ok;
}

static void KiApplyLogLevel(void)
{
    if (BiCommandLineHasOption("hz.log=trace"))
        DbgSetLogLevel(DPFLTR_TRACE_LEVEL);
    else if (BiCommandLineHasOption("hz.log=warning"))
        DbgSetLogLevel(DPFLTR_WARNING_LEVEL);
}

/* ---- Phase 1 (runs in the Phase1Init system thread) ----------------------------------------- */

static uint64_t KiTotalContextSwitches(void)
{
    uint64_t Total = 0;
    for (uint32_t n = 0; n < KI_MAX_PROCESSORS; n++)
        if (KiProcessorBlock[n] && KiProcessorBlock[n]->Online)
            Total += KiProcessorBlock[n]->ContextSwitches;
    return Total;
}

static void KiStatusThread(void *Context)
{
    bool HeartbeatSent = false;
    for (;;) {
        uint64_t Us = KeQueryUptimeMicroseconds();
        uint64_t Seconds = Us / 1000000;
        char Free[24], Status[160];
        RtlFormatByteSize(Free, sizeof(Free), MmQueryAvailablePages() << PAGE_SHIFT);
        RtlFormatString(Status, sizeof(Status),
                        "%s complete  |  up %llu:%02llu:%02llu  |  %s free  |  %u CPUs  |  %u threads  |  %llu context switches",
                        HZ_MILESTONE, (unsigned long long)(Seconds / 3600), (unsigned long long)(Seconds / 60 % 60),
                        (unsigned long long)(Seconds % 60), Free, KeNumberProcessors, KeQueryThreadCount(),
                        (unsigned long long)KiTotalContextSwitches());
        InbvSetStatusText(Status);

        if (!HeartbeatSent && Seconds >= KI_HEARTBEAT_SECONDS) {
            HeartbeatSent = true;
            MM_STATISTICS Mm;
            MmQueryStatistics(&Mm);
            extern volatile uint64_t KiClockKicks; /* ke/time.c */
            DbgPrint("[HZ] HEARTBEAT: %u processors, %u threads, %llu context switches, idle thread CPU %llu ms, %llu "
                     "pages pre-zeroed, %llu clock watchdog kicks\n",
                     KeNumberProcessors, KeQueryThreadCount(), (unsigned long long)KiTotalContextSwitches(),
                     (unsigned long long)(KeQueryThreadCpuTimeUs(KeGetCurrentPrcb()->IdleThread) / 1000),
                     (unsigned long long)Mm.ZeroedPages, (unsigned long long)KiClockKicks);
            if (KeNumberProcessors > 1) { /* how well each processor's clock is delivered (M11) */
                char Ticks[96] = "";
                size_t Used = 0;
                for (uint32_t n = 0; n < KI_MAX_PROCESSORS && Used < sizeof(Ticks) - 24; n++) {
                    KPRCB *P = KiProcessorBlock[n];
                    if (P)
                        Used += (size_t)RtlFormatString(Ticks + Used, sizeof(Ticks) - Used, "%s%llu", n ? " " : "",
                                                        (unsigned long long)(P->IdleTicks + P->BusyTicks));
                }
                DbgPrint("Clock: ticks per processor since start: %s (uptime %llu ms)\n", Ticks,
                         (unsigned long long)(KeQueryUptimeMicroseconds() / 1000));
            }
        } else if (Seconds % 30 == 0 && Seconds > 0) {
            DbgPrintEx(DPFLTR_TRACE_LEVEL, "Status: up %llu s, %llu context switches, %llu interrupts\n",
                       (unsigned long long)Seconds, (unsigned long long)KeGetCurrentPrcb()->ContextSwitches,
                       (unsigned long long)KeGetCurrentPrcb()->InterruptCount);
        }
        KeDelayExecutionThread(1000000 - (KeQueryUptimeMicroseconds() % 1000000)); /* next whole second */
    }
}

static void KiBootAnimationThread(void *Context)
{
    (void)Context;
    while (InbvIsDisplayReady()) {
        InbvAnimate();
        KeDelayExecutionThread(50000);
    }
}

/*
 * The session thread (M11): starts the shell and starts it again if it exits (crashed or ended
 * from Task Manager), with "restart" on its command line so it skips the lock screen. More than
 * five exits within a minute stop the restarts (a shell that cannot start would loop).
 */
static void KiSessionThread(void *Context)
{
    uint64_t WindowStart = KeQueryUptimeMicroseconds();
    uint32_t Restarts = 0;
    for (bool First = true;; First = false) {
        EPROCESS *Shell;
        STATUS Status = PsCreateProcess("\\??\\C:\\Nexora\\System\\shell.exe", First ? "shell.exe" : "shell.exe restart",
                                        0, &Shell);
        if (!HZ_SUCCESS(Status)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "Desktop: cannot start the shell (0x%08X)\n", (uint32_t)Status);
            if (First && HZ_SUCCESS(PsCreateProcess("\\??\\C:\\Nexora\\System\\welcome.exe", "welcome.exe", 0, &Shell)))
                ObDereferenceObject(Shell);
            return;
        }
        KeWaitForSingleObject(Shell, NULL);
        STATUS Exit = Shell->ExitStatus;
        ObDereferenceObject(Shell);
        uint64_t Now = KeQueryUptimeMicroseconds();
        if (Now - WindowStart > 60000000ull) {
            WindowStart = Now;
            Restarts = 0;
        }
        if (++Restarts > 5) {
            DbgPrintEx(DPFLTR_ERROR_LEVEL, "Session: the shell exited %u times within a minute; not restarting\n", Restarts);
            return;
        }
        DbgPrint("Session: shell.exe exited (0x%08X); restarting it\n", (uint32_t)Exit);
        KeDelayExecutionThread(500000);
    }
}

static void KiStartSystemThread(KSTART_ROUTINE Routine, KPRIORITY Priority, const char *Name)
{
    if (!HZ_SUCCESS(PsCreateSystemThread(Routine, NULL, Priority, Name, NULL)))
        DbgPrintEx(DPFLTR_ERROR_LEVEL, "Init: could not start the %s thread\n", Name);
}

static void KiPhase1Initialization(void *Context)
{
    bool DisplayOk = Context != NULL;
    DbgPrint("Scheduler: running in thread '%s' (id %u, priority %d)\n", KeGetCurrentThread()->Name,
             KeGetCurrentThread()->ThreadId, KeGetCurrentThread()->Priority);
    ExInitializeWorkerThread();
    KeStartApplicationProcessors();

    bool BreakpointOk = KiBreakpointSelfTest();
    DbgPrintEx(BreakpointOk ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL, "Traps: int3 round trip %s\n",
               BreakpointOk ? "OK" : "FAILED");
    bool ClockOk = KiClockSelfTest();
    bool IoApicOk = KiIoApicSelfTest();
    bool MemoryOk = MmRunSelfTests();
    bool SchedulerOk = KeRunSchedulerSelfTests();

    /* Drivers (PCI, disks, PS/2, ramdisk), file systems and drive letters. */
    IoInitializeDrivers();
    IoReportVolumes();
    bool NetworkOk = NetRunProtocolSelfTests();
    bool DhcpTestsOk = NetDhcpRunSelfTests();
    bool DhcpStartOk = true;
    if (NetworkOk && DhcpTestsOk && NetGetInterfaceList()) {
        STATUS DhcpStatus = NetStart();
        DhcpStartOk = HZ_SUCCESS(DhcpStatus);
        if (!DhcpStartOk)
            DbgPrintEx(DPFLTR_ERROR_LEVEL, "Network: could not start DHCP client (0x%08X)\n",
                       (uint32_t)DhcpStatus);
    }
    W32kInitialize();
    ObDumpNamespace();
    bool LanguagesOk = KiRunLanguageSelfTests();

    KiRunCommandLineDiagnostics();
    bool ProcessesOk = PsRunSelfTests();
    bool FileSystemsOk = IoRunSelfTests();
    bool DesktopOk = W32kRunSelfTests();

    /* Background system threads start after the self-tests so they cannot disturb the counts. */
    KiStartSystemThread(MmZeroPageThread, 1, "ZeroPage");
    KiStartSystemThread(KiStatusThread, KI_THREAD_PRIORITY_DEFAULT + 1, "Status");

    bool AllOk = BreakpointOk && ClockOk && IoApicOk && MemoryOk && SchedulerOk && LanguagesOk && ProcessesOk &&
                 FileSystemsOk && NetworkOk && DhcpTestsOk && DhcpStartOk && DesktopOk;
    if (DisplayOk)
        InbvSetStatusText(AllOk ? HZ_MILESTONE " complete" : HZ_MILESTONE " complete with warnings (see log)");
    DbgPrint("[HZ] BOOT %s: %s kernel finished phase 1 (%llu ms after kernel entry)\n", AllOk ? "OK" : "DEGRADED",
             HZ_MILESTONE, (unsigned long long)(KeQueryUptimeMicroseconds() / 1000));

    /* The desktop takes over the screen and the shell starts (hz.desktop=off keeps the boot
       screen, for diagnostics). Without a shell image, the welcome program runs on its own. */
    if (!BiCommandLineHasOption("hz.desktop=off")) {
        W32kShowDesktop();
        KiStartSystemThread(KiSessionThread, KI_THREAD_PRIORITY_DEFAULT, "Session");
    }

    /* Power diagnostics run last, after a complete boot. */
    if (BiCommandLineHasOption("hz.test=shutdown"))
        ExShutdownSystem(HZ_SHUTDOWN_POWER_OFF);
    if (BiCommandLineHasOption("hz.test=reboot"))
        ExShutdownSystem(HZ_SHUTDOWN_REBOOT);
}

/* ---- Phase 0 (boot stack; becomes the idle thread) ---------------------------------------------- */

HZ_NORETURN static void KiIdleLoop(void)
{
    for (;;)
        HalEnableInterruptsAndHalt(); /* the DISPATCH interrupt switches away when work appears */
}

HZ_NORETURN void KiInitializeSystem(void)
{
    KiBootTsc = HalReadTsc();
    bool SerialOk = HalInitializeSerial();
    HalSerialWriteString("\n");
    DbgPrint("%s %s (%s, build %s) starting\n", HZ_PRODUCT_NAME, HZ_VERSION_STRING, HZ_MILESTONE,
             HZ_BUILD_ID);

    /* Global C++ constructors run before the pool exists and must not allocate. */
    RtlRunStaticInitializers();

    if (!BiCaptureLoaderBlock()) {
        DbgPrint("Boot: bootloader does not support Limine base revision 6\n");
        KeBugCheckEx(BOOT_PROTOCOL_MISMATCH, 6, 0, 0, 0);
    }
    KiApplyLogLevel();

    /* From here on, CPU exceptions reach the stop screen instead of triple-faulting. */
    HalInitializeProcessor();

    bool DisplayOk = InbvInitialize();
    DbgPrint("%s %s is starting up\n", HZ_PRODUCT_NAME, HZ_VERSION_STRING);
    DbgPrint("Serial: COM1 %s\n", SerialOk ? "115200 8N1" : "not present");
    KiLoadKernelSymbols();
    KiReportLoaderBlock();

    /* Own page tables, PFN database and pool; Limine's memory is reclaimed here. */
    MmInitializeSystem();

    /* Executive objects: the namespace, the System process, device/file/event types and the
       system-service table. No threads exist yet. */
    ObInitializeSystem();
    MmInitializeSections();
    PsInitializeSystem();
    IoInitializeSystem();
    ExInitializeSystem();

    HalInitializeAcpi();
    HalInitializeInterruptControllers();
    HalCalibrateTimers();
    KiInitializeSystemTime();
    KeInitializeExtendedState();

    /* The boot context becomes the idle thread; everything else continues in Phase1Init. */
    KeInitializeScheduler(&PsInitialSystemProcess->Pcb);
    KiStartClock(KI_CLOCK_RATE_HZ);
    STATUS Status = PsCreateSystemThread(KiPhase1Initialization, DisplayOk ? (void *)1 : NULL,
                                         KI_THREAD_PRIORITY_DEFAULT, "Phase1Init", NULL);
    if (!HZ_SUCCESS(Status))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_SYSTEM_THREAD, (uint64_t)Status, 0, 0);
    if (DisplayOk)
        KiStartSystemThread(KiBootAnimationThread, KI_THREAD_PRIORITY_DEFAULT, "BootAnimation");

    KeLowerIrql(PASSIVE_LEVEL);
    HalEnableInterrupts(); /* the pending DISPATCH interrupt switches to Phase1Init right here */
    KiIdleLoop();
}
