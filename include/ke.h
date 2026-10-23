/*
 * Nexora OS - include/ke.h
 * Kernel core (Ke*): IRQL, spinlocks, per-processor block, trap frames, interrupt objects,
 * threads and processes, the dispatcher (scheduler, dispatcher objects, waits), DPCs, timers,
 * system time, bug checks, debug output and kernel symbols.
 */
#ifndef KE_H
#define KE_H

#include "bugcodes.h"
#include "hzbase.h"
#include "rtl.h"
#include "x86.h"

HZ_EXTERN_C_BEGIN

/* ---- IRQL --------------------------------------------------------------------------------
 * IRQL is the CR8 task-priority value (0-15). An interrupt vector's priority class is
 * vector >> 4, so a vector is masked while IRQL >= its class. Vector map:
 *   0x00-0x1F exceptions        0x2F DISPATCH software interrupt (2)
 *   0x30-0xBF devices (3-11)    0xD0-0xDF clock (13)    0xE0-0xEF IPIs (14, M11)
 *   0xFE APIC error (15)        0xFF APIC spurious
 * SYNCH_LEVEL (12) protects the dispatcher database: above every device, below the clock.
 */
typedef uint8_t KIRQL;

#define PASSIVE_LEVEL 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define DEVICE_LEVEL_LOW 3
#define DEVICE_LEVEL_HIGH 11
#define SYNCH_LEVEL 12
#define CLOCK_LEVEL 13
#define IPI_LEVEL 14
#define HIGH_LEVEL 15

#define KI_VECTOR_IRQL(v) ((KIRQL)((v) >> 4))
#define DISPATCH_VECTOR 0x2F
#define KI_FIRST_DEVICE_VECTOR 0x30
#define KI_LAST_DEVICE_VECTOR 0xBF
#define CLOCK_VECTOR 0xD1
#define APIC_ERROR_VECTOR 0xFE
#define APIC_SPURIOUS_VECTOR 0xFF

HZ_NORETURN void KiIrqlViolation(KIRQL Current, KIRQL Requested, bool Raising);

static inline KIRQL KeGetCurrentIrql(void)
{
    return (KIRQL)__readcr8();
}

static inline void KeRaiseIrql(KIRQL NewIrql, KIRQL *OldIrql)
{
    KIRQL Current = KeGetCurrentIrql();
    if (HZ_UNLIKELY(NewIrql < Current))
        KiIrqlViolation(Current, NewIrql, true);
    __writecr8(NewIrql);
    *OldIrql = Current;
}

static inline void KeLowerIrql(KIRQL NewIrql)
{
    KIRQL Current = KeGetCurrentIrql();
    if (HZ_UNLIKELY(NewIrql > Current))
        KiIrqlViolation(Current, NewIrql, false);
    __writecr8(NewIrql);
}

/* ---- Spinlocks --------------------------------------------------------------------------- */

typedef volatile uint64_t KSPIN_LOCK;

static inline void KeInitializeSpinLock(KSPIN_LOCK *Lock)
{
    *Lock = 0;
}

/* Raise to DISPATCH_LEVEL and acquire. */
void KeAcquireSpinLock(KSPIN_LOCK *Lock, KIRQL *OldIrql);
void KeReleaseSpinLock(KSPIN_LOCK *Lock, KIRQL OldIrql);
/* Raise to an arbitrary level (>= current) and acquire; used for locks shared with ISRs. */
void KeAcquireSpinLockRaiseToLevel(KSPIN_LOCK *Lock, KIRQL Level, KIRQL *OldIrql);
/* Caller is already at or above DISPATCH_LEVEL. */
void KeAcquireSpinLockAtDpcLevel(KSPIN_LOCK *Lock);
void KeReleaseSpinLockFromDpcLevel(KSPIN_LOCK *Lock);
bool KeTryToAcquireSpinLockAtDpcLevel(KSPIN_LOCK *Lock);

/* ---- Trap frame (layout shared with hal/trap.asm) ---------------------------------------- */

