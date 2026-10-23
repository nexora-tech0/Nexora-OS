; Nexora OS - ke/ctxswap.asm
; Stack switching primitives: the thread context switch, the first-run trampoline for new
; threads, and KiCallOnStack.

bits 64
default rel

extern KiThreadStartup
extern KiXsaveEnabled

section .text

; void KiSaveExtendedState(void *Area) / void KiRestoreExtendedState(void *Area)
; Area is 64-byte aligned. XSAVE64/XRSTOR64 with all enabled components (XCR0), or the
; 512-byte FXSAVE64 format on CPUs without XSAVE.
global KiSaveExtendedState
KiSaveExtendedState:
    cmp byte [KiXsaveEnabled], 0
    je .fxsave
    mov eax, -1
    mov edx, -1
    xsave64 [rdi]
    ret
.fxsave:
    fxsave64 [rdi]
    ret

global KiRestoreExtendedState
KiRestoreExtendedState:
    cmp byte [KiXsaveEnabled], 0
    je .fxrstor
    mov eax, -1
    mov edx, -1
    xrstor64 [rdi]
    ret
.fxrstor:
    fxrstor64 [rdi]
    ret

; void KiSwapContext(uint64_t *OldStackPointer, uint64_t NewStackPointer, volatile uint8_t *OldSwapBusy)
; Saves the callee-saved registers on the current stack, stores RSP through OldStackPointer,
; switches to NewStackPointer, clears *OldSwapBusy (the old thread's stack is no longer in use:
; another processor may now run it) and restores the registers saved on the new stack. Called by
; KiSwitchTo at DISPATCH_LEVEL with interrupts disabled; CR3, TSS.RSP0 and the PRCB are done in C.
; Kernel code is general-registers-only, so there is no FPU/SSE state to save (user threads
; add XSAVE in M4).
global KiSwapContext
KiSwapContext:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    mov byte [rdx], 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First code a new thread runs: KiSwapContext "returns" here with r12 = the KTHREAD (see the
; initial frame built by KeCreateThread). rbp = 0 terminates stack walks.
global KiThreadStartupStub
KiThreadStartupStub:
    xor ebp, ebp
    push rbp                    ; rsp 16-byte aligned for the call
    mov rdi, r12
    call KiThreadStartup        ; never returns
    ud2

; void KiCallOnStack(void (*Routine)(void *), void *Context, void *StackTop)
; Runs Routine(Context) on the stack ending at StackTop (16-byte aligned) and returns on the
; original stack. The new stack starts with a frame record {caller rbp, caller rip} so rbp-chain
; stack walks continue from the new stack back into the caller.
global KiCallOnStack
KiCallOnStack:
    push rbp
    mov rbp, rsp                ; rbp = original stack (restored below)
    mov rsp, rdx
    push qword [rbp + 8]        ; frame record on the new stack: return address ...
    push qword [rbp]            ; ... and the caller's rbp
    mov rax, rdi
    mov rdi, rsi
    push rbp                    ; keep the original rbp across the call (rsp now 8 mod 16)
    push rbp                    ; padding: rsp back to 0 mod 16 before the call
    lea rbp, [rsp + 16]         ; rbp -> frame record
    call rax
    mov rsp, rbp                ; back to the frame record ...
    mov rbp, [rsp - 8]          ; ... original rbp was saved just below it
    mov rsp, rbp
    pop rbp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
