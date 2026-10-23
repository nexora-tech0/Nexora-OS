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
#include "ps.h"
#include "rtl.h"

#define PSP_PROCESS_TAG HZ_POOL_TAG('P', 's', 'P', 'r')
#define PSP_THREAD_TAG HZ_POOL_TAG('P', 's', 'T', 'h')
#define PSP_IMAGE_TAG HZ_POOL_TAG('P', 's', 'I', 'm')

#define PSP_USER_THREAD_PRIORITY KI_THREAD_PRIORITY_DEFAULT
#define PSP_DEFAULT_STACK_RESERVE (1024 * 1024)
#define PSP_MIN_STACK_RESERVE (64 * 1024)
#define PSP_MAX_STACK_RESERVE (16 * 1024 * 1024)
#define PSP_MAX_IMAGE_FILE (64ull * 1024 * 1024)

extern KSPIN_LOCK PspLock;
extern LIST_ENTRY PspActiveProcessList;

/* ps/peload.c: maps the image into Process (reserved, copied, relocated, protected). */
STATUS PspMapImage(EPROCESS *Process, const void *File, uint64_t FileSize, uint32_t Flags);

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