typedef struct _KTRAP_FRAME {
    uint64_t R15, R14, R13, R12, R11, R10, R9, R8;
    uint64_t Rbp, Rdi, Rsi, Rdx, Rcx, Rbx, Rax;
    uint64_t Vector;
    uint64_t ErrorCode;
    /* pushed by the CPU */
    uint64_t Rip;
    uint64_t SegCs;
    uint64_t RFlags;
    uint64_t Rsp;
    uint64_t SegSs;
} KTRAP_FRAME;

_Static_assert(sizeof(KTRAP_FRAME) == 22 * 8, "KTRAP_FRAME layout must match hal/trap.asm");

static inline bool KiIsUserModeFrame(const KTRAP_FRAME *Frame)
{
    return (Frame->SegCs & 3) != 0;
}

/* ---- Processor control block (reached through GS base) ----------------------------------- */

struct _KTHREAD;

typedef struct _KPRCB {
    struct _KPRCB *Self;     /* gs:[0] */
    uint64_t UserRsp;        /* gs:[8]  scratch for the SYSCALL entry (hal/syscall.asm) */
    uint64_t KernelStackTop; /* gs:[16] current thread's kernel stack top (= TSS.RSP0) */
    uint32_t Number;
    uint32_t ApicId;
    KTSS64 *Tss;
    volatile uint64_t InterruptCount;
    volatile uint64_t ExceptionCount;
    volatile uint32_t NestedInterrupts;
    struct _KTHREAD *CurrentThread;
    struct _KTHREAD *IdleThread;
    LIST_ENTRY DpcListHead;   /* queued KDPCs, drained by the DISPATCH software interrupt */
    KSPIN_LOCK DpcLock;       /* taken at HIGH_LEVEL (DPCs may be queued from any ISR) */
    volatile bool QuantumEnd; /* set by the clock when the current thread's quantum runs out */
    volatile bool PreemptRequested; /* a higher-priority thread became ready */
    volatile uint64_t ContextSwitches;
    volatile uint64_t DpcCount;
    volatile uint64_t IdleTicks;
    volatile uint64_t BusyTicks;
    volatile uint64_t SystemCalls;
    volatile uint32_t IpiRequests;  /* KI_IPI_* bits for this processor (M11) */
    volatile uint32_t Online;       /* set once the processor runs its idle loop */
    volatile uint64_t TlbShootdowns; /* flush requests from other processors handled */
    uint64_t LastApicTouchTsc;       /* KeYieldThread's idle-yield VM exit (M11) */
    /* Set by this processor's NMI when another one bug checks: where it was stopped (M11). */
    const struct _KTRAP_FRAME *volatile FrozenFrame;
    volatile uint8_t FrozenIrql;
} KPRCB;

_Static_assert(offsetof(KPRCB, UserRsp) == 8, "KPRCB layout is shared with hal/syscall.asm");
_Static_assert(offsetof(KPRCB, KernelStackTop) == 16, "KPRCB layout is shared with hal/syscall.asm");

typedef enum _KPROCESSOR_MODE {
    KernelMode = 0,
    UserMode = 1,
} KPROCESSOR_MODE;

static inline KPRCB *KeGetCurrentPrcb(void)
{
    KPRCB *Prcb;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(Prcb));
    return Prcb;
}

/* ---- Processors (M11, ke/mp.c) -------------------------------------------------------------
 * Limine starts the application processors (APs) and parks them in its own code. Right after the
 * kernel's page tables are active (before bootloader memory is reclaimed) KeParkApplicationProcessors
 * moves every AP onto the kernel's tables and a kernel stack, where it spins. Phase 1 then gives
 * each one a PRCB, GDT, TSS and idle thread (KeStartApplicationProcessors). Processor numbers are
 * 0 (the boot processor) to KeNumberProcessors - 1.
 */
#define KI_MAX_PROCESSORS 8
#define IPI_FLUSH_VECTOR 0xE1 /* TLB shootdown (IPI_LEVEL) */
#define KI_IPI_FLUSH 0x1

