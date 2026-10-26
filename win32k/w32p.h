/*
 * Nexora OS - win32k/w32p.h
 * win32k internals (C++20, kernel mode): the window manager (Wm*), message queues, input
 * routing (Inp*) and the compositor (Cmp*).
 *
 * Locking:
 *   - WmLock (a mutex, PASSIVE_LEVEL) protects windows, z-order, activation, capture, queues and
 *     timers: every system service and the input threads hold it while they touch that state.
 *   - The compositor's dirty region has its own spinlock (CmpInvalidate* may be called with
 *     WmLock held). The compositor thread takes WmLock only to snapshot the visible windows; it
 *     composes from referenced surfaces without holding it.
 *   Lock order: WmLock -> compositor dirty lock -> pool.
 */
#ifndef W32P_H
#define W32P_H

#include "bootinfo.h"
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "kcxx.h"
#include "ke.h"
#include "mm.h"
#include "ob.h"
#include "ps.h"
#include "rtl.h"
#include "uapi/hzuser.h"
#include "w32k.h"

#define W32_TAG HZ_POOL_TAG('W', '3', '2', 'k')
#define W32_MAX_WINDOWS 1024u
#define W32_MAX_QUEUED_MESSAGES 4096u

/* Shadow geometry around a window (compositor and dirty-region bounds agree on it). */
#define CMP_SHADOW_EXTENT 24
#define CMP_SHADOW_OFFSET_Y 8
#define CMP_POPUP_SHADOW_EXTENT 10

struct W32THREAD;

struct W32TIMER {
    LIST_ENTRY Link;
    uint64_t Id;
    uint64_t PeriodUs;
    uint64_t DueUs;
};

struct WINDOW {
    HWND Handle;
    W32THREAD *Owner;
    EPROCESS *Process;       /* not referenced: windows are destroyed before their process */
    uint32_t ThreadId;
    HZ_RECT Rect;            /* screen coordinates */
    uint32_t Style;
    bool Visible;
    char Title[128];
    RGN *Update;             /* window coordinates */
    MM_SECTION *Surface;     /* referenced */
    uint32_t *Pixels;        /* system view of the surface */
    uint64_t UserPixels;     /* view in the owner process */
    uint32_t SurfaceWidth, SurfaceHeight;
    uint32_t Generation;
    WINDOW *Above;           /* z-order: towards the top */
    WINDOW *Below;
    LIST_ENTRY ThreadLink;
    LIST_ENTRY Timers;
    bool ResizePending;
    HZ_RECT PendingRect;
    uint32_t State;          /* HZ_WINDOW_STATE_MINIMIZED, HZ_WINDOW_STATE_MAXIMIZED */
    HZ_RECT RestoreRect;     /* the normal rectangle while maximized */
    uint32_t SnapState;      /* WmSnap*; zero when neither snapped nor maximized */
    uint32_t Cursor;         /* HZ_CURSOR_* over the client area */
    int32_t MinWidth, MinHeight; /* interactive resizing lower bound (with HZ_MIN_WINDOW_*) */
};

struct QUEUED_MESSAGE {
    LIST_ENTRY Link;
    HZ_MSG Msg;
};

struct W32THREAD {
    ETHREAD *Thread;
    LIST_ENTRY Windows;
    LIST_ENTRY Messages;
    uint32_t MessageCount;
    KEVENT Wake;             /* synchronization event: a message arrived */
    bool QuitPosted;
    int64_t QuitCode;
};

/* ---- Window manager (win32k/window.cpp) ---------------------------------------------------- */

extern KMUTEX WmLock;
extern WINDOW *WmBottom;
extern WINDOW *WmTop;
extern WINDOW *WmActive;
extern uint32_t WmWindowCount;
extern WINDOW *WmShellWindow;     /* the shell's registered window (M8), or NULL */
extern EPROCESS *WmShellProcess;  /* its process (not referenced; cleared with the window) */

