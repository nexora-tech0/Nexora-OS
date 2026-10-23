/*
 * Nexora OS - user/lib/hzrt/hzcore.cpp
 * The out-of-line parts of hz::String (user/include/hzcore.h).
 */
#include "hzcore.h"
#include "rtl.h"

namespace hz {

void String::Reserve(size_t Capacity)
{
    if (Capacity <= Capacity_ && Data_)
        return;
    size_t NewCapacity = Max<size_t>(Capacity, Capacity_ * 2);
    NewCapacity = Max<size_t>(NewCapacity, 15);
    char *New = (char *)realloc(Data_, NewCapacity + 1);
    if (!New)
        OutOfMemory();
    if (!Data_)
        New[0] = '\0';
    Data_ = New;
    Capacity_ = NewCapacity;
}

void String::Append(const char *Text, size_t Length)
{
    Insert(Length_, Text, Length);
}

void String::AppendCodePoint(uint32_t C)
{
    char Bytes[4];
    size_t Count;
    if (C < 0x80) {
        Bytes[0] = (char)C;
        Count = 1;
    } else if (C < 0x800) {
        Bytes[0] = (char)(0xC0 | (C >> 6));
        Bytes[1] = (char)(0x80 | (C & 0x3F));
        Count = 2;
    } else if (C < 0x10000) {
        Bytes[0] = (char)(0xE0 | (C >> 12));
        Bytes[1] = (char)(0x80 | ((C >> 6) & 0x3F));
        Bytes[2] = (char)(0x80 | (C & 0x3F));
        Count = 3;
    } else {
        Bytes[0] = (char)(0xF0 | (C >> 18));
        Bytes[1] = (char)(0x80 | ((C >> 12) & 0x3F));
        Bytes[2] = (char)(0x80 | ((C >> 6) & 0x3F));
        Bytes[3] = (char)(0x80 | (C & 0x3F));
        Count = 4;
    }
    Append(Bytes, Count);
}

void String::Insert(size_t At, const char *Text, size_t Length)
{
    if (At > Length_)
        At = Length_;
    if (!Data_)
        Reserve(Length);
    if (!Length)
        return;
    /* Text may point into this string: copy it out before the buffer moves. */
    bool Inside = Text >= Data_ && Text < Data_ + Capacity_ + 1;
    char *Copy = nullptr;
    if (Inside) {
        Copy = (char *)malloc(Length);
        if (!Copy)
            OutOfMemory();
        memcpy(Copy, Text, Length);
        Text = Copy;
    }
    Reserve(Length_ + Length);
    memmove(Data_ + At + Length, Data_ + At, Length_ - At + 1);
    memcpy(Data_ + At, Text, Length);
    Length_ += Length;
    free(Copy);
}

void String::Erase(size_t At, size_t Count)
{
    if (At >= Length_)
        return;
    Count = Min(Count, Length_ - At);
    memmove(Data_ + At, Data_ + At + Count, Length_ - At - Count + 1);
    Length_ -= Count;
}

String String::Substring(size_t At, size_t Count) const
{
    if (At >= Length_)
        return String();
    return String(CStr() + At, Min(Count, Length_ - At));
}

size_t String::Find(const char *Needle, size_t From) const
{
    size_t N = strlen(Needle);
    if (N == 0)
        return From <= Length_ ? From : NotFound;
    for (size_t i = From; i + N <= Length_; i++)
        if (memcmp(Data_ + i, Needle, N) == 0)
            return i;
    return NotFound;
}

bool String::StartsWith(const char *Prefix) const
{
    size_t N = strlen(Prefix);
    return N <= Length_ && memcmp(CStr(), Prefix, N) == 0;
}

static char LowerAscii(char C)
{
    return (C >= 'A' && C <= 'Z') ? (char)(C + 32) : C;
}

bool String::ContainsIgnoreCase(const char *Needle) const
{
    size_t N = strlen(Needle);
    if (N == 0)
        return true;
    for (size_t i = 0; i + N <= Length_; i++) {
        size_t j = 0;
        while (j < N && LowerAscii(Data_[i + j]) == LowerAscii(Needle[j]))
            j++;
        if (j == N)
            return true;
    }
    return false;
}

String String::Format(const char *Format, ...)
{
    /* Most results fit the stack buffer; longer ones are formatted again at their full length
       (RtlFormatStringV reports it, like vsnprintf). */
    char Buffer[512];
    va_list Args;
    va_start(Args, Format);
    int Length = RtlFormatStringV(Buffer, sizeof(Buffer), Format, Args);
    va_end(Args);
    if (Length < (int)sizeof(Buffer))
        return String(Buffer, Length > 0 ? (size_t)Length : 0);
    String Out;
    Out.Reserve((size_t)Length + 1);
    va_start(Args, Format);
    RtlFormatStringV(Out.Data_, (size_t)Length + 1, Format, Args);
    va_end(Args);
    Out.Length_ = (size_t)Length;
    return Out;
}

size_t String::NextChar(size_t At) const
{
    if (At >= Length_)
        return Length_;
    At++;
    while (At < Length_ && ((uint8_t)Data_[At] & 0xC0) == 0x80)
        At++;
    return At;
}

size_t String::PreviousChar(size_t At) const
{
    if (At == 0)
        return 0;
    At = Min(At, Length_) - 1;
    while (At > 0 && ((uint8_t)Data_[At] & 0xC0) == 0x80)
        At--;
    return At;
}

uint32_t String::CodePointAt(size_t At) const
{
    if (At >= Length_)
        return 0;
    const uint8_t *P = (const uint8_t *)Data_ + At;
    uint32_t C = P[0];
    size_t End = NextChar(At);
    if (C < 0x80 || End - At == 1)
        return C;
    int Extra = (int)(End - At) - 1;
    C &= 0xFF >> (Extra + 2);
    for (int i = 1; i <= Extra; i++)
        C = (C << 6) | (P[i] & 0x3F);
    return C;
}

}  // namespace hz
