/*
 * Nexora OS - include/ps.h
 * Process structure (Ps*): process and thread objects, the PE32+ image loader, system threads,
 * user threads and termination.
 *
 * EPROCESS and ETHREAD are object-manager objects whose bodies begin with the kernel's KPROCESS
 * and KTHREAD, so they are waitable (a process is signaled when its last thread exits, a thread
 * when it terminates). A thread holds a reference to its process; a running thread holds a
 * reference to itself, dropped after its kernel stack is freed.
 *
 * Termination: PsTerminateProcess marks the process and asks every thread to run the
 * user-return callout (ke/usermode.c), which exits the thread in its own context; threads
 * blocked in alertable waits are woken with STATUS_ALERTED. The last thread to exit closes the
 * handle table and signals the process; the address space goes when the object is deleted.
 */
#ifndef PS_H
#define PS_H

#include "hzbase.h"
#include "ke.h"
#include "mm.h"
#include "ob.h"
#include "uapi/hzapi.h"

HZ_EXTERN_C_BEGIN

/* ---- PE32+ images ---------------------------------------------------------------------------- */

typedef struct _PE_IMAGE_INFO {
    uint16_t Machine;
    uint16_t SectionCount;
    uint16_t Subsystem;
    uint16_t Characteristics;
    uint32_t EntryPointRva;
    uint64_t ImageBase;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t SectionAlignment;
    uint64_t StackReserve;
    uint64_t StackCommit;
    bool Relocatable;
} PE_IMAGE_INFO;

/* Validates a PE32+ AMD64 image held in memory and extracts the key header fields. */
STATUS PsValidatePeImage(const void *Image, size_t Size, PE_IMAGE_INFO *Info);

/* ---- Objects ----------------------------------------------------------------------------------- */

typedef struct _EPROCESS {
    KPROCESS Pcb;                 /* must be first (waitable) */
    MMADDRESS_SPACE *AddressSpace;
    HANDLE_TABLE *ObjectTable;    /* NULL once the process has exited */
    uint32_t ProcessId;
    uint32_t ParentProcessId;
    STATUS ExitStatus;
    volatile bool Terminating;
    bool Exited;
    bool IsSystem;
    LIST_ENTRY ActiveProcessLinks; /* PspActiveProcessList */
    LIST_ENTRY ThreadListHead;     /* ETHREAD.ThreadListEntry, under PspLock */
    uint32_t ActiveThreads;
    uint64_t ImageBase;
    uint64_t ImageSize;
    uint64_t EntryPoint;
    uint64_t PebAddress;
    uint64_t StackReserve;
    uint64_t CreateTimeUs;
    uint64_t ExitTimeUs;
    char ImageName[32];
} EPROCESS;

typedef struct _ETHREAD {
    KTHREAD Tcb;                  /* must be first (waitable) */
    EPROCESS *Process;            /* referenced */
    LIST_ENTRY ThreadListEntry;
    STATUS ExitStatus;
    volatile bool Terminating;
    bool HasExited;
    bool IsUserThread;
    /* System threads */
    KSTART_ROUTINE SystemRoutine;
    void *SystemContext;
    /* User threads */
    uint64_t UserStartAddress;
    uint64_t UserArgument1;
    uint64_t UserArgument2;
    uint64_t TebAddress;
    uint64_t UserStackReservation; /* allocation base (guard page at the bottom) */
    uint64_t UserStackBase;        /* top, exclusive */
    uint64_t UserStackLimit;       /* lowest committed address */
    void *Win32Thread;             /* win32k per-thread state (message queue, windows), or NULL */
} ETHREAD;

extern OBJECT_TYPE *PsProcessType;
extern OBJECT_TYPE *PsThreadType;
extern EPROCESS *PsInitialSystemProcess;

static inline ETHREAD *PsGetCurrentThread(void)
{
    return (ETHREAD *)KeGetCurrentThread();
}

/* The executive process of the running thread (NULL before the scheduler starts). */
static inline EPROCESS *PsGetCurrentProcess(void)
{
    KTHREAD *Thread = KeGetCurrentThread();
    return Thread ? (EPROCESS *)Thread->Process : NULL;
}

