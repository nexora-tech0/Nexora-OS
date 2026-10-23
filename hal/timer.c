/*
 * Nexora OS - hal/timer.c
 * Timers. A reference timer with a known frequency is chosen in order of preference:
 *   HPET (if ACPI reports one; QEMU q35 has it, VirtualBox only with --x86-hpet on)
 *   ACPI PM timer (3.579545 MHz, present on both reference VMs)
 *   PIT channel 2 (1.193182 MHz, gated through port 0x61)
 * Under a hypervisor that publishes CPUID 0x40000010, the TSC and LAPIC frequencies are taken
 * from it. Otherwise the TSC and the Local APIC timer (divide-by-16) are measured over 10 ms
 * reference windows until three consecutive windows agree within 0.5 % (virtual clocks run fast
 * for a while after boot while they catch up). The TSC then serves stalls and timestamps, and the
 * LAPIC timer drives the periodic clock interrupt.
 */
#include "acpi.h"
#include "halp.h"
#include "rtl.h"

#define PM_TIMER_HZ 3579545ull
#define PIT_HZ 1193182ull
#define HPET_GENERAL_CAPS 0x000
#define HPET_GENERAL_CONFIG 0x010
#define HPET_MAIN_COUNTER 0x0F0
#define HPET_CONFIG_ENABLE 1ull
#define HPET_CAPS_64BIT (1ull << 13)
#define CALIBRATION_US 10000

#define PIT_TEST_VECTOR 0x40 /* IRQL 4 */

static HAL_REFERENCE_TIMER HalpReference;
static volatile uint8_t *HalpHpet;
static uint64_t HalpHpetHz;
static bool HalpHpet64;
static uint64_t HalpTscHz;
static uint64_t HalpLapicHz;

/* ---- Reference timer waits ------------------------------------------------------------------ */

static void HalpHpetWait(uint64_t Microseconds)
{
    uint64_t Ticks = Microseconds * HalpHpetHz / 1000000u;
    uint64_t Start = HalReadRegister64(HalpHpet + HPET_MAIN_COUNTER);
    for (;;) {
        uint64_t Now = HalReadRegister64(HalpHpet + HPET_MAIN_COUNTER);
        uint64_t Elapsed = HalpHpet64 ? Now - Start : (uint32_t)((uint32_t)Now - (uint32_t)Start);
        if (Elapsed >= Ticks)
            break;
        HalPause();
    }
}

static void HalpPmTimerWait(uint64_t Microseconds)
{
    uint64_t Ticks = Microseconds * PM_TIMER_HZ / 1000000u;
    uint32_t Mask = HalAcpi.PmTimer32Bit ? 0xFFFFFFFFu : 0x00FFFFFFu;
    uint32_t Last = HalReadPort32(HalAcpi.PmTimerPort) & Mask;
    uint64_t Elapsed = 0;
    while (Elapsed < Ticks) {
        uint32_t Now = HalReadPort32(HalAcpi.PmTimerPort) & Mask;
        Elapsed += (Now - Last) & Mask;
        Last = Now;
    }
}

static void HalpPitWait(uint64_t Microseconds)
{
    while (Microseconds > 0) {
        uint64_t Chunk = HZ_MIN(Microseconds, 50000u); /* 16-bit counter: max ~54.9 ms */
        uint16_t Count = (uint16_t)(Chunk * PIT_HZ / 1000000u);
        uint8_t Gate = HalReadPort8(0x61);
        HalWritePort8(0x61, (uint8_t)((Gate & ~0x02) | 0x01)); /* speaker off, gate on */
        HalWritePort8(0x43, 0xB0);                            /* ch2, lo/hi byte, mode 0 */
        HalWritePort8(0x42, (uint8_t)Count);
        HalWritePort8(0x42, (uint8_t)(Count >> 8));
        Gate = (uint8_t)(HalReadPort8(0x61) & ~0x01);         /* restart the count */
        HalWritePort8(0x61, Gate);
        HalWritePort8(0x61, (uint8_t)(Gate | 0x01));
        while (!(HalReadPort8(0x61) & 0x20))                  /* OUT2 high at terminal count */
            HalPause();
        Microseconds -= Chunk;
    }
}

