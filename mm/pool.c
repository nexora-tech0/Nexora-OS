/*
 * Nexora OS - mm/pool.c
 * Kernel pool (ExAllocatePoolWithTag / ExFreePoolWithTag). It lives in mm/ because it is built
 * on the PFN database; the API belongs to the executive.
 *
 * Small blocks (block size <= 2048 bytes including the 16-byte header) come from slab pages,
 * one size class per page, accessed through the direct map. The slab's free list, class and
 * in-use count live in the page's PFN entry; the free list threads through the block headers.
 * Large blocks are whole pages: one page through the direct map, several pages mapped
 * contiguously in system VA (with an unmapped page after them).
 *
 * Every block starts with a POOL_HEADER whose Check field is a checksum over the header and
 * its address, so overwritten headers and double frees stop the system at the free that finds
 * them (BAD_POOL_HEADER / BAD_POOL_CALLER). Freed small blocks are filled with 0xDD.
 */
#include "mi.h"

typedef struct _POOL_HEADER {
    uint32_t Tag;       /* owner tag; for free blocks the offset of the next free block */
    uint32_t Size;      /* requested bytes */
    uint16_t Class;     /* size class, or POOL_CLASS_LARGE */
    uint16_t Flags;
    uint32_t Check;
} POOL_HEADER;

_Static_assert(sizeof(POOL_HEADER) == 16, "POOL_HEADER is 16 bytes");

#define POOL_MAGIC 0x506C5A48u /* "HZlP" */
#define POOL_FLAG_ALLOCATED 0x0001
#define POOL_FLAG_LARGE 0x0002
#define POOL_FLAG_FREE 0x0004
#define POOL_CLASS_LARGE 0xFFFF
#define POOL_NO_BLOCK 0xFFFF
#define POOL_FREE_FILL 0xDD
#define POOL_MAX_TAGS 96

static const uint16_t PoolClassSize[] = {32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048};
#define POOL_CLASS_COUNT ((int)HZ_ARRAYSIZE(PoolClassSize))

typedef struct _POOL_CLASS {
    uint32_t PartialHead; /* slabs with at least one free block (PFN list) */
    uint32_t EmptySlabs;  /* fully free slabs kept on the partial list (at most 1) */
} POOL_CLASS;

typedef struct _POOL_TAG_STATS {
    uint32_t Tag;
    uint64_t Allocations;
    uint64_t Frees;
    uint64_t BytesInUse;
} POOL_TAG_STATS;

static POOL_CLASS PoolClasses[POOL_CLASS_COUNT];
static POOL_TAG_STATS PoolTags[POOL_MAX_TAGS];
static POOL_STATISTICS PoolStats;
static KSPIN_LOCK PoolLock;
static bool PoolReady;

static uint32_t PoolChecksum(const POOL_HEADER *H)
{
    return POOL_MAGIC ^ H->Tag ^ H->Size ^ (((uint32_t)H->Class << 16) | H->Flags) ^
           (uint32_t)((uint64_t)H >> 4) ^ (uint32_t)((uint64_t)H >> 36);
}

static void PoolSeal(POOL_HEADER *H)
{
    H->Check = PoolChecksum(H);
}

