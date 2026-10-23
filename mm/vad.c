/*
 * Nexora OS - mm/vad.c
 * Virtual address descriptors (VADs), demand-zero page faults, protection changes, region
 * queries and I/O-space mappings.
 *
 * A VAD records a reserved range, its protection and whether it is committed. Committed ranges
 * get no physical pages up front: the first touch of each page faults, and MmAccessFault maps a
 * zeroed page with the VAD's protection. Committing part of a reserved range splits its VAD;
 * all pieces keep the AllocationBase of the original reservation so MmFreeVirtualMemory releases
 * them together. Each address space keeps its VADs in a sorted singly linked list (a balanced
 * tree can replace it once processes have many ranges).
 */
#include "mi.h"

#define MI_USER_SEARCH_START 0x0000000010000000ull

volatile uint64_t MiDemandZeroFaults;

static MMVAD *MiFindVad(MMADDRESS_SPACE *As, uint64_t Va)
{
    for (MMVAD *Vad = As->Vads; Vad; Vad = Vad->Next) {
        if (Va >= Vad->Start && Va < Vad->End)
            return Vad;
        if (Vad->Start > Va)
            break;
    }
    return NULL;
}

static bool MiRangeIsFree(MMADDRESS_SPACE *As, uint64_t Start, uint64_t End)
{
    for (MMVAD *Vad = As->Vads; Vad; Vad = Vad->Next) {
        if (Vad->Start < End && Start < Vad->End)
            return false;
    }
    return true;
}

static uint64_t MiFindUserGap(MMADDRESS_SPACE *As, uint64_t Size)
{
    uint64_t Candidate = MI_USER_SEARCH_START;
    for (MMVAD *Vad = As->Vads; Vad; Vad = Vad->Next) {
        if (Vad->End <= Candidate)
            continue;
        if (Vad->Start >= Candidate + Size)
            break;
        Candidate = Vad->End;
    }
    return Candidate + Size <= MM_USER_TOP ? Candidate : 0;
}

static void MiInsertVad(MMADDRESS_SPACE *As, MMVAD *Vad)
{
    MMVAD **Link = &As->Vads;
    while (*Link && (*Link)->Start < Vad->Start)
        Link = &(*Link)->Next;
    Vad->Next = *Link;
    *Link = Vad;
}

/* Descriptors for splitting a VAD, allocated before the address-space lock is taken. */
typedef struct _MI_VAD_SPARES {
    MMVAD *Vad[2];
    int Used;
} MI_VAD_SPARES;

static bool MiAllocateSpares(MI_VAD_SPARES *Spares)
{
    Spares->Used = 0;
    Spares->Vad[0] = ExAllocatePoolZero(NonPagedPool, sizeof(MMVAD), MI_TAG_VAD);
    Spares->Vad[1] = ExAllocatePoolZero(NonPagedPool, sizeof(MMVAD), MI_TAG_VAD);
    if (Spares->Vad[0] && Spares->Vad[1])
        return true;
    if (Spares->Vad[0])
        ExFreePool(Spares->Vad[0]);
    if (Spares->Vad[1])
        ExFreePool(Spares->Vad[1]);
    return false;
}

static void MiFreeUnusedSpares(MI_VAD_SPARES *Spares)
{
    for (int i = Spares->Used; i < 2; i++)
        ExFreePool(Spares->Vad[i]);
}

/* Splits Vad so that [Start, End) (inside it) becomes a VAD of its own; returns that VAD. */
static MMVAD *MiIsolateRangeLocked(MMADDRESS_SPACE *As, MMVAD *Vad, uint64_t Start, uint64_t End,
                                   MI_VAD_SPARES *Spares)
{
    if (Start > Vad->Start) {
        MMVAD *Prefix = Spares->Vad[Spares->Used++];
        *Prefix = *Vad;
        Prefix->End = Start;
        Vad->Start = Start;
        MiInsertVad(As, Prefix);
    }
    if (End < Vad->End) {
        MMVAD *Suffix = Spares->Vad[Spares->Used++];
        *Suffix = *Vad;
        Suffix->Start = End;
        Vad->End = End;
        MiInsertVad(As, Suffix);
    }
    return Vad;
}

