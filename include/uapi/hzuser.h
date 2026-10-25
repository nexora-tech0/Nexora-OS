/*
 * Nexora OS - include/uapi/hzuser.h
 * The window manager ABI shared by win32k (kernel) and hzuser (user mode): window handles,
 * messages, styles and metrics, surfaces, and the win32k system services (numbers from
 * HZ_WIN32K_SERVICE_BASE, dispatched through win32k's own service table).
 *
 * Model: top-level windows only. Every window owns a surface (32-bit 0xAARRGGBB pixels in a
 * section mapped into its process and into the compositor). The application paints the whole
 * window, caption included (hzuser draws the standard caption). The window manager hit-tests the
 * caption, its buttons and the resize borders from fixed metrics, moves windows itself, and
 * asks the owning thread to resize (HZ_WM_SIZE_REQUEST) because resizing reallocates the surface.
 * Maximizing and restoring are resize requests too; minimizing hides the window.
 */
#ifndef HZUSER_ABI_H
#define HZUSER_ABI_H

#include "hzapi.h"

HZ_EXTERN_C_BEGIN

typedef uint32_t HWND; /* 0 = no window; not a kernel handle */

typedef struct _HZ_RECT {
    int32_t Left, Top, Right, Bottom; /* Right/Bottom exclusive */
} HZ_RECT;

typedef struct _HZ_POINT {
    int32_t X, Y;
} HZ_POINT;

/* ---- Metrics --------------------------------------------------------------------------------- */

#define HZ_CAPTION_HEIGHT 32
#define HZ_CAPTION_BUTTON_WIDTH 46
#define HZ_RESIZE_BORDER 6
#define HZ_WINDOW_RADIUS 8
/* Interactive resizing stops at the minimum; programs may create smaller windows (popups,
   tooltips). Every window is 1..HZ_MAX_WINDOW_SIZE pixels in each direction. */
#define HZ_MIN_WINDOW_WIDTH 160
#define HZ_MIN_WINDOW_HEIGHT 64
#define HZ_MAX_WINDOW_SIZE 4096

/* ---- Styles ------------------------------------------------------------------------------------ */

#define HZ_WS_CAPTION 0x0001u    /* caption: drag to move, close button at the right end */
#define HZ_WS_RESIZABLE 0x0002u  /* resize from the borders */
#define HZ_WS_POPUP 0x0004u      /* menus, tooltips: smaller shadow, no caption */
#define HZ_WS_TOPMOST 0x0008u    /* above every non-topmost window */
#define HZ_WS_MICA 0x0010u       /* translucent pixels show the blurred wallpaper (Mica-like) */
#define HZ_WS_NOACTIVATE 0x0020u /* clicks do not activate it */
#define HZ_WS_VISIBLE 0x0040u    /* shown (and activated) at creation */
#define HZ_WS_MINIMIZEBOX 0x0080u /* caption button: minimize (with HZ_WS_CAPTION) */
#define HZ_WS_MAXIMIZEBOX 0x0100u /* caption button: maximize/restore; caption double-click */
#define HZ_WS_TRANSPARENT 0x0200u /* per-pixel alpha over the windows below; no frame or shadow */
#define HZ_WS_NOFRAME 0x0400u     /* square corners, no border, no shadow (taskbars, overlays) */
#define HZ_WS_OWNED 0x0800u       /* a dialog of another window: not an app window (no taskbar button) */
/* The desktop: it stays below every other window, and a click activates it without raising it, so
   it can take the keyboard (F2, Delete, Ctrl+C/X/V) while the icons stay behind the windows. */
#define HZ_WS_DESKTOP 0x1000u
#define HZ_WS_VALID 0x1FFFu

/* Window state (HZ_WINDOW_INFO.State) */
#define HZ_WINDOW_STATE_VISIBLE 0x1u
#define HZ_WINDOW_STATE_ACTIVE 0x2u
#define HZ_WINDOW_STATE_MINIMIZED 0x4u
#define HZ_WINDOW_STATE_MAXIMIZED 0x8u

/* Caption buttons, right to left: close, maximize (HZ_WS_MAXIMIZEBOX), minimize
   (HZ_WS_MINIMIZEBOX), each HZ_CAPTION_BUTTON_WIDTH wide. */