static void PoolFormatTag(char Out[5], uint32_t Tag)
{
    for (int i = 0; i < 4; i++) {
        char c = (char)(Tag >> (8 * i));
        Out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    Out[4] = '\0';
}

HZ_NORETURN static void PoolCorrupted(const POOL_HEADER *H)
{
    static char Message[128];
    char Tag[5];
    PoolFormatTag(Tag, H->Tag);
    RtlFormatString(Message, sizeof(Message),
                    "Pool block 0x%016llX has a corrupted header (tag '%s', size field %u)",
                    (unsigned long long)(H + 1), Tag, H->Size);
    KeBugCheckExMessage(BAD_POOL_HEADER, (uint64_t)(H + 1), H->Check, PoolChecksum(H),
                        (uint64_t)__builtin_return_address(0), Message);
}

HZ_NORETURN static void PoolBadCaller(uint32_t Reason, uint64_t Block, uint64_t Detail, uint64_t Extra)
{
    static char Message[128];
    const char *What = Reason == POOL_CALLER_DOUBLE_FREE    ? "was freed twice"
                       : Reason == POOL_CALLER_TAG_MISMATCH ? "was freed with the wrong tag"
                       : Reason == POOL_CALLER_BAD_IRQL     ? "was used above DISPATCH_LEVEL"
                       : Reason == POOL_CALLER_NOT_POOL     ? "is not a pool address"
                                                            : "was used before the pool was initialized";
    RtlFormatString(Message, sizeof(Message), "Pool block 0x%016llX %s", (unsigned long long)Block, What);
    KeBugCheckExMessage(BAD_POOL_CALLER, Reason, Block, Detail, Extra, Message);
}

static void PoolAccount(uint32_t Tag, int64_t Bytes, bool Allocation)
{
    POOL_TAG_STATS *Slot = NULL;
    for (int i = 0; i < POOL_MAX_TAGS; i++) {
        if (PoolTags[i].Tag == Tag || PoolTags[i].Tag == 0) {
            Slot = &PoolTags[i];
            break;
        }
    }
    if (Slot) {
        Slot->Tag = Tag;
        if (Allocation)
            Slot->Allocations++;
        else
            Slot->Frees++;
        Slot->BytesInUse += (uint64_t)Bytes;
    }
    if (Allocation)
        PoolStats.Allocations++;
    else
        PoolStats.Frees++;
    PoolStats.BytesInUse += (uint64_t)Bytes;
}

static int PoolClassFor(size_t Block)
{
    for (int c = 0; c < POOL_CLASS_COUNT; c++) {
        if (Block <= PoolClassSize[c])
            return c;
    }
    return -1;
}

/* ---- Partial-slab lists (threaded through PFN Next/Prev) ----------------------------------------- */

static void PoolPushSlab(POOL_CLASS *Class, PFN_NUMBER Pfn)
{
    MMPFN *Entry = MiGetPfn(Pfn);
    Entry->Prev = MI_PFN_LIST_END;
    Entry->Next = Class->PartialHead;
    if (Class->PartialHead != MI_PFN_LIST_END)
        MiGetPfn(Class->PartialHead)->Prev = (uint32_t)Pfn;
    Class->PartialHead = (uint32_t)Pfn;
}

static void PoolUnlinkSlab(POOL_CLASS *Class, PFN_NUMBER Pfn)
{
    MMPFN *Entry = MiGetPfn(Pfn);
    if (Entry->Prev != MI_PFN_LIST_END)
        MiGetPfn(Entry->Prev)->Next = Entry->Next;
    else
        Class->PartialHead = Entry->Next;
    if (Entry->Next != MI_PFN_LIST_END)
        MiGetPfn(Entry->Next)->Prev = Entry->Prev;
    Entry->Next = Entry->Prev = MI_PFN_LIST_END;
}

static bool PoolNewSlab(int ClassIndex)
{
    PFN_NUMBER Pfn = MmAllocatePhysicalPage(false);
    if (Pfn == MM_INVALID_PFN)
        return false;
    MiSetPfnState(Pfn, MiPfnPool);
    uint8_t *Page = (uint8_t *)MmPfnToVirtual(Pfn);
    uint16_t Size = PoolClassSize[ClassIndex];
    uint16_t Count = (uint16_t)(PAGE_SIZE / Size);
    for (uint16_t i = 0; i < Count; i++) {
        POOL_HEADER *H = (POOL_HEADER *)(Page + i * Size);
        H->Tag = (i + 1 < Count) ? (uint32_t)((i + 1) * Size) : POOL_NO_BLOCK;
        H->Size = 0;
        H->Class = (uint16_t)ClassIndex;
        H->Flags = POOL_FLAG_FREE;
        PoolSeal(H);
    }
    MMPFN *Entry = MiGetPfn(Pfn);
    Entry->Pool.FreeHead = 0;
    Entry->Pool.Class = (uint8_t)ClassIndex;
    Entry->Pool.InUse = 0;
    PoolPushSlab(&PoolClasses[ClassIndex], Pfn);
    PoolClasses[ClassIndex].EmptySlabs++;
    PoolStats.SlabPages++;
    return true;
}

/* ---- Small blocks ---------------------------------------------------------------------------------- */

static void *PoolAllocateSmall(int ClassIndex, size_t Size, uint32_t Tag)
{
    POOL_CLASS *Class = &PoolClasses[ClassIndex];
    KIRQL OldIrql;
    KeAcquireSpinLock(&PoolLock, &OldIrql);
    if (Class->PartialHead == MI_PFN_LIST_END && !PoolNewSlab(ClassIndex)) {
        KeReleaseSpinLock(&PoolLock, OldIrql);
        return NULL;
    }
    PFN_NUMBER Pfn = Class->PartialHead;
    MMPFN *Entry = MiGetPfn(Pfn);
    uint8_t *Page = (uint8_t *)MmPfnToVirtual(Pfn);
    POOL_HEADER *H = (POOL_HEADER *)(Page + Entry->Pool.FreeHead);
    if (H->Check != PoolChecksum(H) || !(H->Flags & POOL_FLAG_FREE))
        PoolCorrupted(H);

    if (Entry->Pool.InUse == 0)
        Class->EmptySlabs--;
    Entry->Pool.FreeHead = (uint16_t)H->Tag;
    Entry->Pool.InUse++;
    if (Entry->Pool.FreeHead == POOL_NO_BLOCK)
        PoolUnlinkSlab(Class, Pfn); /* slab is now full */

    H->Tag = Tag;
    H->Size = (uint32_t)Size;
    H->Flags = POOL_FLAG_ALLOCATED;
    PoolSeal(H);
    PoolAccount(Tag, (int64_t)Size, true);
    KeReleaseSpinLock(&PoolLock, OldIrql);
    return H + 1;
}

static void PoolFreeSmall(POOL_HEADER *H)
{
    PFN_NUMBER Pfn = ((uint64_t)H - KeLoaderBlock.HhdmOffset) >> PAGE_SHIFT;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PoolLock, &OldIrql);
    MMPFN *Entry = MiGetPfn(Pfn);
    if (Entry->State != MiPfnPool || Entry->Pool.Class != H->Class)
        PoolCorrupted(H);
    POOL_CLASS *Class = &PoolClasses[H->Class];
    uint16_t Offset = (uint16_t)((uint64_t)H & (PAGE_SIZE - 1));

    PoolAccount(H->Tag, -(int64_t)H->Size, false);
    memset(H + 1, POOL_FREE_FILL, PoolClassSize[H->Class] - sizeof(POOL_HEADER));
    bool WasFull = Entry->Pool.FreeHead == POOL_NO_BLOCK;
    H->Tag = Entry->Pool.FreeHead;
    H->Size = 0;
    H->Flags = POOL_FLAG_FREE;
    PoolSeal(H);
    Entry->Pool.FreeHead = Offset;
    Entry->Pool.InUse--;
    if (WasFull)
        PoolPushSlab(Class, Pfn);

    if (Entry->Pool.InUse == 0) {
        if (Class->EmptySlabs >= 1) {
            /* Keep one empty slab per class to avoid thrashing; return the rest. */
            PoolUnlinkSlab(Class, Pfn);
            PoolStats.SlabPages--;
            KeReleaseSpinLock(&PoolLock, OldIrql);
            MmFreePhysicalPage(Pfn);
            return;
        }
        Class->EmptySlabs++;
    }
    KeReleaseSpinLock(&PoolLock, OldIrql);
}

