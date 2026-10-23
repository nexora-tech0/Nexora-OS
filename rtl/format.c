/*
 * Nexora OS - rtl/format.c
 * Freestanding printf-style formatter (integer-only; the kernel is built without FP/SIMD).
 */
#include "rtl.h"

typedef struct _FMT_SINK {
    char *Buffer;
    size_t Size;
    size_t Length; /* characters produced so far (may exceed Size) */
} FMT_SINK;

static void FmtPut(FMT_SINK *Sink, char Ch)
{
    if (Sink->Size && Sink->Length < Sink->Size - 1)
        Sink->Buffer[Sink->Length] = Ch;
    Sink->Length++;
}

static void FmtRepeat(FMT_SINK *Sink, char Ch, int Count)
{
    while (Count-- > 0)
        FmtPut(Sink, Ch);
}

#define FLAG_LEFT 0x01
#define FLAG_ZERO 0x02
#define FLAG_PLUS 0x04
#define FLAG_SPACE 0x08
#define FLAG_ALT 0x10
#define FLAG_UPPER 0x20

static void FmtInteger(FMT_SINK *Sink, uint64_t Value, bool Negative, unsigned Base, int Flags,
                       int Width, int Precision)
{
    char Digits[66];
    int n = 0;
    const char *Set = (Flags & FLAG_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";

    if (Value == 0 && Precision != 0)
        Digits[n++] = '0';
    while (Value) {
        Digits[n++] = Set[Value % Base];
        Value /= Base;
    }

    char Sign = 0;
    if (Negative)
        Sign = '-';
    else if (Flags & FLAG_PLUS)
        Sign = '+';
    else if (Flags & FLAG_SPACE)
        Sign = ' ';

    const char *Prefix = "";
    if ((Flags & FLAG_ALT) && Base == 16)
        Prefix = (Flags & FLAG_UPPER) ? "0X" : "0x";
    else if ((Flags & FLAG_ALT) && Base == 2)
        Prefix = "0b";
    else if ((Flags & FLAG_ALT) && Base == 8)
        Prefix = "0";
    int PrefixLength = (int)strlen(Prefix);

    int ZeroPad = Precision > n ? Precision - n : 0;
    int Body = n + ZeroPad + PrefixLength + (Sign ? 1 : 0);
    int Pad = Width > Body ? Width - Body : 0;

    if ((Flags & FLAG_ZERO) && !(Flags & FLAG_LEFT) && Precision < 0) {
        ZeroPad += Pad;
        Pad = 0;
    }
    if (!(Flags & FLAG_LEFT))
        FmtRepeat(Sink, ' ', Pad);
    if (Sign)
        FmtPut(Sink, Sign);
    for (int i = 0; i < PrefixLength; i++)
        FmtPut(Sink, Prefix[i]);
    FmtRepeat(Sink, '0', ZeroPad);
    while (n > 0)
        FmtPut(Sink, Digits[--n]);
    if (Flags & FLAG_LEFT)
        FmtRepeat(Sink, ' ', Pad);
}

static void FmtString(FMT_SINK *Sink, const char *Str, int Flags, int Width, int Precision)
{
    if (!Str)
        Str = "(null)";
    int Length = (int)(Precision >= 0 ? strnlen(Str, (size_t)Precision) : strlen(Str));
    int Pad = Width > Length ? Width - Length : 0;
    if (!(Flags & FLAG_LEFT))
        FmtRepeat(Sink, ' ', Pad);
    for (int i = 0; i < Length; i++)
        FmtPut(Sink, Str[i]);
    if (Flags & FLAG_LEFT)
        FmtRepeat(Sink, ' ', Pad);
}

int RtlFormatStringV(char *Buffer, size_t Size, const char *Format, va_list Args)
{
    FMT_SINK Sink = {.Buffer = Buffer, .Size = Size, .Length = 0};

    for (const char *p = Format; *p; p++) {
        if (*p != '%') {
            FmtPut(&Sink, *p);
            continue;
        }
        p++;

        int Flags = 0;
        for (;; p++) {
            if (*p == '-') Flags |= FLAG_LEFT;
            else if (*p == '0') Flags |= FLAG_ZERO;
            else if (*p == '+') Flags |= FLAG_PLUS;
            else if (*p == ' ') Flags |= FLAG_SPACE;
            else if (*p == '#') Flags |= FLAG_ALT;
            else break;
        }

        int Width = 0;
        if (*p == '*') {
            Width = va_arg(Args, int);
            if (Width < 0) {
                Flags |= FLAG_LEFT;
                Width = -Width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                Width = Width * 10 + (*p++ - '0');
        }

        int Precision = -1;
        if (*p == '.') {
            p++;
            Precision = 0;
            if (*p == '*') {
                Precision = va_arg(Args, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    Precision = Precision * 10 + (*p++ - '0');
            }
        }

        /* Length modifier: 0 = int, 1 = long, 2 = long long, -1 = short, -2 = char, 3 = size/ptrdiff/intmax */
        int LengthMod = 0;
        if (*p == 'h') {
            LengthMod = -1;
            if (*++p == 'h') {
                LengthMod = -2;
                p++;
            }
        } else if (*p == 'l') {
            LengthMod = 1;
            if (*++p == 'l') {
                LengthMod = 2;
                p++;
            }
        } else if (*p == 'z' || *p == 'j' || *p == 't') {
            LengthMod = 3;
            p++;
        }

        char Spec = *p;
        if (Spec == '\0')
            break;

        switch (Spec) {
        case '%':
            FmtPut(&Sink, '%');
            break;
        case 'c': {
            char Ch = (char)va_arg(Args, int);
            int Pad = Width > 1 ? Width - 1 : 0;
            if (!(Flags & FLAG_LEFT))
                FmtRepeat(&Sink, ' ', Pad);
            FmtPut(&Sink, Ch);
            if (Flags & FLAG_LEFT)
                FmtRepeat(&Sink, ' ', Pad);
            break;
        }
        case 's':
            FmtString(&Sink, va_arg(Args, const char *), Flags, Width, Precision);
            break;
        case 'd':
        case 'i': {
            int64_t v;
            if (LengthMod >= 1)
                v = (int64_t)va_arg(Args, long long);
            else
                v = va_arg(Args, int);
            if (LengthMod == -1) v = (short)v;
            if (LengthMod == -2) v = (signed char)v;
            uint64_t Magnitude = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            FmtInteger(&Sink, Magnitude, v < 0, 10, Flags, Width, Precision);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'b': {
            uint64_t v;
            if (LengthMod >= 1)
                v = (uint64_t)va_arg(Args, unsigned long long);
            else
                v = va_arg(Args, unsigned int);
            if (LengthMod == -1) v = (unsigned short)v;
            if (LengthMod == -2) v = (unsigned char)v;
            unsigned Base = Spec == 'u' ? 10 : Spec == 'o' ? 8 : Spec == 'b' ? 2 : 16;
            if (Spec == 'X')
                Flags |= FLAG_UPPER;
            FmtInteger(&Sink, v, false, Base, Flags, Width, Precision);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(Args, void *);
            FmtInteger(&Sink, v, false, 16, Flags | FLAG_ALT | FLAG_ZERO, Width ? Width : 18, -1);
            break;
        }
        default:
            /* Unknown conversion: print it verbatim so the mistake is visible. */
            FmtPut(&Sink, '%');
            FmtPut(&Sink, Spec);
            break;
        }
    }

    if (Size)
        Buffer[Sink.Length < Size ? Sink.Length : Size - 1] = '\0';
    return (int)Sink.Length;
}

int RtlFormatString(char *Buffer, size_t Size, const char *Format, ...)
{
    va_list Args;
    va_start(Args, Format);
    int n = RtlFormatStringV(Buffer, Size, Format, Args);
    va_end(Args);
    return n;
}

void RtlFormatByteSize(char *Buffer, size_t Size, uint64_t Bytes)
{
    static const char *const Units[] = {"bytes", "KiB", "MiB", "GiB", "TiB"};
    if (Bytes < 1024) {
        RtlFormatString(Buffer, Size, "%llu bytes", (unsigned long long)Bytes);
        return;
    }
    unsigned Unit = 0;
    uint64_t Scaled10 = Bytes * 10; /* one decimal digit, integer-only */
    uint64_t Value = Bytes;
    while (Value >= 1024 && Unit < 4) {
        Scaled10 = (Value * 10 + 512) / 1024; /* rounded to the nearest tenth */
        Value /= 1024;
        Unit++;
    }
    if (Value >= 100)
        RtlFormatString(Buffer, Size, "%llu %s", (unsigned long long)Value, Units[Unit]);
    else
        RtlFormatString(Buffer, Size, "%llu.%llu %s", (unsigned long long)(Scaled10 / 10),
                        (unsigned long long)(Scaled10 % 10), Units[Unit]);
}