#define HZ_CAPTION_BUTTON_NONE 0u
#define HZ_CAPTION_BUTTON_MINIMIZE 1u
#define HZ_CAPTION_BUTTON_MAXIMIZE 2u
#define HZ_CAPTION_BUTTON_CLOSE 3u

/* Cursor shapes (HZ_WA_CURSOR, HZ_DISPLAY_INFO.CursorShape) */
#define HZ_CURSOR_ARROW 0u
#define HZ_CURSOR_IBEAM 1u
#define HZ_CURSOR_HAND 2u
#define HZ_CURSOR_SIZE_WE 3u
#define HZ_CURSOR_SIZE_NS 4u
#define HZ_CURSOR_SIZE_NWSE 5u
#define HZ_CURSOR_SIZE_NESW 6u
#define HZ_CURSOR_COUNT 7u
#define HZ_CURSOR_HIDDEN 0xFFu /* reported while the mouse is locked (nothing is drawn) */

/* Window attributes (SetWindowAttribute) */
#define HZ_WA_CURSOR 1u   /* cursor shape over the client area (default HZ_CURSOR_ARROW) */
#define HZ_WA_MIN_SIZE 2u /* HZ_MAKE_SIZE(width, height): lower bound for interactive resizing */
/* Mouse lock (games): 1 locks, 0 releases; only the active window may lock. While locked the
   cursor is hidden and held at the window's centre, motion arrives as HZ_WM_MOUSEDELTA, and
   buttons and the wheel go to this window. The lock ends with activation changes, hiding the
   window, the logo key or Alt+Tab, and the window then receives HZ_WM_MOUSEUNLOCKED. */
#define HZ_WA_MOUSE_LOCK 3u

/* ---- Messages ---------------------------------------------------------------------------------- */

typedef struct _HZ_MSG {
    HWND Window;
    uint32_t Message;
    uint64_t WParam;
    uint64_t LParam;
    uint64_t TimeUs;  /* uptime when the message was generated */
    HZ_POINT Cursor;  /* screen position of the cursor at that time */
} HZ_MSG;

#define HZ_WM_NULL 0x0000
#define HZ_WM_CREATE 0x0001        /* sent by hzuser right after creation */
#define HZ_WM_DESTROY 0x0002       /* sent by hzuser before destruction */
#define HZ_WM_MOVE 0x0003          /* LParam = HZ_MAKE_POINT(new left, new top) */
#define HZ_WM_SIZE 0x0005          /* WParam = HZ_SIZE_*, LParam = HZ_MAKE_SIZE(width, height) */
#define HZ_WM_ACTIVATE 0x0006      /* WParam = 1 activated, 0 deactivated */
#define HZ_WM_PAINT 0x000F         /* generated while the update region is not empty */
#define HZ_WM_CLOSE 0x0010         /* close button or Alt+F4 */
#define HZ_WM_QUIT 0x0012          /* WParam = exit code; GetMessage returns false */
#define HZ_WM_SIZE_REQUEST 0x0090  /* interactive resize: the owner applies it (DefWindowProc) */
#define HZ_WM_CAPTIONSTATE 0x0091  /* WParam = hot button | pressed button << 8 (HZ_CAPTION_BUTTON_*) */
#define HZ_WM_SHELLNOTIFY 0x0092   /* to the shell window: WParam = HZ_SHELL_*, LParam = HWND or detail */
#define HZ_WM_SHELLREQUEST 0x0093  /* from programs to the shell window (GetShellWindow): HZ_SHELL_REQUEST_* */
#define HZ_WM_KEYDOWN 0x0100       /* WParam = virtual key, LParam = HZ key data */
#define HZ_WM_KEYUP 0x0101
#define HZ_WM_CHAR 0x0102          /* WParam = character (US layout), LParam = HZ key data */
#define HZ_WM_TIMER 0x0113         /* WParam = timer id */
#define HZ_WM_MOUSEMOVE 0x0200     /* WParam = buttons | modifiers << 16, LParam = window point */
#define HZ_WM_LBUTTONDOWN 0x0201
#define HZ_WM_LBUTTONUP 0x0202
#define HZ_WM_LBUTTONDBLCLK 0x0203
#define HZ_WM_RBUTTONDOWN 0x0204
#define HZ_WM_RBUTTONUP 0x0205
#define HZ_WM_MBUTTONDOWN 0x0207
#define HZ_WM_MBUTTONUP 0x0208
#define HZ_WM_MOUSEWHEEL 0x020A    /* WParam = wheel delta (int64, 120 per notch, positive = away) */
#define HZ_WM_MOUSEDELTA 0x020C    /* locked mouse: WParam as MOUSEMOVE, LParam = HZ_MAKE_POINT(dx, dy) */
#define HZ_WM_MOUSEUNLOCKED 0x0216 /* the mouse lock of this window ended without it asking */
#define HZ_WM_CAPTURECHANGED 0x0215 /* this window lost the explicit capture */
#define HZ_WM_MOUSELEAVE 0x02A3
#define HZ_WM_USER 0x0400          /* first application message */

