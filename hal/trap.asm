; Nexora OS - hal/trap.asm
; Trap entry stubs for all 256 vectors, the common save/restore path, and the GDT loader.
;
; Every stub leaves the stack as: [vector] [error code] [rip cs rflags rsp ss], pushing a zero
; error code where the CPU does not. KiTrapCommon then saves the general registers to form a
; KTRAP_FRAME (include/ke.h) and calls KiDispatchTrap(frame). A fake stack frame
; {interrupted rbp, interrupted rip} is linked in first, so rbp-chain stack walks continue
; from the handler into the interrupted code.

bits 64
default rel

extern KiDispatchTrap

section .text

KTRAP_GPR_COUNT equ 15
KTRAP_RIP_OFFSET equ (KTRAP_GPR_COUNT + 2) * 8   ; after GPRs, vector and error code
KTRAP_CS_FROM_VECTOR equ 3 * 8                    ; [vector][error][rip][cs]

%assign i 0
%rep 256
KiTrap%[i]:
%if (i == 8) || (i == 10) || (i == 11) || (i == 12) || (i == 13) || (i == 14) || (i == 17) || (i == 21) || (i == 29) || (i == 30)
    ; the CPU pushed an error code
%else
    push 0
%endif
    push i
    jmp KiTrapCommon
%assign i i + 1
%endrep

global KiTrapCommon
KiTrapCommon:
    test qword [rsp + KTRAP_CS_FROM_VECTOR], 3
    jz .from_kernel
    swapgs
.from_kernel:
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
    push qword [rdi + KTRAP_RIP_OFFSET] ; fake frame: return address = interrupted rip
    push rbp                            ;             saved rbp     = interrupted rbp
    mov rbp, rsp
    call KiDispatchTrap
    add rsp, 16
; Common exit: rsp points at a KTRAP_FRAME's saved general registers. Also the tail of the
; system-call path and the way new user threads first enter ring 3 (KiEnterUserMode).
global KiTrapRestore
KiTrapRestore:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    test qword [rsp + KTRAP_CS_FROM_VECTOR], 3
    jz .to_kernel
    swapgs
.to_kernel:
    add rsp, 16                         ; vector + error code
    iretq

; void KiEnterUserMode(KTRAP_FRAME *Frame) - interrupts disabled; never returns.
global KiEnterUserMode
KiEnterUserMode:
    cli
    mov rsp, rdi
    jmp KiTrapRestore

; void HalpLoadGdt(const KDESCRIPTOR *Gdt, uint16_t CodeSelector, uint16_t DataSelector)
global HalpLoadGdt
HalpLoadGdt:
    lgdt [rdi]
    mov ds, dx
    mov es, dx
    mov ss, dx
    xor eax, eax
    mov fs, ax
    mov gs, ax
    movzx esi, si
    pop rax                             ; return address
    push rsi                            ; new CS
    push rax
    retfq                               ; reload CS and return

section .rodata
align 8
global KiTrapStubTable
KiTrapStubTable:
%assign i 0
%rep 256
    dq KiTrap%[i]
%assign i i + 1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
