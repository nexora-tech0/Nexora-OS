/*
 * Nexora OS - mm/usercopy.c
 * Checked copies between kernel buffers and user addresses (mm/usercopy.asm does the work).
 *
 * Callers pass raw user pointers; the range check here guarantees the access stays below
 * MM_USER_TOP (so it can never reach kernel memory), and a fault on an unmapped or protected
 * user page comes back as STATUS_ACCESS_VIOLATION. Demand-zero pages are materialized normally
 * by the page-fault handler during the copy, so these must run below DISPATCH_LEVEL.
 */
#include "mi.h"

int64_t MmpCopyUserMemory(void *Destination, const void *Source, size_t Length);
int64_t MmpCopyUserString(char *Destination, const char *UserSource, size_t Capacity);
extern uint8_t MmpUserCopyStart[], MmpUserCopyEnd[], MmpUserCopyFault[];

bool MmSmapEnabled;

bool MmIsUserCopyFault(uint64_t Rip)
{
    return Rip >= (uint64_t)MmpUserCopyStart && Rip < (uint64_t)MmpUserCopyEnd;
}

uint64_t MmUserCopyFaultResume(void)
{
    return (uint64_t)MmpUserCopyFault;
}

STATUS MmCopyFromUser(void *Destination, const void *UserSource, size_t Length)
{
    if (Length == 0)
        return STATUS_SUCCESS;
    if (!MmIsUserRange(UserSource, Length))
        return STATUS_ACCESS_VIOLATION;
    return MmpCopyUserMemory(Destination, UserSource, Length) == 0 ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

STATUS MmCopyToUser(void *UserDestination, const void *Source, size_t Length)
{
    if (Length == 0)
        return STATUS_SUCCESS;
    if (!MmIsUserRange(UserDestination, Length))
        return STATUS_ACCESS_VIOLATION;
    return MmpCopyUserMemory(UserDestination, Source, Length) == 0 ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}

STATUS MmCopyStringFromUser(char *Buffer, size_t Capacity, const char *UserString)
{
    if (Capacity == 0)
        return STATUS_BUFFER_TOO_SMALL;
    /* Never read past the top of user space: clamp the capacity to what remains below it. */
    uint64_t Start = (uint64_t)UserString;
    if (Start >= MM_USER_TOP)
        return STATUS_ACCESS_VIOLATION;
    size_t Limit = Capacity;
    if (Limit > MM_USER_TOP - Start)
        Limit = MM_USER_TOP - Start;
    int64_t Copied = MmpCopyUserString(Buffer, UserString, Limit);
    if (Copied < 0) {
        Buffer[0] = 0;
        return STATUS_ACCESS_VIOLATION;
    }
    if (Copied == 0) {
        Buffer[Capacity - 1] = 0;
        return Limit < Capacity ? STATUS_ACCESS_VIOLATION : STATUS_NAME_TOO_LONG;
    }
    return STATUS_SUCCESS;
}
