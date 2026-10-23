/*
 * Nexora OS - hal/apic.c
 * Local APIC (xAPIC through MMIO, or x2APIC through MSRs if firmware enabled it) and I/O APICs.
 *
 * Local APIC setup: software-enabled with spurious vector 0xFF, error LVT on vector 0xFE,
 * LINT0/LINT1 configured from the MADT NMI entries (everything else masked), timer masked until
 * the clock starts. I/O APIC setup: every redirection entry masked; drivers unmask inputs via
 * HalEnableSystemInterrupt / HalEnableIsaInterrupt (fixed delivery, physical destination BSP).
 */
#include "acpi.h"
#include "halp.h"
#include "rtl.h"

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VERSION 0x01
#define IOAPIC_REDIRECTION(n) (0x10 + 2 * (n))
#define IOAPIC_MASKED (1u << 16)
#define IOAPIC_ACTIVE_LOW (1u << 13)
#define IOAPIC_LEVEL (1u << 15)

static volatile uint8_t *HalpLapicBase;
static bool HalpX2Apic;
static uint32_t HalpBspApicId;

static struct {
    volatile uint8_t *Base;
    uint32_t GsiBase;
    uint32_t InputCount;
} HalpIoApics[HAL_MAX_IOAPICS];
static uint32_t HalpIoApicCount;
static KSPIN_LOCK HalpIoApicLock;

/* ---- Local APIC -------------------------------------------------------------------------- */

uint32_t HalpLapicRead(uint32_t Register)
{
    if (HalpX2Apic)
        return (uint32_t)__readmsr(MSR_X2APIC_BASE + (Register >> 4));
    return HalReadRegister32(HalpLapicBase + Register);
}

void HalpLapicWrite(uint32_t Register, uint32_t Value)
{
    if (HalpX2Apic)
        __writemsr(MSR_X2APIC_BASE + (Register >> 4), Value);
    else
        HalWriteRegister32(HalpLapicBase + Register, Value);
}

void HalEndOfInterrupt(void)
{
    HalpLapicWrite(LAPIC_EOI, 0);
}