extern KPRCB *KiProcessorBlock[KI_MAX_PROCESSORS];
extern volatile uint32_t KeNumberProcessors;  /* online processors */
extern volatile uint64_t KeActiveProcessors;  /* bit n = processor n is online */
/* Before reclaiming bootloader memory: moves the APs to the kernel's page tables (KernelPml4). */
void KeParkApplicationProcessors(uint64_t KernelPml4);
/* Phase 1: brings the parked APs online; returns how many processors run. */
uint32_t KeStartApplicationProcessors(void);
/* Invalidates Pages pages at Va on every processor: in the kernel's mappings (Pml4 == 0, global
   entries included) or in the address space whose PML4 is Pml4 (processors running it). Pages
   beyond a few dozen flush everything. Callable at any IRQL below IPI_LEVEL. */
void KeFlushTb(uint64_t Pml4, uint64_t Va, uint64_t Pages);
/* Handles this processor's pending IPI requests (also polled while spinning). */
void KiServiceIpiRequests(void);
bool KeIsBugCheckActive(void);

/* ---- Dispatcher objects -------------------------------------------------------------------
 * Every waitable object starts with a DISPATCHER_HEADER. An object is signaled while
 * SignalState > 0 (a mutex is also "signaled" for its owner, for recursive acquisition).
 * All state changes happen under the dispatcher lock at SYNCH_LEVEL.
 */

typedef enum _KOBJECT_TYPE {
    KObjectEventNotification = 0,
    KObjectEventSynchronization,
    KObjectMutex,
    KObjectSemaphore,
    KObjectTimerNotification,
    KObjectTimerSynchronization,
    KObjectThread,
    KObjectProcess,
} KOBJECT_TYPE;

typedef struct _DISPATCHER_HEADER {
    uint8_t Type;
    uint8_t Reserved[3];
    volatile int32_t SignalState;
    LIST_ENTRY WaitListHead; /* KWAIT_BLOCKs of waiting threads, FIFO */
} DISPATCHER_HEADER;

typedef enum _WAIT_TYPE {
    WaitAll = 0,
    WaitAny = 1,
} WAIT_TYPE;

typedef struct _KWAIT_BLOCK {
    LIST_ENTRY WaitListEntry;
    struct _KTHREAD *Thread;
    DISPATCHER_HEADER *Object;
    uint16_t WaitKey; /* index into the wait array, or KI_TIMEOUT_WAIT_KEY */
    uint8_t WaitType;
    bool Linked;
} KWAIT_BLOCK;

#define KI_TIMEOUT_WAIT_KEY 0xFFFF
#define THREAD_WAIT_OBJECTS 3   /* waits on more objects need a caller-supplied block array */
#define MAXIMUM_WAIT_OBJECTS 64

typedef enum _EVENT_TYPE {
    NotificationEvent = 0,  /* stays signaled until reset; wakes every waiter */
    SynchronizationEvent,   /* auto-reset; wakes one waiter */
} EVENT_TYPE;

typedef struct _KEVENT {
    DISPATCHER_HEADER Header;
} KEVENT;

typedef struct _KSEMAPHORE {
    DISPATCHER_HEADER Header; /* SignalState = count */
    int32_t Limit;
} KSEMAPHORE;

typedef struct _KMUTEX {
    DISPATCHER_HEADER Header; /* SignalState: 1 = free, <= 0 = owned (recursion depth) */
    LIST_ENTRY MutantListEntry; /* in the owner's MutantListHead */
    struct _KTHREAD *OwnerThread;
    bool Abandoned;
} KMUTEX;

/* ---- DPCs --------------------------------------------------------------------------------- */

typedef struct _KDPC KDPC;
typedef void (*KDEFERRED_ROUTINE)(KDPC *Dpc, void *DeferredContext, void *SystemArgument1, void *SystemArgument2);

struct _KDPC {
    LIST_ENTRY DpcListEntry;
    KDEFERRED_ROUTINE DeferredRoutine;
    void *DeferredContext;
    void *SystemArgument1;
    void *SystemArgument2;
    volatile bool Inserted;          /* claimed atomically: queued on one processor at a time */
    struct _KPRCB *volatile Queue;   /* the processor whose queue holds it (M11) */
};

