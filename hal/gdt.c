/*
 * Nexora OS - hal/gdt.c
 * Per-processor tables: GDT (flat 64-bit segments + TSS), TSS with IST stacks, and the
 * processor control block reached through GS base. The boot processor's live in .bss; each
 * application processor gets its own (HAL_PROCESSOR_TABLES, M11).
 *
 * GDT layout (fixed; SYSCALL/SYSRET in M4 depend on the order):
 *   0x00 null   0x08 kernel code   0x10 kernel data   0x18 user data   0x20 user code (64-bit)
 *   0x28 TSS (two slots)
 * STAR will be: [47:32] = 0x08 (SYSCALL CS; SS = 0x10), [63:48] = 0x10 (SYSRET CS = 0x20|3,
 * SS = 0x18|3).
 */
#include "halp.h"
#include "rtl.h"

#define IST_DOUBLE_FAULT 1
#define IST_NMI 2
#define IST_MACHINE_CHECK 3
#define IST_STACK_SIZE HAL_IST_STACK_SIZE

/* Descriptor encodings (access byte | flags) for flat segments. */
#define GDT_KERNEL_CODE 0x00AF9A000000FFFFull /* P, DPL0, code, readable, L=1 */
#define GDT_KERNEL_DATA 0x00CF92000000FFFFull /* P, DPL0, data, writable */
#define GDT_USER_DATA 0x00CFF2000000FFFFull   /* P, DPL3, data, writable */
#define GDT_USER_CODE 0x00AFFA000000FFFFull   /* P, DPL3, code, readable, L=1 */

static uint64_t HalpGdt[KGDT_ENTRY_COUNT] HZ_ALIGNED(16);
static KTSS64 HalpTss HZ_ALIGNED(16);
static KPRCB HalpBootPrcb;

static uint8_t HalpIstStacks[3][IST_STACK_SIZE] HZ_ALIGNED(16);

/* Boot stack from init/entry.asm, used as RSP0 until threads exist. */
extern uint8_t KiBootStackTop[];

static void HalpSetTssDescriptor(uint64_t *Slot, const KTSS64 *Tss)
{
    uint64_t Base = (uint64_t)Tss;
    uint64_t Limit = sizeof(KTSS64) - 1;
    Slot[0] = (Limit & 0xFFFF) | ((Base & 0xFFFFFF) << 16) | (0x89ull << 40) /* P, type 9 */ |
              (((Limit >> 16) & 0xF) << 48) | (((Base >> 24) & 0xFF) << 56);
    Slot[1] = Base >> 32;
}

void HalSetKernelStack(uint64_t StackTop)
{
    KeGetCurrentPrcb()->Tss->Rsp0 = StackTop;
}

static void HalpBuildTables(uint64_t *Gdt, KTSS64 *Tss, uint8_t (*Ist)[IST_STACK_SIZE], uint64_t StackTop)
{
    Gdt[0] = 0;
    Gdt[KGDT_KERNEL_CODE / 8] = GDT_KERNEL_CODE;
    Gdt[KGDT_KERNEL_DATA / 8] = GDT_KERNEL_DATA;
    Gdt[KGDT_USER_DATA / 8] = GDT_USER_DATA;
    Gdt[KGDT_USER_CODE / 8] = GDT_USER_CODE;

    memset(Tss, 0, sizeof(*Tss));
    Tss->Rsp0 = StackTop;
    Tss->Ist[IST_DOUBLE_FAULT] = (uint64_t)(Ist[0] + IST_STACK_SIZE);
    Tss->Ist[IST_NMI] = (uint64_t)(Ist[1] + IST_STACK_SIZE);
    Tss->Ist[IST_MACHINE_CHECK] = (uint64_t)(Ist[2] + IST_STACK_SIZE);
    Tss->IoMapBase = sizeof(KTSS64); /* no I/O permission bitmap */
    HalpSetTssDescriptor(&Gdt[KGDT_TSS / 8], Tss);

    KDESCRIPTOR Gdtr = {.Limit = KGDT_ENTRY_COUNT * 8 - 1, .Base = (uint64_t)Gdt};
    HalpLoadGdt(&Gdtr, KGDT_KERNEL_CODE, KGDT_KERNEL_DATA); /* also reloads CS/DS/ES/SS, clears FS/GS */
    __ltr(KGDT_TSS);
}

static void HalpSetProcessorBlock(KPRCB *Prcb)
{
    /* GS base must be written after the GS selector load (which zeroes it). */
    __writemsr(MSR_GS_BASE, (uint64_t)Prcb);
    __writemsr(MSR_KERNEL_GS_BASE, 0);
}

static void HalpInitializeSystemCalls(void)
{
    /* SYSCALL/SYSRET: STAR[47:32] = kernel CS (SS = +8), STAR[63:48] = 0x10 so that SYSRET would
       load CS = 0x20|3 and SS = 0x18|3; LSTAR = entry; FMASK clears TF, IF, DF, IOPL, NT, AC. */
    __writemsr(MSR_STAR, ((uint64_t)KGDT_KERNEL_DATA << 48) | ((uint64_t)KGDT_KERNEL_CODE << 32));
    __writemsr(MSR_LSTAR, (uint64_t)KiSystemCall64);
    __writemsr(MSR_FMASK, 0x47700);
    __writemsr(MSR_EFER, __readmsr(MSR_EFER) | EFER_SCE);
}

void HalInitializeProcessor(void)
{
    HalpBuildTables(HalpGdt, &HalpTss, HalpIstStacks, (uint64_t)KiBootStackTop);
    memset(&HalpBootPrcb, 0, sizeof(HalpBootPrcb));
    HalpBootPrcb.Self = &HalpBootPrcb;
    HalpBootPrcb.Number = 0;
    HalpBootPrcb.Tss = &HalpTss;
    HalpSetProcessorBlock(&HalpBootPrcb);
    HalpInitializeIdt();
    HalpInitializeSystemCalls();
}

void HalLoadBootProcessorTables(void)
{
    KDESCRIPTOR Gdtr = {.Limit = sizeof(HalpGdt) - 1, .Base = (uint64_t)HalpGdt};
    HalpLoadGdt(&Gdtr, KGDT_KERNEL_CODE, KGDT_KERNEL_DATA);
    HalpLoadIdt();
}

void HalInitializeApplicationProcessor(KPRCB *Prcb, HAL_PROCESSOR_TABLES *Tables, uint64_t StackTop)
{
    HalpBuildTables(Tables->Gdt, &Tables->Tss, Tables->IstStacks, StackTop);
    Prcb->Tss = &Tables->Tss;
    HalpSetProcessorBlock(Prcb);
    HalpLoadIdt();
    HalpInitializeSystemCalls();
    HalpInitializeLocalApicForProcessor();
}