/* ---- Large blocks ------------------------------------------------------------------------------- */

static void *PoolAllocateLarge(size_t Size, uint32_t Tag)
{
    uint64_t Pages = BYTES_TO_PAGES(Size + sizeof(POOL_HEADER));
    POOL_HEADER *H;
    if (Pages == 1) {
        PFN_NUMBER Pfn = MmAllocatePhysicalPage(false);
        if (Pfn == MM_INVALID_PFN)
            return NULL;
        MiSetPfnState(Pfn, MiPfnPool);
        MiGetPfn(Pfn)->Pool.Class = MI_POOL_CLASS_LARGE;
        H = (POOL_HEADER *)MmPfnToVirtual(Pfn);
    } else {
        uint64_t Va = MiAllocateSystemVa(Pages);
        if (!Va)
            return NULL;
        uint64_t Flags = MiProtectionToPte(MM_PROT_READWRITE, false);
        KIRQL OldIrql;
        KeAcquireSpinLock(&MmKernelAddressSpace.Lock, &OldIrql);
        for (uint64_t i = 0; i < Pages; i++) {
            PFN_NUMBER Pfn = MmAllocatePhysicalPage(false);
            if (Pfn == MM_INVALID_PFN) {
                for (uint64_t j = 0; j < i; j++) {
                    uint64_t Old = MiUnmapPage(&MmKernelAddressSpace, Va + (j << PAGE_SHIFT));
                    MmFreePhysicalPage((Old & PTE_ADDRESS_MASK) >> PAGE_SHIFT);
                }
                KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
                MiFreeSystemVa(Va, Pages);
                return NULL;
            }
            MiSetPfnState(Pfn, MiPfnPool);
            MiGetPfn(Pfn)->Pool.Class = MI_POOL_CLASS_LARGE;
            MiMapPage(&MmKernelAddressSpace, Va + (i << PAGE_SHIFT), Pfn, Flags);
        }
        KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
        H = (POOL_HEADER *)Va;
    }
    H->Tag = Tag;
    H->Size = (uint32_t)Size;
    H->Class = POOL_CLASS_LARGE;
    H->Flags = POOL_FLAG_ALLOCATED | POOL_FLAG_LARGE;
    PoolSeal(H);

    KIRQL OldIrql;
    KeAcquireSpinLock(&PoolLock, &OldIrql);
    PoolAccount(Tag, (int64_t)Size, true);
    PoolStats.LargePages += Pages;
    KeReleaseSpinLock(&PoolLock, OldIrql);
    return H + 1;
}