void KeInitializeDpc(KDPC *Dpc, KDEFERRED_ROUTINE Routine, void *Context);
/* Queues the DPC on this CPU (any IRQL). Returns false if it was already queued. */
bool KeInsertQueueDpc(KDPC *Dpc, void *SystemArgument1, void *SystemArgument2);
bool KeRemoveQueueDpc(KDPC *Dpc);

/* ---- Timers ------------------------------------------------------------------------------
 * Due times are absolute microseconds of TSC uptime (KeQueryUptimeMicroseconds); the clock
 * interrupt only checks whether the earliest timer is due (ARCHITECTURE §17).
 */

typedef enum _TIMER_TYPE {
    NotificationTimer = 0,
    SynchronizationTimer,
} TIMER_TYPE;

typedef struct _KTIMER {
    DISPATCHER_HEADER Header;
    uint64_t DueTimeUs;
    uint64_t PeriodUs;
    LIST_ENTRY TimerListEntry;
    KDPC *Dpc;
    bool Inserted;
} KTIMER;

void KeInitializeTimerEx(KTIMER *Timer, TIMER_TYPE Type);
/* Arms the timer DueInUs from now (periodic if PeriodUs != 0; Dpc optional). Returns true if
   the timer was already armed. */
bool KeSetTimerEx(KTIMER *Timer, uint64_t DueInUs, uint64_t PeriodUs, KDPC *Dpc);
bool KeCancelTimer(KTIMER *Timer);
bool KeReadStateTimer(KTIMER *Timer);

/* ---- Threads and processes ------------------------------------------------------------------ */

typedef int8_t KPRIORITY;

#define LOW_PRIORITY 0
#define LOW_REALTIME_PRIORITY 16
#define HIGH_PRIORITY 31
#define KI_THREAD_PRIORITY_DEFAULT 8
/* Quantum length. Charged in TSC cycles (like NT since Vista), not clock ticks: a hypervisor
   that delivers ticks late or coalesced (VirtualBox on Hyper-V) still gets 10 ms slices. */
#define KI_QUANTUM_US 10000

typedef enum _KTHREAD_STATE {
    ThreadInitialized = 0,
    ThreadReady,
    ThreadRunning,
    ThreadWaiting,
    ThreadTerminated,
} KTHREAD_STATE;

struct _MMADDRESS_SPACE;

typedef struct _KPROCESS {
    DISPATCHER_HEADER Header;
    struct _MMADDRESS_SPACE *AddressSpace;
    LIST_ENTRY ThreadListHead;
    uint32_t ProcessId;
    uint32_t ThreadCount;
    KPRIORITY BasePriority;
    char Name[32];
} KPROCESS;

typedef void (*KSTART_ROUTINE)(void *Context);