/* Window points (LParam of mouse messages, window coordinates) and sizes */
#define HZ_MAKE_POINT(x, y) (((uint64_t)(uint32_t)(int32_t)(y) << 32) | (uint32_t)(int32_t)(x))
#define HZ_POINT_X(l) ((int32_t)(uint32_t)((uint64_t)(l) & 0xFFFFFFFFu))
#define HZ_POINT_Y(l) ((int32_t)(uint32_t)((uint64_t)(l) >> 32))
#define HZ_MAKE_SIZE(w, h) HZ_MAKE_POINT(w, h)

/* Key data (LParam of key and character messages) */
#define HZ_KEY_DATA(scan, flags, modifiers) \
    ((uint64_t)(scan) | ((uint64_t)(flags) << 16) | ((uint64_t)(modifiers) << 32))
#define HZ_KEY_DATA_SCAN(l) ((uint16_t)((l) & 0xFFFF))
#define HZ_KEY_DATA_FLAGS(l) ((uint16_t)(((l) >> 16) & 0xFFFF))
#define HZ_KEY_DATA_MODIFIERS(l) ((uint16_t)(((l) >> 32) & 0xFFFF))

#define HZ_PM_REMOVE 0x1u

/* HZ_WM_SHELLNOTIFY events. "App windows" have HZ_WS_CAPTION, not HZ_WS_POPUP, and do not belong
   to the shell's process; window events are about app windows only. */
#define HZ_SHELL_WINDOW_CREATED 1u   /* LParam = HWND */
#define HZ_SHELL_WINDOW_DESTROYED 2u /* LParam = HWND */
#define HZ_SHELL_WINDOW_ACTIVATED 3u /* LParam = HWND, 0 = none (the desktop) */
#define HZ_SHELL_WINDOW_CHANGED 4u   /* LParam = HWND: title, visibility or state */
#define HZ_SHELL_START_KEY 5u        /* the logo key was pressed and released on its own */
#define HZ_SHELL_SWITCH_NEXT 6u      /* Alt+Tab (LParam = 1 with Shift) while Alt is held */
#define HZ_SHELL_SWITCH_END 7u       /* LParam = 1: Alt released (switch), 0: Escape (cancel) */
#define HZ_SHELL_PROCESS_CRASHED 8u  /* M11: a process ended by an exception; LParam = PID << 32 | status */

/* HZ_WM_SHELLREQUEST (M9): WParam = request, LParam = argument. The shell decides. */
#define HZ_SHELL_REQUEST_WALLPAPER 1u /* LParam = wallpaper index */
#define HZ_SHELL_REQUEST_SETTINGS 2u  /* C:\Nexora\Settings\shell.ini changed: read it again (M10) */
#define HZ_SHELL_REQUEST_LOCK 3u      /* M11: show the lock screen */
#define HZ_SHELL_REQUEST_SHUTDOWN 4u  /* M11: close the programs, then LParam = HZ_SHUTDOWN_* */
#define HZ_SHELL_REQUEST_LOCK_WALLPAPER 5u /* LParam = independent lock-screen wallpaper index */

#define HZ_MAX_WALLPAPER 4u          /* The five bundled Nexora wallpaper images, indexed 0..4 */
#define HZ_WALLPAPER_AUTO (HZ_MAX_WALLPAPER + 1u) /* automatic boot wallpaper; display info only */
#define HZ_LOCK_WALLPAPER_DEFAULT HZ_MAX_WALLPAPER
#define HZ_MAX_CLIPBOARD 65536u      /* bytes of clipboard text */