/* Commits [Start, End) inside one reserved VAD, splitting it into up to three pieces. */
static STATUS MiCommitRange(MMADDRESS_SPACE *As, uint64_t Start, uint64_t End, uint32_t Protection)
{
    MI_VAD_SPARES Spares;
    if (!MiAllocateSpares(&Spares))
        return STATUS_NO_MEMORY;
    STATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;
    KeAcquireSpinLock(&As->Lock, &OldIrql);
    MMVAD *Vad = MiFindVad(As, Start);
    if (!Vad || End > Vad->End) {
        Status = STATUS_CONFLICTING_ADDRESSES; /* not inside a single reservation */
    } else if (Vad->Flags & MI_VAD_COMMITTED) {
        Status = Vad->Protection == Protection ? STATUS_SUCCESS : STATUS_CONFLICTING_ADDRESSES;
    } else {
        Vad = MiIsolateRangeLocked(As, Vad, Start, End, &Spares);
        Vad->Flags |= MI_VAD_COMMITTED;
        Vad->Protection = Protection;
    }
    KeReleaseSpinLock(&As->Lock, OldIrql);
    MiFreeUnusedSpares(&Spares);
    return Status;
}

STATUS MmAllocateVirtualMemory(MMADDRESS_SPACE *As, void **Base, uint64_t Size, uint32_t Type, uint32_t Protection)
{
    if (Size == 0 || !(Type & (MEM_RESERVE | MEM_COMMIT)) || (Type & ~(MEM_RESERVE | MEM_COMMIT | MEM_IMAGE)))
        return STATUS_INVALID_PARAMETER;
    uint64_t Pages = BYTES_TO_PAGES(Size + ((uint64_t)*Base & (PAGE_SIZE - 1)));
    uint64_t Bytes = Pages << PAGE_SHIFT;

    if (As->IsKernel)
        Protection &= ~MM_PROT_USER;
    else
        Protection |= MM_PROT_USER;

    /* MEM_COMMIT alone with an address commits inside an existing user reservation; without an
       address (or in the kernel address space) it reserves and commits in one step. */
    if (!(Type & MEM_RESERVE) && *Base) {
        if (As->IsKernel)
            return STATUS_INVALID_PARAMETER;
        uint64_t Start = HZ_ALIGN_DOWN((uint64_t)*Base, PAGE_SIZE);
        STATUS Status = MiCommitRange(As, Start, Start + Bytes, Protection);
        if (HZ_SUCCESS(Status))
            *Base = (void *)Start;
        return Status;
    }

    /* Allocate the descriptor before taking the address-space lock (lock order: pool first). */
    MMVAD *Vad = ExAllocatePoolZero(NonPagedPool, sizeof(MMVAD), MI_TAG_VAD);
    if (!Vad)
        return STATUS_NO_MEMORY;

    uint64_t Start = 0;
    if (As->IsKernel) {
        if (*Base) {
            ExFreePool(Vad);
            return STATUS_INVALID_PARAMETER;
        }
        Start = MiAllocateSystemVa(Pages);
        if (!Start) {
            ExFreePool(Vad);
            return STATUS_NO_MEMORY;
        }
    }

    KIRQL OldIrql;
    KeAcquireSpinLock(&As->Lock, &OldIrql);
    if (!As->IsKernel) {
        if (*Base) {
            Start = HZ_ALIGN_DOWN((uint64_t)*Base, PAGE_SIZE);
            if (Start < MM_USER_MIN || Start + Bytes > MM_USER_TOP || Start + Bytes < Start ||
                !MiRangeIsFree(As, Start, Start + Bytes))
                Start = 0;
        } else {
            Start = MiFindUserGap(As, Bytes);
        }
        if (!Start) {
            KeReleaseSpinLock(&As->Lock, OldIrql);
            ExFreePool(Vad);
            return STATUS_CONFLICTING_ADDRESSES;
        }
    }
    Vad->Start = Start;
    Vad->End = Start + Bytes;
    Vad->AllocationBase = Start;
    Vad->Protection = Protection;
    Vad->Flags = ((Type & MEM_COMMIT) ? MI_VAD_COMMITTED : 0) | ((Type & MEM_IMAGE) ? MI_VAD_IMAGE : 0);
    MiInsertVad(As, Vad);
    KeReleaseSpinLock(&As->Lock, OldIrql);

    *Base = (void *)Start;
    return STATUS_SUCCESS;
}

