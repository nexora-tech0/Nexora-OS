/*
 * Nexora OS - mm/paging.c
 * Page tables and address spaces.
 *
 * MiInitializePaging builds the kernel's own 4-level tables and switches CR3 away from Limine's:
 *   - kernel image, page by page from the linker-script section bounds: RW (requests), RX (.text),
 *     R+NX (.rodata/.init_array), RW+NX (.data/.bss); the physical frames are read from the boot
 *     page tables, so no assumption about how Limine placed the image is needed;
 *   - the direct map (same HHDM offset as Limine, so existing pointers stay valid) for every RAM
 *     range Limine mapped (usable, reclaimable, kernel/modules, ACPI, reserved-mapped), write-back,
 *     NX, global, 2 MiB pages where alignment allows;
 *   - the framebuffer, write-combining (PAT entry 5);
 *   - the HAL I/O window (PML4 slot 508) carried over as-is.
 * All 256 kernel-half PML4 entries are populated up front, so every address space created later
 * shares the kernel half by copying them once and never needs synchronization.
 * Afterwards: CR4.PGE (global pages) and, if available, CR4.SMEP and CR4.SMAP (kernel accesses to
 * user pages fault unless made through mm/usercopy.asm).
 */
#include "mi.h"

extern uint8_t __KernelStart[], __TextStart[], __RodataStart[], __DataStart[], __KernelEnd[];

#define MI_HAL_IO_PML4_SLOT 508
#define CR4_PGE (1ull << 7)
#define CR4_SMEP (1ull << 20)
#define CR4_SMAP (1ull << 21)
#define MI_PAT_EXPECTED 0x0007010500070406ull /* WB WT UC- UC WP WC UC- UC (Limine's layout) */

MMADDRESS_SPACE MmKernelAddressSpace = {.IsKernel = true};
static MMADDRESS_SPACE *MiBootAddressSpace = &MmKernelAddressSpace; /* before threads exist */
uint64_t MiNxBit;

static inline uint64_t *MiTable(uint64_t Physical)
{
    return (uint64_t *)MiDirect(Physical & PTE_ADDRESS_MASK);
}

static inline unsigned MiIndex(uint64_t Va, int Level) /* Level 3 = PML4 ... 0 = PT */
{
    return (unsigned)((Va >> (12 + 9 * Level)) & 0x1FF);
}

uint64_t MiProtectionToPte(uint32_t Protection, bool UserSpace)
{
    uint64_t Pte = PTE_PRESENT;
    if (Protection & MM_PROT_WRITE)
        Pte |= PTE_WRITE;
    if (!(Protection & MM_PROT_EXECUTE))
        Pte |= MiNxBit;
    if (UserSpace)
        Pte |= PTE_USER;
    else
        Pte |= PTE_GLOBAL;
    if (Protection & MM_CACHE_UNCACHED)
        Pte |= PTE_PCD | PTE_PWT;           /* PAT 3 = UC */
    else if (Protection & MM_CACHE_WRITECOMBINE)
        Pte |= PTE_PAT_4K | PTE_PWT;        /* PAT 5 = WC */
    return Pte;
}

