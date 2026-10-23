; Nexora OS - hal/syscall.asm
; SYSCALL entry (IA32_LSTAR). User mode calls a stub that does "mov r10, rcx; mov eax, n;
; syscall". The CPU saves RIP in rcx and RFLAGS in r11, masks RFLAGS with IA32_FMASK (IF, TF,
; DF, AC, NT, IOPL cleared) and jumps here at CPL 0 with the user RSP and user GS base.
;
; The entry switches to the thread's kernel stack (KPRCB.KernelStackTop) and builds a full
; KTRAP_FRAME identical to an interrupt frame (vector 0xFFFF marks a system call), so the
; return path is the common KiTrapRestore: pop registers, swapgs, iretq. iretq is used instead
; of sysretq on purpose: it restores everything from the frame and has no non-canonical-RIP
; pitfall; a sysret fast path can come later.

bits 64
default rel

extern KiSystemServiceHandler
extern KiTrapRestore

USER_DATA_SELECTOR equ 0x1B             ; KGDT_USER_DATA | RPL 3
USER_CODE_SELECTOR equ 0x23             ; KGDT_USER_CODE | RPL 3
PRCB_USER_RSP equ 8                     ; KPRCB.UserRsp
PRCB_KERNEL_STACK equ 16                ; KPRCB.KernelStackTop
KTRAP_RIP_OFFSET equ 17 * 8             ; 15 GPRs + vector + error code

section .text

global KiSystemCall64
KiSystemCall64:
    swapgs                              ; GS base -> KPRCB
    mov [abs gs:PRCB_USER_RSP], rsp
    mov rsp, [abs gs:PRCB_KERNEL_STACK]
    push USER_DATA_SELECTOR             ; SS
    push qword [abs gs:PRCB_USER_RSP]   ; RSP
    push r11                            ; RFLAGS
    push USER_CODE_SELECTOR             ; CS
    push rcx                            ; RIP
    push 0                              ; error code
    push 0xFFFF                         ; "vector": system call
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp                        ; KTRAP_FRAME *
    push qword [rdi + KTRAP_RIP_OFFSET] ; fake frame so stack walks reach the user RIP
    push rbp
    mov rbp, rsp
    sti
    call KiSystemServiceHandler         ; returns with interrupts disabled
    add rsp, 16
    jmp KiTrapRestore

section .note.GNU-stack noalloc noexec nowrite progbits
