/*
 * Nexora OS - rtl/string.c
 * Memory and string primitives. The mem* functions use `rep movsb/stosb` (fast on every CPU
 * with ERMS, correct everywhere) and are written in inline assembly so the optimizer cannot
 * turn their loops back into calls to themselves.
 */
#include "rtl.h"

int abs(int Value)
{
    return Value < 0 ? -Value : Value;
}

void *memcpy(void *Dest, const void *Src, size_t Count)
{
    void *Ret = Dest;
    __asm__ volatile("rep movsb" : "+D"(Dest), "+S"(Src), "+c"(Count) : : "memory");
    return Ret;
}

void *memmove(void *Dest, const void *Src, size_t Count)
{
    uint8_t *d = (uint8_t *)Dest;
    const uint8_t *s = (const uint8_t *)Src;
    if (d == s || Count == 0)
        return Dest;
    if (d < s || d >= s + Count)
        return memcpy(Dest, Src, Count);
    /* Overlapping with Dest above Src: copy backwards (direction flag set, then restored). */
    d += Count - 1;
    s += Count - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(Count) : : "memory");
    return Dest;
}

void *memset(void *Dest, int Value, size_t Count)
{
    void *Ret = Dest;
    __asm__ volatile("rep stosb" : "+D"(Dest), "+c"(Count) : "a"(Value) : "memory");
    return Ret;
}

int memcmp(const void *A, const void *B, size_t Count)
{
    const uint8_t *a = (const uint8_t *)A;
    const uint8_t *b = (const uint8_t *)B;
    for (size_t i = 0; i < Count; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *Str)
{
    const char *p = Str;
    while (*p)
        p++;
    return (size_t)(p - Str);
}

size_t strnlen(const char *Str, size_t Max)
{
    size_t n = 0;
    while (n < Max && Str[n])
        n++;
    return n;
}

int strcmp(const char *A, const char *B)
{
    while (*A && *A == *B) {
        A++;
        B++;
    }
    return (int)(uint8_t)*A - (int)(uint8_t)*B;
}

int strncmp(const char *A, const char *B, size_t Count)
{
    for (size_t i = 0; i < Count; i++) {
        if (A[i] != B[i] || A[i] == '\0')
            return (int)(uint8_t)A[i] - (int)(uint8_t)B[i];
    }
    return 0;
}

static inline int RtlpFoldCase(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

int _strnicmp(const char *A, const char *B, size_t Count)
{
    for (size_t i = 0; i < Count; i++) {
        int a = RtlpFoldCase((uint8_t)A[i]), b = RtlpFoldCase((uint8_t)B[i]);
        if (a != b || a == '\0')
            return a - b;
    }
    return 0;
}

int _stricmp(const char *A, const char *B)
{
    return _strnicmp(A, B, (size_t)-1);
}

char *strstr(const char *Haystack, const char *Needle)
{
    size_t n = strlen(Needle);
    if (n == 0)
        return (char *)Haystack;
    for (; *Haystack; Haystack++) {
        if (*Haystack == *Needle && strncmp(Haystack, Needle, n) == 0)
            return (char *)Haystack;
    }
    return NULL;
}

size_t RtlStringCopy(char *Dest, size_t Size, const char *Src)
{
    size_t Length = strlen(Src);
    if (Size == 0)
        return Length;
    size_t n = Length < Size - 1 ? Length : Size - 1;
    memcpy(Dest, Src, n);
    Dest[n] = '\0';
    return Length;
}