STATUS MmFreeVirtualMemory(MMADDRESS_SPACE *As, void *Base)
{
    MMVAD *Freed = NULL;
    uint64_t AllocationStart = 0, AllocationEnd = 0;
    KIRQL OldIrql;
    KeAcquireSpinLock(&As->Lock, &OldIrql);
    MMVAD **Link = &As->Vads;
    while (*Link) {
        MMVAD *Vad = *Link;
        if (Vad->AllocationBase != (uint64_t)Base) {
            Link = &Vad->Next;
            continue;
        }
        *Link = Vad->Next;
        if (!Freed || Vad->Start < AllocationStart)
            AllocationStart = Vad->Start;
        if (Vad->End > AllocationEnd)
            AllocationEnd = Vad->End;
        for (uint64_t Va = Vad->Start; Va < Vad->End; Va += PAGE_SIZE) {
            uint64_t Old = MiUnmapPage(As, Va);
            if ((Old & PTE_PRESENT) && !(Vad->Flags & MI_VAD_SECTION)) {
                MmFreePhysicalPage((Old & PTE_ADDRESS_MASK) >> PAGE_SHIFT);
                As->ResidentPages--;
            }
        }
        Vad->Next = Freed;
        Freed = Vad;
    }
    KeReleaseSpinLock(&As->Lock, OldIrql);
    if (!Freed)
        return STATUS_INVALID_PARAMETER;

    if (As->IsKernel)
        MiFreeSystemVa(AllocationStart, (AllocationEnd - AllocationStart) >> PAGE_SHIFT);
    while (Freed) {
        MMVAD *Next = Freed->Next;
        if (Freed->Section)
            MiDereferenceSection(Freed->Section); /* the view's reference */
        ExFreePool(Freed);
        Freed = Next;
    }
    return STATUS_SUCCESS;
}

STATUS MmMapViewOfSection(MM_SECTION *Section, MMADDRESS_SPACE *As, void **Base, uint32_t Protection)
{
    if (As->IsKernel || !(Protection & (MM_PROT_READ | MM_PROT_WRITE)))
        return STATUS_INVALID_PARAMETER;
    uint64_t Bytes = Section->PageCount << PAGE_SHIFT;
    MMVAD *Vad = ExAllocatePoolZero(NonPagedPool, sizeof(MMVAD), MI_TAG_VAD);
    if (!Vad)
        return STATUS_NO_MEMORY;
    Protection = (Protection & (MM_PROT_READ | MM_PROT_WRITE)) | MM_PROT_READ | MM_PROT_USER;

    KIRQL OldIrql;
    KeAcquireSpinLock(&As->Lock, &OldIrql);
    uint64_t Start;
    if (*Base) {
        Start = HZ_ALIGN_DOWN((uint64_t)*Base, PAGE_SIZE);
        if (Start < MM_USER_MIN || Start + Bytes > MM_USER_TOP || !MiRangeIsFree(As, Start, Start + Bytes))
            Start = 0;
    } else {
        Start = MiFindUserGap(As, Bytes);
    }
    if (!Start) {
        KeReleaseSpinLock(&As->Lock, OldIrql);
        ExFreePool(Vad);
        return STATUS_CONFLICTING_ADDRESSES;
    }
    Vad->Start = Vad->AllocationBase = Start;
    Vad->End = Start + Bytes;
    Vad->Protection = Protection;
    Vad->Flags = MI_VAD_COMMITTED | MI_VAD_SECTION;
    Vad->Section = Section;
    MiInsertVad(As, Vad);
    uint64_t Flags = MiProtectionToPte(Protection, true);
    for (uint64_t i = 0; i < Section->PageCount; i++)
        MiMapPage(As, Start + (i << PAGE_SHIFT), Section->Pages[i], Flags); /* range was free: cannot fail */
    KeReleaseSpinLock(&As->Lock, OldIrql);

    MiReferenceSection(Section);
    *Base = (void *)Start;
    return STATUS_SUCCESS;
}

STATUS MmUnmapViewOfSection(MMADDRESS_SPACE *As, void *Base)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&As->Lock, &OldIrql);
    MMVAD *Vad = MiFindVad(As, (uint64_t)Base);
    bool IsView = Vad && Vad->Start == (uint64_t)Base && (Vad->Flags & MI_VAD_SECTION);
    KeReleaseSpinLock(&As->Lock, OldIrql);
    return IsView ? MmFreeVirtualMemory(As, Base) : STATUS_INVALID_PARAMETER;
}

