/*
 * Nexora OS - ke/debug.c
 * Kernel debug log. Every message goes to COM1 with an uptime stamp once the TSC is
 * calibrated ("[   1.234567] "), warnings/errors get a tag, and messages up to INFO level are
 * mirrored to the on-screen boot log. A spinlock taken at HIGH_LEVEL keeps lines from
 * different contexts (thread, ISR) from interleaving. Bug checks bypass this path entirely.
 */
#include "hal.h"
#include "inbv.h"
#include "ke.h"
#include "rtl.h"

static KSPIN_LOCK KdpLock;
static uint32_t KdpLevel = DPFLTR_INFO_LEVEL;
static bool KdpAtLineStart = true;

void DbgSetLogLevel(uint32_t Level)
{
    KdpLevel = Level;
}

static void KdpWriteLinePrefix(uint32_t Level)
{
    char Prefix[40];
    int n = 0;
    uint64_t Us = KeQueryUptimeMicroseconds();
    if (Us != 0)
        n = RtlFormatString(Prefix, sizeof(Prefix), "[%5llu.%06llu] ", (unsigned long long)(Us / 1000000),
                            (unsigned long long)(Us % 1000000));
    if (Level == DPFLTR_ERROR_LEVEL)
        n += RtlFormatString(Prefix + n, sizeof(Prefix) - (size_t)n, "ERROR: ");
    else if (Level == DPFLTR_WARNING_LEVEL)
        n += RtlFormatString(Prefix + n, sizeof(Prefix) - (size_t)n, "WARNING: ");
    HalSerialWrite(Prefix, (size_t)n);
}

void DbgPrintExV(uint32_t Level, const char *Format, va_list Args)
{
    if (Level > KdpLevel)
        return;

    char Buffer[512];
    int Length = RtlFormatStringV(Buffer, sizeof(Buffer), Format, Args);
    if (Length >= (int)sizeof(Buffer))
        Length = (int)sizeof(Buffer) - 1;

    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&KdpLock, HIGH_LEVEL, &OldIrql);
    const char *p = Buffer;
    const char *End = Buffer + Length;
    while (p < End) {
        if (KdpAtLineStart)
            KdpWriteLinePrefix(Level);
        const char *Newline = p;
        while (Newline < End && *Newline != '\n')
            Newline++;
        bool HasNewline = Newline < End;
        HalSerialWrite(p, (size_t)(Newline - p) + (HasNewline ? 1 : 0));
        KdpAtLineStart = HasNewline;
        p = Newline + (HasNewline ? 1 : 0);
    }
    if (Level <= DPFLTR_INFO_LEVEL)
        InbvDisplayString(Buffer);
    KeReleaseSpinLock(&KdpLock, OldIrql);
}

void DbgPrintEx(uint32_t Level, const char *Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    DbgPrintExV(Level, Format, Args);
    va_end(Args);
}

void DbgPrint(const char *Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    DbgPrintExV(DPFLTR_INFO_LEVEL, Format, Args);
    va_end(Args);
}