static void PoolFreeLarge(POOL_HEADER *H)
{
    uint64_t Pages = BYTES_TO_PAGES(H->Size + sizeof(POOL_HEADER));
    KIRQL OldIrql;
    KeAcquireSpinLock(&PoolLock, &OldIrql);
    PoolAccount(H->Tag, -(int64_t)H->Size, false);
    PoolStats.LargePages -= Pages;
    KeReleaseSpinLock(&PoolLock, OldIrql);

    H->Flags = POOL_FLAG_FREE;
    PoolSeal(H);
    uint64_t Va = (uint64_t)H;
    if (MiIsDirectMapAddress(Va)) {
        MmFreePhysicalPage((Va - KeLoaderBlock.HhdmOffset) >> PAGE_SHIFT);
        return;
    }
    KeAcquireSpinLock(&MmKernelAddressSpace.Lock, &OldIrql);
    for (uint64_t i = 0; i < Pages; i++) {
        uint64_t Old = MiUnmapPage(&MmKernelAddressSpace, Va + (i << PAGE_SHIFT));
        MmFreePhysicalPage((Old & PTE_ADDRESS_MASK) >> PAGE_SHIFT);
    }
    KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
    MiFreeSystemVa(Va, Pages);
}

/* ---- Public API --------------------------------------------------------------------------------- */

void ExInitializePool(void)
{
    for (int c = 0; c < POOL_CLASS_COUNT; c++)
        PoolClasses[c].PartialHead = MI_PFN_LIST_END;
    PoolReady = true;
}