/* HZ_WM_SIZE WParam */
#define HZ_SIZE_RESTORED 0u
#define HZ_SIZE_MINIMIZED 1u
#define HZ_SIZE_MAXIMIZED 2u

/* ---- Window services' structures -------------------------------------------------------------- */

typedef struct _HZ_CREATE_WINDOW {
    HZ_RECT Rect;     /* screen coordinates */
    uint32_t Style;   /* HZ_WS_* */
    uint32_t Reserved;
    char Title[128];
} HZ_CREATE_WINDOW;

typedef struct _HZ_SURFACE {
    uint64_t Pixels;      /* user address of pixel (0, 0); 0xAARRGGBB */
    uint32_t Width;
    uint32_t Height;
    uint32_t Stride;      /* bytes per row */
    uint32_t Generation;  /* changes when a resize replaced the surface */
} HZ_SURFACE;

typedef struct _HZ_WINDOW_INFO {
    HZ_RECT Rect;
    uint32_t Style;
    uint32_t State;       /* HZ_WINDOW_STATE_* */
    uint32_t ThreadId;
    uint32_t ProcessId;
    HZ_SURFACE Surface;   /* only for the owner's process */
    char Title[128];
} HZ_WINDOW_INFO;

typedef struct _HZ_PAINT {
    HZ_RECT Update;       /* bounds of the update region, window coordinates */
    HZ_SURFACE Surface;
} HZ_PAINT;

typedef struct _HZ_DISPLAY_INFO {
    uint32_t Width;
    uint32_t Height;
    uint32_t FrameIntervalUs;
    uint32_t Composited;  /* 1 once the desktop owns the screen */
    HZ_RECT WorkArea;     /* where maximized windows go (the whole screen until the taskbar) */
    uint32_t CursorShape; /* HZ_CURSOR_* currently shown */
    uint32_t Wallpaper;   /* index of the current wallpaper (M9) */
} HZ_DISPLAY_INFO;

#define HZ_SW_HIDE 0u
#define HZ_SW_SHOW 1u           /* show and activate (restores a minimized window) */
#define HZ_SW_SHOWNOACTIVATE 2u
#define HZ_SW_MINIMIZE 3u
#define HZ_SW_MAXIMIZE 4u       /* a resize request to the work area */
#define HZ_SW_RESTORE 5u        /* from minimized or maximized */

#define HZ_SWP_NOMOVE 0x01u
#define HZ_SWP_NOSIZE 0x02u
#define HZ_SWP_PENDING 0x10u    /* apply the rectangle of the last HZ_WM_SIZE_REQUEST */

/* Injected input (SendInput): the same path as the PS/2 devices. */
#define HZ_INPUT_KEYBOARD 1u
#define HZ_INPUT_MOUSE 2u
#define HZ_INPUT_MOUSE_ABSOLUTE 0x1u /* DeltaX/DeltaY are screen coordinates */

typedef struct _HZ_INPUT {
    uint32_t Type;
    uint32_t Flags;
    union {
        HZ_KEY_EVENT Key;
        HZ_MOUSE_EVENT Mouse;
    };
} HZ_INPUT;

