/*
 * Nexora OS - include/mm.h
 * Memory manager (Mm*): physical pages (PFN database), page tables and address spaces, the
 * kernel virtual address allocator, virtual memory ranges (VADs) with demand-zero faults,
 * I/O-space mappings and guarded kernel stacks. The pool allocator (Ex*) is in ex.h.
 */
#ifndef MM_H
#define MM_H

#include "hzbase.h"
#include "ke.h"
#include "uapi/hzapi.h" /* MEM_*, PAGE_* */

HZ_EXTERN_C_BEGIN

#define PAGE_SIZE 4096ull
#define PAGE_SHIFT 12
#define LARGE_PAGE_SIZE (2ull * 1024 * 1024)
#define BYTES_TO_PAGES(b) (((uint64_t)(b) + PAGE_SIZE - 1) >> PAGE_SHIFT)

typedef uint64_t PFN_NUMBER;
#define MM_INVALID_PFN ((PFN_NUMBER)-1)

/* ---- Virtual address layout ---------------------------------------------------------------- */

#define MM_USER_TOP 0x0000800000000000ull      /* exclusive */
#define MM_USER_MIN 0x0000000000010000ull      /* first 64 KiB never mapped */
#define MM_SYSTEM_VA_START 0xFFFFC00000000000ull
#define MM_SYSTEM_VA_SIZE (256ull << 30)        /* 256 GiB: pool runs, stacks, I/O, VADs */
#define MM_SYSTEM_VA_END (MM_SYSTEM_VA_START + MM_SYSTEM_VA_SIZE)

/* ---- Protection and caching (combinable) --------------------------------------------------- */

#define MM_PROT_READ 0x01
#define MM_PROT_WRITE 0x02
#define MM_PROT_EXECUTE 0x04
#define MM_PROT_USER 0x08
#define MM_CACHE_UNCACHED 0x10
#define MM_CACHE_WRITECOMBINE 0x20

#define MM_PROT_READWRITE (MM_PROT_READ | MM_PROT_WRITE)

/* ---- Initialization and statistics ------------------------------------------------------------ */

/* Builds the PFN database, switches to the kernel's own page tables, reclaims loader memory and
   initializes the pool. Runs once, with interrupts disabled, before the HAL maps devices. */
void MmInitializeSystem(void);
bool MmRunSelfTests(void);

typedef struct _MM_STATISTICS {
    uint64_t TotalPages;          /* RAM managed by the PFN database */
    uint64_t AvailablePages;      /* free + zeroed */
    uint64_t ZeroedPages;         /* pre-zeroed by the zero-page thread */
    uint64_t KernelImagePages;    /* kernel image and boot modules */
    uint64_t PfnDatabasePages;
    uint64_t PageTablePages;
    uint64_t PoolPages;
    uint64_t ActivePages;         /* other allocated pages (stacks, VAD pages, ...) */
    uint64_t ReclaimedPages;      /* loader memory returned to the free list */
    uint64_t DemandZeroFaults;
} MM_STATISTICS;

/* Full statistics (walks the PFN database; not for hot paths). */
void MmQueryStatistics(MM_STATISTICS *Stats);
/* Free + zeroed pages (O(1)). */
uint64_t MmQueryAvailablePages(void);
/* Body of the zero-page thread (priority 1): keeps a reserve of pre-zeroed pages. */
void MmZeroPageThread(void *Context);

/* ---- Physical pages --------------------------------------------------------------------------- */

/* Returns MM_INVALID_PFN when memory is exhausted. Callable at IRQL <= DISPATCH_LEVEL. */
PFN_NUMBER MmAllocatePhysicalPage(bool Zero);
void MmFreePhysicalPage(PFN_NUMBER Pfn);
/* Physically contiguous run below MaxPhysical (exclusive), aligned to AlignmentPages. */
STATUS MmAllocateContiguousPages(uint64_t Count, uint64_t MaxPhysical, uint64_t AlignmentPages,
                                 PFN_NUMBER *FirstPfn);
