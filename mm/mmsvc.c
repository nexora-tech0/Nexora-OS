/*
 * Nexora OS - mm/mmsvc.c
 * Virtual-memory system services. They operate on the caller's own process (the handle must
 * name it); cross-process memory operations need attach support in the services and come later.
 * Image ranges (MEM_IMAGE) cannot be released through these calls.
 */
#include "mi.h"
#include "ob.h"
#include "ps.h"

static STATUS MiReferenceOwnProcess(uint64_t Handle, EPROCESS **Process)
{
    STATUS Status = ObReferenceObjectByHandle((HANDLE)Handle, PsProcessType, PROCESS_VM_OPERATION, UserMode,
                                              (void **)Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (*Process != PsGetCurrentProcess()) {
        ObDereferenceObject(*Process);
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

/* AllocateVirtualMemory(HANDLE Process, void **Base, uint64_t *Size, uint32_t Type, uint32_t PageProtection) */
STATUS NtAllocateVirtualMemory(const uint64_t *Args)
{
    uint32_t Type = (uint32_t)Args[3];
    uint32_t Protection;
    if (!(Type & (MEM_RESERVE | MEM_COMMIT)) || (Type & ~(MEM_RESERVE | MEM_COMMIT)))
        return STATUS_INVALID_PARAMETER;
    if (!MmPageFlagsToProtection((uint32_t)Args[4], &Protection))
        return STATUS_INVALID_PAGE_PROTECTION;
    void *Base;
    uint64_t Size;
    STATUS Status = MmCopyFromUser(&Base, (const void *)Args[1], sizeof(Base));
    if (HZ_SUCCESS(Status))
        Status = MmCopyFromUser(&Size, (const void *)Args[2], sizeof(Size));
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Size == 0 || Size > MM_USER_TOP || (uint64_t)Base >= MM_USER_TOP)
        return STATUS_INVALID_PARAMETER;

    EPROCESS *Process;
    Status = MiReferenceOwnProcess(Args[0], &Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    uint64_t Offset = (uint64_t)Base & (PAGE_SIZE - 1);
    Status = MmAllocateVirtualMemory(Process->AddressSpace, &Base, Size, Type, Protection);
    ObDereferenceObject(Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    Size = HZ_ALIGN_UP(Size + Offset, PAGE_SIZE);
    Status = MmCopyToUser((void *)Args[1], &Base, sizeof(Base));
    if (HZ_SUCCESS(Status))
        Status = MmCopyToUser((void *)Args[2], &Size, sizeof(Size));
    return Status;
}

/* FreeVirtualMemory(HANDLE Process, void *Base, uint32_t FreeType) */
STATUS NtFreeVirtualMemory(const uint64_t *Args)
{
    if ((uint32_t)Args[2] != MEM_RELEASE)
        return STATUS_INVALID_PARAMETER; /* decommit without release is not supported yet */
    if (Args[1] >= MM_USER_TOP)
        return STATUS_INVALID_PARAMETER;
    EPROCESS *Process;
    STATUS Status = MiReferenceOwnProcess(Args[0], &Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_MEMORY_BASIC_INFORMATION Info;
    Status = MmQueryVirtualMemory(Process->AddressSpace, Args[1], &Info);
    if (HZ_SUCCESS(Status) && (Info.State == MEM_FREE || Info.AllocationBase != Args[1]))
        Status = STATUS_INVALID_PARAMETER;
    else if (HZ_SUCCESS(Status) && (Info.Type == MEM_IMAGE || Info.Type == MEM_MAPPED))
        Status = STATUS_CONFLICTING_ADDRESSES; /* images stay; views go with UnmapViewOfSection */
    if (HZ_SUCCESS(Status))
        Status = MmFreeVirtualMemory(Process->AddressSpace, (void *)Args[1]);
    ObDereferenceObject(Process);
    return Status;
}

/* QueryVirtualMemory(HANDLE Process, void *Address, HZ_MEMORY_BASIC_INFORMATION *Info, uint64_t InfoLength) */
STATUS NtQueryVirtualMemory(const uint64_t *Args)
{
    if (Args[3] != sizeof(HZ_MEMORY_BASIC_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;
    if (Args[1] >= MM_USER_TOP)
        return STATUS_INVALID_PARAMETER;
    EPROCESS *Process;
    STATUS Status = MiReferenceOwnProcess(Args[0], &Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_MEMORY_BASIC_INFORMATION Info;
    Status = MmQueryVirtualMemory(Process->AddressSpace, Args[1], &Info);
    ObDereferenceObject(Process);
    if (HZ_SUCCESS(Status))
        Status = MmCopyToUser((void *)Args[2], &Info, sizeof(Info));
    return Status;
}

/* ProtectVirtualMemory(HANDLE Process, void *Base, uint64_t Size, uint32_t PageProtection, uint32_t *Old) */
STATUS NtProtectVirtualMemory(const uint64_t *Args)
{
    uint32_t Protection, Old = 0;
    if (!MmPageFlagsToProtection((uint32_t)Args[3], &Protection))
        return STATUS_INVALID_PAGE_PROTECTION;
    if (Args[1] >= MM_USER_TOP || Args[2] == 0 || Args[2] > MM_USER_TOP - Args[1])
        return STATUS_INVALID_PARAMETER;
    if (Args[4]) {
        STATUS Probe = MmCopyToUser((void *)Args[4], &Old, sizeof(Old));
        if (!HZ_SUCCESS(Probe))
            return Probe;
    }
    EPROCESS *Process;
    STATUS Status = MiReferenceOwnProcess(Args[0], &Process);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = MmProtectVirtualMemory(Process->AddressSpace, (void *)Args[1], Args[2], Protection, &Old);
    ObDereferenceObject(Process);
    if (HZ_SUCCESS(Status) && Args[4]) {
        uint32_t PageFlags = MmProtectionToPageFlags(Old);
        Status = MmCopyToUser((void *)Args[4], &PageFlags, sizeof(PageFlags));
    }
    return Status;
}
