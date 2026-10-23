/*
 * Nexora OS - hal/serial.c
 * COM1 16550 UART driver used for the kernel debug log (115200 8N1, polled).
 * VirtualBox logs COM1 to a file and QEMU maps it to stdio, so this is our primary diagnostic
 * channel. If no UART answers the scratch-register probe (e.g. COM1 disabled in the VM),
 * output is silently dropped instead of spinning on a missing device.
 */
#include "hal.h"

#define COM1_BASE 0x3F8

#define UART_DATA 0       /* DLAB=0: RBR/THR */
#define UART_IER 1        /* DLAB=0: interrupt enable */
#define UART_DLL 0        /* DLAB=1: divisor low */
#define UART_DLM 1        /* DLAB=1: divisor high */
#define UART_FCR 2        /* FIFO control */
#define UART_LCR 3        /* line control */
#define UART_MCR 4        /* modem control */
#define UART_LSR 5        /* line status */
#define UART_SCR 7        /* scratch */

#define LCR_8N1 0x03
#define LCR_DLAB 0x80
#define FCR_ENABLE_CLEAR_14 0xC7
#define MCR_DTR_RTS_OUT2 0x0B
#define MCR_LOOPBACK 0x1E
#define MCR_NORMAL 0x0F
#define LSR_DATA_READY 0x01
#define LSR_THR_EMPTY 0x20

static bool HalpSerialPresent;

/*
 * Presence is decided by the scratch register, which is synchronous on real and emulated
 * UARTs (an absent port floats to 0xFF). The loopback echo is NOT used for that decision:
 * emulators such as VirtualBox deliver the echoed byte asynchronously, so reading it back
 * immediately races the device model. It only flushes the receive path here.
 */
static bool HalpProbeUart(void)
{
    HalWritePort8(COM1_BASE + UART_SCR, 0x5A);
    if (HalReadPort8(COM1_BASE + UART_SCR) != 0x5A)
        return false;
    HalWritePort8(COM1_BASE + UART_SCR, 0xA5);
    if (HalReadPort8(COM1_BASE + UART_SCR) != 0xA5)
        return false;
    return HalReadPort8(COM1_BASE + UART_LSR) != 0xFF;
}

bool HalInitializeSerial(void)
{
    if (!HalpProbeUart()) {
        HalpSerialPresent = false;
        return false;
    }

    HalWritePort8(COM1_BASE + UART_IER, 0x00);             /* polled mode */
    HalWritePort8(COM1_BASE + UART_LCR, LCR_DLAB);
    HalWritePort8(COM1_BASE + UART_DLL, 0x01);             /* 115200 / 1 */
    HalWritePort8(COM1_BASE + UART_DLM, 0x00);
    HalWritePort8(COM1_BASE + UART_LCR, LCR_8N1);
    HalWritePort8(COM1_BASE + UART_FCR, FCR_ENABLE_CLEAR_14);

    /* Loopback echo with a bounded wait for Data Ready, then drain whatever arrived. */
    HalWritePort8(COM1_BASE + UART_MCR, MCR_LOOPBACK);
    HalWritePort8(COM1_BASE + UART_DATA, 0xAE);
    for (uint32_t Spin = 0; Spin < 100000; Spin++) {
        if (HalReadPort8(COM1_BASE + UART_LSR) & LSR_DATA_READY)
            break;
        HalPause();
    }
    for (int i = 0; i < 16 && (HalReadPort8(COM1_BASE + UART_LSR) & LSR_DATA_READY); i++)
        (void)HalReadPort8(COM1_BASE + UART_DATA);

    HalWritePort8(COM1_BASE + UART_MCR, MCR_NORMAL);
    HalpSerialPresent = true;
    return true;
}

bool HalIsSerialPresent(void)
{
    return HalpSerialPresent;
}

static void HalpSerialPutRaw(char Ch)
{
    /* Bounded wait: never hang the kernel on a wedged UART. */
    for (uint32_t Spin = 0; Spin < 100000; Spin++) {
        if (HalReadPort8(COM1_BASE + UART_LSR) & LSR_THR_EMPTY)
            break;
        HalPause();
    }
    HalWritePort8(COM1_BASE + UART_DATA, (uint8_t)Ch);
}

void HalSerialWriteChar(char Ch)
{
    if (!HalpSerialPresent)
        return;
    if (Ch == '\n')
        HalpSerialPutRaw('\r');
    HalpSerialPutRaw(Ch);
}

void HalSerialWrite(const char *Text, size_t Length)
{
    for (size_t i = 0; i < Length; i++)
        HalSerialWriteChar(Text[i]);
}

void HalSerialWriteString(const char *Text)
{
    while (*Text)
        HalSerialWriteChar(*Text++);
}
