/*
 * Nexora OS - mm/mmtest.c
 * Boot-time memory-manager self-tests. Each test logs one line; failures are reported as
 * errors (the boot then ends DEGRADED) instead of stopping the system, so every result is seen.
 * The final check compares allocated-page counts before and after to catch leaks.
 */
#include "mi.h"

#define MI_TEST_TAG HZ_POOL_TAG('M', 'm', 'T', 's')

static uint64_t MiRandom(uint64_t *State)
{
    uint64_t x = *State;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *State = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static bool MiTestPhysicalPages(void)
{
    enum { Count = 512 };
    static PFN_NUMBER Pages[Count];
    uint64_t Before = MmQueryAvailablePages();
    bool Ok = true;
    for (int i = 0; i < Count; i++) {
        Pages[i] = MmAllocatePhysicalPage(true);
        if (Pages[i] == MM_INVALID_PFN) {
            Ok = false;
            break;
        }
        uint64_t *Page = MmPfnToVirtual(Pages[i]);
        for (int w = 0; w < 512 && Ok; w++)
            Ok = Page[w] == 0;
        for (int w = 0; w < 512; w++)
            Page[w] = Pages[i] * 0x9E3779B97F4A7C15ull + (uint64_t)w;
    }
    for (int i = 0; i < Count && Ok; i++) {
        uint64_t *Page = MmPfnToVirtual(Pages[i]);
        Ok = Page[0] == Pages[i] * 0x9E3779B97F4A7C15ull && Page[511] == Pages[i] * 0x9E3779B97F4A7C15ull + 511;
    }
    for (int i = 0; i < Count; i++) {
        if (Pages[i] != MM_INVALID_PFN)
            MmFreePhysicalPage(Pages[i]);
    }
    Ok = Ok && MmQueryAvailablePages() == Before;
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL, "MmTest: %d zeroed pages allocated, verified, freed: %s\n",
               Count, Ok ? "OK" : "FAILED");
    return Ok;
}

static bool MiTestContiguous(void)
{
    PFN_NUMBER First;
    bool Ok = HZ_SUCCESS(MmAllocateContiguousPages(64, 1ull << 32, 16, &First));
    if (Ok) {
        Ok = (First % 16) == 0 && ((First + 64) << PAGE_SHIFT) <= (1ull << 32);
        MmFreeContiguousPages(First, 64);
    }
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL, "MmTest: 64 contiguous pages below 4 GiB, 64 KiB aligned: %s\n",
               Ok ? "OK" : "FAILED");
    return Ok;
}

static bool MiTestPool(uint32_t *Operations)
{
    enum { Slots = 256, Ops = 6000 };
    static uint8_t *Blocks[Slots];
    static uint32_t Sizes[Slots];
    POOL_STATISTICS Before, After;
    ExQueryPoolStatistics(&Before);
    uint64_t Seed = 0x484F52495A4F4Eull;
    bool Ok = true;

    for (int op = 0; op < Ops && Ok; op++) {
        int i = (int)(MiRandom(&Seed) % Slots);
        if (!Blocks[i]) {
            uint64_t r = MiRandom(&Seed);
            uint32_t Size = (r % 4 == 0) ? (uint32_t)(r % 12000) + 1 : (uint32_t)(r % 600) + 1;
            Blocks[i] = ExAllocatePoolWithTag(NonPagedPool, Size, MI_TEST_TAG);
            if (!Blocks[i] || ((uint64_t)Blocks[i] & 15)) {
                Ok = false;
                break;
            }
            Sizes[i] = Size;
            memset(Blocks[i], (uint8_t)(i ^ Size), Size);
        } else {
            uint8_t Expected = (uint8_t)(i ^ Sizes[i]);
            for (uint32_t b = 0; b < Sizes[i] && Ok; b++)
                Ok = Blocks[i][b] == Expected;
            ExFreePoolWithTag(Blocks[i], MI_TEST_TAG);
            Blocks[i] = NULL;
        }
    }
    for (int i = 0; i < Slots; i++) {
        if (Blocks[i]) {
            ExFreePoolWithTag(Blocks[i], MI_TEST_TAG);
            Blocks[i] = NULL;
        }
    }
    ExQueryPoolStatistics(&After);
    Ok = Ok && After.BytesInUse == Before.BytesInUse && After.LargePages == Before.LargePages;
    *Operations = Ops;
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL,
               "MmTest: %d pool operations (1 B - 12 KB), patterns verified, no leaks: %s\n", Ops, Ok ? "OK" : "FAILED");
    return Ok;
}

