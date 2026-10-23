/*
 * Nexora OS - rtl/unicode.c
 * UTF-8 <-> UTF-16 conversion (file names on disk are UTF-16; every Nexora OS API uses UTF-8)
 * and the CRC-32 used by GPT.
 */
#include "rtl.h"

int RtlUtf16ToUtf8(const uint16_t *Source, size_t Count, char *Destination, size_t Capacity)
{
    size_t Out = 0;
    for (size_t i = 0; i < Count; i++) {
        uint32_t c = Source[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < Count && Source[i + 1] >= 0xDC00 && Source[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (Source[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD; /* unpaired surrogate */
        }
        uint8_t Bytes[4];
        size_t n;
        if (c < 0x80) {
            Bytes[0] = (uint8_t)c;
            n = 1;
        } else if (c < 0x800) {
            Bytes[0] = (uint8_t)(0xC0 | (c >> 6));
            Bytes[1] = (uint8_t)(0x80 | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            Bytes[0] = (uint8_t)(0xE0 | (c >> 12));
            Bytes[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            Bytes[2] = (uint8_t)(0x80 | (c & 0x3F));
            n = 3;
        } else {
            Bytes[0] = (uint8_t)(0xF0 | (c >> 18));
            Bytes[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
            Bytes[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            Bytes[3] = (uint8_t)(0x80 | (c & 0x3F));
            n = 4;
        }
        if (Out + n + 1 > Capacity)
            return -1;
        memcpy(Destination + Out, Bytes, n);
        Out += n;
    }
    if (Out + 1 > Capacity)
        return -1;
    Destination[Out] = '\0';
    return (int)Out;
}

int RtlUtf8ToUtf16(const char *Source, uint16_t *Destination, size_t Capacity)
{
    size_t Out = 0;
    const uint8_t *p = (const uint8_t *)Source;
    while (*p) {
        uint32_t c;
        int Extra;
        if (*p < 0x80) {
            c = *p;
            Extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            c = *p & 0x1F;
            Extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            c = *p & 0x0F;
            Extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            c = *p & 0x07;
            Extra = 3;
        } else {
            return -1;
        }
        p++;
        for (int i = 0; i < Extra; i++, p++) {
            if ((*p & 0xC0) != 0x80)
                return -1;
            c = (c << 6) | (*p & 0x3F);
        }
        if (c >= 0x10000) {
            if (c > 0x10FFFF || Out + 2 > Capacity)
                return -1;
            c -= 0x10000;
            Destination[Out++] = (uint16_t)(0xD800 + (c >> 10));
            Destination[Out++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else {
            if ((c >= 0xD800 && c <= 0xDFFF) || Out + 1 > Capacity)
                return -1;
            Destination[Out++] = (uint16_t)c;
        }
    }
    return (int)Out;
}

uint32_t RtlComputeCrc32(uint32_t Crc, const void *Buffer, size_t Length)
{
    /* Reflected CRC-32 (polynomial 0xEDB88320), bitwise: small and fast enough for GPT. */
    const uint8_t *p = (const uint8_t *)Buffer;
    Crc = ~Crc;
    while (Length--) {
        Crc ^= *p++;
        for (int i = 0; i < 8; i++)
            Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1)));
    }
    return ~Crc;
}
