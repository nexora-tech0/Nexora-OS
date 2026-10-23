/*
 * Nexora OS - hal/iospace.c
 * Uncached device-memory mappings for the HAL before the memory manager exists.
 *
 * Limine's higher-half direct map (base revision 6) covers RAM, ACPI and the framebuffer but
 * not MMIO such as the Local APIC, I/O APICs or HPET. HalMapIoSpace maps such ranges into a
 * dedicated window (PML4 slot 508, 0xFFFFFE0000000000) of the active page tables, taking
 * page-table pages from a small static pool. Mappings are UC (PCD|PWT = PAT entry 3, which
 * Limine programs as UC) and NX. The memory manager (M2) carries the window over when it
 * builds its own page tables.
 */
#include "bootinfo.h"
#include "halp.h"
#include "rtl.h"

#define HAL_IO_SPACE_BASE 0xFFFFFE0000000000ull
#define HAL_IO_SPACE_SIZE (1ull << 30) /* 1 GiB of VA; bounded in practice by the pool */
#define HAL_PT_POOL_PAGES 16
#define HAL_MAX_MAPPINGS 32
#define PAGE_SIZE 4096ull

static uint8_t HalpPageTablePool[HAL_PT_POOL_PAGES][PAGE_SIZE] HZ_ALIGNED(4096);
static uint32_t HalpPoolUsed;
static uint64_t HalpNextVa = HAL_IO_SPACE_BASE;
static KSPIN_LOCK HalpIoSpaceLock;

static struct {
    uint64_t PhysicalPage;
    uint64_t Pages;
    uint64_t VirtualAddress;
} HalpMappings[HAL_MAX_MAPPINGS];
static uint32_t HalpMappingCount;

static uint64_t HalpKernelVaToPa(const void *Va)
{
    return KeLoaderBlock.KernelPhysicalBase + ((uint64_t)Va - KeLoaderBlock.KernelVirtualBase);
}

static uint64_t *HalpPaToVa(uint64_t Pa)
{
    return (uint64_t *)(Pa + KeLoaderBlock.HhdmOffset);
}

static uint64_t *HalpNextLevel(uint64_t *Table, unsigned Index)
{
    uint64_t Entry = Table[Index];
    if (!(Entry & PTE_PRESENT)) {
        if (HalpPoolUsed >= HAL_PT_POOL_PAGES)
            KeBugCheckEx(HAL_INITIALIZATION_FAILED, HAL_FAILURE_IO_SPACE_EXHAUSTED, HalpPoolUsed, 0, 0);
        uint8_t *Page = HalpPageTablePool[HalpPoolUsed++];
        memset(Page, 0, PAGE_SIZE);
        Entry = HalpKernelVaToPa(Page) | PTE_PRESENT | PTE_WRITE;
        Table[Index] = Entry;
    }
    HZ_ASSERT(!(Entry & PTE_LARGE)); /* the window is ours; nothing maps it with large pages */
    return HalpPaToVa(Entry & PTE_ADDRESS_MASK);
}

static void HalpMapPage(uint64_t Va, uint64_t Pa, uint64_t Flags)
{
    uint64_t *Pml4 = HalpPaToVa(__readcr3() & PTE_ADDRESS_MASK);
    uint64_t *Pdpt = HalpNextLevel(Pml4, (Va >> 39) & 0x1FF);
    uint64_t *Pd = HalpNextLevel(Pdpt, (Va >> 30) & 0x1FF);
    uint64_t *Pt = HalpNextLevel(Pd, (Va >> 21) & 0x1FF);
    Pt[(Va >> 12) & 0x1FF] = Pa | Flags;
    __invlpg((void *)Va);
}

void *HalMapIoSpace(uint64_t PhysicalAddress, uint64_t Size)
{
    uint64_t PhysicalPage = PhysicalAddress & ~(PAGE_SIZE - 1);
    uint64_t Offset = PhysicalAddress - PhysicalPage;
    uint64_t Pages = (Offset + Size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *Result = NULL;

    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&HalpIoSpaceLock, HIGH_LEVEL, &OldIrql);

    /* Reuse an existing mapping that already covers the range. */
    for (uint32_t i = 0; i < HalpMappingCount; i++) {
        uint64_t First = HalpMappings[i].PhysicalPage;
        uint64_t End = First + HalpMappings[i].Pages * PAGE_SIZE;
        if (PhysicalPage >= First && PhysicalPage + Pages * PAGE_SIZE <= End) {
            Result = (void *)(HalpMappings[i].VirtualAddress + (PhysicalPage - First) + Offset);
            goto Done;
        }
    }

    if (HalpNextVa + Pages * PAGE_SIZE > HAL_IO_SPACE_BASE + HAL_IO_SPACE_SIZE ||
        HalpMappingCount >= HAL_MAX_MAPPINGS)
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, HAL_FAILURE_IO_SPACE_EXHAUSTED, PhysicalAddress, Size, 0);

    uint64_t Flags = PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT;
    if (__readmsr(MSR_EFER) & EFER_NXE)
        Flags |= PTE_NX;

    uint64_t Va = HalpNextVa;
    for (uint64_t p = 0; p < Pages; p++)
        HalpMapPage(Va + p * PAGE_SIZE, PhysicalPage + p * PAGE_SIZE, Flags);
    HalpNextVa += Pages * PAGE_SIZE;

    HalpMappings[HalpMappingCount].PhysicalPage = PhysicalPage;
    HalpMappings[HalpMappingCount].Pages = Pages;
    HalpMappings[HalpMappingCount].VirtualAddress = Va;
    HalpMappingCount++;
    Result = (void *)(Va + Offset);

Done:
    KeReleaseSpinLock(&HalpIoSpaceLock, OldIrql);
    return Result;
}
