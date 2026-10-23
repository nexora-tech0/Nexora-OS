; Nexora OS - init/entry.asm
; Kernel entry point. Limine jumps here in 64-bit long mode with paging on, interrupts off,
; the kernel mapped at 0xffffffff80000000 and a bootloader-provided stack. We switch to our
; own boot stack in .bss (the loader stack lives in reclaimable memory) and enter C.

bits 64
default rel

KI_BOOT_STACK_SIZE equ 64 * 1024

section .bss
align 16
global KiBootStack
KiBootStack:
    resb KI_BOOT_STACK_SIZE
global KiBootStackTop
KiBootStackTop:

section .text
global KiSystemStartup
extern KiInitializeSystem

KiSystemStartup:
    cli
    cld
    lea rsp, [KiBootStackTop]   ; 16-byte aligned; the call below leaves rsp = 8 mod 16
    xor ebp, ebp                ; rbp = 0 terminates stack walks
    call KiInitializeSystem     ; does not return

.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
