/*
 * Nexora OS - include/hal.h
 * Hardware Abstraction Layer (Hal*): port I/O, CPU control, processor tables (GDT/TSS/IDT),
 * uncached I/O-space mappings, ACPI table discovery, the Local/I/O APIC, timers and COM1.
 */
#ifndef HAL_H
#define HAL_H

#include "hzbase.h"
#include "x86.h"

HZ_EXTERN_C_BEGIN

/* ---- Port I/O ------------------------------------------------------------------------- */

static inline void HalWritePort8(uint16_t Port, uint8_t Value)
{
    __asm__ volatile("outb %0, %1" : : "a"(Value), "Nd"(Port) : "memory");
}

static inline uint8_t HalReadPort8(uint16_t Port)
{
    uint8_t Value;
    __asm__ volatile("inb %1, %0" : "=a"(Value) : "Nd"(Port) : "memory");
    return Value;
}

static inline void HalWritePort16(uint16_t Port, uint16_t Value)
{
    __asm__ volatile("outw %0, %1" : : "a"(Value), "Nd"(Port) : "memory");
}

static inline uint16_t HalReadPort16(uint16_t Port)
{
    uint16_t Value;
    __asm__ volatile("inw %1, %0" : "=a"(Value) : "Nd"(Port) : "memory");
    return Value;
}

static inline void HalWritePort32(uint16_t Port, uint32_t Value)
{
    __asm__ volatile("outl %0, %1" : : "a"(Value), "Nd"(Port) : "memory");
}

static inline uint32_t HalReadPort32(uint16_t Port)
{
    uint32_t Value;
    __asm__ volatile("inl %1, %0" : "=a"(Value) : "Nd"(Port) : "memory");
    return Value;
}

/* A write to an unused port gives legacy devices ~1 us to settle. */
static inline void HalIoDelay(void)
{
    HalWritePort8(0x80, 0);
}

/* ---- MMIO -------------------------------------------------------------------------------- */

static inline uint32_t HalReadRegister32(volatile void *Address)
{
    return *(volatile uint32_t *)Address;
}

static inline void HalWriteRegister32(volatile void *Address, uint32_t Value)
{
    *(volatile uint32_t *)Address = Value;
}

static inline uint64_t HalReadRegister64(volatile void *Address)
{
    return *(volatile uint64_t *)Address;
}

static inline void HalWriteRegister64(volatile void *Address, uint64_t Value)
{
    *(volatile uint64_t *)Address = Value;
}

/* ---- CPU control ---------------------------------------------------------------------- */

static inline void HalDisableInterrupts(void)
{
    __asm__ volatile("cli" : : : "memory");
}

static inline void HalEnableInterrupts(void)
{
    __asm__ volatile("sti" : : : "memory");
}

static inline bool HalAreInterruptsEnabled(void)
{
    uint64_t Flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(Flags));
    return (Flags & (1ull << 9)) != 0;
}

static inline void HalHalt(void)
{
    __asm__ volatile("hlt" : : : "memory");
}

/* Atomically enables interrupts and halts until the next interrupt. */
static inline void HalEnableInterruptsAndHalt(void)
{
    __asm__ volatile("sti; hlt" : : : "memory");
}

static inline void HalPause(void)
{
    __builtin_ia32_pause();
}

static inline uint64_t HalReadTsc(void)
{
    uint32_t Lo, Hi;
    __asm__ volatile("rdtsc" : "=a"(Lo), "=d"(Hi));
    return ((uint64_t)Hi << 32) | Lo;
}

/* Stops this CPU for good: interrupts off, halt forever. */
HZ_NORETURN void HalHaltSystem(void);

/* ---- Processor tables --------------------------------------------------------------------- */

/* Loads the GDT, TSS (with IST stacks), IDT and the per-processor block (GS base). */
void HalInitializeProcessor(void);
/* Kernel stack used on ring 3 -> ring 0 transitions (this processor's TSS.RSP0). */
void HalSetKernelStack(uint64_t StackTop);

/* ---- Multiprocessor support (M11) ---------------------------------------------------------- */

#define HAL_IST_STACK_SIZE (16 * 1024)
/* Tables of one application processor, allocated by Ke. */
typedef struct _HAL_PROCESSOR_TABLES {
    uint64_t Gdt[KGDT_ENTRY_COUNT] HZ_ALIGNED(16);
    KTSS64 Tss HZ_ALIGNED(16);
    uint8_t IstStacks[3][HAL_IST_STACK_SIZE] HZ_ALIGNED(16);
} HAL_PROCESSOR_TABLES;
/* A parked AP: loads the boot processor's GDT and the IDT (no TSS yet). */
void HalLoadBootProcessorTables(void);
/* An AP coming online: its own GDT and TSS (RSP0 = StackTop), the IDT, GS base = Prcb, SYSCALL MSRs,
   and its local APIC (same configuration as the boot processor's, timer stopped). */
struct _KPRCB;
void HalInitializeApplicationProcessor(struct _KPRCB *Prcb, HAL_PROCESSOR_TABLES *Tables, uint64_t StackTop);
/* Fixed IPI with Vector to the processor with local APIC id ApicId. */
void HalSendIpi(uint32_t ApicId, uint8_t Vector);
/* NMI to every other processor (bug checks freeze them). */
void HalFreezeOtherProcessors(void);

