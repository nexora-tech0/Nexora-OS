/*
 * Nexora OS - ke/symbols.c
 * Kernel symbol lookup for stop screens and logs. The table is the "symbols" boot module
 * (build/hzkrnl.sym, produced by tools/mksym.py from the linked kernel), so no second link
 * pass is needed. It is validated against a known function address before use.
 */
#include "ke.h"
#include "rtl.h"

typedef struct HZ_PACKED _SYM_HEADER {
    char Magic[8];
    uint32_t Count;
    uint32_t StringsSize;
    uint64_t TextStart;
    uint64_t TextEnd;
} SYM_HEADER;

typedef struct HZ_PACKED _SYM_ENTRY {
    uint64_t Address;
    uint32_t NameOffset;
    uint32_t Reserved;
} SYM_ENTRY;

static const SYM_HEADER *KiSymHeader;
static const SYM_ENTRY *KiSymEntries;
static const char *KiSymStrings;

bool KeLookupSymbol(uint64_t Address, const char **Name, uint64_t *Offset)
{
    if (!KiSymHeader || Address < KiSymHeader->TextStart || Address >= KiSymHeader->TextEnd)
        return false;
    uint32_t Lo = 0, Hi = KiSymHeader->Count;
    while (Lo < Hi) { /* first entry with Address > target */
        uint32_t Mid = Lo + (Hi - Lo) / 2;
        if (KiSymEntries[Mid].Address <= Address)
            Lo = Mid + 1;
        else
            Hi = Mid;
    }
    if (Lo == 0)
        return false;
    const SYM_ENTRY *E = &KiSymEntries[Lo - 1];
    *Name = KiSymStrings + E->NameOffset;
    *Offset = Address - E->Address;
    return true;
}

bool KeLoadSymbols(const void *Data, uint64_t Size)
{
    const SYM_HEADER *H = (const SYM_HEADER *)Data;
    if (!Data || Size < sizeof(*H) || memcmp(H->Magic, "HZSYMTB1", 8) != 0)
        return false;
    uint64_t Needed = sizeof(*H) + (uint64_t)H->Count * sizeof(SYM_ENTRY) + H->StringsSize;
    if (Needed > Size || H->StringsSize == 0)
        return false;

    KiSymHeader = H;
    KiSymEntries = (const SYM_ENTRY *)(H + 1);
    KiSymStrings = (const char *)(KiSymEntries + H->Count);

    /* The table must describe this very kernel image. */
    const char *Name;
    uint64_t Offset;
    if (!KeLookupSymbol((uint64_t)&KeLoadSymbols, &Name, &Offset) || Offset != 0 ||
        strcmp(Name, "KeLoadSymbols") != 0) {
        KiSymHeader = NULL;
        return false;
    }
    return true;
}

uint32_t KeSymbolCount(void)
{
    return KiSymHeader ? KiSymHeader->Count : 0;
}

void KeFormatAddress(char *Buffer, size_t Size, uint64_t Address)
{
    KeFormatStackFrame(Buffer, Size, Address, false);
}

void KeFormatStackFrame(char *Buffer, size_t Size, uint64_t Address, bool IsReturnAddress)
{
    /* A return address points just past the call and may already belong to the next
       function or label; look up the byte before it so the caller is named. */
    uint64_t Lookup = IsReturnAddress ? Address - 1 : Address;
    const char *Name;
    uint64_t Offset;
    if (KeLookupSymbol(Lookup, &Name, &Offset))
        RtlFormatString(Buffer, Size, "%s+0x%llx", Name, (unsigned long long)(Offset + (Address - Lookup)));
    else
        RtlFormatString(Buffer, Size, "0x%016llX", (unsigned long long)Address);
}