void MmFreeContiguousPages(PFN_NUMBER FirstPfn, uint64_t Count);

/* Direct-map (HHDM) address of a physical page / address. */
void *MmPfnToVirtual(PFN_NUMBER Pfn);
void *MmPhysicalToVirtual(uint64_t Physical);
/* Physical address behind a mapped virtual address in the current address space, or 0. */
uint64_t MmGetPhysicalAddress(const void *Address);

/* ---- Address spaces ------------------------------------------------------------------------ */

struct _MMVAD;

typedef struct _MMADDRESS_SPACE {
    uint64_t Pml4;             /* physical address of the top-level table */
    KSPIN_LOCK Lock;           /* page tables below the kernel half + VAD list */
    struct _MMVAD *Vads;       /* sorted by start address */
    uint64_t ResidentPages;    /* pages mapped on behalf of VADs */
    uint64_t PageTablePages;   /* private (user-half) page-table pages */
    bool IsKernel;
} MMADDRESS_SPACE;

extern MMADDRESS_SPACE MmKernelAddressSpace;

/* New address space sharing the kernel half. */
STATUS MmCreateAddressSpace(MMADDRESS_SPACE **AddressSpace);
/* Frees all user mappings, VADs and page tables. Must not be the current address space. */
void MmDeleteAddressSpace(MMADDRESS_SPACE *AddressSpace);
void MmSwitchAddressSpace(MMADDRESS_SPACE *AddressSpace);
MMADDRESS_SPACE *MmGetCurrentAddressSpace(void);

/* ---- Virtual memory ------------------------------------------------------------------------ */

/* Allocation types: MEM_RESERVE, MEM_COMMIT (uapi/hzapi.h). Committed pages are materialized as
   zeroed pages on first touch. */

/*
 * Reserves (and optionally commits) a range. In the kernel address space *Base must be NULL and
 * the range comes from system VA; in a user address space *Base may be a hint (NULL = any).
 */
STATUS MmAllocateVirtualMemory(MMADDRESS_SPACE *AddressSpace, void **Base, uint64_t Size, uint32_t Type,
                               uint32_t Protection);
/* Releases a whole reservation (all of its committed and reserved pieces) by its base address. */
STATUS MmFreeVirtualMemory(MMADDRESS_SPACE *AddressSpace, void *Base);
/* Changes the protection of a committed range, which must lie inside one region (as reported
   by MmQueryVirtualMemory); the region is split as needed. Present pages are updated in place.
   *OldProtection receives the region's previous protection. No-access is not supported. */
STATUS MmProtectVirtualMemory(MMADDRESS_SPACE *AddressSpace, void *Base, uint64_t Size, uint32_t Protection,
                              uint32_t *OldProtection);
/* Describes the region (VAD piece or free gap) containing Address. */
STATUS MmQueryVirtualMemory(MMADDRESS_SPACE *AddressSpace, uint64_t Address, HZ_MEMORY_BASIC_INFORMATION *Info);
/* PAGE_* (user API) <-> MM_PROT_* conversions; the PAGE_* form is a single value, not a mask. */
uint32_t MmProtectionToPageFlags(uint32_t Protection);
bool MmPageFlagsToProtection(uint32_t PageFlags, uint32_t *Protection);

/* ---- User buffers ---------------------------------------------------------------------------- */

/*
 * Copies to/from user addresses of the current address space. The whole range must lie below
 * MM_USER_TOP; faults (unmapped, protection) return STATUS_ACCESS_VIOLATION instead of crashing.
 * With SMAP enabled these are the only kernel paths that may touch user pages. IRQL < DISPATCH.
 */
STATUS MmCopyFromUser(void *Destination, const void *UserSource, size_t Length);
STATUS MmCopyToUser(void *UserDestination, const void *Source, size_t Length);
/* Copies a NUL-terminated user string into Buffer (Capacity includes the terminator);
   STATUS_NAME_TOO_LONG if it does not fit. */