static void HalpReferenceWait(uint64_t Microseconds)
{
    switch (HalpReference) {
    case HalReferenceHpet: HalpHpetWait(Microseconds); break;
    case HalReferencePmTimer: HalpPmTimerWait(Microseconds); break;
    case HalReferencePit: HalpPitWait(Microseconds); break;
    default: break;
    }
}

const char *HalGetReferenceTimerName(void)
{
    switch (HalpReference) {
    case HalReferenceHpet: return "HPET";
    case HalReferencePmTimer: return "ACPI PM timer";
    case HalReferencePit: return "PIT";
    default: return "none";
    }
}

static void HalpSelectReferenceTimer(void)
{
    if (HalAcpi.HpetAddress) {
        HalpHpet = (volatile uint8_t *)HalMapIoSpace(HalAcpi.HpetAddress, 0x400);
        uint64_t Caps = HalReadRegister64(HalpHpet + HPET_GENERAL_CAPS);
        uint64_t PeriodFs = Caps >> 32; /* femtoseconds per tick */
        if (PeriodFs > 0 && PeriodFs <= 100000000ull) {
            HalpHpetHz = 1000000000000000ull / PeriodFs;
            HalpHpet64 = (Caps & HPET_CAPS_64BIT) != 0;
            uint64_t Config = HalReadRegister64(HalpHpet + HPET_GENERAL_CONFIG);
            HalWriteRegister64(HalpHpet + HPET_GENERAL_CONFIG, Config | HPET_CONFIG_ENABLE);
            HalpReference = HalReferenceHpet;
            return;
        }
    }
    if (HalAcpi.PmTimerPort) {
        HalpReference = HalReferencePmTimer;
        return;
    }
    HalpReference = HalReferencePit;
}

/* ---- Calibration ------------------------------------------------------------------------------ */

/*
 * Frequencies straight from the hypervisor (CPUID 0x40000010: EAX = TSC kHz, EBX = APIC bus kHz),
 * as provided by VMware, VirtualBox with the "Minimal" paravirt provider, and KVM/QEMU with
 * vmware-cpuid-freq. Exact, and immune to virtual-clock catch-up.
 */
static bool HalpHypervisorFrequencies(uint64_t *TscHz, uint64_t *LapicHz, char Vendor[13])
{
    uint32_t R[4];
    __cpuid(1, 0, R);
    Vendor[0] = '\0';
    if (!(R[2] & (1u << 31)))
        return false; /* bare metal */
    __cpuid(0x40000000u, 0, R);
    memcpy(Vendor, &R[1], 4);
    memcpy(Vendor + 4, &R[2], 4);
    memcpy(Vendor + 8, &R[3], 4);
    Vendor[12] = '\0';
    if (R[0] < 0x40000010u)
        return false;
    __cpuid(0x40000010u, 0, R);
    if (R[0] == 0 || R[1] == 0)
        return false;
    *TscHz = (uint64_t)R[0] * 1000u;
    *LapicHz = (uint64_t)R[1] * 1000u / 16u; /* the LAPIC timer counts the bus clock / 16 */
    return true;
}

static bool HalpWithin(uint64_t a, uint64_t b, uint64_t PerMille)
{
    uint64_t Lo = HZ_MIN(a, b), Hi = HZ_MAX(a, b);
    return (Hi - Lo) * 1000u <= Lo * PerMille;
}

/*
 * Measures TSC and LAPIC ticks over 10 ms reference windows until three consecutive windows
 * agree within 0.5 %, then averages those three. Hypervisors let their virtual clock (PM timer,
 * HPET, LAPIC) run fast for a while after boot to catch up with real time; the first windows
 * are then far off (seen in VirtualBox: a "10 ms" window lasting 0.2 ms of TSC time), so a
 * fixed small sample set is not enough.
 */
