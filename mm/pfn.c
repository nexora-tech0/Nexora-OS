/*
 * Nexora OS - mm/pfn.c
 * The PFN database and the physical page allocator.
 *
 * One 16-byte MMPFN per physical page up to the highest RAM address, stored in a contiguous
 * run carved out of usable RAM and reached through the direct map (no mapping needed before
 * the memory manager exists). Free and zeroed pages sit on doubly linked lists threaded
 * through the PFN entries by 32-bit index. The first 1 MiB stays reserved (SMP trampolines,
 * firmware leftovers).
 */
#include "mi.h"

#define MI_LOW_MEMORY_PAGES 256 /* 1 MiB */

typedef struct _MI_PFN_LIST {
    uint32_t Head;
    uint64_t Count;
} MI_PFN_LIST;

MMPFN *MiPfnDatabase;
PFN_NUMBER MiPfnCount;
KSPIN_LOCK MiPfnLock;
uint64_t MiReclaimedPages;
static MI_PFN_LIST MiFreeList = {MI_PFN_LIST_END, 0};
static MI_PFN_LIST MiZeroedList = {MI_PFN_LIST_END, 0};
static PFN_NUMBER MiDatabaseFirstPfn, MiDatabasePages;
static uint64_t MiTotalPages;

static MI_PFN_LIST *MiListForState(uint8_t State)
{
    return State == MiPfnZeroed ? &MiZeroedList : State == MiPfnFree ? &MiFreeList : NULL;
}

static void MiInsertList(MI_PFN_LIST *List, PFN_NUMBER Pfn, MI_PFN_STATE State)
{
    MMPFN *Entry = MiGetPfn(Pfn);
    Entry->State = (uint8_t)State;
    Entry->ShareCount = 0;
    Entry->Prev = MI_PFN_LIST_END;
    Entry->Next = List->Head;
    if (List->Head != MI_PFN_LIST_END)
        MiGetPfn(List->Head)->Prev = (uint32_t)Pfn;
    List->Head = (uint32_t)Pfn;
    List->Count++;
}

static void MiUnlinkList(MI_PFN_LIST *List, PFN_NUMBER Pfn)
{
    MMPFN *Entry = MiGetPfn(Pfn);
    if (Entry->Prev != MI_PFN_LIST_END)
        MiGetPfn(Entry->Prev)->Next = Entry->Next;
    else
        List->Head = Entry->Next;
    if (Entry->Next != MI_PFN_LIST_END)
        MiGetPfn(Entry->Next)->Prev = Entry->Prev;
    Entry->Next = Entry->Prev = MI_PFN_LIST_END;
    List->Count--;
}

void *MmPfnToVirtual(PFN_NUMBER Pfn)
{
    return MiDirect(Pfn << PAGE_SHIFT);
}

void *MmPhysicalToVirtual(uint64_t Physical)
{
    return MiDirect(Physical);
}

/* ---- Bring-up ---------------------------------------------------------------------------------- */

static bool MiIsManagedRam(MEMORY_TYPE Type)
{
    return Type == MemoryUsable || Type == MemoryLoaderReclaimable || Type == MemoryKernelAndModules;
}