STATUS MmProtectVirtualMemory(MMADDRESS_SPACE *As, void *Base, uint64_t Size, uint32_t Protection,
                              uint32_t *OldProtection)
{
    uint64_t Start = HZ_ALIGN_DOWN((uint64_t)Base, PAGE_SIZE);
    uint64_t End = HZ_ALIGN_UP((uint64_t)Base + Size, PAGE_SIZE);
    if (Size == 0 || End <= Start)
        return STATUS_INVALID_PARAMETER;
    /* Present pages keep their frames; a no-access protection would need them unmapped. */
    if (!(Protection & (MM_PROT_READ | MM_PROT_WRITE | MM_PROT_EXECUTE)))
        return STATUS_INVALID_PAGE_PROTECTION;
    if (!As->IsKernel)
        Protection |= MM_PROT_USER;
    MI_VAD_SPARES Spares;
    if (!MiAllocateSpares(&Spares))
        return STATUS_NO_MEMORY;
    KIRQL OldIrql;
    KeAcquireSpinLock(&As->Lock, &OldIrql);
    MMVAD *Vad = MiFindVad(As, Start);
    STATUS Status = STATUS_CONFLICTING_ADDRESSES;
    if (Vad && End <= Vad->End && (Vad->Flags & MI_VAD_COMMITTED) && !(Vad->Flags & MI_VAD_SECTION)) {
        if (OldProtection)
            *OldProtection = Vad->Protection;
        Vad = MiIsolateRangeLocked(As, Vad, Start, End, &Spares);
        Vad->Protection = Protection;
        uint64_t Flags = MiProtectionToPte(Protection, !As->IsKernel);
        for (uint64_t Va = Start; Va < End; Va += PAGE_SIZE) {
            uint64_t *Pte = MiGetPte(As, Va, false);
            if (Pte && (*Pte & PTE_PRESENT))
                *Pte = (*Pte & PTE_ADDRESS_MASK) | Flags;
        }
        KeFlushTb(As->IsKernel ? 0 : As->Pml4, Start, (End - Start) / PAGE_SIZE); /* M11: every processor */
        Status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&As->Lock, OldIrql);
    MiFreeUnusedSpares(&Spares);
    return Status;
}

uint32_t MmProtectionToPageFlags(uint32_t Protection)
{
    bool R = Protection & MM_PROT_READ, W = Protection & MM_PROT_WRITE, X = Protection & MM_PROT_EXECUTE;
    if (X)
        return W ? PAGE_EXECUTE_READWRITE : R ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (W)
        return PAGE_READWRITE;
    return R ? PAGE_READONLY : PAGE_NOACCESS;
}

bool MmPageFlagsToProtection(uint32_t PageFlags, uint32_t *Protection)
{
    switch (PageFlags) {
    case PAGE_READONLY: *Protection = MM_PROT_READ; return true;
    case PAGE_READWRITE: *Protection = MM_PROT_READWRITE; return true;
    case PAGE_EXECUTE: *Protection = MM_PROT_EXECUTE; return true;
    case PAGE_EXECUTE_READ: *Protection = MM_PROT_READ | MM_PROT_EXECUTE; return true;
    case PAGE_EXECUTE_READWRITE: *Protection = MM_PROT_READWRITE | MM_PROT_EXECUTE; return true;
    case PAGE_NOACCESS: *Protection = 0; return true;
    default: return false;
    }
}

STATUS MmQueryVirtualMemory(MMADDRESS_SPACE *As, uint64_t Address, HZ_MEMORY_BASIC_INFORMATION *Info)
{
    if (Address >= MM_USER_TOP && !As->IsKernel)
        return STATUS_INVALID_PARAMETER;
    uint64_t Page = HZ_ALIGN_DOWN(Address, PAGE_SIZE);
    memset(Info, 0, sizeof(*Info));
    KIRQL OldIrql;
    KeAcquireSpinLock(&As->Lock, &OldIrql);
    MMVAD *Vad = MiFindVad(As, Page);
    if (Vad) {
        Info->BaseAddress = Vad->Start;
        Info->AllocationBase = Vad->AllocationBase;
        Info->RegionSize = Vad->End - Vad->Start;
        Info->State = (Vad->Flags & MI_VAD_COMMITTED) ? MEM_COMMIT : MEM_RESERVE;
        Info->Protect = (Vad->Flags & MI_VAD_COMMITTED) ? MmProtectionToPageFlags(Vad->Protection) : 0;
        Info->Type = (Vad->Flags & MI_VAD_IMAGE) ? MEM_IMAGE : (Vad->Flags & MI_VAD_SECTION) ? MEM_MAPPED : MEM_PRIVATE;
    } else {
        uint64_t End = As->IsKernel ? MM_SYSTEM_VA_END : MM_USER_TOP;
        for (MMVAD *V = As->Vads; V; V = V->Next) {
            if (V->Start > Page) {
                End = V->Start;
                break;
            }
        }
        Info->BaseAddress = Page;
        Info->RegionSize = End - Page;
        Info->State = MEM_FREE;
        Info->Protect = PAGE_NOACCESS;
    }
    KeReleaseSpinLock(&As->Lock, OldIrql);
    return STATUS_SUCCESS;
}

