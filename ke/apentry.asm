; Nexora OS - ke/apentry.asm
; First kernel code an application processor runs (M11). Limine jumps here in long mode with
; interrupts off, its own page tables, GDT and stack (all in bootloader-reclaimable memory) and
; rdi = the processor's limine_mp_info, whose extra argument (offset 24) is the kernel's processor
; number. Before touching anything mapped NX the processor enables NX (EFER.NXE), matches the boot
; processor's PAT, turns on write protection and switches to the kernel's page tables and to its
; own stack in .bss; KiParkProcessor (ke/mp.c) does the rest in C.

bits 64
default rel

KI_AP_STACK_SIZE equ 32 * 1024
KI_MAX_PROCESSORS equ 8
MSR_EFER equ 0xC0000080
MSR_PAT equ 0x277
EFER_NXE equ 1 << 11
CR0_WP equ 1 << 16

extern KiParkProcessor
extern KiApPml4
extern KiApPat
extern KiApHaltedCount

section .bss
align 16
global KiApStacks
KiApStacks:
    resb KI_AP_STACK_SIZE * KI_MAX_PROCESSORS

section .text
global KiApplicationProcessorEntry
KiApplicationProcessorEntry:
    cli
    cld
    mov r12, [rdi + 24]             ; processor number (1 .. KI_MAX_PROCESSORS - 1)

    mov ecx, MSR_EFER
    rdmsr
    or eax, EFER_NXE
    wrmsr

    mov ecx, MSR_PAT
    mov rax, [KiApPat]
    mov rdx, rax
    shr rdx, 32
    wrmsr

    mov rax, cr0
    or rax, CR0_WP
    mov cr0, rax

    mov rax, [KiApPml4]
    mov cr3, rax
    test r12, r12                   ; 0: a processor the kernel does not use - halt it for good
    jz .halted

    lea rsp, [KiApStacks]           ; stack n ends at KiApStacks + n * size (n >= 1)
    mov rax, r12
    imul rax, rax, KI_AP_STACK_SIZE
    add rsp, rax
    xor ebp, ebp
    mov rdi, r12
    call KiParkProcessor            ; never returns
.halted:
    lock inc dword [KiApHaltedCount]
.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
