/*
 * Nexora OS - hal/pic.c
 * Legacy 8259 PICs. Nexora OS routes all interrupts through the APICs, so the PICs are only
 * re-initialized to vectors 0x20-0x2F (away from the exception range, in case a spurious
 * IRQ 7/15 is ever delivered) and then fully masked.
 */
#include "halp.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

void HalpMaskLegacyPic(void)
{
    HalWritePort8(PIC1_COMMAND, 0x11); /* ICW1: init, ICW4 follows */
    HalIoDelay();
    HalWritePort8(PIC2_COMMAND, 0x11);
    HalIoDelay();
    HalWritePort8(PIC1_DATA, 0x20);    /* ICW2: vector base */
    HalIoDelay();
    HalWritePort8(PIC2_DATA, 0x28);
    HalIoDelay();
    HalWritePort8(PIC1_DATA, 0x04);    /* ICW3: slave on IRQ 2 */
    HalIoDelay();
    HalWritePort8(PIC2_DATA, 0x02);
    HalIoDelay();
    HalWritePort8(PIC1_DATA, 0x01);    /* ICW4: 8086 mode */
    HalIoDelay();
    HalWritePort8(PIC2_DATA, 0x01);
    HalIoDelay();
    HalWritePort8(PIC1_DATA, 0xFF);    /* mask everything */
    HalWritePort8(PIC2_DATA, 0xFF);
}
