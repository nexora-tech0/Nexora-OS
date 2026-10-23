; Nexora OS - user/lib/hzuser/hzusersys.asm
; win32k system-call stubs, numbered from HZ_WIN32K_SERVICE_BASE (0x1000) in the order of
; enum HZ_WIN32K_SERVICE in include/uapi/hzuser.h (checked by the count below and by a
; static_assert in user/lib/hzuser/window.cpp). Same convention as hzsys.asm.

bits 64
default rel

section .text

%assign HZ_SERVICE_NUMBER 0x1000

%macro SERVICE 1
global %1
%1:
    mov r10, rcx
    mov eax, HZ_SERVICE_NUMBER
    syscall
    ret
%assign HZ_SERVICE_NUMBER HZ_SERVICE_NUMBER + 1
%endmacro

SERVICE NtUserCreateWindow
SERVICE NtUserDestroyWindow
SERVICE NtUserShowWindow
SERVICE NtUserSetWindowPos
SERVICE NtUserGetWindowInfo
SERVICE NtUserSetWindowText
SERVICE NtUserGetMessage
SERVICE NtUserPeekMessage
SERVICE NtUserPostMessage
SERVICE NtUserPostQuitMessage
SERVICE NtUserInvalidateRect
SERVICE NtUserBeginPaint
SERVICE NtUserEndPaint
SERVICE NtUserSetTimer
SERVICE NtUserKillTimer
SERVICE NtUserGetDisplayInfo
SERVICE NtUserGetCursorPos
SERVICE NtUserSetForegroundWindow
SERVICE NtUserWindowFromPoint
SERVICE NtUserSendInput
SERVICE NtUserFlush
SERVICE NtUserGetScreenPixel
SERVICE NtUserSetCapture
SERVICE NtUserSetWindowAttribute
SERVICE NtUserRegisterShell
SERVICE NtUserEnumWindows
SERVICE NtUserSetWorkArea
SERVICE NtUserSetClipboardText
SERVICE NtUserGetClipboardText
SERVICE NtUserSetWallpaper
SERVICE NtUserGetShellWindow

%if HZ_SERVICE_NUMBER != 0x1000 + 31
%error "win32k stub count does not match HZ_WIN32K_SERVICE_COUNT"
%endif