void *ExAllocatePoolWithTag(POOL_TYPE Type, size_t Size, uint32_t Tag)
{
    if (!PoolReady)
        PoolBadCaller(POOL_CALLER_BEFORE_INIT, Size, Tag, 0);
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
        PoolBadCaller(POOL_CALLER_BAD_IRQL, Size, KeGetCurrentIrql(), Tag);
    if (Type != NonPagedPool || Size > (1ull << 30))
        return NULL;
    if (Size == 0)
        Size = 1;
    int Class = PoolClassFor(Size + sizeof(POOL_HEADER));
    return Class >= 0 ? PoolAllocateSmall(Class, Size, Tag) : PoolAllocateLarge(Size, Tag);
}

void *ExAllocatePoolZero(POOL_TYPE Type, size_t Size, uint32_t Tag)
{
    void *P = ExAllocatePoolWithTag(Type, Size, Tag);
    if (P)
        memset(P, 0, Size);
    return P;
}

void ExFreePoolWithTag(void *Pointer, uint32_t Tag)
{
    if (!Pointer)
        return;
    uint64_t Va = (uint64_t)Pointer;
    if ((Va & 15) != 0 || !(MiIsDirectMapAddress(Va) || (Va >= MM_SYSTEM_VA_START && Va < MM_SYSTEM_VA_END)))
        PoolBadCaller(POOL_CALLER_NOT_POOL, Va, Tag, 0);
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
        PoolBadCaller(POOL_CALLER_BAD_IRQL, Va, KeGetCurrentIrql(), Tag);

    POOL_HEADER *H = (POOL_HEADER *)Pointer - 1;
    if (H->Check != PoolChecksum(H))
        PoolCorrupted(H);
    if (!(H->Flags & POOL_FLAG_ALLOCATED))
        PoolBadCaller(POOL_CALLER_DOUBLE_FREE, Va, H->Flags, 0);
    if (Tag != 0 && H->Tag != Tag)
        PoolBadCaller(POOL_CALLER_TAG_MISMATCH, Va, H->Tag, Tag);

    if (H->Flags & POOL_FLAG_LARGE)
        PoolFreeLarge(H);
    else if (H->Class < POOL_CLASS_COUNT)
        PoolFreeSmall(H);
    else
        PoolCorrupted(H);
}

void ExFreePool(void *Pointer)
{
    ExFreePoolWithTag(Pointer, 0);
}

void ExQueryPoolStatistics(POOL_STATISTICS *Stats)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&PoolLock, &OldIrql);
    *Stats = PoolStats;
    KeReleaseSpinLock(&PoolLock, OldIrql);
}

void ExDumpPoolUsage(uint32_t MaxTags)
{
    POOL_TAG_STATS Copy[POOL_MAX_TAGS];
    POOL_STATISTICS Totals;
    KIRQL OldIrql;
    KeAcquireSpinLock(&PoolLock, &OldIrql);
    memcpy(Copy, PoolTags, sizeof(Copy));
    Totals = PoolStats;
    KeReleaseSpinLock(&PoolLock, OldIrql);

    DbgPrintEx(DPFLTR_TRACE_LEVEL,
               "Pool: %llu bytes in use; %llu slab pages, %llu large pages; %llu allocations, %llu frees\n",
               (unsigned long long)Totals.BytesInUse, (unsigned long long)Totals.SlabPages,
               (unsigned long long)Totals.LargePages, (unsigned long long)Totals.Allocations,
               (unsigned long long)Totals.Frees);

    for (uint32_t n = 0; n < MaxTags; n++) {
        int Best = -1;
        for (int i = 0; i < POOL_MAX_TAGS; i++) {
            if (Copy[i].Tag && Copy[i].BytesInUse && (Best < 0 || Copy[i].BytesInUse > Copy[Best].BytesInUse))
                Best = i;
        }
        if (Best < 0)
            break;
        char Tag[5];
        memcpy(Tag, &Copy[Best].Tag, 4);
        Tag[4] = '\0';
        DbgPrintEx(DPFLTR_TRACE_LEVEL, "Pool: tag '%s' %llu bytes in use (%llu allocations, %llu frees)\n", Tag,
                   (unsigned long long)Copy[Best].BytesInUse, (unsigned long long)Copy[Best].Allocations,
                   (unsigned long long)Copy[Best].Frees);
        Copy[Best].BytesInUse = 0;
    }
}