static int HalpCalibrateAgainstReference(uint64_t *TscHz, uint64_t *LapicHz, uint64_t TscBudget, bool *Settled)
{
    enum { MaxWindows = 25 };
    uint64_t Tsc[MaxWindows], Lapic[MaxWindows];
    int Used = 0;
    bool Stable = false;
    uint64_t Begin = HalReadTsc();

    bool Enabled = HalAreInterruptsEnabled();
    HalDisableInterrupts();
    /* TscBudget (TSC ticks, 0 = none) bounds the whole calibration: early in a VirtualBox boot the
       reference timer can crawl while the virtual clock catches up (a check once took 55 s). */
    while (Used < MaxWindows && !Stable && (!TscBudget || HalReadTsc() - Begin < TscBudget)) {
        HalpLapicWrite(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIVIDE_16);
        HalpLapicWrite(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
        HalpLapicWrite(LAPIC_TIMER_INITIAL, 0xFFFFFFFFu);
        uint64_t Tsc0 = HalReadTsc();
        HalpReferenceWait(CALIBRATION_US);
        uint64_t Tsc1 = HalReadTsc();
        uint32_t Remaining = HalpLapicRead(LAPIC_TIMER_CURRENT);
        HalpLapicWrite(LAPIC_TIMER_INITIAL, 0);
        Tsc[Used] = Tsc1 - Tsc0;
        Lapic[Used] = 0xFFFFFFFFu - Remaining;
        Used++;
        if (Used >= 3) {
            int a = Used - 3, b = Used - 2, c = Used - 1;
            Stable = HalpWithin(Tsc[a], Tsc[b], 5) && HalpWithin(Tsc[b], Tsc[c], 5) && HalpWithin(Tsc[a], Tsc[c], 5) &&
                     HalpWithin(Lapic[a], Lapic[b], 5) && HalpWithin(Lapic[b], Lapic[c], 5) &&
                     HalpWithin(Lapic[a], Lapic[c], 5);
        }
    }
    if (Enabled)
        HalEnableInterrupts();

    for (int i = 0; i < Used; i++)
        DbgPrintEx(DPFLTR_TRACE_LEVEL, "Timers: calibration window %d: %llu TSC ticks, %llu LAPIC ticks per %u us\n", i,
                   (unsigned long long)Tsc[i], (unsigned long long)Lapic[i], CALIBRATION_US);

    if (Settled)
        *Settled = Stable;
    if (Used == 0) {
        *TscHz = *LapicHz = 0;
        return 0;
    }
    int First = HZ_MAX(0, Used - 3);
    uint64_t TscSum = 0, LapicSum = 0;
    for (int i = First; i < Used; i++) {
        TscSum += Tsc[i];
        LapicSum += Lapic[i];
    }
    int Count = Used - First;
    *TscHz = TscSum / (uint64_t)Count * (1000000u / CALIBRATION_US);
    *LapicHz = LapicSum / (uint64_t)Count * (1000000u / CALIBRATION_US);
    if (!Stable && !TscBudget)
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Timers: calibration did not settle within %d windows\n", MaxWindows);
    return Used;
}

void HalCalibrateTimers(void)
{
    HalpSelectReferenceTimer();

    char Vendor[13];
    char Method[64];
    uint64_t TscHz = 0, LapicHz = 0;
    if (HalpHypervisorFrequencies(&TscHz, &LapicHz, Vendor)) {
        /* Trust the leaf only after checking it (M11): VirtualBox's Hyper-V backend with several
           processors reported 4391.660 MHz while the TSC ran at about 2842 MHz, so every TSC-based
           time was a third slow. A disagreement of more than 2 % takes the calibrated values. */
        uint64_t CalTsc = 0, CalLapic = 0;
        bool Settled = false;
        int Windows = HalpCalibrateAgainstReference(&CalTsc, &CalLapic, TscHz / 2, &Settled); /* 0.5 s at most */
        bool TscAgrees = HalpWithin(TscHz, CalTsc, 20), LapicAgrees = HalpWithin(LapicHz, CalLapic, 20);
        if (!Settled) {
            RtlFormatString(Method, sizeof(Method), "hypervisor timing leaf%s%s%s (the check did not settle)",
                            Vendor[0] ? " (" : "", Vendor, Vendor[0] ? ")" : "");
        } else if (TscAgrees && LapicAgrees) {
            RtlFormatString(Method, sizeof(Method), "hypervisor timing leaf%s%s%s, checked", Vendor[0] ? " (" : "", Vendor,
                            Vendor[0] ? ")" : "");
        } else {
            DbgPrintEx(DPFLTR_WARNING_LEVEL,
                       "Timers: the hypervisor's timing leaf says TSC %llu kHz, LAPIC %llu kHz, but the %s measures "
                       "%llu kHz and %llu kHz; using the measured %s\n",
                       (unsigned long long)(TscHz / 1000), (unsigned long long)(LapicHz / 1000), HalGetReferenceTimerName(),
                       (unsigned long long)(CalTsc / 1000), (unsigned long long)(CalLapic / 1000),
                       TscAgrees ? "LAPIC rate" : (LapicAgrees ? "TSC rate" : "rates"));
            if (!TscAgrees)
                TscHz = CalTsc;
            if (!LapicAgrees)
                LapicHz = CalLapic;
            RtlFormatString(Method, sizeof(Method), "%s, %d windows (the hypervisor leaf was off)",
                            HalGetReferenceTimerName(), Windows);
        }
    } else {
        int Windows = HalpCalibrateAgainstReference(&TscHz, &LapicHz, 0, NULL);
        RtlFormatString(Method, sizeof(Method), "%s, %d windows%s%s%s", HalGetReferenceTimerName(), Windows,
                        Vendor[0] ? ", hypervisor \"" : "", Vendor, Vendor[0] ? "\"" : "");
    }

    if (TscHz < 50000000ull || LapicHz < 100000ull)
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, HAL_FAILURE_TIMER_CALIBRATION, TscHz, LapicHz, HalpReference);
    HalpTscHz = TscHz;
    HalpLapicHz = LapicHz;

    uint32_t Regs[4];
    __cpuid(0x80000000u, 0, Regs);
    bool Invariant = false;
    if (Regs[0] >= 0x80000007u) {
        __cpuid(0x80000007u, 0, Regs);
        Invariant = (Regs[3] & (1u << 8)) != 0;
    }
    DbgPrint("Timers: TSC %llu.%03llu MHz%s, LAPIC timer %llu.%03llu MHz (divide 16), from %s\n",
             (unsigned long long)(HalpTscHz / 1000000), (unsigned long long)(HalpTscHz / 1000 % 1000),
             Invariant ? " (invariant)" : "", (unsigned long long)(HalpLapicHz / 1000000),
             (unsigned long long)(HalpLapicHz / 1000 % 1000), Method);
}

