/*
 * Nexora OS - include/x86.h
 * x86-64 architecture definitions and intrinsics shared by the HAL and the kernel core:
 * control registers, MSRs, CPUID, descriptor-table layouts and segment selectors.
 */
#ifndef X86_H
#define X86_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

/* ---- Segment selectors (GDT layout is fixed; see hal/gdt.c) ------------------------------ */

#define KGDT_NULL 0x00
#define KGDT_KERNEL_CODE 0x08
#define KGDT_KERNEL_DATA 0x10
#define KGDT_USER_DATA 0x18   /* used as 0x1B (RPL 3) */
#define KGDT_USER_CODE 0x20   /* used as 0x23 (RPL 3) */
#define KGDT_TSS 0x28         /* 16-byte system descriptor */
#define KGDT_ENTRY_COUNT 7

/* ---- MSRs -------------------------------------------------------------------------------- */

#define MSR_IA32_APIC_BASE 0x0000001B
#define MSR_IA32_PAT 0x00000277
#define MSR_X2APIC_BASE 0x00000800
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

#define APIC_BASE_BSP (1ull << 8)
#define APIC_BASE_X2APIC (1ull << 10)
#define APIC_BASE_ENABLE (1ull << 11)
#define APIC_BASE_ADDRESS_MASK 0x000FFFFFFFFFF000ull

#define EFER_SCE (1ull << 0)
#define EFER_NXE (1ull << 11)

/* ---- RFLAGS / CR bits -------------------------------------------------------------------- */

#define RFLAGS_IF (1ull << 9)

/* ---- Page table entry bits ---------------------------------------------------------------- */

#define PTE_PRESENT (1ull << 0)
#define PTE_WRITE (1ull << 1)
#define PTE_USER (1ull << 2)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)
#define PTE_LARGE (1ull << 7)
#define PTE_GLOBAL (1ull << 8)
#define PTE_NX (1ull << 63)
#define PTE_ADDRESS_MASK 0x000FFFFFFFFFF000ull

/* ---- Descriptor tables ------------------------------------------------------------------- */

typedef struct HZ_PACKED _KDESCRIPTOR {
    uint16_t Limit;
    uint64_t Base;
} KDESCRIPTOR;

typedef struct HZ_PACKED _KIDTENTRY64 {
    uint16_t OffsetLow;
    uint16_t Selector;
    uint8_t Ist;       /* bits 0-2 */
    uint8_t TypeAttr;  /* P | DPL | 0 | type */
    uint16_t OffsetMiddle;
    uint32_t OffsetHigh;
    uint32_t Reserved;
} KIDTENTRY64;

typedef struct HZ_PACKED _KTSS64 {
    uint32_t Reserved0;
    uint64_t Rsp0;
    uint64_t Rsp1;
    uint64_t Rsp2;
    uint64_t Ist[8]; /* Ist[0] is the reserved qword at offset 0x1C; IST1..IST7 follow */
    uint64_t Reserved2;
    uint16_t Reserved3;
    uint16_t IoMapBase;
} KTSS64;

_Static_assert(sizeof(KIDTENTRY64) == 16, "IDT entry is 16 bytes");
_Static_assert(sizeof(KTSS64) == 104, "TSS is 104 bytes");

/* ---- Intrinsics -------------------------------------------------------------------------- */

static inline uint64_t __readmsr(uint32_t Msr)
{
    uint32_t Lo, Hi;
    __asm__ volatile("rdmsr" : "=a"(Lo), "=d"(Hi) : "c"(Msr));
    return ((uint64_t)Hi << 32) | Lo;
}

static inline void __writemsr(uint32_t Msr, uint64_t Value)
{
    __asm__ volatile("wrmsr" : : "c"(Msr), "a"((uint32_t)Value), "d"((uint32_t)(Value >> 32)) : "memory");
}

static inline void __cpuid(uint32_t Leaf, uint32_t Subleaf, uint32_t Regs[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(Regs[0]), "=b"(Regs[1]), "=c"(Regs[2]), "=d"(Regs[3])
                     : "a"(Leaf), "c"(Subleaf));
}

#define HZ_DEFINE_CR_ACCESSORS(n)                                                            \
    static inline uint64_t __readcr##n(void)                                                 \
    {                                                                                        \
        uint64_t Value;                                                                      \
        __asm__ volatile("mov %%cr" #n ", %0" : "=r"(Value));                                \
        return Value;                                                                        \
    }                                                                                        \
    static inline void __writecr##n(uint64_t Value)                                          \
    {                                                                                        \
        __asm__ volatile("mov %0, %%cr" #n : : "r"(Value) : "memory");                       \
    }

HZ_DEFINE_CR_ACCESSORS(0)
HZ_DEFINE_CR_ACCESSORS(2)
HZ_DEFINE_CR_ACCESSORS(3)
HZ_DEFINE_CR_ACCESSORS(4)
HZ_DEFINE_CR_ACCESSORS(8)

static inline uint64_t __readrflags(void)
{
    uint64_t Flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(Flags));
    return Flags;
}

static inline void __invlpg(const void *Address)
{
    __asm__ volatile("invlpg (%0)" : : "r"(Address) : "memory");
}

static inline void __lidt(const KDESCRIPTOR *Descriptor)
{
    __asm__ volatile("lidt %0" : : "m"(*Descriptor));
}

static inline void __ltr(uint16_t Selector)
{
    __asm__ volatile("ltr %0" : : "r"(Selector));
}

HZ_EXTERN_C_END

#endif /* X86_H */