static bool MiTestDemandZeroKernel(uint64_t *Faults)
{
    enum { Pages = 16 };
    void *Base = NULL;
    bool Ok = HZ_SUCCESS(MmAllocateVirtualMemory(&MmKernelAddressSpace, &Base, Pages * PAGE_SIZE, MEM_COMMIT,
                                                 MM_PROT_READWRITE));
    uint64_t Before = MiDemandZeroFaults;
    if (Ok) {
        volatile uint64_t *p = (volatile uint64_t *)Base;
        for (int i = 0; i < Pages && Ok; i++) {
            Ok = p[i * 512] == 0 && p[i * 512 + 511] == 0; /* read fault maps a zero page */
            p[i * 512 + 7] = (uint64_t)i;                   /* write hits the now-present page */
        }
        for (int i = 0; i < Pages && Ok; i++)
            Ok = p[i * 512 + 7] == (uint64_t)i;
        Ok = Ok && MiDemandZeroFaults - Before == Pages;
        Ok = HZ_SUCCESS(MmFreeVirtualMemory(&MmKernelAddressSpace, Base)) && Ok;
    }
    *Faults += MiDemandZeroFaults - Before;
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL, "MmTest: %d demand-zero kernel pages: %s\n", Pages,
               Ok ? "OK" : "FAILED");
    return Ok;
}

static bool MiTestAddressSpace(uint64_t *Faults)
{
    MMADDRESS_SPACE *As;
    if (!HZ_SUCCESS(MmCreateAddressSpace(&As)))
        return false;
    void *Base = (void *)0x400000;
    uint64_t Before = MiDemandZeroFaults;
    bool Ok = HZ_SUCCESS(MmAllocateVirtualMemory(As, &Base, 3 * PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, MM_PROT_READWRITE)) &&
              Base == (void *)0x400000;
    void *Hinted = NULL;
    Ok = Ok && HZ_SUCCESS(MmAllocateVirtualMemory(As, &Hinted, 16 * PAGE_SIZE, MEM_RESERVE, MM_PROT_READ));
    if (Ok) {
        /* User pages are only reachable through the checked copy routines (SMAP). */
        MmSwitchAddressSpace(As);
        uint32_t Values[3] = {0x48525A4E, 0x4F53, 0xFFFFFFFF};
        Ok = HZ_SUCCESS(MmCopyToUser(Base, &Values[0], 4)) &&
             HZ_SUCCESS(MmCopyToUser((uint8_t *)Base + 8192, &Values[1], 4));
        uint32_t Back[2] = {0, 0};
        Ok = Ok && HZ_SUCCESS(MmCopyFromUser(Back, Base, 8)) && Back[0] == Values[0] && Back[1] == 0;
        Ok = Ok && HZ_SUCCESS(MmCopyFromUser(&Back[0], (uint8_t *)Base + 8192, 4)) && Back[0] == Values[1];
        /* Reserved-only and unmapped pages fail cleanly instead of crashing. */
        Ok = Ok && MmCopyFromUser(Back, Hinted, 4) == STATUS_ACCESS_VIOLATION;
        Ok = Ok && MmCopyToUser((void *)0x1000, &Values[2], 4) == STATUS_ACCESS_VIOLATION;
        Ok = Ok && MmCopyToUser((void *)(MM_USER_TOP - 2), &Values[2], 4) == STATUS_ACCESS_VIOLATION;
        /* Commit four pages in the middle of the reservation: the VAD splits in three. */
        void *Middle = (uint8_t *)Hinted + 4 * PAGE_SIZE;
        HZ_MEMORY_BASIC_INFORMATION Info;
        Ok = Ok && HZ_SUCCESS(MmAllocateVirtualMemory(As, &Middle, 4 * PAGE_SIZE, MEM_COMMIT, MM_PROT_READWRITE)) &&
             HZ_SUCCESS(MmCopyToUser((uint8_t *)Middle + PAGE_SIZE, &Values[0], 4)) &&
             HZ_SUCCESS(MmQueryVirtualMemory(As, (uint64_t)Middle + 100, &Info)) &&
             Info.BaseAddress == (uint64_t)Middle && Info.RegionSize == 4 * PAGE_SIZE &&
             Info.AllocationBase == (uint64_t)Hinted && Info.State == MEM_COMMIT && Info.Protect == PAGE_READWRITE;
        Ok = Ok && HZ_SUCCESS(MmQueryVirtualMemory(As, (uint64_t)Hinted, &Info)) && Info.State == MEM_RESERVE &&
             Info.RegionSize == 4 * PAGE_SIZE;
        /* Read-only after a protection change: writes fail, reads still work. */
        Ok = Ok && HZ_SUCCESS(MmProtectVirtualMemory(As, Middle, 4 * PAGE_SIZE, MM_PROT_READ, NULL)) &&
             MmCopyToUser((uint8_t *)Middle + PAGE_SIZE, &Values[1], 4) == STATUS_ACCESS_VIOLATION &&
             HZ_SUCCESS(MmCopyFromUser(&Back[0], (uint8_t *)Middle + PAGE_SIZE, 4)) && Back[0] == Values[0];
        char Text[8];
        Ok = Ok && HZ_SUCCESS(MmCopyToUser((uint8_t *)Base + 16, "HZ", 3)) &&
             HZ_SUCCESS(MmCopyStringFromUser(Text, sizeof(Text), (const char *)Base + 16)) && strcmp(Text, "HZ") == 0;
        MmSwitchAddressSpace(&MmKernelAddressSpace);
        /* The user page is invisible from the kernel address space; all three pieces of the split
           reservation are released together. */
        Ok = Ok && MmGetPhysicalAddress(Base) == 0 && MiDemandZeroFaults - Before == 3 &&
             HZ_SUCCESS(MmFreeVirtualMemory(As, Hinted)) && !As->Vads->Next;
    }
    *Faults += MiDemandZeroFaults - Before;
    MmDeleteAddressSpace(As);
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL,
               "MmTest: private address space at 0x400000 (user copies, commit split, protect, query): %s\n",
               Ok ? "OK" : "FAILED");
    return Ok;
}