typedef struct _KTHREAD {
    DISPATCHER_HEADER Header;      /* signaled when the thread terminates */
    uint64_t KernelStackPointer;   /* saved RSP while switched out */
    void *StackTop;
    KPROCESS *Process;
    struct _MMADDRESS_SPACE *AddressSpace; /* current (MmSwitchAddressSpace attaches) */
    LIST_ENTRY ThreadListEntry;    /* process thread list */
    LIST_ENTRY GlobalListEntry;    /* all threads */
    LIST_ENTRY QueueEntry;         /* ready queue, or the reaper list once terminated */
    volatile uint8_t State;
    KPRIORITY Priority;
    KPRIORITY BasePriority;
    int64_t QuantumRemaining;      /* TSC cycles left in the current quantum */
    uint64_t QuantumStart;         /* TSC when the thread last started running */
    uint64_t CycleTime;            /* total TSC cycles spent running */
    KWAIT_BLOCK *WaitBlockList;
    uint32_t WaitCount;
    uint8_t WaitType;
    STATUS WaitStatus;
    KWAIT_BLOCK WaitBlock[THREAD_WAIT_OBJECTS];
    KWAIT_BLOCK TimerWaitBlock;
    KTIMER Timer;                  /* wait timeouts and KeDelayExecutionThread */
    LIST_ENTRY MutantListHead;     /* owned mutexes (abandoned on exit) */
    KSTART_ROUTINE StartRoutine;
    void *StartContext;
    volatile int32_t ReferenceCount;
    uint32_t ThreadId;
    uint64_t ContextSwitches;
    char Name[24];
    /* Drops the thread's self-reference once it has terminated and its stack is freed. */
    void (*ReleaseRoutine)(struct _KTHREAD *Thread);
    /* User-mode support (M4) */
    void *XSaveArea;               /* extended FPU/SSE/AVX state; NULL for kernel-only threads */
    uint64_t UserGsBase;           /* GS base in user mode (the TEB); kept in KERNEL_GS_BASE */
    volatile bool Alerted;         /* KeAlertThread happened; consumed by the next alertable wait */
    bool WaitIsAlertable;
    volatile bool UserReturnPending; /* run the user-return callout before going back to ring 3 */
    uint8_t PreviousMode;          /* KPROCESSOR_MODE of the current system service caller */
    KTRAP_FRAME *TrapFrame;        /* user-mode frame of the current system service, or NULL */
    /* SMP (M11) */
    volatile uint8_t SwapBusy;     /* its registers are still being saved by the processor leaving it */
    volatile uint8_t Processor;    /* the processor it runs (or last ran) on */
    uint64_t Affinity;             /* processors it may run on (bit n = processor n) */
} KTHREAD;

/* Sets the processors Thread may run on (at least one online one); returns the old mask. New
   threads inherit their creator's affinity. */
uint64_t KeSetAffinityThread(KTHREAD *Thread, uint64_t Affinity);

/* The system process (PID 4). Its executive object lives in Ps; Ke sees the KPROCESS. */
extern KPROCESS *KeSystemProcess;

static inline KTHREAD *KeGetCurrentThread(void)
{
    return KeGetCurrentPrcb()->CurrentThread;
}

/* Phase 0: ready queues, and the boot context becomes the idle thread of SystemProcess. */
void KeInitializeScheduler(KPROCESS *SystemProcess);
void KeInitializeProcess(KPROCESS *Process, struct _MMADDRESS_SPACE *AddressSpace, KPRIORITY BasePriority,
                         const char *Name, uint32_t ProcessId);
/*
 * Initializes a thread embedded in a caller-owned object (e.g. an ETHREAD): kernel stack,
 * initial frame, dispatcher header. The thread does not run until KeStartThread.
 * ReleaseRoutine is called (at DISPATCH_LEVEL) after the thread terminated and its stack was
 * freed, to drop the thread's reference to its owning object.
 */
STATUS KeInitializeThread(KTHREAD *Thread, KPROCESS *Process, KSTART_ROUTINE Routine, void *Context,
                          KPRIORITY Priority, const char *Name, void (*ReleaseRoutine)(KTHREAD *Thread));
void KeStartThread(KTHREAD *Thread);
/*
 * Bare kernel thread allocated from the pool (no executive object; used by Ke self-tests).
 * *Thread holds a reference: release it with KeDereferenceThread. Use PsCreateSystemThread
 * for ordinary system threads.
 */
STATUS KeCreateThread(KTHREAD **Thread, KPROCESS *Process, KSTART_ROUTINE Routine, void *Context,
                      KPRIORITY Priority, const char *Name);
HZ_NORETURN void KeTerminateThread(void);
void KeReferenceThread(KTHREAD *Thread);
void KeDereferenceThread(KTHREAD *Thread);
KPRIORITY KeSetPriorityThread(KTHREAD *Thread, KPRIORITY Priority);
/* Gives the rest of the quantum to another ready thread of the same or higher priority. */
void KeYieldThread(void);
uint32_t KeQueryThreadCount(void);
/* CPU time a thread has used, in microseconds (includes the running slice for the caller). */
uint64_t KeQueryThreadCpuTimeUs(KTHREAD *Thread);
/* Sleeps for at least Microseconds (IRQL < DISPATCH_LEVEL). */
void KeDelayExecutionThread(uint64_t Microseconds);

