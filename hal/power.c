/*
 * Nexora OS - hal/power.c
 * Power off (ACPI S5) and reset.
 *
 * Power off writes SLP_TYPx | SLP_EN to the PM1 control registers, with the sleep types found in
 * the DSDT's \_S5 package (hal/acpi.c), after switching the chipset to ACPI mode if the firmware
 * left it in legacy mode. Reset tries, in order: the FADT reset register, the 8042 pulse, the
 * PCI reset control register (0xCF9), and finally a triple fault.
 */
#include "acpi.h"
#include "halp.h"
#include "mm.h"
#include "rtl.h"

#define PM1_SCI_EN 0x0001
#define PM1_SLP_TYP_SHIFT 10
#define PM1_SLP_TYP_MASK (7u << PM1_SLP_TYP_SHIFT)
#define PM1_SLP_EN (1u << 13)

static void HalpWait(uint32_t Microseconds)
{
    HalStallExecution(Microseconds);
}

HZ_NORETURN void HalPowerOff(void)
{
    HalDisableInterrupts();
    if (HalAcpi.HasS5 && HalAcpi.Pm1aControlPort) {
        if (!(HalReadPort16(HalAcpi.Pm1aControlPort) & PM1_SCI_EN) && HalAcpi.SmiCommandPort &&
            HalAcpi.AcpiEnableValue) {
            HalWritePort8(HalAcpi.SmiCommandPort, HalAcpi.AcpiEnableValue);
            for (int i = 0; i < 300 && !(HalReadPort16(HalAcpi.Pm1aControlPort) & PM1_SCI_EN); i++)
                HalpWait(10000);
        }
        uint16_t Value = HalReadPort16(HalAcpi.Pm1aControlPort) & ~PM1_SLP_TYP_MASK;
        HalWritePort16(HalAcpi.Pm1aControlPort,
                       (uint16_t)(Value | ((uint16_t)HalAcpi.S5SleepTypeA << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN));
        if (HalAcpi.Pm1bControlPort) {
            Value = HalReadPort16(HalAcpi.Pm1bControlPort) & ~PM1_SLP_TYP_MASK;
            HalWritePort16(HalAcpi.Pm1bControlPort,
                           (uint16_t)(Value | ((uint16_t)HalAcpi.S5SleepTypeB << PM1_SLP_TYP_SHIFT) | PM1_SLP_EN));
        }
        HalpWait(1000000);
    }
    DbgPrintEx(DPFLTR_ERROR_LEVEL, "Power: the machine did not power off; it is safe to turn it off now\n");
    HalHaltSystem();
}

static void HalpWriteResetRegister(void)
{
    const ACPI_GAS *Reg = &HalAcpi.ResetRegister;
    switch (Reg->AddressSpace) {
    case ACPI_GAS_IO:
        HalWritePort8((uint16_t)Reg->Address, HalAcpi.ResetValue);
        break;
    case ACPI_GAS_MEMORY: {
        volatile uint8_t *p = MmMapIoSpace(Reg->Address, 1, MM_CACHE_UNCACHED);
        if (p)
            *p = HalAcpi.ResetValue;
        break;
    }
    case 2: { /* PCI configuration space of bus 0: device, function, offset packed in Address */
        uint32_t Device = (uint32_t)(Reg->Address >> 32) & 0x1F, Function = (uint32_t)(Reg->Address >> 16) & 7;
        uint32_t Offset = (uint32_t)Reg->Address & 0xFF;
        HalWritePort32(0xCF8, 0x80000000u | (Device << 11) | (Function << 8) | (Offset & 0xFC));
        HalWritePort8((uint16_t)(0xCFC + (Offset & 3)), HalAcpi.ResetValue);
        break;
    }
    default:
        break;
    }
}

HZ_NORETURN void HalReboot(void)
{
    HalDisableInterrupts();
    if (HalAcpi.HasResetRegister) {
        HalpWriteResetRegister();
        HalpWait(500000);
    }
    /* 8042: wait for an empty input buffer, then pulse the reset line. */
    for (int i = 0; i < 100000 && (HalReadPort8(0x64) & 0x02); i++)
        HalPause();
    HalWritePort8(0x64, 0xFE);
    HalpWait(500000);
    /* PCI reset control register (Intel chipsets): system reset + full reset. */
    HalWritePort8(0xCF9, 0x02);
    HalpWait(10);
    HalWritePort8(0xCF9, 0x06);
    HalpWait(500000);
    /* Triple fault: an empty IDT makes the next exception unrecoverable. */
    static const KDESCRIPTOR Empty = {0, 0};
    __lidt(&Empty);
    __asm__ volatile("int3");
    HalHaltSystem();
}
