/*
 * Nexora OS - rtl/time.c
 * Calendar conversion (days-from-civil algorithm, integer only).
 */
#include "rtl.h"

void RtlUnixTimeToTimeFields(int64_t UnixSeconds, TIME_FIELDS *Fields)
{
    int64_t Days = UnixSeconds / 86400;
    int64_t Rem = UnixSeconds % 86400;
    if (Rem < 0) {
        Rem += 86400;
        Days -= 1;
    }
    Fields->Hour = (uint8_t)(Rem / 3600);
    Fields->Minute = (uint8_t)((Rem % 3600) / 60);
    Fields->Second = (uint8_t)(Rem % 60);
    Fields->Weekday = (uint8_t)(((Days % 7) + 11) % 7); /* 1970-01-01 was a Thursday (4) */

    /* Civil-from-days: shift the epoch to 0000-03-01 so leap days fall at year end. */
    int64_t z = Days + 719468;
    int64_t Era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t Doe = z - Era * 146097;
    int64_t Yoe = (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
    int64_t y = Yoe + Era * 400;
    int64_t Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
    int64_t Mp = (5 * Doy + 2) / 153;
    int64_t d = Doy - (153 * Mp + 2) / 5 + 1;
    int64_t m = Mp < 10 ? Mp + 3 : Mp - 9;
    if (m <= 2)
        y += 1;

    Fields->Year = (uint16_t)y;
    Fields->Month = (uint8_t)m;
    Fields->Day = (uint8_t)d;
}

int64_t RtlTimeFieldsToUnixTime(const TIME_FIELDS *Fields)
{
    /* Days-from-civil (inverse of the above). */
    int64_t y = Fields->Year;
    int64_t m = Fields->Month;
    int64_t d = Fields->Day;
    y -= m <= 2;
    int64_t Era = (y >= 0 ? y : y - 399) / 400;
    int64_t Yoe = y - Era * 400;
    int64_t Doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
    int64_t Days = Era * 146097 + Doe - 719468;
    return Days * 86400 + Fields->Hour * 3600 + Fields->Minute * 60 + Fields->Second;
}

void RtlTimeToTimeFields(uint64_t Time, TIME_FIELDS *Fields)
{
    int64_t Unix = (int64_t)(Time / 10000000ull) - (int64_t)(116444736000000000ull / 10000000ull);
    RtlUnixTimeToTimeFields(Unix, Fields);
}

uint64_t RtlTimeFieldsToTime(const TIME_FIELDS *Fields)
{
    int64_t Unix = RtlTimeFieldsToUnixTime(Fields);
    return (uint64_t)Unix * 10000000ull + 116444736000000000ull;
}