/* ---- Synchronization ------------------------------------------------------------------------- */

void KeInitializeEvent(KEVENT *Event, EVENT_TYPE Type, bool Signaled);
int32_t KeSetEvent(KEVENT *Event);   /* returns the previous state */
void KeResetEvent(KEVENT *Event);
int32_t KeReadStateEvent(KEVENT *Event);

void KeInitializeSemaphore(KSEMAPHORE *Semaphore, int32_t Count, int32_t Limit);
STATUS KeReleaseSemaphore(KSEMAPHORE *Semaphore, int32_t Adjustment, int32_t *PreviousCount);

void KeInitializeMutex(KMUTEX *Mutex);
/* Releasing a mutex the caller does not own stops the system (MUTEX_NOT_OWNED). */
void KeReleaseMutex(KMUTEX *Mutex);
bool KeReadStateMutex(KMUTEX *Mutex); /* true if free */

/*
 * Waits. Timeout: NULL = forever, 0 = poll, otherwise relative microseconds.
 * Returns STATUS_WAIT_0 + index, STATUS_ABANDONED_WAIT_0 + index, or STATUS_TIMEOUT.
 * Blocking waits require IRQL < DISPATCH_LEVEL (WAIT_AT_RAISED_IRQL otherwise).
 */
STATUS KeWaitForSingleObject(void *Object, const uint64_t *TimeoutUs);
STATUS KeWaitForMultipleObjects(uint32_t Count, void *const Objects[], WAIT_TYPE WaitType,
                                const uint64_t *TimeoutUs, KWAIT_BLOCK *WaitBlockArray);
/* Alertable variants return STATUS_ALERTED when KeAlertThread targets the waiting thread (used
   for waits on behalf of user mode, so termination requests are not stuck behind them). */
STATUS KeWaitForMultipleObjectsEx(uint32_t Count, void *const Objects[], WAIT_TYPE WaitType,
                                  const uint64_t *TimeoutUs, KWAIT_BLOCK *WaitBlockArray, bool Alertable);
STATUS KeDelayExecutionThreadEx(uint64_t Microseconds, bool Alertable);
/* Wakes Thread from an alertable wait with STATUS_ALERTED (or marks it alerted for the next one). */
void KeAlertThread(KTHREAD *Thread);
/* Sets a dispatcher object (process, notification object) to signaled and wakes waiters. */
void KeSignalObject(DISPATCHER_HEADER *Header);

/* ---- Return to user mode ------------------------------------------------------------------
 * Before a thread returns to ring 3 (system call exit, interrupt or exception return), Ke runs
 * the user-return callout if the thread has UserReturnPending set. Ps uses it to terminate
 * threads in their own context (the role NT gives to user/kernel APCs).
 */
typedef void (*KUSER_RETURN_CALLOUT)(KTRAP_FRAME *Frame);
void KeSetUserReturnCallout(KUSER_RETURN_CALLOUT Callout);
void KeRequestUserReturnCallout(KTHREAD *Thread);
/* Called on every return path to user mode (interrupts disabled, IRQL PASSIVE). */
void KiCheckUserReturn(KTRAP_FRAME *Frame);
/* Starts executing in ring 3 with the register state in Frame (never returns). */
HZ_NORETURN void KiEnterUserMode(KTRAP_FRAME *Frame);

/* ---- System services and user exceptions ----------------------------------------------------
 * The SYSCALL entry (hal/syscall.asm) dispatches through a service table that the executive
 * registers. User arguments 1-4 arrive in r10, rdx, r8, r9 (Microsoft x64 convention, rcx moved
 * to r10 by the stub); arguments 5-8 are copied from the user stack above the 32-byte home area.
 * Routines receive the raw 64-bit argument values and must treat pointers as untrusted user
 * addresses (MmCopyFromUser/MmCopyToUser).
 */