void MiInitializePfnDatabase(void)
{
    const LOADER_BLOCK *Lb = &KeLoaderBlock;
    MiPfnCount = Lb->HighestPhysicalAddress >> PAGE_SHIFT;
    MiDatabasePages = BYTES_TO_PAGES(MiPfnCount * sizeof(MMPFN));

    /* Carve the database from the largest usable range above 1 MiB. */
    uint64_t BestBase = 0, BestLength = 0;
    for (uint32_t i = 0; i < Lb->MemoryDescriptorCount; i++) {
        const MEMORY_DESCRIPTOR *D = &Lb->MemoryDescriptors[i];
        if (D->Type != MemoryUsable)
            continue;
        uint64_t Base = HZ_MAX(HZ_ALIGN_UP(D->Base, PAGE_SIZE), (uint64_t)MI_LOW_MEMORY_PAGES << PAGE_SHIFT);
        uint64_t End = HZ_ALIGN_DOWN(D->Base + D->Length, PAGE_SIZE);
        if (End > Base && End - Base > BestLength) {
            BestBase = Base;
            BestLength = End - Base;
        }
    }
    if (BestLength < (MiDatabasePages << PAGE_SHIFT))
        KeBugCheckEx(MEMORY_MANAGEMENT, MM_FAILURE_NO_PFN_DATABASE_SPACE, MiDatabasePages, BestLength, 0);

    MiDatabaseFirstPfn = BestBase >> PAGE_SHIFT;
    MiPfnDatabase = (MMPFN *)MiDirect(BestBase);
    memset(MiPfnDatabase, 0, MiDatabasePages << PAGE_SHIFT); /* every entry: MiPfnReserved */

    for (uint32_t i = 0; i < Lb->MemoryDescriptorCount; i++) {
        const MEMORY_DESCRIPTOR *D = &Lb->MemoryDescriptors[i];
        if (!MiIsManagedRam(D->Type))
            continue;
        PFN_NUMBER First = HZ_ALIGN_UP(D->Base, PAGE_SIZE) >> PAGE_SHIFT;
        PFN_NUMBER End = HZ_MIN((D->Base + D->Length) >> PAGE_SHIFT, MiPfnCount);
        for (PFN_NUMBER Pfn = First; Pfn < End; Pfn++) {
            MiTotalPages++;
            if (Pfn < MI_LOW_MEMORY_PAGES) {
                MiGetPfn(Pfn)->State = MiPfnReserved;
                MiTotalPages--;
            } else if (Pfn >= MiDatabaseFirstPfn && Pfn < MiDatabaseFirstPfn + MiDatabasePages) {
                MiGetPfn(Pfn)->State = MiPfnDatabasePage;
            } else if (D->Type == MemoryUsable) {
                MiInsertList(&MiFreeList, Pfn, MiPfnFree);
            } else if (D->Type == MemoryLoaderReclaimable) {
                MiGetPfn(Pfn)->State = MiPfnLoaderReclaimable;
            } else {
                MiGetPfn(Pfn)->State = MiPfnKernelImage;
            }
        }
    }
}

void MiReclaimLoaderMemory(void)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiPfnLock, &OldIrql);
    for (PFN_NUMBER Pfn = MI_LOW_MEMORY_PAGES; Pfn < MiPfnCount; Pfn++) {
        if (MiGetPfn(Pfn)->State == MiPfnLoaderReclaimable) {
            MiInsertList(&MiFreeList, Pfn, MiPfnFree);
            MiReclaimedPages++;
        }
    }
    KeReleaseSpinLock(&MiPfnLock, OldIrql);
}

/* ---- Allocation --------------------------------------------------------------------------------- */

PFN_NUMBER MmAllocatePhysicalPage(bool Zero)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiPfnLock, &OldIrql);
    MI_PFN_LIST *List = NULL;
    if (Zero && MiZeroedList.Count)
        List = &MiZeroedList;
    else if (MiFreeList.Count)
        List = &MiFreeList;
    else if (MiZeroedList.Count)
        List = &MiZeroedList;
    if (!List) {
        KeReleaseSpinLock(&MiPfnLock, OldIrql);
        return MM_INVALID_PFN;
    }
    PFN_NUMBER Pfn = List->Head;
    bool NeedsZero = Zero && List == &MiFreeList;
    MiUnlinkList(List, Pfn);
    MMPFN *Entry = MiGetPfn(Pfn);
    Entry->State = MiPfnActive;
    Entry->ShareCount = 1;
    KeReleaseSpinLock(&MiPfnLock, OldIrql);

    if (NeedsZero)
        memset(MmPfnToVirtual(Pfn), 0, PAGE_SIZE);
    return Pfn;
}

static void MiFreePageLocked(PFN_NUMBER Pfn, void *Caller)
{
    MMPFN *Entry = Pfn < MiPfnCount ? MiGetPfn(Pfn) : NULL;
    if (!Entry || !(Entry->State == MiPfnActive || Entry->State == MiPfnPageTable || Entry->State == MiPfnPool))
        KeBugCheckEx(PFN_LIST_CORRUPT, Pfn, Entry ? Entry->State : 0xFF, (uint64_t)Caller, 0);
    MiInsertList(&MiFreeList, Pfn, MiPfnFree);
}

void MmFreePhysicalPage(PFN_NUMBER Pfn)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiPfnLock, &OldIrql);
    MiFreePageLocked(Pfn, __builtin_return_address(0));
    KeReleaseSpinLock(&MiPfnLock, OldIrql);
}