STATUS MmAccessFault(uint64_t Address, uint64_t ErrorCode, KTRAP_FRAME *Frame)
{
    if (ErrorCode & 0x1)
        return STATUS_ACCESS_VIOLATION; /* protection violation on a present page */

    MMADDRESS_SPACE *As;
    if (Address < MM_USER_TOP)
        As = MmGetCurrentAddressSpace();
    else if (Address >= MM_SYSTEM_VA_START && Address < MM_SYSTEM_VA_END)
        As = &MmKernelAddressSpace;
    else
        return STATUS_ACCESS_VIOLATION;

    if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
        return STATUS_IRQL_TOO_HIGH;

    KIRQL OldIrql;
    KeAcquireSpinLock(&As->Lock, &OldIrql);
    MMVAD *Vad = MiFindVad(As, Address);
    STATUS Status = STATUS_ACCESS_VIOLATION;
    bool Write = (ErrorCode & 0x2) != 0, User = (ErrorCode & 0x4) != 0, Execute = (ErrorCode & 0x10) != 0;
    bool Readable = Vad && (Vad->Protection & (MM_PROT_READ | MM_PROT_WRITE | MM_PROT_EXECUTE));
    if (Vad && Readable && (Vad->Flags & MI_VAD_COMMITTED) && !(Vad->Flags & MI_VAD_SECTION) &&
        !(Write && !(Vad->Protection & MM_PROT_WRITE)) &&
        !(Execute && !(Vad->Protection & MM_PROT_EXECUTE)) && !(User && !(Vad->Protection & MM_PROT_USER))) {
        PFN_NUMBER Pfn = MmAllocatePhysicalPage(true);
        if (Pfn == MM_INVALID_PFN) {
            Status = STATUS_NO_MEMORY;
        } else if (HZ_SUCCESS(MiMapPage(As, HZ_ALIGN_DOWN(Address, PAGE_SIZE), Pfn,
                                        MiProtectionToPte(Vad->Protection, !As->IsKernel)))) {
            As->ResidentPages++;
            MiDemandZeroFaults++;
            Status = STATUS_SUCCESS;
        } else {
            MmFreePhysicalPage(Pfn); /* resolved concurrently */
            Status = STATUS_SUCCESS;
        }
    }
    KeReleaseSpinLock(&As->Lock, OldIrql);
    return Status;
}

/* ---- I/O space ---------------------------------------------------------------------------------- */

void *MmMapIoSpace(uint64_t Physical, uint64_t Size, uint32_t CacheType)
{
    uint64_t Offset = Physical & (PAGE_SIZE - 1);
    uint64_t Pages = BYTES_TO_PAGES(Offset + Size);
    uint64_t Va = MiAllocateSystemVa(Pages);
    if (!Va)
        return NULL;
    uint64_t Flags = MiProtectionToPte(MM_PROT_READWRITE | (CacheType & (MM_CACHE_UNCACHED | MM_CACHE_WRITECOMBINE)),
                                       false);
    KIRQL OldIrql;
    KeAcquireSpinLock(&MmKernelAddressSpace.Lock, &OldIrql);
    for (uint64_t i = 0; i < Pages; i++)
        MiMapPage(&MmKernelAddressSpace, Va + (i << PAGE_SHIFT), (Physical >> PAGE_SHIFT) + i, Flags);
    KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
    return (void *)(Va + Offset);
}

void MmUnmapIoSpace(void *Address, uint64_t Size)
{
    uint64_t Va = HZ_ALIGN_DOWN((uint64_t)Address, PAGE_SIZE);
    uint64_t Pages = BYTES_TO_PAGES(((uint64_t)Address - Va) + Size);
    KIRQL OldIrql;
    KeAcquireSpinLock(&MmKernelAddressSpace.Lock, &OldIrql);
    for (uint64_t i = 0; i < Pages; i++)
        MiUnmapPage(&MmKernelAddressSpace, Va + (i << PAGE_SHIFT));
    KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
    MiFreeSystemVa(Va, Pages);
}
