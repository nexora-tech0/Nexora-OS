/*
 * Nexora OS - hal/rtc.c
 * CMOS real-time clock (MC146818-compatible): read the time of day, BCD or binary, 12- or 24-hour,
 * with the century from the register the FADT names (else 20xx). Reads repeat until two
 * consecutive snapshots agree, so an update in progress cannot tear the value.
 */
#include "acpi.h"
#include "halp.h"
#include "rtl.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA 0x71
#define CMOS_NMI_DISABLE 0x80
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B
#define CMOS_A_UPDATE_IN_PROGRESS 0x80
#define CMOS_B_24_HOUR 0x02
#define CMOS_B_BINARY 0x04

static uint8_t HalpReadCmos(uint8_t Register)
{
    HalWritePort8(CMOS_INDEX, CMOS_NMI_DISABLE | Register);
    HalIoDelay();
    return HalReadPort8(CMOS_DATA);
}

typedef struct _HALP_RTC_SNAPSHOT {
    uint8_t Second, Minute, Hour, Day, Month, Year, Century;
} HALP_RTC_SNAPSHOT;

static void HalpReadSnapshot(HALP_RTC_SNAPSHOT *S)
{
    for (int i = 0; i < 100000 && (HalpReadCmos(CMOS_STATUS_A) & CMOS_A_UPDATE_IN_PROGRESS); i++)
        HalPause();
    S->Second = HalpReadCmos(0x00);
    S->Minute = HalpReadCmos(0x02);
    S->Hour = HalpReadCmos(0x04);
    S->Day = HalpReadCmos(0x07);
    S->Month = HalpReadCmos(0x08);
    S->Year = HalpReadCmos(0x09);
    S->Century = HalAcpi.CenturyRegister ? HalpReadCmos(HalAcpi.CenturyRegister) : 0;
}

static uint8_t HalpFromBcd(uint8_t Value)
{
    return (uint8_t)((Value >> 4) * 10 + (Value & 0x0F));
}

bool HalQueryRealTimeClock(TIME_FIELDS *Fields)
{
    bool Enabled = HalAreInterruptsEnabled();
    HalDisableInterrupts();
    HALP_RTC_SNAPSHOT A, B;
    HalpReadSnapshot(&B);
    int Tries = 0;
    do {
        A = B;
        HalpReadSnapshot(&B);
    } while (memcmp(&A, &B, sizeof(A)) != 0 && ++Tries < 10);
    uint8_t StatusB = HalpReadCmos(CMOS_STATUS_B);
    HalWritePort8(CMOS_INDEX, CMOS_STATUS_B); /* leave NMIs enabled */
    if (Enabled)
        HalEnableInterrupts();

    bool Pm = (B.Hour & 0x80) != 0;
    B.Hour &= 0x7F;
    if (!(StatusB & CMOS_B_BINARY)) {
        B.Second = HalpFromBcd(B.Second);
        B.Minute = HalpFromBcd(B.Minute);
        B.Hour = HalpFromBcd(B.Hour);
        B.Day = HalpFromBcd(B.Day);
        B.Month = HalpFromBcd(B.Month);
        B.Year = HalpFromBcd(B.Year);
        B.Century = HalpFromBcd(B.Century);
    }
    if (!(StatusB & CMOS_B_24_HOUR)) {
        B.Hour %= 12;
        if (Pm)
            B.Hour += 12;
    }
    unsigned Century = (B.Century >= 19 && B.Century <= 30) ? B.Century : 20;
    Fields->Year = (uint16_t)(Century * 100 + B.Year);
    Fields->Month = B.Month;
    Fields->Day = B.Day;
    Fields->Hour = B.Hour;
    Fields->Minute = B.Minute;
    Fields->Second = B.Second;
    Fields->Weekday = 0;
    return B.Month >= 1 && B.Month <= 12 && B.Day >= 1 && B.Day <= 31 && B.Hour < 24 && B.Minute < 60 &&
           B.Second < 60;
}
