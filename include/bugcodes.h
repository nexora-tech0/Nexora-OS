/*
 * Nexora OS - include/bugcodes.h
 * Stop (bug check) codes. Nexora OS numbering; the names describe our own failure classes.
 */
#ifndef BUGCODES_H
#define BUGCODES_H

#define KERNEL_ASSERTION_FAILED 0x00000001u
#define BOOT_PROTOCOL_MISMATCH 0x00000002u
#define PURE_VIRTUAL_FUNCTION_CALL 0x00000003u
#define KERNEL_ALLOCATION_BEFORE_POOL 0x00000004u
#define IRQL_INVALID_TRANSITION 0x00000009u  /* p1 = current, p2 = requested, p3 = 1 if raising */
#define IRQL_NOT_LESS_OR_EQUAL 0x0000000Au   /* p1 = address, p2 = IRQL, p3 = error code, p4 = RIP */
#define SPIN_LOCK_TIMEOUT 0x0000000Bu        /* p1 = lock address, p2 = caller */
#define WAIT_AT_RAISED_IRQL 0x0000000Cu      /* p1 = IRQL, p2 = first object, p3 = caller */
#define MUTEX_NOT_OWNED 0x0000000Du          /* p1 = mutex, p2 = owner thread, p3 = releasing thread */
#define THREAD_STATE_CORRUPT 0x0000000Eu     /* p1 = thread, p2 = state, p3 = reason */
#define RUST_PANIC 0x00000010u
#define UNEXPECTED_KERNEL_TRAP 0x00000020u   /* p1 = vector, p2 = error code, p3 = RIP, p4 = RSP */
#define KERNEL_PAGE_FAULT 0x00000021u        /* p1 = CR2, p2 = error code, p3 = RIP */
#define DOUBLE_FAULT 0x00000022u             /* p1 = RIP, p2 = RSP */
#define NMI_HARDWARE_FAILURE 0x00000023u     /* p1 = RIP */
#define MACHINE_CHECK_EXCEPTION 0x00000024u  /* p1 = RIP */
#define UNEXPECTED_INTERRUPT 0x00000025u     /* p1 = vector */
#define HAL_INITIALIZATION_FAILED 0x00000030u /* p1 = HAL_FAILURE_* reason */
#define BAD_POOL_HEADER 0x00000040u          /* p1 = block, p2 = check found, p3 = check expected */
#define BAD_POOL_CALLER 0x00000041u          /* p1 = POOL_CALLER_* reason, p2 = block, p3 = tag/IRQL */
#define PFN_LIST_CORRUPT 0x00000042u         /* p1 = PFN, p2 = state, p3 = caller */
#define NO_PAGES_AVAILABLE 0x00000043u       /* p1 = pages requested, p2 = pages available */
#define KERNEL_STACK_OVERFLOW 0x00000044u    /* p1 = faulting address, p2 = RSP, p3 = RIP */
#define MEMORY_MANAGEMENT 0x00000045u        /* p1 = MM_FAILURE_* reason */
#define INSUFFICIENT_RESOURCES 0x00000046u   /* p1 = bytes requested, p2 = tag */
#define SUPERVISOR_MODE_USER_ACCESS 0x00000026u /* SMAP: p1 = address, p2 = error code, p3 = RIP */
#define SUPERVISOR_MODE_USER_EXECUTE 0x00000027u /* SMEP: p1 = address, p2 = error code, p3 = RIP */
#define IRQL_GT_ZERO_AT_SYSTEM_SERVICE 0x00000028u /* p1 = service, p2 = IRQL */
#define OBJECT_REFCOUNT_CORRUPT 0x00000050u  /* p1 = object, p2 = pointer count, p3 = handle count */
#define PROCESS_INITIALIZATION_FAILED 0x00000051u /* p1 = PS_INIT_FAILURE_* reason, p2 = status */
#define MANUALLY_INITIATED_CRASH 0x000000E2u

/* HAL_INITIALIZATION_FAILED reasons */
#define HAL_FAILURE_NO_RSDP 1
#define HAL_FAILURE_BAD_ACPI_TABLES 2
#define HAL_FAILURE_NO_MADT 3
#define HAL_FAILURE_NO_IOAPIC 4
#define HAL_FAILURE_NO_REFERENCE_TIMER 5
#define HAL_FAILURE_TIMER_CALIBRATION 6
#define HAL_FAILURE_IO_SPACE_EXHAUSTED 7