/* Phase 0, after Ob and before the scheduler: object types and the System process (PID 4). */
void PsInitializeSystem(void);

/* System thread in the System process. If Thread is non-NULL it receives a reference. */
STATUS PsCreateSystemThread(KSTART_ROUTINE Routine, void *Context, KPRIORITY Priority, const char *Name,
                            ETHREAD **Thread);

/* ---- User processes ------------------------------------------------------------------------- */

#define PS_CREATE_FORCE_RELOCATION 0x1 /* load away from the preferred base (loader self-test) */
#define PS_CREATE_SUSPENDED_START 0x2  /* reserved */

/*
 * Creates a process from a PE32+ image named by an object path (e.g.
 * "\??\C:\Nexora\System\notepad.exe") and starts its initial thread. *Process receives a
 * reference. The initial thread starts at the entry point with rcx = PEB address.
 */
STATUS PsCreateProcess(const char *ImagePath, const char *CommandLine, uint32_t Flags, EPROCESS **Process);
/* The same with standard handles: referenced objects (NULL = none) the new process gets handles
   to (with the given access) before it starts; their values go into its PEB. */
typedef struct _PS_STANDARD_HANDLES {
    void *Object[3];          /* input, output, error */
    ACCESS_MASK Access[3];
} PS_STANDARD_HANDLES;
STATUS PsCreateProcessEx(const char *ImagePath, const char *CommandLine, uint32_t Flags,
                         const PS_STANDARD_HANDLES *Standard, EPROCESS **Process);
/* Returns the process with that id, referenced (STATUS_INVALID_CID if there is none). */
STATUS PsLookupProcessById(uint32_t ProcessId, EPROCESS **Process);

/* Creates and starts a user thread: rip = StartAddress, rcx = Argument1, rdx = Argument2. */
STATUS PsCreateUserThread(EPROCESS *Process, uint64_t StartAddress, uint64_t Argument1, uint64_t Argument2,
                          ETHREAD **Thread);

/* Requests termination of every thread in Process; returns without waiting. */
STATUS PsTerminateProcess(EPROCESS *Process, STATUS ExitStatus);
/* Requests termination of Thread (immediate if it is the caller). */
STATUS PsTerminateThread(ETHREAD *Thread, STATUS ExitStatus);
/* Exits the calling thread. */
HZ_NORETURN void PsExitThread(STATUS ExitStatus);

/* Called in the exiting thread's context (PASSIVE_LEVEL) when it has win32k state. */
typedef void (*PS_WIN32_THREAD_CALLOUT)(ETHREAD *Thread);
void PsSetWin32ThreadCallout(PS_WIN32_THREAD_CALLOUT Callout);

uint32_t PsQueryProcessCount(void);
/* Fills up to Count entries; returns the number of processes. */
uint32_t PsQueryProcessList(HZ_SYSTEM_PROCESS_INFORMATION *Entries, uint32_t Count);

/* Attaches the current thread to Process's address space (for reading/writing its memory). */
MMADDRESS_SPACE *PsAttachProcess(EPROCESS *Process);
void PsDetachProcess(MMADDRESS_SPACE *Previous);

/* Runs the user-mode self-tests (ps/pstest.c): processes, loader, system calls, faults. */
bool PsRunSelfTests(void);

/* ---- System services implemented by Ps (ps/pssvc.c) --------------------------------------- */

STATUS NtTerminateProcess(const uint64_t *Args);
STATUS NtTerminateThread(const uint64_t *Args);
STATUS NtCreateThread(const uint64_t *Args);
STATUS NtCreateProcess(const uint64_t *Args);
STATUS NtCreateProcessEx(const uint64_t *Args);
STATUS NtOpenProcess(const uint64_t *Args);
STATUS NtQueryInformationProcess(const uint64_t *Args);
STATUS NtQueryInformationThread(const uint64_t *Args);

HZ_EXTERN_C_END

#endif /* PS_H */