void HalRequestDispatchInterrupt(void)
{
    if (HalpX2Apic) {
        __writemsr(MSR_X2APIC_SELF_IPI, DISPATCH_VECTOR);
        return;
    }
    uint64_t Flags = __readrflags();
    HalDisableInterrupts();
    while (HalpLapicRead(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVERY_PENDING)
        HalPause();
    HalpLapicWrite(LAPIC_ICR_LOW, DISPATCH_VECTOR | LAPIC_ICR_DEST_SELF);
    if (Flags & RFLAGS_IF)
        HalEnableInterrupts();
}

uint32_t HalGetLocalApicId(void)
{
    uint32_t Id = HalpLapicRead(LAPIC_ID);
    return HalpX2Apic ? Id : Id >> 24;
}

bool HalIsX2ApicMode(void)
{
    return HalpX2Apic;
}

static uint32_t HalpLvtFlagsFromMps(uint16_t MpsFlags)
{
    uint32_t Flags = 0;
    if ((MpsFlags & 3) == 3)
        Flags |= LAPIC_LVT_ACTIVE_LOW;
    if (((MpsFlags >> 2) & 3) == 3)
        Flags |= LAPIC_LVT_LEVEL;
    return Flags;
}

static bool HalpApicErrorInterrupt(KTRAP_FRAME *Frame, void *Context)
{
    if (!HalpX2Apic)
        HalpLapicWrite(LAPIC_ESR, 0); /* xAPIC: write to latch the current errors */
    uint32_t Errors = HalpLapicRead(LAPIC_ESR);
    DbgPrintEx(DPFLTR_WARNING_LEVEL, "Local APIC error status 0x%x\n", Errors);
    return true;
}

/* Registers every processor programs the same way (the boot processor and, since M11, each AP). */
static void HalpConfigureLocalApic(void)
{
    HalpLapicWrite(LAPIC_TPR, 0); /* IRQL is CR8; keep the memory-mapped TPR neutral */
    HalpLapicWrite(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    HalpLapicWrite(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    HalpLapicWrite(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);

    for (uint32_t i = 0; i < HalAcpi.LapicNmiCount; i++) {
        const HAL_LAPIC_NMI *N = &HalAcpi.LapicNmis[i];
        uint32_t Lvt = LAPIC_LVT_NMI | HalpLvtFlagsFromMps(N->Flags);
        Lvt &= ~LAPIC_LVT_LEVEL; /* NMI delivery is always edge */
        if (N->Lint <= 1)
            HalpLapicWrite(N->Lint == 0 ? LAPIC_LVT_LINT0 : LAPIC_LVT_LINT1, Lvt);
    }

    HalpLapicWrite(LAPIC_LVT_ERROR, APIC_ERROR_VECTOR);
    if (!HalpX2Apic)
        HalpLapicWrite(LAPIC_ESR, 0);
    HalpLapicWrite(LAPIC_ESR, 0);

    HalpLapicWrite(LAPIC_SVR, LAPIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    HalEndOfInterrupt(); /* drop anything left in service by firmware */
}

static void HalpInitializeLocalApic(void)
{
    uint64_t ApicBase = __readmsr(MSR_IA32_APIC_BASE);
    HalpX2Apic = (ApicBase & APIC_BASE_X2APIC) != 0;
    if (!(ApicBase & APIC_BASE_ENABLE))
        __writemsr(MSR_IA32_APIC_BASE, ApicBase | APIC_BASE_ENABLE);

    if (!HalpX2Apic) {
        uint64_t Physical = HalAcpi.LocalApicAddress ? HalAcpi.LocalApicAddress
                                                     : (ApicBase & APIC_BASE_ADDRESS_MASK);
        HalpLapicBase = (volatile uint8_t *)HalMapIoSpace(Physical, 0x1000);
    }

    HalpBspApicId = HalGetLocalApicId();
    KeGetCurrentPrcb()->ApicId = HalpBspApicId;
    KeConnectInterrupt(APIC_ERROR_VECTOR, HalpApicErrorInterrupt, NULL);
    HalpConfigureLocalApic();
}

void HalpInitializeLocalApicForProcessor(void)
{
    /* Same mode as the boot processor (the MMIO window is the same physical page on every CPU). */
    uint64_t ApicBase = __readmsr(MSR_IA32_APIC_BASE);
    uint64_t Wanted = ApicBase | APIC_BASE_ENABLE | (HalpX2Apic ? APIC_BASE_X2APIC : 0);
    if (Wanted != ApicBase)
        __writemsr(MSR_IA32_APIC_BASE, Wanted);
    KeGetCurrentPrcb()->ApicId = HalGetLocalApicId();
    HalpConfigureLocalApic();
}

/* ---- Interprocessor interrupts (M11) ---------------------------------------------------------- */

static void HalpWriteIcr(uint32_t ApicId, uint32_t Low)
{
    uint64_t Flags = __readrflags();
    HalDisableInterrupts();
    if (HalpX2Apic) {
        __writemsr(MSR_X2APIC_ICR, ((uint64_t)ApicId << 32) | Low);
    } else {
        while (HalpLapicRead(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVERY_PENDING)
            HalPause();
        HalpLapicWrite(LAPIC_ICR_HIGH, ApicId << 24);
        HalpLapicWrite(LAPIC_ICR_LOW, Low);
    }
    if (Flags & RFLAGS_IF)
        HalEnableInterrupts();
}

void HalSendIpi(uint32_t ApicId, uint8_t Vector)
{
    HalpWriteIcr(ApicId, Vector | LAPIC_ICR_ASSERT);
}

void HalFreezeOtherProcessors(void)
{
    if (HalpX2Apic || HalpLapicBase)
        HalpWriteIcr(0, LAPIC_ICR_NMI | LAPIC_ICR_ASSERT | LAPIC_ICR_DEST_ALL_BUT_SELF);
}

/* ---- I/O APIC ---------------------------------------------------------------------------- */

static uint32_t HalpIoApicRead(volatile uint8_t *Base, uint32_t Register)
{
    HalWriteRegister32(Base + IOAPIC_REGSEL, Register);
    return HalReadRegister32(Base + IOAPIC_WINDOW);
}

static void HalpIoApicWrite(volatile uint8_t *Base, uint32_t Register, uint32_t Value)
{
    HalWriteRegister32(Base + IOAPIC_REGSEL, Register);
    HalWriteRegister32(Base + IOAPIC_WINDOW, Value);
}

static void HalpInitializeIoApics(void)
{
    for (uint32_t i = 0; i < HalAcpi.IoApicCount && i < HAL_MAX_IOAPICS; i++) {
        volatile uint8_t *Base = (volatile uint8_t *)HalMapIoSpace(HalAcpi.IoApics[i].PhysicalAddress, 0x20);
        uint32_t Version = HalpIoApicRead(Base, IOAPIC_REG_VERSION);
        uint32_t Inputs = ((Version >> 16) & 0xFF) + 1;
        for (uint32_t n = 0; n < Inputs; n++) {
            HalpIoApicWrite(Base, IOAPIC_REDIRECTION(n) + 1, 0);
            HalpIoApicWrite(Base, IOAPIC_REDIRECTION(n), IOAPIC_MASKED);
        }
        HalpIoApics[HalpIoApicCount].Base = Base;
        HalpIoApics[HalpIoApicCount].GsiBase = HalAcpi.IoApics[i].GsiBase;
        HalpIoApics[HalpIoApicCount].InputCount = Inputs;
        HalpIoApicCount++;
        DbgPrint("APIC: I/O APIC %u version 0x%02x, GSIs %u-%u (all masked)\n", HalAcpi.IoApics[i].Id,
                 Version & 0xFF, HalAcpi.IoApics[i].GsiBase, HalAcpi.IoApics[i].GsiBase + Inputs - 1);
    }
}

static bool HalpFindIoApicInput(uint32_t Gsi, volatile uint8_t **Base, uint32_t *Input)
{
    for (uint32_t i = 0; i < HalpIoApicCount; i++) {
        if (Gsi >= HalpIoApics[i].GsiBase && Gsi < HalpIoApics[i].GsiBase + HalpIoApics[i].InputCount) {
            *Base = HalpIoApics[i].Base;
            *Input = Gsi - HalpIoApics[i].GsiBase;
            return true;
        }
    }
    return false;
}

STATUS HalEnableSystemInterrupt(uint32_t Gsi, uint8_t Vector, bool ActiveLow, bool LevelTriggered)
{
    volatile uint8_t *Base;
    uint32_t Input;
    if (Vector < KI_FIRST_DEVICE_VECTOR || !HalpFindIoApicInput(Gsi, &Base, &Input))
        return STATUS_INVALID_PARAMETER;
    uint32_t Low = Vector | (ActiveLow ? IOAPIC_ACTIVE_LOW : 0) | (LevelTriggered ? IOAPIC_LEVEL : 0);
    uint32_t High = HalpX2Apic ? HalpBspApicId : (HalpBspApicId << 24);

    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&HalpIoApicLock, HIGH_LEVEL, &OldIrql);
    HalpIoApicWrite(Base, IOAPIC_REDIRECTION(Input) + 1, High);
    HalpIoApicWrite(Base, IOAPIC_REDIRECTION(Input), Low);
    KeReleaseSpinLock(&HalpIoApicLock, OldIrql);
    return STATUS_SUCCESS;
}

void HalDisableSystemInterrupt(uint32_t Gsi)
{
    volatile uint8_t *Base;
    uint32_t Input;
    if (!HalpFindIoApicInput(Gsi, &Base, &Input))
        return;
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&HalpIoApicLock, HIGH_LEVEL, &OldIrql);
    uint32_t Low = HalpIoApicRead(Base, IOAPIC_REDIRECTION(Input));
    HalpIoApicWrite(Base, IOAPIC_REDIRECTION(Input), Low | IOAPIC_MASKED);
    KeReleaseSpinLock(&HalpIoApicLock, OldIrql);
}

uint32_t HalTranslateIsaIrq(uint8_t IsaIrq, bool *ActiveLow, bool *LevelTriggered)
{
    uint32_t Gsi = IsaIrq;
    bool Low = false, Level = false; /* ISA default: active high, edge */
    for (uint32_t i = 0; i < HalAcpi.OverrideCount; i++) {
        const HAL_ISA_OVERRIDE *O = &HalAcpi.Overrides[i];
        if (O->IsaIrq != IsaIrq)
            continue;
        Gsi = O->Gsi;
        if ((O->Flags & 3) == 3)
            Low = true;
        if (((O->Flags >> 2) & 3) == 3)
            Level = true;
        break;
    }
    if (ActiveLow)
        *ActiveLow = Low;
    if (LevelTriggered)
        *LevelTriggered = Level;
    return Gsi;
}

STATUS HalEnableIsaInterrupt(uint8_t IsaIrq, uint8_t Vector)
{
    bool Low, Level;
    uint32_t Gsi = HalTranslateIsaIrq(IsaIrq, &Low, &Level);
    return HalEnableSystemInterrupt(Gsi, Vector, Low, Level);
}

void HalDisableIsaInterrupt(uint8_t IsaIrq)
{
    HalDisableSystemInterrupt(HalTranslateIsaIrq(IsaIrq, NULL, NULL));
}

/* ---- Bring-up ------------------------------------------------------------------------------- */

void HalInitializeInterruptControllers(void)
{
    if (HalAcpi.Has8259)
        HalpMaskLegacyPic();
    HalpInitializeLocalApic();
    DbgPrint("APIC: Local APIC id %u, version 0x%02x, %s mode\n", HalpBspApicId,
             HalpLapicRead(LAPIC_VERSION) & 0xFF, HalpX2Apic ? "x2APIC" : "xAPIC");
    HalpInitializeIoApics();
}
