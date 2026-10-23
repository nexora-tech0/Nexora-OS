; Nexora OS - mm/usercopy.asm
; The only kernel code allowed to touch user pages. Both routines run between
; MmpUserCopyStart and MmpUserCopyEnd: a page fault inside that range that the memory manager
; cannot resolve (unmapped page, protection) resumes at MmpUserCopyFault, which returns -1
; instead of bug checking (see the page-fault path in ke/trap.c). With SMAP enabled, stac/clac
; open the user-access window only for the duration of the copy.

bits 64
default rel

extern MmSmapEnabled

section .text

; int64_t MmpCopyUserMemory(void *Destination, const void *Source, size_t Length)
; Returns 0, or -1 if a user page faulted.
global MmpCopyUserMemory
MmpCopyUserMemory:
    cmp byte [MmSmapEnabled], 0
    je MmpUserCopyStart
    stac
global MmpUserCopyStart
MmpUserCopyStart:
    mov rcx, rdx
    rep movsb
    xor eax, eax
    jmp MmpUserCopyExit

; int64_t MmpCopyUserString(char *Destination, const char *UserSource, size_t Capacity)
; Copies bytes up to and including the terminator. Returns the length including the NUL,
; 0 if no terminator was found within Capacity bytes, or -1 if a user page faulted.
global MmpCopyUserString
MmpCopyUserString:
    cmp byte [MmSmapEnabled], 0
    je MmpCopyUserStringBody
    stac
MmpCopyUserStringBody:
    xor eax, eax
MmpCopyUserStringLoop:
    cmp rax, rdx
    je MmpCopyUserStringOverflow
    mov cl, [rsi + rax]
    mov [rdi + rax], cl
    inc rax
    test cl, cl
    jnz MmpCopyUserStringLoop
    jmp MmpUserCopyExit
MmpCopyUserStringOverflow:
    xor eax, eax
    jmp MmpUserCopyExit
global MmpUserCopyEnd
MmpUserCopyEnd:

global MmpUserCopyFault
MmpUserCopyFault:
    mov rax, -1
MmpUserCopyExit:
    cmp byte [MmSmapEnabled], 0
    je MmpUserCopyReturn
    clac
MmpUserCopyReturn:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
