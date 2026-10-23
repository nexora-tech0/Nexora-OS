/*
 * Nexora OS - mm/sysva.c
 * Kernel virtual address allocator for the system VA region (MM_SYSTEM_VA_START, 256 GiB).
 * Free space is a sorted array of extents; allocation is first fit, freeing merges neighbours.
 * Every allocation is followed by one unmapped page so overruns fault instead of corrupting the
 * next mapping.
 */
#include "mi.h"

#define MI_MAX_EXTENTS 1024

typedef struct _MI_EXTENT {
    uint64_t Start; /* page aligned */
    uint64_t Pages;
} MI_EXTENT;

static MI_EXTENT MiExtents[MI_MAX_EXTENTS];
static uint32_t MiExtentCount;
static KSPIN_LOCK MiSystemVaLock;

void MiInitializeSystemVa(void)
{
    MiExtents[0].Start = MM_SYSTEM_VA_START;
    MiExtents[0].Pages = MM_SYSTEM_VA_SIZE >> PAGE_SHIFT;
    MiExtentCount = 1;
}

uint64_t MiAllocateSystemVa(uint64_t Pages)
{
    uint64_t Needed = Pages + 1; /* trailing guard gap */
    uint64_t Va = 0;
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiSystemVaLock, &OldIrql);
    for (uint32_t i = 0; i < MiExtentCount; i++) {
        if (MiExtents[i].Pages < Needed)
            continue;
        Va = MiExtents[i].Start;
        MiExtents[i].Start += Needed << PAGE_SHIFT;
        MiExtents[i].Pages -= Needed;
        if (MiExtents[i].Pages == 0) {
            memmove(&MiExtents[i], &MiExtents[i + 1], (MiExtentCount - i - 1) * sizeof(MI_EXTENT));
            MiExtentCount--;
        }
        break;
    }
    KeReleaseSpinLock(&MiSystemVaLock, OldIrql);
    return Va;
}

void MiFreeSystemVa(uint64_t Va, uint64_t Pages)
{
    uint64_t Count = Pages + 1;
    uint64_t End = Va + (Count << PAGE_SHIFT);
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiSystemVaLock, &OldIrql);

    uint32_t i = 0;
    while (i < MiExtentCount && MiExtents[i].Start < Va)
        i++;
    bool MergePrev = i > 0 && MiExtents[i - 1].Start + (MiExtents[i - 1].Pages << PAGE_SHIFT) == Va;
    bool MergeNext = i < MiExtentCount && MiExtents[i].Start == End;

    if (MergePrev && MergeNext) {
        MiExtents[i - 1].Pages += Count + MiExtents[i].Pages;
        memmove(&MiExtents[i], &MiExtents[i + 1], (MiExtentCount - i - 1) * sizeof(MI_EXTENT));
        MiExtentCount--;
    } else if (MergePrev) {
        MiExtents[i - 1].Pages += Count;
    } else if (MergeNext) {
        MiExtents[i].Start = Va;
        MiExtents[i].Pages += Count;
    } else if (MiExtentCount < MI_MAX_EXTENTS) {
        memmove(&MiExtents[i + 1], &MiExtents[i], (MiExtentCount - i) * sizeof(MI_EXTENT));
        MiExtents[i].Start = Va;
        MiExtents[i].Pages = Count;
        MiExtentCount++;
    } else {
        /* Too fragmented to track: leak the range rather than corrupt the table. */
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Mm: system VA extent table full; leaking %llu pages at 0x%llx\n",
                   (unsigned long long)Count, (unsigned long long)Va);
    }
    KeReleaseSpinLock(&MiSystemVaLock, OldIrql);
}

uint64_t MiSystemVaFreePages(void)
{
    uint64_t Pages = 0;
    for (uint32_t i = 0; i < MiExtentCount; i++)
        Pages += MiExtents[i].Pages;
    return Pages;
}
