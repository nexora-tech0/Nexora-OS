/*
 * Nexora OS - hal/halp.h
 * HAL-private declarations shared between the C files in hal/.
 */
#ifndef HALP_H
#define HALP_H

#include "acpi.h"
#include "hal.h"
#include "ke.h"
#include "x86.h"

/* hal/trap.asm, hal/syscall.asm */
extern const uint64_t KiTrapStubTable[256];
void KiSystemCall64(void);
void HalpLoadGdt(const KDESCRIPTOR *Gdt, uint16_t CodeSelector, uint16_t DataSelector);

/* hal/idt.c */
void HalpInitializeIdt(void);
void HalpLoadIdt(void);

/* hal/apic.c */
void HalpLapicWrite(uint32_t Register, uint32_t Value);
uint32_t HalpLapicRead(uint32_t Register);
void HalpInitializeLocalApicForProcessor(void); /* an application processor (M11) */

#define LAPIC_ID 0x020
#define LAPIC_VERSION 0x030
#define LAPIC_TPR 0x080
#define LAPIC_EOI 0x0B0
#define LAPIC_SVR 0x0F0
#define LAPIC_ESR 0x280
#define LAPIC_ICR_LOW 0x300
#define LAPIC_ICR_HIGH 0x310
#define LAPIC_ICR_DELIVERY_PENDING (1u << 12)
#define LAPIC_ICR_DEST_SELF (1u << 18)
#define LAPIC_ICR_DEST_ALL_BUT_SELF (3u << 18)
#define LAPIC_ICR_ASSERT (1u << 14)
#define LAPIC_ICR_NMI (4u << 8)
#define MSR_X2APIC_SELF_IPI 0x83F
#define MSR_X2APIC_ICR 0x830
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE 0x3E0

#define LAPIC_LVT_MASKED (1u << 16)
#define LAPIC_LVT_PERIODIC (1u << 17)
#define LAPIC_LVT_NMI (4u << 8)
#define LAPIC_LVT_ACTIVE_LOW (1u << 13)
#define LAPIC_LVT_LEVEL (1u << 15)
#define LAPIC_SVR_ENABLE (1u << 8)
#define LAPIC_TIMER_DIVIDE_16 0x3

/* hal/timer.c: reference-timer primitives used for calibration */
typedef enum _HAL_REFERENCE_TIMER {
    HalReferenceNone = 0,
    HalReferenceHpet,
    HalReferencePmTimer,
    HalReferencePit,
} HAL_REFERENCE_TIMER;

/* hal/pic.c */
void HalpMaskLegacyPic(void);

#endif /* HALP_H */
