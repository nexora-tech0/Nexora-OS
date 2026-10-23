/*
 * Nexora OS - hal/idt.c
 * Interrupt descriptor table: one interrupt gate per vector, pointing at the stubs in
 * hal/trap.asm. NMI, double fault and machine check run on dedicated IST stacks so they work
 * even when the current kernel stack is unusable.
 */
#include "halp.h"

#define GATE_INTERRUPT_DPL0 0x8E
#define GATE_INTERRUPT_DPL3 0xEE /* int3 may be issued from user mode (M4) */

static KIDTENTRY64 HalpIdt[256] HZ_ALIGNED(16);

/* Every processor uses the same IDT (M11). */
void HalpLoadIdt(void)
{
    KDESCRIPTOR Idtr = {.Limit = sizeof(HalpIdt) - 1, .Base = (uint64_t)HalpIdt};
    __lidt(&Idtr);
}

static uint8_t HalpIstForVector(unsigned Vector)
{
    switch (Vector) {
    case 2: return 2;  /* NMI */
    case 8: return 1;  /* double fault */
    case 18: return 3; /* machine check */
    default: return 0;
    }
}

void HalpInitializeIdt(void)
{
    for (unsigned v = 0; v < 256; v++) {
        uint64_t Handler = KiTrapStubTable[v];
        KIDTENTRY64 *E = &HalpIdt[v];
        E->OffsetLow = (uint16_t)Handler;
        E->Selector = KGDT_KERNEL_CODE;
        E->Ist = HalpIstForVector(v);
        E->TypeAttr = v == 3 ? GATE_INTERRUPT_DPL3 : GATE_INTERRUPT_DPL0;
        E->OffsetMiddle = (uint16_t)(Handler >> 16);
        E->OffsetHigh = (uint32_t)(Handler >> 32);
        E->Reserved = 0;
    }
    HalpLoadIdt();
}