STATUS MmAllocateContiguousPages(uint64_t Count, uint64_t MaxPhysical, uint64_t AlignmentPages,
                                 PFN_NUMBER *FirstPfn)
{
    if (Count == 0)
        return STATUS_INVALID_PARAMETER;
    if (AlignmentPages == 0)
        AlignmentPages = 1;
    PFN_NUMBER Limit = HZ_MIN(MaxPhysical >> PAGE_SHIFT, MiPfnCount);

    KIRQL OldIrql;
    KeAcquireSpinLock(&MiPfnLock, &OldIrql);
    PFN_NUMBER Start = HZ_ALIGN_UP((PFN_NUMBER)MI_LOW_MEMORY_PAGES, AlignmentPages);
    while (Start + Count <= Limit) {
        PFN_NUMBER Bad = MM_INVALID_PFN;
        for (PFN_NUMBER p = Start; p < Start + Count; p++) {
            uint8_t State = MiGetPfn(p)->State;
            if (State != MiPfnFree && State != MiPfnZeroed) {
                Bad = p;
                break;
            }
        }
        if (Bad == MM_INVALID_PFN) {
            for (PFN_NUMBER p = Start; p < Start + Count; p++) {
                MiUnlinkList(MiListForState(MiGetPfn(p)->State), p);
                MiGetPfn(p)->State = MiPfnActive;
                MiGetPfn(p)->ShareCount = 1;
            }
            KeReleaseSpinLock(&MiPfnLock, OldIrql);
            *FirstPfn = Start;
            return STATUS_SUCCESS;
        }
        Start = HZ_ALIGN_UP(Bad + 1, AlignmentPages);
    }
    KeReleaseSpinLock(&MiPfnLock, OldIrql);
    return STATUS_NO_MEMORY;
}

void MmFreeContiguousPages(PFN_NUMBER FirstPfn, uint64_t Count)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiPfnLock, &OldIrql);
    for (uint64_t i = 0; i < Count; i++)
        MiFreePageLocked(FirstPfn + i, __builtin_return_address(0));
    KeReleaseSpinLock(&MiPfnLock, OldIrql);
}

PFN_NUMBER MiAllocatePageTablePage(void)
{
    PFN_NUMBER Pfn = MmAllocatePhysicalPage(true);
    if (Pfn == MM_INVALID_PFN)
        KeBugCheckEx(MEMORY_MANAGEMENT, MM_FAILURE_PAGE_TABLE_ALLOCATION, 0, 0, 0);
    MiSetPfnState(Pfn, MiPfnPageTable);
    return Pfn;
}

void MiFreePageTablePage(PFN_NUMBER Pfn)
{
    MmFreePhysicalPage(Pfn);
}

void MiSetPfnState(PFN_NUMBER Pfn, MI_PFN_STATE State)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiPfnLock, &OldIrql);
    MiGetPfn(Pfn)->State = (uint8_t)State;
    KeReleaseSpinLock(&MiPfnLock, OldIrql);
}

uint64_t MiCountPagesInState(MI_PFN_STATE State)
{
    uint64_t Count = 0;
    for (PFN_NUMBER Pfn = 0; Pfn < MiPfnCount; Pfn++)
        Count += MiGetPfn(Pfn)->State == State;
    return Count;
}

/* ---- Zero-page thread support (mm/zero.c) ------------------------------------------------- */

PFN_NUMBER MiRemoveFreePageForZeroing(void)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiPfnLock, &OldIrql);
    PFN_NUMBER Pfn = MM_INVALID_PFN;
    if (MiFreeList.Count) {
        Pfn = MiFreeList.Head;
        MiUnlinkList(&MiFreeList, Pfn);
        MiGetPfn(Pfn)->State = MiPfnActive; /* off both lists while it is being zeroed */
    }
    KeReleaseSpinLock(&MiPfnLock, OldIrql);
    return Pfn;
}

void MiInsertZeroedPage(PFN_NUMBER Pfn)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&MiPfnLock, &OldIrql);
    MiInsertList(&MiZeroedList, Pfn, MiPfnZeroed);
    KeReleaseSpinLock(&MiPfnLock, OldIrql);
}

uint64_t MiZeroedPageCount(void)
{
    return MiZeroedList.Count;
}

uint64_t MmQueryAvailablePages(void)
{
    return MiFreeList.Count + MiZeroedList.Count;
}

void MmQueryStatistics(MM_STATISTICS *Stats)
{
    memset(Stats, 0, sizeof(*Stats));
    Stats->TotalPages = MiTotalPages;
    Stats->AvailablePages = MiFreeList.Count + MiZeroedList.Count;
    Stats->ZeroedPages = MiZeroedList.Count;
    Stats->KernelImagePages = MiCountPagesInState(MiPfnKernelImage);
    Stats->PfnDatabasePages = MiDatabasePages;
    Stats->PageTablePages = MiCountPagesInState(MiPfnPageTable);
    Stats->PoolPages = MiCountPagesInState(MiPfnPool);
    Stats->ActivePages = MiCountPagesInState(MiPfnActive);
    Stats->ReclaimedPages = MiReclaimedPages;
    Stats->DemandZeroFaults = MiDemandZeroFaults;
}