/* ---- I/O space ----------------------------------------------------------------------------- */

/*
 * Maps device memory (uncached, non-executable) into the HAL I/O-space window and returns its
 * virtual address. Mappings are permanent. Bug checks if the window is exhausted.
 */
void *HalMapIoSpace(uint64_t PhysicalAddress, uint64_t Size);

/* ---- ACPI ------------------------------------------------------------------------------------ */

/* Validates RSDP/XSDT and caches the MADT, FADT and HPET facts in HalAcpi (see acpi.h). */
void HalInitializeAcpi(void);
/* Returns the Index-th table with the given 4-character signature (virtual), or NULL. */
const void *HalAcpiFindTable(const char *Signature, uint32_t Index);

/* ---- Interrupt controllers ----------------------------------------------------------------- */

/* Masks the 8259 PICs, enables the Local APIC and masks every I/O APIC input. */
void HalInitializeInterruptControllers(void);
void HalEndOfInterrupt(void);
/* Requests the DISPATCH_LEVEL software interrupt (LAPIC self-IPI on DISPATCH_VECTOR). The
   APIC holds it pending until IRQL drops below DISPATCH_LEVEL. */
void HalRequestDispatchInterrupt(void);
uint32_t HalGetLocalApicId(void);
bool HalIsX2ApicMode(void);

#define HAL_POLARITY_DEFAULT 0
#define HAL_POLARITY_HIGH 1
#define HAL_POLARITY_LOW 3
#define HAL_TRIGGER_DEFAULT 0
#define HAL_TRIGGER_EDGE 1
#define HAL_TRIGGER_LEVEL 3

/* Routes a global system interrupt to Vector on this CPU and unmasks it. */
STATUS HalEnableSystemInterrupt(uint32_t Gsi, uint8_t Vector, bool ActiveLow, bool LevelTriggered);
void HalDisableSystemInterrupt(uint32_t Gsi);
/* ISA IRQ -> GSI, honouring MADT interrupt source overrides. */
uint32_t HalTranslateIsaIrq(uint8_t IsaIrq, bool *ActiveLow, bool *LevelTriggered);
STATUS HalEnableIsaInterrupt(uint8_t IsaIrq, uint8_t Vector);
void HalDisableIsaInterrupt(uint8_t IsaIrq);

/* ---- Timers ------------------------------------------------------------------------------------ */

/* Picks a reference timer (HPET > ACPI PM timer > PIT) and calibrates the TSC and LAPIC timer. */
void HalCalibrateTimers(void);
const char *HalGetReferenceTimerName(void);
uint64_t HalQueryTscFrequency(void);       /* Hz */
uint64_t HalQueryLapicTimerFrequency(void); /* Hz after the divider */
uint64_t HalQueryPerformanceCounter(void);  /* TSC */
/* Busy-wait. Uses the TSC once calibrated, the reference timer before. */
void HalStallExecution(uint32_t Microseconds);
/* Busy-wait on the reference timer (HPET / PM timer / PIT). Under a hypervisor this is the same
   virtual clock domain as the LAPIC timer, unlike the TSC. Interrupts stay as they are. */
void HalReferenceStall(uint32_t Microseconds);
/* Programs the LAPIC timer in periodic mode on CLOCK_VECTOR. */
void HalStartClockInterrupt(uint32_t RateHz);
/*
 * Self-test for the I/O APIC path: runs PIT channel 0 through ISA IRQ 0 (with its MADT
 * override) for DurationMs and returns the number of interrupts received.
 */
uint32_t HalRunIoApicSelfTest(uint32_t DurationMs, uint32_t RateHz);

/* ---- Serial (COM1) -------------------------------------------------------------------- */

/* Initializes COM1 at 115200 8N1. Returns false if no UART is present. */
bool HalInitializeSerial(void);

/* ---- Real-time clock and power (hal/rtc.c, hal/power.c) --------------------------------------- */

struct _TIME_FIELDS;
/* Reads the CMOS clock (UTC is assumed); false if the values are implausible. */
bool HalQueryRealTimeClock(struct _TIME_FIELDS *Fields);
/* ACPI S5; halts with a message if the machine stays on. */
/* M11: the reference timer (HPET or ACPI PM timer) as a monotonic microsecond count shared by
   every processor, for when the TSC cannot be trusted. Start it before the first reading. */
/* Reads a harmless local APIC register (M11). Under a hypervisor that emulates the APIC this is
   a VM exit, which lets one that runs its timers on this processor's thread run them. */
void HalTouchLocalApic(void);
bool HalStartReferenceTime(void);
uint64_t HalReadReferenceTimeUs(void);

HZ_NORETURN void HalPowerOff(void);
/* Reset register, 8042, 0xCF9, triple fault, in that order. */
HZ_NORETURN void HalReboot(void);
bool HalIsSerialPresent(void);
void HalSerialWriteChar(char Ch);
void HalSerialWrite(const char *Text, size_t Length);
void HalSerialWriteString(const char *Text);

HZ_EXTERN_C_END

#endif /* HAL_H */