uint64_t HalQueryTscFrequency(void)
{
    return HalpTscHz;
}

uint64_t HalQueryLapicTimerFrequency(void)
{
    return HalpLapicHz;
}

uint64_t HalQueryPerformanceCounter(void)
{
    return HalReadTsc();
}

void HalReferenceStall(uint32_t Microseconds)
{
    HalpReferenceWait(Microseconds);
}

void HalTouchLocalApic(void)
{
    (void)HalpLapicRead(LAPIC_TIMER_CURRENT);
}

/* ---- Reference time (M11) ------------------------------------------------------------------ */

/* The reference timer's count extended to 64 bits, shared by every processor. A reading that is
   behind the stored count (another processor read later and stored first) returns the stored
   count, so time never goes backwards; a 24-bit PM timer must be read at least every 2.3 s (the
   clock interrupts do). */
static volatile uint64_t HalpReferenceTicks;

static bool HalpReadReference(uint64_t *Raw, uint64_t *Mask, uint64_t *Hz)
{
    if (HalpReference == HalReferenceHpet) {
        *Mask = HalpHpet64 ? ~0ull : 0xFFFFFFFFull;
        *Raw = HalReadRegister64(HalpHpet + HPET_MAIN_COUNTER) & *Mask;
        *Hz = HalpHpetHz;
        return true;
    }
    if (HalpReference == HalReferencePmTimer) {
        *Mask = HalAcpi.PmTimer32Bit ? 0xFFFFFFFFull : 0x00FFFFFFull;
        *Raw = HalReadPort32(HalAcpi.PmTimerPort) & *Mask;
        *Hz = PM_TIMER_HZ;
        return true;
    }
    return false;
}