static bool MiTestKernelStack(void)
{
    uint8_t *Top = MmCreateKernelStack();
    bool Ok = Top != NULL;
    if (Ok) {
        volatile uint8_t *Lowest = Top - KERNEL_STACK_SIZE;
        Lowest[0] = 0x5A;
        Top[-1] = 0xA5;
        Ok = Lowest[0] == 0x5A && MmIsKernelStackGuard((uint64_t)Lowest - 1) && !MmIsKernelStackGuard((uint64_t)Lowest);
        MmDeleteKernelStack(Top);
    }
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL, "MmTest: %llu KiB kernel stack with guard page: %s\n",
               (unsigned long long)(KERNEL_STACK_SIZE / 1024), Ok ? "OK" : "FAILED");
    return Ok;
}

static bool MiTestIoMapping(void)
{
    PFN_NUMBER Pfn = MmAllocatePhysicalPage(true);
    if (Pfn == MM_INVALID_PFN)
        return false;
    volatile uint32_t *Direct = MmPfnToVirtual(Pfn);
    Direct[100] = 0xC0FFEE42;
    /* Write-back alias on purpose: mapping RAM with a different memory type than the direct map
       (e.g. UC) is an unsupported mixed-type alias on x86. */
    volatile uint32_t *Mapped = MmMapIoSpace((Pfn << PAGE_SHIFT) + 0x10, 0x200, 0);
    bool Ok = Mapped && Mapped[(400 - 0x10) / 4] == 0xC0FFEE42 && MmGetPhysicalAddress((const void *)Mapped) ==
                                                                        (Pfn << PAGE_SHIFT) + 0x10;
    if (Mapped)
        MmUnmapIoSpace((void *)Mapped, 0x200);
    MmFreePhysicalPage(Pfn);
    DbgPrintEx(Ok ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL, "MmTest: system-VA mapping of a physical range: %s\n",
               Ok ? "OK" : "FAILED");
    return Ok;
}

bool MmRunSelfTests(void)
{
    MM_STATISTICS Before, After;
    MmQueryStatistics(&Before);
    uint64_t StartUs = KeQueryUptimeMicroseconds();

    uint32_t PoolOps = 0;
    uint64_t Faults = 0;
    int Passed = 0, Total = 0;
    Total++, Passed += MiTestPhysicalPages();
    Total++, Passed += MiTestContiguous();
    Total++, Passed += MiTestPool(&PoolOps);
    Total++, Passed += MiTestDemandZeroKernel(&Faults);
    Total++, Passed += MiTestAddressSpace(&Faults);
    Total++, Passed += MiTestKernelStack();
    Total++, Passed += MiTestIoMapping();

    MmQueryStatistics(&After);
    bool NoLeak = After.ActivePages == Before.ActivePages;
    Total++, Passed += NoLeak;
    if (!NoLeak)
        DbgPrintEx(DPFLTR_ERROR_LEVEL, "MmTest: %lld allocated pages leaked\n",
                   (long long)(After.ActivePages - Before.ActivePages));

    uint64_t ElapsedMs = (KeQueryUptimeMicroseconds() - StartUs) / 1000;
    DbgPrintEx(Passed == Total ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "Memory self-tests: %d/%d passed (%u pool operations, %llu demand-zero faults, %llu ms)\n", Passed,
               Total, PoolOps, (unsigned long long)Faults, (unsigned long long)ElapsedMs);
    ExDumpPoolUsage(6);
    return Passed == Total;
}