#define KE_MAX_SERVICE_ARGUMENTS 8
typedef STATUS (*KSYSTEM_SERVICE_ROUTINE)(const uint64_t *Arguments);
typedef struct _KSYSTEM_SERVICE {
    KSYSTEM_SERVICE_ROUTINE Routine;
    uint8_t ArgumentCount;
    const char *Name;
} KSYSTEM_SERVICE;
void KeSetSystemServiceTable(const KSYSTEM_SERVICE *Table, uint32_t Count);
/* Services numbered from KE_WIN32_SERVICE_BASE go to this table (win32k). */
#define KE_WIN32_SERVICE_BASE 0x1000u
void KeSetWin32ServiceTable(const KSYSTEM_SERVICE *Table, uint32_t Count);
/* Called from hal/syscall.asm with interrupts enabled; returns with interrupts disabled. */
void KiSystemServiceHandler(KTRAP_FRAME *Frame);

static inline KPROCESSOR_MODE KeGetPreviousMode(void)
{
    KTHREAD *Thread = KeGetCurrentThread();
    return Thread ? (KPROCESSOR_MODE)Thread->PreviousMode : KernelMode;
}

/*
 * An exception in ring 3 that Ke cannot resolve (anything but a demand-zero fault or int3) is
 * handed to this routine with an NT status describing it (STATUS_ACCESS_VIOLATION, ...) and the
 * faulting data address (page faults) or 0. Ps registers one that terminates the process.
 */
typedef void (*KUSER_EXCEPTION_ROUTINE)(KTRAP_FRAME *Frame, STATUS ExceptionStatus, uint64_t Address);
void KeSetUserExceptionRoutine(KUSER_EXCEPTION_ROUTINE Routine);
extern volatile uint64_t KiUserBreakpointCount;

/* ---- Extended processor state (user FPU/SSE/AVX) ------------------------------------------- */

/* Enables FXSR/XSAVE for user mode; called once at boot. */
void KeInitializeExtendedState(void);
/* Allocates and initializes an XSAVE area for a user thread (pool; free with KeFreeExtendedState). */
void *KeAllocateExtendedState(void);
void KeFreeExtendedState(void *Area);
uint32_t KeExtendedStateSize(void);

/* Runs the scheduler self-tests (ke/ketest.c); returns true if all passed. */
bool KeRunSchedulerSelfTests(void);

/* ---- Interrupts -------------------------------------------------------------------------- */

/* Runs at the vector's IRQL with interrupts enabled; returns true if the interrupt was ours. */
typedef bool (*KSERVICE_ROUTINE)(KTRAP_FRAME *Frame, void *Context);

STATUS KeConnectInterrupt(uint8_t Vector, KSERVICE_ROUTINE Routine, void *Context);
void KeDisconnectInterrupt(uint8_t Vector);
/* Connects Routine to a free device vector whose IRQL is Irql (DEVICE_LEVEL_LOW..HIGH); used for
   MSI and ISA interrupts. Returns STATUS_INSUFFICIENT_RESOURCES when that IRQL band is full. */
STATUS KeConnectInterruptAnyVector(KIRQL Irql, KSERVICE_ROUTINE Routine, void *Context, uint8_t *Vector);

/* Called from the trap stubs in hal/trap.asm. */
void KiDispatchTrap(KTRAP_FRAME *Frame);

/* Number of times int3 was handled (used by the boot self-test). */
extern volatile uint64_t KiBreakpointCount;

/* ---- Time -------------------------------------------------------------------------------- */

/* TSC value at kernel entry; uptime is measured from here. */
extern uint64_t KiBootTsc;