STATUS MmCopyStringFromUser(char *Buffer, size_t Capacity, const char *UserString);
/* True if [Address, Address+Length) lies entirely in user space (no overflow). */
static inline bool MmIsUserRange(const void *Address, size_t Length)
{
    uint64_t Start = (uint64_t)Address;
    return Start + Length >= Start && Start + Length <= MM_USER_TOP;
}
/* True if the kernel #PF at Rip happened inside the user-copy routine (the trap handler then
   resumes at the copy's failure exit). */
bool MmIsUserCopyFault(uint64_t Rip);
uint64_t MmUserCopyFaultResume(void);
extern bool MmSmapEnabled;

/* ---- System services (mm/mmsvc.c) -------------------------------------------------------- */

STATUS NtAllocateVirtualMemory(const uint64_t *Args);
STATUS NtFreeVirtualMemory(const uint64_t *Args);
STATUS NtQueryVirtualMemory(const uint64_t *Args);
STATUS NtProtectVirtualMemory(const uint64_t *Args);

/* ---- Sections (mm/section.c) -------------------------------------------------------------------
 * Shared memory: resident zeroed pages owned by a Section object, mapped into user address spaces
 * (views, MEM_MAPPED) and at most once into system space. Views reference the section. */

typedef struct _MM_SECTION MM_SECTION;
extern struct _OBJECT_TYPE *MmSectionObjectType;

/* Phase 0, after Ob: the Section object type. */
void MmInitializeSections(void);
/* New section of Size bytes (rounded up to pages, at most 256 MiB) with one reference. */
STATUS MmCreateSection(uint64_t Size, MM_SECTION **Section);
uint64_t MmGetSectionSize(MM_SECTION *Section);
/* Maps the whole section into a user address space (*Base: hint or NULL); Protection is
   MM_PROT_READ or MM_PROT_READWRITE. */
STATUS MmMapViewOfSection(MM_SECTION *Section, MMADDRESS_SPACE *AddressSpace, void **Base, uint32_t Protection);
STATUS MmUnmapViewOfSection(MMADDRESS_SPACE *AddressSpace, void *Base);
/* The section's system-space mapping (created on first use, removed with the section). */
STATUS MmMapViewInSystemSpace(MM_SECTION *Section, void **Base);

STATUS NtCreateSection(const uint64_t *Args);
STATUS NtMapViewOfSection(const uint64_t *Args);
STATUS NtUnmapViewOfSection(const uint64_t *Args);

/* Maps a physical range into system VA. CacheType: MM_CACHE_UNCACHED or MM_CACHE_WRITECOMBINE for
   device memory, 0 (write-back) for RAM (never alias RAM with a different cache type). */
void *MmMapIoSpace(uint64_t Physical, uint64_t Size, uint32_t CacheType);
void MmUnmapIoSpace(void *Address, uint64_t Size);

/* ---- Kernel stacks ------------------------------------------------------------------------- */

#define KERNEL_STACK_PAGES 8
#define KERNEL_STACK_SIZE (KERNEL_STACK_PAGES * PAGE_SIZE)

/* Allocates a kernel stack with an unmapped guard page below it; returns the stack top. */
void *MmCreateKernelStack(void);
void MmDeleteKernelStack(void *StackTop);
/* True if Address lies in the guard page of a kernel stack (safe to call from a double fault). */
bool MmIsKernelStackGuard(uint64_t Address);

/* ---- Page faults ------------------------------------------------------------------------- */

#define STATUS_IRQL_TOO_HIGH ((STATUS)0xC0000220)

/* Resolves demand-zero faults. Anything else returns an error for the trap handler to report. */
STATUS MmAccessFault(uint64_t Address, uint64_t ErrorCode, KTRAP_FRAME *Frame);

HZ_EXTERN_C_END

#endif /* MM_H */
