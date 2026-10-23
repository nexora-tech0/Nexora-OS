/*
 * Nexora OS - mm/mminit.c
 * Memory-manager bring-up. Order matters:
 *   1. PFN database (uses Limine's direct map, which is still active)
 *   2. system VA allocator
 *   3. kernel page tables, CR3 switch (Limine's tables become garbage)
 *   4. reclaim bootloader-reclaimable memory (includes Limine's old page tables)
 *   5. pool
 */
#include "mi.h"

static void MiFormatPages(char *Buffer, size_t Size, uint64_t Pages)
{
    RtlFormatByteSize(Buffer, Size, Pages << PAGE_SHIFT);
}

void MmInitializeSystem(void)
{
    MiInitializePfnDatabase();
    MiInitializeSystemVa();
    KeInitializeSpinLock(&MmKernelAddressSpace.Lock);
    MiInitializePaging();
    /* The application processors still spin in bootloader memory: move them first (M11). */
    KeParkApplicationProcessors(MmKernelAddressSpace.Pml4);
    MiReclaimLoaderMemory();
    ExInitializePool();

    MM_STATISTICS S;
    MmQueryStatistics(&S);
    char Total[24], Avail[24], Kernel[24], Pfn[24], Tables[24], Reclaimed[24];
    MiFormatPages(Total, sizeof(Total), S.TotalPages);
    MiFormatPages(Avail, sizeof(Avail), S.AvailablePages);
    MiFormatPages(Kernel, sizeof(Kernel), S.KernelImagePages);
    MiFormatPages(Pfn, sizeof(Pfn), S.PfnDatabasePages);
    MiFormatPages(Tables, sizeof(Tables), S.PageTablePages);
    MiFormatPages(Reclaimed, sizeof(Reclaimed), S.ReclaimedPages);
    DbgPrint("Memory: %s managed, %s available; kernel+modules %s, PFN database %s, page tables %s\n", Total,
             Avail, Kernel, Pfn, Tables);
    DbgPrint("Memory: reclaimed %s of bootloader memory; pool online\n", Reclaimed);
}