/* Clock interrupts since the clock started. */
uint64_t KeQueryTickCount(void);
/* Clock interrupt frequency in Hz (0 until the clock runs). */
uint32_t KeQueryClockRate(void);
/* Microseconds since the TSC was calibrated (0 before). */
uint64_t KeQueryUptimeMicroseconds(void);
/* M11: switch uptime to the HAL's reference timer (when the TSC rate is wrong). */
bool KeUseReferenceClock(void);
bool KeIsReferenceClockInUse(void);
/* Wall-clock time in HZ time units (100 ns since 1601 UTC); 0 until KeSetSystemTime ran. */
uint64_t KeQuerySystemTime(void);
void KeSetSystemTime(uint64_t Time);
/* Busy-waits (TSC-based after calibration; reference-timer based before). */
void KeStallExecutionProcessor(uint32_t Microseconds);
/* Clock interrupt body (connected by the HAL). */
bool KiClockInterrupt(KTRAP_FRAME *Frame, void *Context);
void KiStartClock(uint32_t RateHz);

/* ---- Stacks ------------------------------------------------------------------------------ */

/* Runs Routine(Context) on the stack ending at StackTop and returns on the current stack. */
void KiCallOnStack(void (*Routine)(void *), void *Context, void *StackTop);

/* ---- Bug checks -------------------------------------------------------------------------- */

HZ_NORETURN void KeBugCheckEx(uint32_t Code, uint64_t Param1, uint64_t Param2, uint64_t Param3,
                              uint64_t Param4);
HZ_NORETURN void KeBugCheck(uint32_t Code);
/* KeBugCheckEx with a human-readable explanation shown on the stop screen. */
HZ_NORETURN void KeBugCheckExMessage(uint32_t Code, uint64_t Param1, uint64_t Param2, uint64_t Param3,
                                     uint64_t Param4, const char *Message);
HZ_NORETURN void KeBugCheckWithMessage(uint32_t Code, const char *Message, const char *File,
                                       uint32_t Line);
/* Bug check raised from a trap: the stop screen shows the frame's registers. */
HZ_NORETURN void KeBugCheckTrap(uint32_t Code, uint64_t Param1, uint64_t Param2, uint64_t Param3,
                                uint64_t Param4, const KTRAP_FRAME *Frame, const char *Message);
/* Entry point for the Rust panic handler (win32k/rgn). Strings are not NUL-terminated. */
HZ_NORETURN void KeBugCheckRustPanic(const uint8_t *Message, size_t MessageLength,
                                     const uint8_t *File, size_t FileLength, uint32_t Line);

#define HZ_ASSERT(expr)                                                                       \
    do {                                                                                      \
        if (HZ_UNLIKELY(!(expr)))                                                             \
            KeBugCheckWithMessage(KERNEL_ASSERTION_FAILED, "assertion failed: " #expr,         \
                                  __FILE__, __LINE__);                                        \
    } while (0)

/* ---- Debug output ------------------------------------------------------------------------ */

#define DPFLTR_ERROR_LEVEL 0
#define DPFLTR_WARNING_LEVEL 1
#define DPFLTR_INFO_LEVEL 2
#define DPFLTR_TRACE_LEVEL 3

/* Messages above the current level are dropped (default INFO; `hz.log=trace` on the cmdline). */
void DbgSetLogLevel(uint32_t Level);
void DbgPrintEx(uint32_t Level, const char *Format, ...) HZ_PRINTF(2, 3);
void DbgPrintExV(uint32_t Level, const char *Format, va_list Args);
/* DbgPrint == DbgPrintEx(DPFLTR_INFO_LEVEL, ...) */
void DbgPrint(const char *Format, ...) HZ_PRINTF(1, 2);

/* ---- Kernel symbols (hzkrnl.sym boot module) --------------------------------------------- */

bool KeLoadSymbols(const void *Data, uint64_t Size);
uint32_t KeSymbolCount(void);
/* Finds the function containing Address. Returns false if unknown. */
bool KeLookupSymbol(uint64_t Address, const char **Name, uint64_t *Offset);
/* "Name+0x1a" or "0xFFFFFFFF80001234" if unknown. */
void KeFormatAddress(char *Buffer, size_t Size, uint64_t Address);
/* Same, but a return address is resolved through Address - 1 so the calling function is named. */
void KeFormatStackFrame(char *Buffer, size_t Size, uint64_t Address, bool IsReturnAddress);

HZ_EXTERN_C_END

#endif /* KE_H */
