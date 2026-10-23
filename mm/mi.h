/*
 * Nexora OS - mm/mi.h
 * Memory-manager internals shared by the files in mm/.
 *
 * Lock order (outermost first): pool lock -> address-space lock -> system-VA lock -> PFN lock.
 * All are spinlocks acquired at DISPATCH_LEVEL.
 */
#ifndef MI_H
#define MI_H

#include "bootinfo.h"
#include "ex.h"
#include "ke.h"
#include "mm.h"
#include "rtl.h"
#include "x86.h"

/* ---- PFN database ---------------------------------------------------------------------------- */

typedef enum _MI_PFN_STATE {
    MiPfnReserved = 0,       /* not RAM we manage (firmware, ACPI, below 1 MiB, holes) */
    MiPfnFree,               /* on the free list, contents undefined */
    MiPfnZeroed,             /* on the zeroed list */
    MiPfnActive,             /* allocated: stacks, VAD pages, contiguous runs, ... */
    MiPfnPageTable,
    MiPfnPool,               /* pool slab page or single-page large block */
    MiPfnKernelImage,        /* kernel image and boot modules */
    MiPfnDatabasePage,       /* the PFN database itself */
    MiPfnLoaderReclaimable,  /* Limine memory, freed after the page-table switch */
} MI_PFN_STATE;

#define MI_PFN_LIST_END 0xFFFFFFFFu
#define MI_POOL_CLASS_LARGE 0xFF

typedef struct _MMPFN {
    uint32_t Next; /* free/zeroed list, or the pool partial-slab list */
    uint32_t Prev;
    union {
        uint32_t ShareCount;
        struct {
            uint16_t FreeHead; /* offset of the first free block in the slab, 0xFFFF = none */
            uint8_t Class;     /* size class, or MI_POOL_CLASS_LARGE */
            uint8_t InUse;     /* allocated blocks in the slab */
        } Pool;
    };
    uint8_t State;
    uint8_t Flags;
    uint16_t Reserved;
} MMPFN;

_Static_assert(sizeof(MMPFN) == 16, "MMPFN is 16 bytes");

extern MMPFN *MiPfnDatabase;
extern PFN_NUMBER MiPfnCount; /* entries in the database (highest RAM page + 1) */
extern KSPIN_LOCK MiPfnLock;
extern volatile uint64_t MiDemandZeroFaults;

static inline MMPFN *MiGetPfn(PFN_NUMBER Pfn)
{
    return &MiPfnDatabase[Pfn];
}

void MiInitializePfnDatabase(void);
void MiReclaimLoaderMemory(void);
/* Zeroed page for a page table (state MiPfnPageTable). Bug checks when memory is exhausted. */
PFN_NUMBER MiAllocatePageTablePage(void);
void MiFreePageTablePage(PFN_NUMBER Pfn);
/* Changes the state of an allocated page (e.g. Active -> Pool); PFN lock taken inside. */
void MiSetPfnState(PFN_NUMBER Pfn, MI_PFN_STATE State);
uint64_t MiCountPagesInState(MI_PFN_STATE State);
PFN_NUMBER MiRemoveFreePageForZeroing(void);
void MiInsertZeroedPage(PFN_NUMBER Pfn);
uint64_t MiZeroedPageCount(void);
extern uint64_t MiReclaimedPages;

/* ---- Direct map ------------------------------------------------------------------------------ */

static inline void *MiDirect(uint64_t Physical)
{
    return (void *)(Physical + KeLoaderBlock.HhdmOffset);
}

static inline bool MiIsDirectMapAddress(uint64_t Va)
{
    return Va >= KeLoaderBlock.HhdmOffset && Va < KeLoaderBlock.HhdmOffset + (MiPfnCount << PAGE_SHIFT);
}

/* ---- Page tables ------------------------------------------------------------------------------ */

#define PTE_PAT_4K (1ull << 7)
#define PTE_PAT_LARGE (1ull << 12)
#define MI_GUARD_PTE 0x4755415244000000ull /* non-present PTE marking a stack guard page ("GUARD") */

extern uint64_t MiNxBit; /* PTE_NX if EFER.NXE is set, else 0 */

void MiInitializePaging(void);
uint64_t MiProtectionToPte(uint32_t Protection, bool UserSpace);
/* Returns the PTE for Va, creating intermediate tables if Create. Caller holds the AS lock. */
uint64_t *MiGetPte(MMADDRESS_SPACE *AddressSpace, uint64_t Va, bool Create);
/* Maps a 4 KiB page; fails with STATUS_UNSUCCESSFUL if something is already mapped. */
STATUS MiMapPage(MMADDRESS_SPACE *AddressSpace, uint64_t Va, PFN_NUMBER Pfn, uint64_t PteFlags);
/* Clears the PTE and flushes the TLB entry; returns the old PTE (0 if nothing was mapped). */
uint64_t MiUnmapPage(MMADDRESS_SPACE *AddressSpace, uint64_t Va);

/* ---- System VA --------------------------------------------------------------------------------- */

void MiInitializeSystemVa(void);
uint64_t MiAllocateSystemVa(uint64_t Pages); /* 0 if exhausted */
void MiFreeSystemVa(uint64_t Va, uint64_t Pages);
uint64_t MiSystemVaFreePages(void);

/* ---- VADs -------------------------------------------------------------------------------------- */

#define MI_VAD_COMMITTED 0x1
#define MI_VAD_IMAGE 0x2 /* mapped executable image (reported as MEM_IMAGE) */
#define MI_VAD_SECTION 0x4 /* view of a section: pages belong to the section (MEM_MAPPED) */

typedef struct _MMVAD {
    struct _MMVAD *Next;
    uint64_t Start;          /* inclusive, page aligned */
    uint64_t End;            /* exclusive */
    uint64_t AllocationBase; /* start of the original reservation (shared by its split pieces) */
    struct _MM_SECTION *Section; /* MI_VAD_SECTION: the mapped section (referenced) */
    uint32_t Protection;
    uint32_t Flags;
} MMVAD;

#define MI_TAG_VAD HZ_POOL_TAG('M', 'm', 'V', 'd')

/* ---- Sections ------------------------------------------------------------------------------------ */

void MiReferenceSection(struct _MM_SECTION *Section);
void MiDereferenceSection(struct _MM_SECTION *Section);

struct _MM_SECTION {
    uint64_t Size;          /* bytes requested */
    uint64_t PageCount;
    PFN_NUMBER *Pages;      /* resident, zeroed at creation; freed with the section */
    void *SystemView;       /* mapping in system space, or NULL */
};

#endif /* MI_H */