uint64_t *MiGetPte(MMADDRESS_SPACE *As, uint64_t Va, bool Create)
{
    uint64_t *Table = MiTable(As->Pml4);
    for (int Level = 3; Level > 0; Level--) {
        uint64_t *Entry = &Table[MiIndex(Va, Level)];
        if (!(*Entry & PTE_PRESENT)) {
            if (!Create)
                return NULL;
            PFN_NUMBER Pfn = MiAllocatePageTablePage();
            *Entry = (Pfn << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE | (Va < MM_USER_TOP ? PTE_USER : 0);
            if (Va < MM_USER_TOP)
                As->PageTablePages++;
        } else if (*Entry & PTE_LARGE) {
            return NULL; /* inside a large mapping (direct map); no 4 KiB PTE exists */
        }
        Table = MiTable(*Entry);
    }
    return &Table[MiIndex(Va, 0)];
}

STATUS MiMapPage(MMADDRESS_SPACE *As, uint64_t Va, PFN_NUMBER Pfn, uint64_t PteFlags)
{
    uint64_t *Pte = MiGetPte(As, Va, true);
    if (!Pte || (*Pte & PTE_PRESENT))
        return STATUS_UNSUCCESSFUL;
    *Pte = (Pfn << PAGE_SHIFT) | PteFlags;
    return STATUS_SUCCESS;
}

uint64_t MiUnmapPage(MMADDRESS_SPACE *As, uint64_t Va)
{
    uint64_t *Pte = MiGetPte(As, Va, false);
    if (!Pte)
        return 0;
    uint64_t Old = *Pte;
    *Pte = 0;
    if (Old & PTE_PRESENT)
        KeFlushTb(As->IsKernel ? 0 : As->Pml4, Va, 1); /* every processor that may cache it (M11) */
    return Old;
}

static void MiMapLarge(MMADDRESS_SPACE *As, uint64_t Va, uint64_t Pa, uint64_t Flags)
{
    uint64_t *Table = MiTable(As->Pml4);
    for (int Level = 3; Level > 1; Level--) {
        uint64_t *Entry = &Table[MiIndex(Va, Level)];
        if (!(*Entry & PTE_PRESENT))
            *Entry = (MiAllocatePageTablePage() << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE;
        Table = MiTable(*Entry);
    }
    Table[MiIndex(Va, 1)] = Pa | Flags | PTE_LARGE;
}

/* Maps [Pa, Pa+Size) at Va using 2 MiB pages where both addresses are aligned. */
static void MiMapRange(MMADDRESS_SPACE *As, uint64_t Va, uint64_t Pa, uint64_t Size, uint64_t Flags4K,
                       uint64_t FlagsLarge)
{
    uint64_t Offset = 0;
    while (Offset < Size) {
        uint64_t V = Va + Offset, P = Pa + Offset;
        if ((V & (LARGE_PAGE_SIZE - 1)) == 0 && (P & (LARGE_PAGE_SIZE - 1)) == 0 && Size - Offset >= LARGE_PAGE_SIZE) {
            MiMapLarge(As, V, P, FlagsLarge);
            Offset += LARGE_PAGE_SIZE;
        } else {
            uint64_t *Pte = MiGetPte(As, V, true);
            *Pte = P | Flags4K;
            Offset += PAGE_SIZE;
        }
    }
}

/* Translates through an arbitrary PML4 (large pages included); 0 if unmapped. */
static uint64_t MiTranslate(uint64_t Pml4, uint64_t Va)
{
    uint64_t *Table = MiTable(Pml4);
    for (int Level = 3; Level >= 0; Level--) {
        uint64_t Entry = Table[MiIndex(Va, Level)];
        if (!(Entry & PTE_PRESENT))
            return 0;
        if (Level == 2 && (Entry & PTE_LARGE))
            return (Entry & 0x000FFFFFC0000000ull) + (Va & ((1ull << 30) - 1));
        if (Level == 1 && (Entry & PTE_LARGE))
            return (Entry & 0x000FFFFFFFE00000ull) + (Va & (LARGE_PAGE_SIZE - 1));
        if (Level == 0)
            return (Entry & PTE_ADDRESS_MASK) + (Va & (PAGE_SIZE - 1));
        Table = MiTable(Entry);
    }
    return 0;
}

uint64_t MmGetPhysicalAddress(const void *Address)
{
    return MiTranslate(__readcr3(), (uint64_t)Address);
}

static bool MiIsDirectMapped(MEMORY_TYPE Type)
{
    return Type == MemoryUsable || Type == MemoryLoaderReclaimable || Type == MemoryKernelAndModules ||
           Type == MemoryAcpiReclaimable || Type == MemoryAcpiNvs || Type == MemoryReservedMapped;
}

static void MiCheckPat(void)
{
    uint64_t Pat = __readmsr(MSR_IA32_PAT);
    const uint64_t UsedEntries = 0x0000FFFFFFFFFFFFull; /* PAT0-5; Limine leaves 6/7 unspecified */
    if ((Pat & UsedEntries) != (MI_PAT_EXPECTED & UsedEntries)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Paging: PAT was 0x%016llx; reprogramming\n", (unsigned long long)Pat);
        __asm__ volatile("wbinvd" : : : "memory");
        __writemsr(MSR_IA32_PAT, MI_PAT_EXPECTED);
        __writecr3(__readcr3());
    }
}

void MiInitializePaging(void)
{
    MiNxBit = (__readmsr(MSR_EFER) & EFER_NXE) ? PTE_NX : 0;
    MiCheckPat();

    uint64_t BootPml4 = __readcr3() & PTE_ADDRESS_MASK;
    uint64_t *Boot = MiTable(BootPml4);
    MmKernelAddressSpace.Pml4 = MiAllocatePageTablePage() << PAGE_SHIFT;
    uint64_t *Pml4 = MiTable(MmKernelAddressSpace.Pml4);

    for (unsigned i = 256; i < 512; i++) {
        if (i == MI_HAL_IO_PML4_SLOT && (Boot[i] & PTE_PRESENT))
            Pml4[i] = Boot[i];
        else
            Pml4[i] = (MiAllocatePageTablePage() << PAGE_SHIFT) | PTE_PRESENT | PTE_WRITE;
    }

    /* Direct map of RAM: merge adjacent mappable ranges, then map. */
    const uint64_t Hhdm = KeLoaderBlock.HhdmOffset;
    const uint64_t Wb4K = PTE_PRESENT | PTE_WRITE | PTE_GLOBAL | MiNxBit;
    const uint64_t WbLarge = Wb4K;
    const LOADER_BLOCK *Lb = &KeLoaderBlock;
    uint64_t RunStart = 0, RunEnd = 0;
    for (uint32_t i = 0; i <= Lb->MemoryDescriptorCount; i++) {
        const MEMORY_DESCRIPTOR *D = i < Lb->MemoryDescriptorCount ? &Lb->MemoryDescriptors[i] : NULL;
        bool Mappable = D && MiIsDirectMapped(D->Type);
        uint64_t Base = D ? HZ_ALIGN_DOWN(D->Base, PAGE_SIZE) : 0;
        uint64_t End = D ? HZ_ALIGN_UP(D->Base + D->Length, PAGE_SIZE) : 0;
        if (Mappable && RunEnd != RunStart && Base == RunEnd) {
            RunEnd = End;
            continue;
        }
        if (RunEnd != RunStart)
            MiMapRange(&MmKernelAddressSpace, Hhdm + RunStart, RunStart, RunEnd - RunStart, Wb4K, WbLarge);
        RunStart = Mappable ? Base : 0;
        RunEnd = Mappable ? End : 0;
    }

    /* Framebuffer: write-combining. */
    if (Lb->HasFramebuffer) {
        uint64_t FbVa = HZ_ALIGN_DOWN((uint64_t)Lb->Framebuffer.Address, PAGE_SIZE);
        uint64_t FbPa = FbVa - Hhdm;
        uint64_t FbSize = HZ_ALIGN_UP((uint64_t)Lb->Framebuffer.Pitch * Lb->Framebuffer.Height +
                                          ((uint64_t)Lb->Framebuffer.Address - FbVa), PAGE_SIZE);
        MiMapRange(&MmKernelAddressSpace, FbVa, FbPa, FbSize, Wb4K | PTE_PAT_4K | PTE_PWT,
                   WbLarge | PTE_PAT_LARGE | PTE_PWT);
    }

    /* Kernel image, section by section, frames taken from the boot tables. */
    for (uint64_t Va = (uint64_t)__KernelStart; Va < (uint64_t)__KernelEnd; Va += PAGE_SIZE) {
        uint64_t Pa = MiTranslate(BootPml4, Va);
        if (Pa == 0)
            KeBugCheckEx(MEMORY_MANAGEMENT, MM_FAILURE_KERNEL_TRANSLATION, Va, 0, 0);
        uint64_t Flags = PTE_PRESENT | PTE_GLOBAL;
        if (Va >= (uint64_t)__TextStart && Va < (uint64_t)__RodataStart)
            ; /* read + execute */
        else if (Va >= (uint64_t)__RodataStart && Va < (uint64_t)__DataStart)
            Flags |= MiNxBit;
        else
            Flags |= PTE_WRITE | MiNxBit;
        *MiGetPte(&MmKernelAddressSpace, Va, true) = (Pa & PTE_ADDRESS_MASK) | Flags;
    }

    __writecr3(MmKernelAddressSpace.Pml4);

    uint32_t Regs[4];
    __cpuid(7, 0, Regs);
    bool Smep = (Regs[1] & (1u << 7)) != 0;
    bool Smap = (Regs[1] & (1u << 20)) != 0;
    __writecr4(__readcr4() | CR4_PGE | (Smep ? CR4_SMEP : 0) | (Smap ? CR4_SMAP : 0));
    MmSmapEnabled = Smap;
    DbgPrint("Paging: kernel page tables active (W^X image%s, global pages%s%s)\n", MiNxBit ? ", NX" : "",
             Smep ? ", SMEP" : "", Smap ? ", SMAP" : "");
}

/* ---- Address spaces ------------------------------------------------------------------------------ */

/*
 * The current address space belongs to the running thread (KTHREAD.AddressSpace, initially its
 * process's); the context switch loads it into CR3. MmSwitchAddressSpace "attaches" the current
 * thread to another address space, which survives preemption.
 */
MMADDRESS_SPACE *MmGetCurrentAddressSpace(void)
{
    KTHREAD *Thread = KeGetCurrentThread();
    return Thread ? (MMADDRESS_SPACE *)Thread->AddressSpace : MiBootAddressSpace;
}

void MmSwitchAddressSpace(MMADDRESS_SPACE *As)
{
    KTHREAD *Thread = KeGetCurrentThread();
    if (Thread)
        Thread->AddressSpace = As;
    else
        MiBootAddressSpace = As;
    if ((__readcr3() & PTE_ADDRESS_MASK) != As->Pml4)
        __writecr3(As->Pml4);
}

STATUS MmCreateAddressSpace(MMADDRESS_SPACE **Out)
{
    MMADDRESS_SPACE *As = ExAllocatePoolZero(NonPagedPool, sizeof(*As), HZ_POOL_TAG('M', 'm', 'A', 's'));
    if (!As)
        return STATUS_NO_MEMORY;
    PFN_NUMBER Pml4 = MmAllocatePhysicalPage(true);
    if (Pml4 == MM_INVALID_PFN) {
        ExFreePool(As);
        return STATUS_NO_MEMORY;
    }
    MiSetPfnState(Pml4, MiPfnPageTable);
    As->Pml4 = Pml4 << PAGE_SHIFT;
    uint64_t *New = MiTable(As->Pml4);
    uint64_t *Kernel = MiTable(MmKernelAddressSpace.Pml4);
    for (unsigned i = 256; i < 512; i++)
        New[i] = Kernel[i];
    KeInitializeSpinLock(&As->Lock);
    *Out = As;
    return STATUS_SUCCESS;
}

static void MiFreeUserTables(uint64_t TablePhysical, int Level)
{
    uint64_t *Table = MiTable(TablePhysical);
    unsigned Count = Level == 3 ? 256 : 512; /* PML4: user half only */
    for (unsigned i = 0; i < Count; i++) {
        if (!(Table[i] & PTE_PRESENT))
            continue;
        if (Level > 1)
            MiFreeUserTables(Table[i] & PTE_ADDRESS_MASK, Level - 1);
        if (Level > 0)
            MiFreePageTablePage((Table[i] & PTE_ADDRESS_MASK) >> PAGE_SHIFT);
        Table[i] = 0;
    }
}

void MmDeleteAddressSpace(MMADDRESS_SPACE *As)
{
    if (As == MmGetCurrentAddressSpace() || As->IsKernel)
        KeBugCheckEx(MEMORY_MANAGEMENT, MM_FAILURE_DELETE_CURRENT_SPACE, (uint64_t)As, 0, 0);
    while (As->Vads)
        MmFreeVirtualMemory(As, (void *)As->Vads->AllocationBase);
    MiFreeUserTables(As->Pml4, 3);
    MiFreePageTablePage(As->Pml4 >> PAGE_SHIFT);
    ExFreePool(As);
}
