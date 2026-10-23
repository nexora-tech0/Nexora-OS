; Nexora OS - user/lib/hzsys/hzsys.asm
; System-call stubs (assembled with nasm -f win64 into hzsys.lib's object).
;
; Microsoft x64 convention: arguments in rcx, rdx, r8, r9, then the caller's stack above its
; 32-byte home area. SYSCALL overwrites rcx (return RIP) and r11 (RFLAGS), so the first argument
; moves to r10. The kernel returns the STATUS in eax and restores every other register except
; rcx and r11, which are volatile anyway.
;
; Stubs are numbered by position: the SERVICE lines must stay in the order of enum
; HZ_SYSTEM_SERVICE in include/uapi/hzapi.h (checked by the count at the end and by a
; static_assert in user/lib/hzrt/crt.cpp).

bits 64
default rel

section .text

%assign HZ_SERVICE_NUMBER 0

%macro SERVICE 1
global %1
%1:
    mov r10, rcx
    mov eax, HZ_SERVICE_NUMBER
    syscall
    ret
%assign HZ_SERVICE_NUMBER HZ_SERVICE_NUMBER + 1
%endmacro

SERVICE NtClose                         ; HzSysClose
SERVICE NtDisplayString                 ; HzSysDisplayString
SERVICE NtTerminateProcess              ; HzSysTerminateProcess
SERVICE NtTerminateThread               ; HzSysTerminateThread
SERVICE NtCreateThread                  ; HzSysCreateThread
SERVICE NtAllocateVirtualMemory         ; HzSysAllocateVirtualMemory
SERVICE NtFreeVirtualMemory             ; HzSysFreeVirtualMemory
SERVICE NtQueryVirtualMemory            ; HzSysQueryVirtualMemory
SERVICE NtProtectVirtualMemory          ; HzSysProtectVirtualMemory
SERVICE NtCreateEvent                   ; HzSysCreateEvent
SERVICE NtSetEvent                      ; HzSysSetEvent
SERVICE NtResetEvent                    ; HzSysResetEvent
SERVICE NtWaitForSingleObject           ; HzSysWaitForSingleObject
SERVICE NtWaitForMultipleObjects        ; HzSysWaitForMultipleObjects
SERVICE NtDelayExecution                ; HzSysDelayExecution
SERVICE NtYieldExecution                ; HzSysYieldExecution
SERVICE NtQuerySystemInformation        ; HzSysQuerySystemInformation
SERVICE NtQueryUptime                   ; HzSysQueryUptime
SERVICE NtCreateProcess                 ; HzSysCreateProcess
SERVICE NtQueryInformationProcess       ; HzSysQueryInformationProcess
SERVICE NtQueryInformationThread        ; HzSysQueryInformationThread
SERVICE NtCreateFile                    ; HzSysCreateFile
SERVICE NtReadFile                      ; HzSysReadFile
SERVICE NtWriteFile                     ; HzSysWriteFile
SERVICE NtQueryInformationFile          ; HzSysQueryInformationFile
SERVICE NtSetInformationFile            ; HzSysSetInformationFile
SERVICE NtQueryDirectoryFile            ; HzSysQueryDirectoryFile
SERVICE NtDeviceIoControlFile           ; HzSysDeviceIoControlFile
SERVICE NtShutdownSystem                ; HzSysShutdownSystem
SERVICE NtQuerySystemTime               ; HzSysQuerySystemTime
SERVICE NtCreateSection                 ; HzSysCreateSection
SERVICE NtMapViewOfSection              ; HzSysMapViewOfSection
SERVICE NtUnmapViewOfSection            ; HzSysUnmapViewOfSection
SERVICE NtCreatePipe                    ; HzSysCreatePipe
SERVICE NtCreateProcessEx               ; HzSysCreateProcessEx
SERVICE NtOpenProcess                   ; HzSysOpenProcess

%if HZ_SERVICE_NUMBER != 36
%error "service stub count does not match HzSysServiceCount"
%endif

; Exported so crt.cpp can compare it with HzSysServiceCount at run time as well.
section .rdata
global HzSysStubCount
HzSysStubCount:
    dd HZ_SERVICE_NUMBER
