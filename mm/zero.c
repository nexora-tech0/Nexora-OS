/*
 * Nexora OS - mm/zero.c
 * The zero-page thread. Runs at priority 1 (above only the idle thread), moving pages from the
 * free list to the zeroed list until MI_ZEROED_TARGET pages are ready, so that demand-zero
 * faults, page tables and stacks usually get a page without zeroing it on the spot. When the
 * reserve is full it sleeps and checks again.
 */
#include "mi.h"

#define MI_ZEROED_TARGET 2048  /* 8 MiB */
#define MI_ZERO_BATCH 128
#define MI_ZERO_IDLE_US 100000 /* 100 ms */

void MmZeroPageThread(void *Context)
{
    for (;;) {
        uint32_t Zeroed = 0;
        while (Zeroed < MI_ZERO_BATCH && MiZeroedPageCount() < MI_ZEROED_TARGET) {
            PFN_NUMBER Pfn = MiRemoveFreePageForZeroing();
            if (Pfn == MM_INVALID_PFN)
                break;
            memset(MmPfnToVirtual(Pfn), 0, PAGE_SIZE);
            MiInsertZeroedPage(Pfn);
            Zeroed++;
        }
        if (Zeroed < MI_ZERO_BATCH)
            KeDelayExecutionThread(MI_ZERO_IDLE_US);
    }
}
