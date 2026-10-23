/*
 * Nexora OS - hal/cpu.c
 * Processor control helpers that are not worth inlining.
 */
#include "hal.h"

HZ_NORETURN void HalHaltSystem(void)
{
    for (;;) {
        HalDisableInterrupts();
        HalHalt();
    }
}