void WmInitialize();
void WmAcquire();
void WmRelease();
WINDOW *WmFromHandle(HWND Handle);
W32THREAD *WmGetThread(bool Create);
STATUS WmCreateWindow(const HZ_CREATE_WINDOW *Create, WINDOW **Window);
void WmDestroyWindow(WINDOW *Window);
void WmShowWindow(WINDOW *Window, uint32_t Command);
STATUS WmSetWindowPos(WINDOW *Window, int32_t X, int32_t Y, int32_t Width, int32_t Height, uint32_t Flags);
void WmMoveWindow(WINDOW *Window, int32_t X, int32_t Y); /* no surface change */
/* State changes are resize requests the owner applies (HZ_SWP_PENDING). */
void WmMinimizeWindow(WINDOW *Window);
void WmMaximizeWindow(WINDOW *Window);
void WmRestoreWindow(WINDOW *Window);
enum {
    WmSnapNone,
    WmSnapLeft,
    WmSnapRight,
    WmSnapTopLeft,
    WmSnapTopRight,
    WmSnapBottomLeft,
    WmSnapBottomRight
};
bool WmSnapWindow(WINDOW *Window, uint32_t Snap);
void WmRequestResize(WINDOW *Window, const HZ_RECT &Rect);
uint32_t WmSizeType(const WINDOW *Window);                /* HZ_SIZE_* */
HZ_RECT WmWorkArea();
STATUS WmSetWorkArea(const HZ_RECT *Rect);                /* NULL: the whole screen */
/* The shell (M8) */
bool WmIsAppWindow(const WINDOW *Window);                 /* caption, not a popup or owned, not the shell's */
bool WmCallerIsShell();
STATUS WmRegisterShell(WINDOW *Window);                   /* NULL unregisters */
void WmShellNotify(uint32_t Event, const WINDOW *Window); /* app windows only; NULL = none */
void WmShellEvent(uint32_t Event, uint64_t Detail);
uint32_t WmEnumAppWindows(HWND *Buffer, uint32_t Capacity); /* returns how many exist */
void WmActivate(WINDOW *Window);                          /* NULL = no active window */
void WmBringToTop(WINDOW *Window);
WINDOW *WmWindowFromPoint(int32_t X, int32_t Y);
WINDOW *WmNextToActivate(WINDOW *Except);
HZ_RECT WmOuterBounds(const WINDOW *Window);              /* window plus shadow */
void WmInvalidateWindowArea(const WINDOW *Window);        /* queues composition of its bounds */
void WmFillSurfaceInfo(const WINDOW *Window, HZ_SURFACE *Surface);
void WmThreadCleanup(ETHREAD *Thread);

/* ---- Messages (win32k/queue.cpp) ------------------------------------------------------------- */

void WmPostMessage(WINDOW *Window, uint32_t Message, uint64_t WParam, uint64_t LParam);
void WmPostThreadMessage(W32THREAD *Thread, HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam);
void WmPurgeMessages(W32THREAD *Thread, HWND Window);
/* Remove: take the message off the queue. Wait: block (alertably) until one is available. */
STATUS WmGetMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last, bool Remove, bool Wait);
STATUS WmSetTimer(WINDOW *Window, uint64_t Id, uint32_t PeriodMs);
STATUS WmKillTimer(WINDOW *Window, uint64_t Id);
void WmFreeTimers(WINDOW *Window);

/* ---- Input (win32k/input.cpp) ----------------------------------------------------------------- */

void InpInitialize();
/* WmLock held. */
void InpProcessMouse(const HZ_MOUSE_EVENT *Event, bool Absolute);
void InpProcessKey(const HZ_KEY_EVENT *Event);
HZ_POINT InpGetCursor();
uint32_t InpGetCursorShape();
WINDOW *InpGetExplicitCapture();
void InpSetCapture(WINDOW *Window);                       /* NULL releases */
void InpCancelCapture();                                  /* activation changed (and the mouse lock) */
STATUS InpSetMouseLock(WINDOW *Window, bool Lock);        /* HZ_WA_MOUSE_LOCK */
void InpRefreshPointer();                                 /* windows or cursors under it changed */
void InpWindowHidden(WINDOW *Window);
void InpWindowDestroyed(WINDOW *Window);

/* ---- Compositor (win32k/compose.cpp) ------------------------------------------------------- */

bool CmpInitialize();
bool CmpIsAvailable();
int32_t CmpScreenWidth();
int32_t CmpScreenHeight();
/* Queues composition of a screen rectangle (any IRQL <= DISPATCH_LEVEL). */
void CmpInvalidateRect(const HZ_RECT *Rect);
void CmpInvalidateCursor(int32_t OldX, int32_t OldY, int32_t NewX, int32_t NewY);
/* From now on composed frames reach the framebuffer (the boot screen is handed over). */
void CmpShowDesktop();
bool CmpIsDesktopShown();
/* Waits until every queued change has been composed. */
STATUS CmpFlush(uint32_t TimeoutMs);
STATUS CmpGetPixel(int32_t X, int32_t Y, uint32_t *Color);
/* Rebuilds the wallpaper and backdrop (0..HZ_MAX_WALLPAPER) and swaps them in between frames.
   Call without WmLock. */
STATUS CmpSetWallpaper(uint32_t Index);
uint32_t CmpGetWallpaper();

/* ---- Helpers ---------------------------------------------------------------------------------- */

static inline bool WmRectEmpty(const HZ_RECT &R)
{
    return R.Left >= R.Right || R.Top >= R.Bottom;
}

static inline HZ_RECT WmIntersect(const HZ_RECT &A, const HZ_RECT &B)
{
    HZ_RECT R = {HZ_MAX(A.Left, B.Left), HZ_MAX(A.Top, B.Top), HZ_MIN(A.Right, B.Right), HZ_MIN(A.Bottom, B.Bottom)};
    return R;
}

static inline bool WmContains(const HZ_RECT &R, int32_t X, int32_t Y)
{
    return X >= R.Left && X < R.Right && Y >= R.Top && Y < R.Bottom;
}

static inline RGN_RECT WmToRgn(const HZ_RECT &R)
{
    RGN_RECT Out = {R.Left, R.Top, R.Right, R.Bottom};
    return Out;
}

#endif /* W32P_H */
