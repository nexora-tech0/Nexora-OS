/*
 * Nexora OS - ke/syscall.c
 * System-service dispatch. hal/syscall.asm builds a KTRAP_FRAME on the thread's kernel stack
 * and calls KiSystemServiceHandler with interrupts enabled at PASSIVE_LEVEL. The service index
 * is in eax; the status goes back in rax. Every other register is restored from the frame, so
 * no kernel values leak to user mode.
 */
#include "ki.h"

static const KSYSTEM_SERVICE *KiServiceTable;
static uint32_t KiServiceCount;
static const KSYSTEM_SERVICE *KiWin32ServiceTable; /* numbers from KE_WIN32_SERVICE_BASE */
static uint32_t KiWin32ServiceCount;

void KeSetSystemServiceTable(const KSYSTEM_SERVICE *Table, uint32_t Count)
{
    KiServiceTable = Table;
    KiServiceCount = Count;
}

void KeSetWin32ServiceTable(const KSYSTEM_SERVICE *Table, uint32_t Count)
{
    KiWin32ServiceTable = Table;
    KiWin32ServiceCount = Count;
}

void KiSystemServiceHandler(KTRAP_FRAME *Frame)
{
    KTHREAD *Thread = KeGetCurrentThread();
    KeGetCurrentPrcb()->SystemCalls++;
    Thread->PreviousMode = UserMode;
    Thread->TrapFrame = Frame;

    uint32_t Index = (uint32_t)Frame->Rax;
    const KSYSTEM_SERVICE *Table = KiServiceTable;
    uint32_t Count = KiServiceCount;
    if (Index >= KE_WIN32_SERVICE_BASE) {
        Index -= KE_WIN32_SERVICE_BASE; /* the window manager's table (like NT's shadow table) */
        Table = KiWin32ServiceTable;
        Count = KiWin32ServiceCount;
    }
    STATUS Status;
    if (!Table || Index >= Count || !Table[Index].Routine) {
        Status = STATUS_INVALID_SYSTEM_SERVICE;
    } else {
        const KSYSTEM_SERVICE *Service = &Table[Index];
        uint64_t Arguments[KE_MAX_SERVICE_ARGUMENTS] = {Frame->R10, Frame->Rdx, Frame->R8, Frame->R9};
        Status = STATUS_SUCCESS;
        if (Service->ArgumentCount > 4) {
            /* [rsp] = return address into the stub's caller, then the 32-byte home area. */
            Status = MmCopyFromUser(&Arguments[4], (const void *)(Frame->Rsp + 40),
                                    (size_t)(Service->ArgumentCount - 4) * sizeof(uint64_t));
        }
        if (HZ_SUCCESS(Status))
            Status = Service->Routine(Arguments);
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
            KeBugCheckEx(IRQL_GT_ZERO_AT_SYSTEM_SERVICE, Index, KeGetCurrentIrql(), (uint64_t)Service->Routine, 0);
        }
    }
    Frame->Rax = (uint32_t)Status;

    Thread->TrapFrame = NULL;
    Thread->PreviousMode = KernelMode;
    HalDisableInterrupts();
    KiCheckUserReturn(Frame);
}