/* BAD_POOL_CALLER reasons */
#define POOL_CALLER_DOUBLE_FREE 1
#define POOL_CALLER_TAG_MISMATCH 2
#define POOL_CALLER_BAD_IRQL 3
#define POOL_CALLER_NOT_POOL 4
#define POOL_CALLER_BEFORE_INIT 5

/* PROCESS_INITIALIZATION_FAILED reasons */
#define PS_INIT_FAILURE_OBJECT_TYPES 1
#define PS_INIT_FAILURE_SYSTEM_PROCESS 2
#define PS_INIT_FAILURE_SYSTEM_THREAD 3

/* MEMORY_MANAGEMENT reasons */
#define MM_FAILURE_NO_PFN_DATABASE_SPACE 1
#define MM_FAILURE_KERNEL_TRANSLATION 2
#define MM_FAILURE_PAGE_TABLE_ALLOCATION 3
#define MM_FAILURE_SYSTEM_VA_EXHAUSTED 4
#define MM_FAILURE_DELETE_CURRENT_SPACE 5

#ifndef __ASSEMBLER__
static inline const char *KeBugCheckCodeName(unsigned int Code)
{
    switch (Code) {
    case KERNEL_ASSERTION_FAILED: return "KERNEL_ASSERTION_FAILED";
    case BOOT_PROTOCOL_MISMATCH: return "BOOT_PROTOCOL_MISMATCH";
    case PURE_VIRTUAL_FUNCTION_CALL: return "PURE_VIRTUAL_FUNCTION_CALL";
    case KERNEL_ALLOCATION_BEFORE_POOL: return "KERNEL_ALLOCATION_BEFORE_POOL";
    case IRQL_INVALID_TRANSITION: return "IRQL_INVALID_TRANSITION";
    case SPIN_LOCK_TIMEOUT: return "SPIN_LOCK_TIMEOUT";
    case WAIT_AT_RAISED_IRQL: return "WAIT_AT_RAISED_IRQL";
    case MUTEX_NOT_OWNED: return "MUTEX_NOT_OWNED";
    case THREAD_STATE_CORRUPT: return "THREAD_STATE_CORRUPT";
    case RUST_PANIC: return "RUST_PANIC";
    case UNEXPECTED_KERNEL_TRAP: return "UNEXPECTED_KERNEL_TRAP";
    case KERNEL_PAGE_FAULT: return "KERNEL_PAGE_FAULT";
    case DOUBLE_FAULT: return "DOUBLE_FAULT";
    case NMI_HARDWARE_FAILURE: return "NMI_HARDWARE_FAILURE";
    case MACHINE_CHECK_EXCEPTION: return "MACHINE_CHECK_EXCEPTION";
    case UNEXPECTED_INTERRUPT: return "UNEXPECTED_INTERRUPT";
    case HAL_INITIALIZATION_FAILED: return "HAL_INITIALIZATION_FAILED";
    case IRQL_NOT_LESS_OR_EQUAL: return "IRQL_NOT_LESS_OR_EQUAL";
    case BAD_POOL_HEADER: return "BAD_POOL_HEADER";
    case BAD_POOL_CALLER: return "BAD_POOL_CALLER";
    case PFN_LIST_CORRUPT: return "PFN_LIST_CORRUPT";
    case NO_PAGES_AVAILABLE: return "NO_PAGES_AVAILABLE";
    case KERNEL_STACK_OVERFLOW: return "KERNEL_STACK_OVERFLOW";
    case MEMORY_MANAGEMENT: return "MEMORY_MANAGEMENT";
    case INSUFFICIENT_RESOURCES: return "INSUFFICIENT_RESOURCES";
    case SUPERVISOR_MODE_USER_ACCESS: return "SUPERVISOR_MODE_USER_ACCESS";
    case SUPERVISOR_MODE_USER_EXECUTE: return "SUPERVISOR_MODE_USER_EXECUTE";
    case IRQL_GT_ZERO_AT_SYSTEM_SERVICE: return "IRQL_GT_ZERO_AT_SYSTEM_SERVICE";
    case OBJECT_REFCOUNT_CORRUPT: return "OBJECT_REFCOUNT_CORRUPT";
    case PROCESS_INITIALIZATION_FAILED: return "PROCESS_INITIALIZATION_FAILED";
    case MANUALLY_INITIATED_CRASH: return "MANUALLY_INITIATED_CRASH";
    default: return "UNKNOWN_STOP_CODE";
    }
}
#endif

#endif /* BUGCODES_H */
