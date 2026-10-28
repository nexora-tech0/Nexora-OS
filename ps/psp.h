/*
 * Nexora OS - ps/psp.h
 * Process-structure internals shared by the files in ps/.
 *
 * PspLock (spinlock) protects the active-process list, every process's thread list and
 * ActiveThreads count, the Terminating/Exited flags and PID allocation.
 */
#ifndef PSP_H
#define PSP_H

#include "ex.h"
#include "hal.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "ob.h"
#include "pe.h"
#include "ps.h"
#include "rtl.h"

#define PSP_PROCESS_TAG HZ_POOL_TAG('P', 's', 'P', 'r')
#define PSP_THREAD_TAG HZ_POOL_TAG('P', 's', 'T', 'h')
#define PSP_IMAGE_TAG HZ_POOL_TAG('P', 's', 'I', 'm')
#define PSP_WIN_MODULE_TAG HZ_POOL_TAG('P', 's', 'W', 'm')
#define PSP_WIN_MODULE_NAME_MAX 260u
#define PSP_WIN_MODULE_PATH_MAX 512u
#define PSP_WIN_MODULE_MAIN_IMAGE 0x1u

typedef struct _PSP_WIN_MODULE {
    LIST_ENTRY Link;
    uint64_t Base;
    uint64_t Size;
    uint32_t Flags;
    char Name[PSP_WIN_MODULE_NAME_MAX];
    char Path[PSP_WIN_MODULE_PATH_MAX];
} PSP_WIN_MODULE;

#define PSP_USER_THREAD_PRIORITY KI_THREAD_PRIORITY_DEFAULT
#define PSP_DEFAULT_STACK_RESERVE (1024 * 1024)
#define PSP_MIN_STACK_RESERVE (64 * 1024)
#define PSP_MAX_STACK_RESERVE (16 * 1024 * 1024)
#define PSP_MAX_IMAGE_FILE (256ull * 1024 * 1024)
#define PSP_WINCOMPAT_THUNK_PAGE_SIZE (PAGE_SIZE * 16u)

extern KSPIN_LOCK PspLock;
extern LIST_ENTRY PspActiveProcessList;

/* ps/peload.c: maps the image into Process (reserved, copied, relocated, protected). */
STATUS PspMapImage(EPROCESS *Process, const void *File, uint64_t FileSize, uint32_t Flags);
STATUS PspRegisterWinModule(EPROCESS *Process, const char *Name, const char *Path,
                           uint64_t Base, uint64_t Size, uint32_t Flags);
STATUS PspLookupWinModule(EPROCESS *Process, const char *Name, uint64_t *Base, uint64_t *Size);
STATUS PspLookupWinModuleByBase(EPROCESS *Process, uint64_t Base, char *Name,
                                size_t NameCapacity, uint64_t *Size, uint32_t *Flags);
void PspFreeWinModules(EPROCESS *Process);
/* ps/wincompat.c: patches supported Windows imports to a process-local thunk page. */
STATUS PspResolveWindowsImports(EPROCESS *Process, uint8_t *Image, uint64_t ImageSize,
                                const PE_HEADERS *Headers, uint64_t *ThunkBase,
                               uint8_t ThunkPage[PSP_WINCOMPAT_THUNK_PAGE_SIZE]);
STATUS PspBuildWindowsImportReport(const void *File, uint64_t FileSize, char *Buffer, size_t Capacity);
/* ps/wincompat.c: release compatibility-heap bookkeeping before destroying a process. */
void PspFreeWindowsHeapAllocations(EPROCESS *Process);

/* ps/thread.c */
void PspUserReturnCallout(KTRAP_FRAME *Frame);
void PspUserExceptionRoutine(KTRAP_FRAME *Frame, STATUS ExceptionStatus, uint64_t Address);
void PspDeleteThread(void *Object);

/* ps/process.c */
void PspDeleteProcess(void *Object);
/* Called by the last exiting thread (PASSIVE_LEVEL): closes handles, signals the process. */
void PspProcessExited(EPROCESS *Process);

/* ps/process.c: status names for logs ("STATUS_ACCESS_VIOLATION"). */
const char *PspStatusName(STATUS Status);

#endif /* PSP_H */
