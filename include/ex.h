/*
 * Nexora OS - include/ex.h
 * Executive support (Ex*): the kernel pool allocator, the system worker thread, event objects,
 * the system-service table and the miscellaneous system services.
 */
#ifndef EX_H
#define EX_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

typedef enum _POOL_TYPE {
    NonPagedPool = 0, /* always resident; usable at IRQL <= DISPATCH_LEVEL */
} POOL_TYPE;

/* Four-character tag, stored so that it reads left to right in a memory dump ("Pool"). */
#define HZ_POOL_TAG(a, b, c, d)                                                                  \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) |    \
     ((uint32_t)(uint8_t)(d) << 24))

/* Returns 16-byte aligned memory or NULL. */
void *ExAllocatePoolWithTag(POOL_TYPE Type, size_t Size, uint32_t Tag);
void *ExAllocatePoolZero(POOL_TYPE Type, size_t Size, uint32_t Tag);
/* Tag 0 skips the tag check. Freeing with the wrong tag or twice stops the system. */
void ExFreePoolWithTag(void *Pointer, uint32_t Tag);
void ExFreePool(void *Pointer);

typedef struct _POOL_STATISTICS {
    uint64_t Allocations;
    uint64_t Frees;
    uint64_t BytesInUse;      /* requested bytes currently allocated */
    uint64_t SlabPages;       /* pages backing small blocks */
    uint64_t LargePages;      /* pages backing large blocks */
} POOL_STATISTICS;

void ExQueryPoolStatistics(POOL_STATISTICS *Stats);
/* Logs the tags with the most bytes in use. */
void ExDumpPoolUsage(uint32_t MaxTags);

/* Internal: called by MmInitializeSystem once the page tables are live. */
void ExInitializePool(void);

/* ---- Worker thread -----------------------------------------------------------------------------
 * Work that must run at PASSIVE_LEVEL but is discovered at DISPATCH_LEVEL (e.g. deleting an
 * object whose last reference was dropped by the thread reaper) is queued to the system worker
 * thread. Items queued before the worker starts run once it does.
 */
typedef void (*WORKER_THREAD_ROUTINE)(void *Context);

typedef struct _WORK_QUEUE_ITEM {
    struct _WORK_QUEUE_ITEM *Next;
    WORKER_THREAD_ROUTINE Routine;
    void *Context;
    volatile bool Queued;
} WORK_QUEUE_ITEM;

void ExInitializeWorkItem(WORK_QUEUE_ITEM *Item, WORKER_THREAD_ROUTINE Routine, void *Context);
/* IRQL <= DISPATCH_LEVEL. Queuing an item that is already queued does nothing. */
void ExQueueWorkItem(WORK_QUEUE_ITEM *Item);
/* Phase 1: starts the worker thread. */
void ExInitializeWorkerThread(void);

/* ---- Executive objects and system services -------------------------------------------------- */

/* Phase 0 (after Ob and Ps): Ex object types and the system-service table. */
void ExInitializeSystem(void);

/* System services implemented by Ex (ex/event.c, ex/sysinfo.c). */
STATUS NtCreateEvent(const uint64_t *Args);
STATUS NtSetEvent(const uint64_t *Args);
STATUS NtResetEvent(const uint64_t *Args);
STATUS NtDisplayString(const uint64_t *Args);
STATUS NtDelayExecution(const uint64_t *Args);
STATUS NtYieldExecution(const uint64_t *Args);
STATUS NtQueryUptime(const uint64_t *Args);
STATUS NtQuerySystemInformation(const uint64_t *Args);
STATUS NtShutdownSystem(const uint64_t *Args);
STATUS NtQuerySystemTime(const uint64_t *Args);
STATUS NtResolveHostA(const uint64_t *Args);
STATUS NtTcpConnect(const uint64_t *Args);
STATUS NtTcpSend(const uint64_t *Args);
STATUS NtTcpReceive(const uint64_t *Args);
STATUS NtTcpClose(const uint64_t *Args);
STATUS NtGenerateRandomBytes(const uint64_t *Args);
/* Narrow Win32 kernel32 compatibility services implemented by Ps. */
STATUS NtWinHeapAlloc(const uint64_t *Args);
STATUS NtWinHeapFree(const uint64_t *Args);
STATUS NtWinGetModuleHandle(const uint64_t *Args);
STATUS NtWinGetProcAddress(const uint64_t *Args);
STATUS NtWinCreateFile(const uint64_t *Args);
STATUS NtWinGetFileAttributes(const uint64_t *Args);
STATUS NtWinSocketApi(const uint64_t *Args);
/* Called when the last process thread exits, including abnormal Windows PE termination. */
void ExCleanupWinSocketsForCurrentProcess(void);

/* Flushes volumes and powers off (HZ_SHUTDOWN_POWER_OFF) or restarts (HZ_SHUTDOWN_REBOOT). */
HZ_NORETURN void ExShutdownSystem(uint32_t Action);

HZ_EXTERN_C_END

#endif /* EX_H */