/* ---- Services ------------------------------------------------------------------------------- *
 *   CreateWindow(const HZ_CREATE_WINDOW *Create, HWND *Window, HZ_SURFACE *Surface)
 *   DestroyWindow(HWND)                                            owner thread only
 *   ShowWindow(HWND, uint32_t Command)             HZ_SW_*
 *   SetWindowPos(HWND, int32_t X, int32_t Y, int32_t Width, int32_t Height, uint32_t Flags)
 *   GetWindowInfo(HWND, HZ_WINDOW_INFO *Info)
 *   SetWindowText(HWND, const char *Title)
 *   GetMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last)   STATUS_SUCCESS, or
 *                                                   STATUS_ALERTED (retry); WM_QUIT is returned
 *   PeekMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last, uint32_t Flags)
 *                                                   STATUS_NO_MORE_ENTRIES if nothing is queued
 *   PostMessage(HWND, uint32_t Message, uint64_t WParam, uint64_t LParam)
 *   PostQuitMessage(int64_t ExitCode)
 *   InvalidateRect(HWND, const HZ_RECT *Rect)      NULL = the whole window
 *   BeginPaint(HWND, HZ_PAINT *Paint)               validates the update region
 *   EndPaint(HWND, const HZ_RECT *Painted)          NULL = the whole window; queues composition
 *   SetTimer(HWND, uint64_t Id, uint32_t PeriodMs) / KillTimer(HWND, uint64_t Id)
 *   GetDisplayInfo(HZ_DISPLAY_INFO *Info)
 *   GetCursorPos(HZ_POINT *Point)
 *   SetForegroundWindow(HWND)
 *   WindowFromPoint(int32_t X, int32_t Y, HWND *Window)
 *   SendInput(const HZ_INPUT *Inputs, uint32_t Count)
 *   Flush(uint32_t TimeoutMs)                       waits until composition caught up
 *   GetScreenPixel(int32_t X, int32_t Y, uint32_t *Color)   composed pixel (0x00RRGGBB)
 *   SetCapture(HWND)                                owner thread; 0 releases the caller's capture.
 *                       While set, every mouse message goes to that window (window coordinates,
 *                       possibly outside it) and presses neither activate nor hit-test captions.
 *                       Activation changes cancel it (HZ_WM_CAPTURECHANGED).
 *   SetWindowAttribute(HWND, uint32_t Attribute, uint64_t Value)   owner thread; HZ_WA_*
 * Shell services (M8). One process at a time is the shell: the one that registered a window
 * with RegisterShell, until that window is destroyed. The shell may also show, minimize and
 * restore any window (ShowWindow), and the logo key and Alt+Tab go to it as HZ_WM_SHELLNOTIFY.
 *   RegisterShell(HWND)                             owner thread; ACCESS_DENIED while another
 *                                                   process is the shell; 0 unregisters
 *   EnumWindows(HWND *Buffer, uint32_t Capacity, uint32_t *Count)   app windows, top first;
 *                                                   *Count = how many exist
 *   SetWorkArea(const HZ_RECT *Rect)                shell only; maximized windows follow it
 *   SetClipboardText(const char *Text, uint32_t Length)   any process; Length 0 empties it
 *   GetClipboardText(char *Buffer, uint32_t Capacity, uint32_t *Length)   *Length = text length;
 *                                                   BUFFER_TOO_SMALL unless Capacity > Length
 *                                                   (the copy is NUL-terminated)
 *   SetWallpaper(uint32_t Index)                    shell only; 0..HZ_MAX_WALLPAPER
 *   GetShellWindow(HWND *Window)                    the registered shell window, 0 if none (M9)
 */

#define HZ_WIN32K_SERVICE_BASE 0x1000u

enum HZ_WIN32K_SERVICE {
    HzUserCreateWindow = HZ_WIN32K_SERVICE_BASE,
    HzUserDestroyWindow,
    HzUserShowWindow,
    HzUserSetWindowPos,
    HzUserGetWindowInfo,
    HzUserSetWindowText,
    HzUserGetMessage,
    HzUserPeekMessage,
    HzUserPostMessage,
    HzUserPostQuitMessage,
    HzUserInvalidateRect,
    HzUserBeginPaint,
    HzUserEndPaint,
    HzUserSetTimer,
    HzUserKillTimer,
    HzUserGetDisplayInfo,
    HzUserGetCursorPos,
    HzUserSetForegroundWindow,
    HzUserWindowFromPoint,
    HzUserSendInput,
    HzUserFlush,
    HzUserGetScreenPixel,
    HzUserSetCapture,
    HzUserSetWindowAttribute,
    HzUserRegisterShell,
    HzUserEnumWindows,
    HzUserSetWorkArea,
    HzUserSetClipboardText,
    HzUserGetClipboardText,
    HzUserSetWallpaper,
    HzUserGetShellWindow,
    HzUserServiceEnd
};

#define HZ_WIN32K_SERVICE_COUNT (HzUserServiceEnd - HZ_WIN32K_SERVICE_BASE)

HZ_EXTERN_C_END

#endif /* HZUSER_ABI_H */
