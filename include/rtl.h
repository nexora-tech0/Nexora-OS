/*
 * Nexora OS - include/rtl.h
 * Run-time library (Rtl*): memory/string helpers, formatted output, time conversion.
 * The compiler may emit calls to memcpy/memmove/memset/memcmp, so those keep their C names.
 */
#ifndef RTL_H
#define RTL_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

/* ---- Intrusive doubly linked lists (circular, with a sentinel head) ----------------------- */

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY;

#define CONTAINING_RECORD(Address, Type, Field) ((Type *)((uint8_t *)(Address) - offsetof(Type, Field)))

static inline void InitializeListHead(LIST_ENTRY *Head)
{
    Head->Flink = Head->Blink = Head;
}

static inline bool IsListEmpty(const LIST_ENTRY *Head)
{
    return Head->Flink == Head;
}

static inline void InsertTailList(LIST_ENTRY *Head, LIST_ENTRY *Entry)
{
    LIST_ENTRY *Last = Head->Blink;
    Entry->Flink = Head;
    Entry->Blink = Last;
    Last->Flink = Entry;
    Head->Blink = Entry;
}

static inline void InsertHeadList(LIST_ENTRY *Head, LIST_ENTRY *Entry)
{
    LIST_ENTRY *First = Head->Flink;
    Entry->Flink = First;
    Entry->Blink = Head;
    First->Blink = Entry;
    Head->Flink = Entry;
}

/* Returns true if the list became empty. The entry is left self-linked. */
static inline bool RemoveEntryList(LIST_ENTRY *Entry)
{
    LIST_ENTRY *Next = Entry->Flink, *Prev = Entry->Blink;
    Prev->Flink = Next;
    Next->Blink = Prev;
    Entry->Flink = Entry->Blink = Entry;
    return Next == Prev;
}

static inline LIST_ENTRY *RemoveHeadList(LIST_ENTRY *Head)
{
    LIST_ENTRY *Entry = Head->Flink;
    RemoveEntryList(Entry);
    return Entry;
}

/* ---- Memory and strings ------------------------------------------------------------------- */

void *memcpy(void *Dest, const void *Src, size_t Count);
void *memmove(void *Dest, const void *Src, size_t Count);
void *memset(void *Dest, int Value, size_t Count);
int memcmp(const void *A, const void *B, size_t Count);
size_t strlen(const char *Str);
size_t strnlen(const char *Str, size_t Max);
int strcmp(const char *A, const char *B);
int strncmp(const char *A, const char *B, size_t Count);
char *strstr(const char *Haystack, const char *Needle);
/* ASCII case-insensitive comparisons (object and file names). */
int _stricmp(const char *A, const char *B);
int _strnicmp(const char *A, const char *B, size_t Count);

/* Copies at most Size-1 characters and always NUL-terminates (if Size > 0). */
size_t RtlStringCopy(char *Dest, size_t Size, const char *Src);

/*
 * printf-style formatting. Supports: %% %c %s %d %i %u %x %X %o %p %b, flags '-' '0' '+' ' '
 * '#', width and precision (also '*'), length modifiers hh h l ll z j t.
 * Returns the length the full output would have had (like vsnprintf); output is truncated
 * to fit and always NUL-terminated when Size > 0.
 */
int RtlFormatStringV(char *Buffer, size_t Size, const char *Format, va_list Args);
int RtlFormatString(char *Buffer, size_t Size, const char *Format, ...) HZ_PRINTF(3, 4);

/* Formats a byte count as "512 bytes", "12.5 KiB", "2047 MiB", "3.8 GiB". */
void RtlFormatByteSize(char *Buffer, size_t Size, uint64_t Bytes);

typedef struct _TIME_FIELDS {
    uint16_t Year;
    uint8_t Month;   /* 1..12 */
    uint8_t Day;     /* 1..31 */
    uint8_t Hour;    /* 0..23 */
    uint8_t Minute;  /* 0..59 */
    uint8_t Second;  /* 0..59 */
    uint8_t Weekday; /* 0 = Sunday */
} TIME_FIELDS;

/* Converts seconds since 1970-01-01T00:00:00Z to calendar fields (proleptic Gregorian, UTC). */
void RtlUnixTimeToTimeFields(int64_t UnixSeconds, TIME_FIELDS *Fields);
int64_t RtlTimeFieldsToUnixTime(const TIME_FIELDS *Fields);
/* HZ time: 100 ns units since 1601-01-01 UTC (uapi/hzapi.h). */
void RtlTimeToTimeFields(uint64_t Time, TIME_FIELDS *Fields);
uint64_t RtlTimeFieldsToTime(const TIME_FIELDS *Fields);

/* UTF-16 (Count units) -> NUL-terminated UTF-8; returns the length or -1 if it does not fit. */
int RtlUtf16ToUtf8(const uint16_t *Source, size_t Count, char *Destination, size_t Capacity);
/* NUL-terminated UTF-8 -> UTF-16 (no terminator); returns the unit count or -1 (invalid/too long). */
int RtlUtf8ToUtf16(const char *Source, uint16_t *Destination, size_t Capacity);
/* CRC-32 (IEEE 802.3, as used by GPT and zlib); start with Crc = 0. */
uint32_t RtlComputeCrc32(uint32_t Crc, const void *Buffer, size_t Length);

/* Runs the C++ global constructors listed in .init_array. Called once, early in boot. */
void RtlRunStaticInitializers(void);

HZ_EXTERN_C_END

#endif /* RTL_H */
