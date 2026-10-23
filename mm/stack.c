/*
 * Nexora OS - mm/stack.c
 * Kernel stacks: KERNEL_STACK_PAGES mapped pages in system VA with a guard page below them.
 * The guard PTE is non-present but carries the MI_GUARD_PTE signature, so a double fault
 * caused by running off the end of a stack can be recognized and reported as
 * KERNEL_STACK_OVERFLOW (ke/trap.c).
 */
#include "mi.h"

void *MmCreateKernelStack(void)
{
    uint64_t Va = MiAllocateSystemVa(KERNEL_STACK_PAGES + 1);
    if (!Va)
        return NULL;
    uint64_t Flags = MiProtectionToPte(MM_PROT_READWRITE, false);

    KIRQL OldIrql;
    KeAcquireSpinLock(&MmKernelAddressSpace.Lock, &OldIrql);
    *MiGetPte(&MmKernelAddressSpace, Va, true) = MI_GUARD_PTE;
    for (uint64_t i = 1; i <= KERNEL_STACK_PAGES; i++) {
        PFN_NUMBER Pfn = MmAllocatePhysicalPage(true);
        if (Pfn == MM_INVALID_PFN)
            KeBugCheckEx(NO_PAGES_AVAILABLE, KERNEL_STACK_PAGES, 0, 0, 0);
        MiMapPage(&MmKernelAddressSpace, Va + (i << PAGE_SHIFT), Pfn, Flags);
    }
    KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
    return (void *)(Va + ((KERNEL_STACK_PAGES + 1) << PAGE_SHIFT));
}

void MmDeleteKernelStack(void *StackTop)
{
    uint64_t Va = (uint64_t)StackTop - ((KERNEL_STACK_PAGES + 1) << PAGE_SHIFT);
    KIRQL OldIrql;
    KeAcquireSpinLock(&MmKernelAddressSpace.Lock, &OldIrql);
    *MiGetPte(&MmKernelAddressSpace, Va, false) = 0;
    for (uint64_t i = 1; i <= KERNEL_STACK_PAGES; i++) {
        uint64_t Old = MiUnmapPage(&MmKernelAddressSpace, Va + (i << PAGE_SHIFT));
        if (Old & PTE_PRESENT)
            MmFreePhysicalPage((Old & PTE_ADDRESS_MASK) >> PAGE_SHIFT);
    }
    KeReleaseSpinLock(&MmKernelAddressSpace.Lock, OldIrql);
    MiFreeSystemVa(Va, KERNEL_STACK_PAGES + 1);
}

bool MmIsKernelStackGuard(uint64_t Address)
{
    if (Address < MM_SYSTEM_VA_START || Address >= MM_SYSTEM_VA_END || MmKernelAddressSpace.Pml4 == 0)
        return false;
    uint64_t *Pte = MiGetPte(&MmKernelAddressSpace, Address, false); /* lock-free read-only walk */
    return Pte && *Pte == MI_GUARD_PTE;
}