bool HalStartReferenceTime(void)
{
    uint64_t Raw, Mask, Hz;
    if (!HalpReadReference(&Raw, &Mask, &Hz))
        return false;
    __atomic_store_n(&HalpReferenceTicks, Raw, __ATOMIC_RELEASE);
    return true;
}

uint64_t HalReadReferenceTimeUs(void)
{
    uint64_t Raw, Mask, Hz;
    if (!HalpReadReference(&Raw, &Mask, &Hz) || Hz == 0)
        return 0;
    uint64_t Last = __atomic_load_n(&HalpReferenceTicks, __ATOMIC_ACQUIRE);
    for (;;) {
        uint64_t Delta = (Raw - Last) & Mask;
        if (Delta > Mask / 2)
            break; /* another processor already stored a later reading */
        if (__atomic_compare_exchange_n(&HalpReferenceTicks, &Last, Last + Delta, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            Last += Delta;
            break;
        }
    }
    return (Last / Hz) * 1000000u + (Last % Hz) * 1000000u / Hz;
}

void HalStallExecution(uint32_t Microseconds)
{
    if (HalpTscHz == 0) {
        HalpReferenceWait(Microseconds);
        return;
    }
    uint64_t Target = HalReadTsc() + (uint64_t)Microseconds * (HalpTscHz / 1000000u);
    while (HalReadTsc() < Target)
        HalPause();
}

void HalStartClockInterrupt(uint32_t RateHz)
{
    uint32_t Initial = (uint32_t)(HalpLapicHz / RateHz);
    HalpLapicWrite(LAPIC_TIMER_DIVIDE, LAPIC_TIMER_DIVIDE_16);
    HalpLapicWrite(LAPIC_LVT_TIMER, CLOCK_VECTOR | LAPIC_LVT_PERIODIC);
    HalpLapicWrite(LAPIC_TIMER_INITIAL, Initial);
}

/* ---- I/O APIC self-test ----------------------------------------------------------------------- */

static bool HalpPitTestInterrupt(KTRAP_FRAME *Frame, void *Context)
{
    (*(volatile uint32_t *)Context)++;
    return true;
}

uint32_t HalRunIoApicSelfTest(uint32_t DurationMs, uint32_t RateHz)
{
    volatile uint32_t Count = 0;
    if (!HZ_SUCCESS(KeConnectInterrupt(PIT_TEST_VECTOR, HalpPitTestInterrupt, (void *)&Count)))
        return 0;

    uint16_t Divisor = (uint16_t)(PIT_HZ / RateHz);
    HalWritePort8(0x43, 0x34); /* ch0, lo/hi byte, mode 2 (rate generator) */
    HalWritePort8(0x40, (uint8_t)Divisor);
    HalWritePort8(0x40, (uint8_t)(Divisor >> 8));
    HalEnableIsaInterrupt(0, PIT_TEST_VECTOR);

    /* Wait with hlt, not a busy loop: hypervisors (VirtualBox) service emulated timer devices
       such as the PIT on VM exits, and a tight rdtsc/pause spin causes none. The running
       clock interrupt guarantees a wake-up at least every millisecond. */
    uint64_t Deadline = HalReadTsc() + (uint64_t)DurationMs * (HalpTscHz / 1000u);
    while (HalReadTsc() < Deadline)
        HalHalt();

    HalDisableIsaInterrupt(0);
    HalWritePort8(0x43, 0x30); /* ch0, mode 0, no count loaded: output stays low */
    KeDisconnectInterrupt(PIT_TEST_VECTOR);
    return Count;
}
