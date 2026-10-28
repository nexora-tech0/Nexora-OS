/*
 * Nexora OS - win32k/window.cpp
 * The window manager: window handles, z-order, surfaces, showing, moving and resizing,
 * activation and thread cleanup. Every function here runs with WmLock held.
 *
 * Window handles are (generation << 16) | (slot + 1), so a stale handle to a reused slot is
 * rejected. Z-order is one doubly linked list from WmBottom to WmTop; HZ_WS_TOPMOST windows are
 * kept above all others. A surface is a section of Width x Height x 4 bytes mapped into the
 * owner's process (for painting) and into system space (for the compositor); resizing replaces
 * it, which is why only the owning thread may resize (its painting code must see the new one).
 */
#include "w32p.h"

KMUTEX WmLock;
WINDOW *WmBottom;
WINDOW *WmTop;
WINDOW *WmActive;
uint32_t WmWindowCount;
WINDOW *WmShellWindow;
EPROCESS *WmShellProcess;

static HZ_RECT WmWorkAreaRect;
static bool WmWorkAreaSet;

static WINDOW *WmSlots[W32_MAX_WINDOWS];
static uint16_t WmGenerations[W32_MAX_WINDOWS];

void WmInitialize()
{
    KeInitializeMutex(&WmLock);
}

void WmAcquire()
{
    KeWaitForSingleObject(&WmLock, NULL);
}

void WmRelease()
{
    KeReleaseMutex(&WmLock);
}

WINDOW *WmFromHandle(HWND Handle)
{
    uint32_t Slot = (Handle & 0xFFFF) - 1;
    if (Slot >= W32_MAX_WINDOWS || !WmSlots[Slot] || WmGenerations[Slot] != (Handle >> 16))
        return nullptr;
    return WmSlots[Slot];
}

/* ---- Per-thread state ------------------------------------------------------------------------ */

W32THREAD *WmGetThread(bool Create)
{
    ETHREAD *Thread = PsGetCurrentThread();
    if (Thread->Win32Thread || !Create)
        return (W32THREAD *)Thread->Win32Thread;
    W32THREAD *T = (W32THREAD *)ExAllocatePoolZero(NonPagedPool, sizeof(W32THREAD), W32_TAG);
    if (!T)
        return nullptr;
    T->Thread = Thread;
    InitializeListHead(&T->Windows);
    InitializeListHead(&T->Messages);
    KeInitializeEvent(&T->Wake, SynchronizationEvent, false);
    Thread->Win32Thread = T;
    return T;
}

void WmThreadCleanup(ETHREAD *Thread)
{
    WmAcquire();
    W32THREAD *T = (W32THREAD *)Thread->Win32Thread;
    if (T) {
        while (!IsListEmpty(&T->Windows))
            WmDestroyWindow(CONTAINING_RECORD(T->Windows.Flink, WINDOW, ThreadLink));
        WmPurgeMessages(T, 0);
        Thread->Win32Thread = nullptr;
        ExFreePoolWithTag(T, W32_TAG);
    }
    WmRelease();
}

/* ---- Geometry and composition ----------------------------------------------------------------- */

HZ_RECT WmOuterBounds(const WINDOW *W)
{
    int32_t E = (W->Style & HZ_WS_POPUP) ? CMP_POPUP_SHADOW_EXTENT : CMP_SHADOW_EXTENT;
    HZ_RECT R = {W->Rect.Left - E, W->Rect.Top - E, W->Rect.Right + E, W->Rect.Bottom + E + CMP_SHADOW_OFFSET_Y};
    return R;
}

void WmInvalidateWindowArea(const WINDOW *W)
{
    HZ_RECT R = WmOuterBounds(W);
    CmpInvalidateRect(&R);
}

void WmFillSurfaceInfo(const WINDOW *W, HZ_SURFACE *S)
{
    S->Pixels = W->UserPixels;
    S->Width = W->SurfaceWidth;
    S->Height = W->SurfaceHeight;
    S->Stride = W->SurfaceWidth * 4;
    S->Generation = W->Generation;
}

/* ---- Z-order ---------------------------------------------------------------------------------- */

static void WmUnlink(WINDOW *W)
{
    if (W->Above)
        W->Above->Below = W->Below;
    else if (WmTop == W)
        WmTop = W->Below;
    if (W->Below)
        W->Below->Above = W->Above;
    else if (WmBottom == W)
        WmBottom = W->Above;
    W->Above = W->Below = nullptr;
}

/* Inserts at the top of its band (topmost windows above everything else). A desktop window goes
   to the very back instead: it belongs under every other window. */
static void WmLinkTop(WINDOW *W)
{
    if (W->Style & HZ_WS_DESKTOP) {
        WINDOW *Above = WmBottom;
        W->Below = nullptr;
        W->Above = Above;
        if (Above)
            Above->Below = W;
        else
            WmTop = W;
        WmBottom = W;
        return;
    }
    WINDOW *Under = WmTop; /* W goes right above Under */
    if (!(W->Style & HZ_WS_TOPMOST)) {
        while (Under && (Under->Style & HZ_WS_TOPMOST))
            Under = Under->Below;
    }
    W->Below = Under;
    W->Above = Under ? Under->Above : WmBottom;
    if (W->Above)
        W->Above->Below = W;
    else
        WmTop = W;
    if (Under)
        Under->Above = W;
    else
        WmBottom = W;
}

void WmBringToTop(WINDOW *W)
{
    WmUnlink(W);
    WmLinkTop(W);
    if (W->Visible)
        WmInvalidateWindowArea(W);
}

WINDOW *WmWindowFromPoint(int32_t X, int32_t Y)
{
    for (WINDOW *W = WmTop; W; W = W->Below) {
        if (W->Visible && WmContains(W->Rect, X, Y))
            return W;
    }
    return nullptr;
}

WINDOW *WmNextToActivate(WINDOW *Except)
{
    for (WINDOW *W = WmTop; W; W = W->Below) {
        if (W != Except && W->Visible && !(W->Style & (HZ_WS_NOACTIVATE | HZ_WS_POPUP)))
            return W;
    }
    return nullptr;
}

void WmActivate(WINDOW *W)
{
    if (W && (W->Style & HZ_WS_NOACTIVATE))
        return;
    WINDOW *Old = WmActive;
    if (W && !(W->Style & HZ_WS_DESKTOP))
        WmBringToTop(W); /* the desktop takes the keyboard without coming to the front */
    if (Old == W)
        return;
    InpCancelCapture(); /* menus and other captures end when activation moves */
    WmActive = W;
    if (!W || WmIsAppWindow(W))
        WmShellNotify(HZ_SHELL_WINDOW_ACTIVATED, W);
    else if (W->Style & HZ_WS_DESKTOP)
        WmShellEvent(HZ_SHELL_WINDOW_ACTIVATED, W->Handle); /* no app is active any more */
    if (Old) {
        WmPostMessage(Old, HZ_WM_ACTIVATE, 0, 0);
        WmInvalidateWindowArea(Old); /* weaker shadow */
    }
    if (W) {
        WmPostMessage(W, HZ_WM_ACTIVATE, 1, 0);
        WmInvalidateWindowArea(W);
    }
}

/* ---- Surfaces ----------------------------------------------------------------------------------- */

static STATUS WmCreateSurface(WINDOW *W, uint32_t Width, uint32_t Height, MM_SECTION **Section, uint32_t **Pixels,
                              uint64_t *UserPixels)
{
    STATUS Status = MmCreateSection((uint64_t)Width * Height * 4, Section);
    if (!HZ_SUCCESS(Status))
        return Status;
    void *System = nullptr, *User = nullptr;
    Status = MmMapViewInSystemSpace(*Section, &System);
    if (HZ_SUCCESS(Status))
        Status = MmMapViewOfSection(*Section, W->Process->AddressSpace, &User, MM_PROT_READWRITE);
    if (!HZ_SUCCESS(Status)) {
        ObDereferenceObject(*Section);
        return Status;
    }
    *Pixels = (uint32_t *)System;
    *UserPixels = (uint64_t)User;
    return STATUS_SUCCESS;
}

static void WmReleaseSurface(WINDOW *W, MM_SECTION *Section, uint64_t UserPixels)
{
    /* The view lives in the owner's address space; destruction runs in the owner's context. */
    if (UserPixels && PsGetCurrentProcess() == W->Process)
        MmUnmapViewOfSection(W->Process->AddressSpace, (void *)UserPixels);
    ObDereferenceObject(Section); /* the compositor may still hold a reference for a frame */
}

static bool WmValidSize(int32_t Width, int32_t Height)
{
    return Width >= 1 && Height >= 1 && Width <= HZ_MAX_WINDOW_SIZE && Height <= HZ_MAX_WINDOW_SIZE;
}

/* ---- Creation and destruction -------------------------------------------------------------------- */

STATUS WmCreateWindow(const HZ_CREATE_WINDOW *Create, WINDOW **Out)
{
    int32_t Width = Create->Rect.Right - Create->Rect.Left, Height = Create->Rect.Bottom - Create->Rect.Top;
    if (!WmValidSize(Width, Height) || (Create->Style & ~HZ_WS_VALID))
        return STATUS_INVALID_PARAMETER;
    W32THREAD *Thread = WmGetThread(true);
    if (!Thread)
        return STATUS_NO_MEMORY;
    uint32_t Slot = 0;
    while (Slot < W32_MAX_WINDOWS && WmSlots[Slot])
        Slot++;
    if (Slot == W32_MAX_WINDOWS)
        return STATUS_INSUFFICIENT_RESOURCES;

    WINDOW *W = new (std::nothrow) WINDOW();
    if (!W)
        return STATUS_NO_MEMORY;
    W->Owner = Thread;
    W->Process = PsGetCurrentProcess();
    W->ThreadId = Thread->Thread->Tcb.ThreadId;
    W->Rect = Create->Rect;
    W->Style = Create->Style & ~HZ_WS_VISIBLE;
    RtlStringCopy(W->Title, sizeof(W->Title), Create->Title);
    InitializeListHead(&W->Timers);
    STATUS Status = WmCreateSurface(W, (uint32_t)Width, (uint32_t)Height, &W->Surface, &W->Pixels, &W->UserPixels);
    if (!HZ_SUCCESS(Status)) {
        delete W;
        return Status;
    }
    W->SurfaceWidth = (uint32_t)Width;
    W->SurfaceHeight = (uint32_t)Height;
    W->Generation = 1;
    RGN_RECT Full = {0, 0, Width, Height};
    W->Update = RgnCreateRect(&Full);

    if (++WmGenerations[Slot] == 0)
        WmGenerations[Slot] = 1;
    W->Handle = ((uint32_t)WmGenerations[Slot] << 16) | (Slot + 1);
    WmSlots[Slot] = W;
    WmWindowCount++;
    InsertTailList(&Thread->Windows, &W->ThreadLink);
    WmLinkTop(W);
    WmShellNotify(HZ_SHELL_WINDOW_CREATED, W);
    if (Create->Style & HZ_WS_VISIBLE)
        WmShowWindow(W, HZ_SW_SHOW);
    *Out = W;
    return STATUS_SUCCESS;
}

void WmDestroyWindow(WINDOW *W)
{
    if (W == WmShellWindow) { /* the shell is gone: its taskbar too */
        WmShellWindow = nullptr;
        WmShellProcess = nullptr;
        WmSetWorkArea(nullptr);
    } else {
        WmShellNotify(HZ_SHELL_WINDOW_DESTROYED, W);
    }
    if (W->Visible)
        WmInvalidateWindowArea(W);
    InpWindowDestroyed(W);
    W->Visible = false; /* the pointer state below must not see it */
    WmUnlink(W);
    if (WmActive == W) {
        WmActive = nullptr;
        WmActivate(WmNextToActivate(W));
    }
    WmFreeTimers(W);
    WmPurgeMessages(W->Owner, W->Handle);
    RemoveEntryList(&W->ThreadLink);
    WmReleaseSurface(W, W->Surface, W->UserPixels);
    RgnDestroy(W->Update);
    WmSlots[(W->Handle & 0xFFFF) - 1] = nullptr;
    WmWindowCount--;
    delete W;
    InpRefreshPointer();
}

/* ---- Visibility and window states --------------------------------------------------------------- */

static void WmHide(WINDOW *W)
{
    if (!W->Visible)
        return;
    WmInvalidateWindowArea(W);
    W->Visible = false;
    WmShellNotify(HZ_SHELL_WINDOW_CHANGED, W);
    InpWindowHidden(W);
    if (WmActive == W) {
        WmActive = nullptr;
        WmActivate(WmNextToActivate(W));
    }
}

static void WmReveal(WINDOW *W)
{
    if (W->Visible)
        return;
    W->Visible = true;
    WmInvalidateWindowArea(W);
    WmShellNotify(HZ_SHELL_WINDOW_CHANGED, W);
    if (!RgnIsEmpty(W->Update))
        KeSetEvent(&W->Owner->Wake); /* WM_PAINT is generated only while the window is visible */
}

HZ_RECT WmWorkArea()
{
    return WmWorkAreaSet ? WmWorkAreaRect : HZ_RECT{0, 0, CmpScreenWidth(), CmpScreenHeight()};
}

STATUS WmSetWorkArea(const HZ_RECT *Rect)
{
    if (Rect) {
        HZ_RECT Screen = {0, 0, CmpScreenWidth(), CmpScreenHeight()};
        HZ_RECT R = WmIntersect(*Rect, Screen);
        if (R.Left != Rect->Left || R.Top != Rect->Top || R.Right != Rect->Right || R.Bottom != Rect->Bottom ||
            R.Right - R.Left < HZ_MIN_WINDOW_WIDTH || R.Bottom - R.Top < HZ_MIN_WINDOW_HEIGHT)
            return STATUS_INVALID_PARAMETER;
        WmWorkAreaRect = R;
    }
    WmWorkAreaSet = Rect != nullptr;
    for (WINDOW *W = WmBottom; W; W = W->Above) /* maximized windows follow the work area */
        if (W->State & HZ_WINDOW_STATE_MAXIMIZED)
            WmRequestResize(W, WmWorkArea());
    return STATUS_SUCCESS;
}

/* ---- The shell ------------------------------------------------------------------------------------ */

bool WmIsAppWindow(const WINDOW *W)
{
    return (W->Style & HZ_WS_CAPTION) && !(W->Style & (HZ_WS_POPUP | HZ_WS_OWNED)) &&
           (!WmShellProcess || W->Process != WmShellProcess);
}

bool WmCallerIsShell()
{
    return WmShellProcess && PsGetCurrentProcess() == WmShellProcess;
}

STATUS WmRegisterShell(WINDOW *W)
{
    if (WmShellProcess && WmShellProcess != PsGetCurrentProcess())
        return STATUS_ACCESS_DENIED;
    if (!W) {
        if (WmShellWindow) {
            WmShellWindow = nullptr;
            WmShellProcess = nullptr;
            WmSetWorkArea(nullptr);
        }
        return STATUS_SUCCESS;
    }
    WmShellWindow = W;
    WmShellProcess = W->Process;
    return STATUS_SUCCESS;
}

void WmShellEvent(uint32_t Event, uint64_t Detail)
{
    if (WmShellWindow)
        WmPostMessage(WmShellWindow, HZ_WM_SHELLNOTIFY, Event, Detail);
}

extern "C" void W32kNotifyProcessCrash(uint32_t ProcessId, STATUS Status)
{
    WmAcquire();
    WmShellEvent(HZ_SHELL_PROCESS_CRASHED, ((uint64_t)ProcessId << 32) | (uint32_t)Status);
    WmRelease();
}

void WmShellNotify(uint32_t Event, const WINDOW *W)
{
    if (W && !WmIsAppWindow(W))
        return;
    WmShellEvent(Event, W ? W->Handle : 0);
}

uint32_t WmEnumAppWindows(HWND *Buffer, uint32_t Capacity)
{
    uint32_t Count = 0;
    for (WINDOW *W = WmTop; W; W = W->Below) {
        if (!WmIsAppWindow(W) || !(W->Visible || (W->State & HZ_WINDOW_STATE_MINIMIZED)))
            continue;
        if (Count < Capacity)
            Buffer[Count] = W->Handle;
        Count++;
    }
    return Count;
}

uint32_t WmSizeType(const WINDOW *W)
{
    if (W->State & HZ_WINDOW_STATE_MINIMIZED)
        return HZ_SIZE_MINIMIZED;
    return (W->State & HZ_WINDOW_STATE_MAXIMIZED) ? HZ_SIZE_MAXIMIZED : HZ_SIZE_RESTORED;
}

void WmRequestResize(WINDOW *W, const HZ_RECT &Rect)
{
    W->PendingRect = Rect;
    if (!W->ResizePending) {
        W->ResizePending = true;
        WmPostMessage(W, HZ_WM_SIZE_REQUEST, 0, 0);
    }
}

void WmMinimizeWindow(WINDOW *W)
{
    if (W->State & HZ_WINDOW_STATE_MINIMIZED)
        return;
    W->State |= HZ_WINDOW_STATE_MINIMIZED;
    WmHide(W); /* tells the shell */
    WmPostMessage(W, HZ_WM_SIZE, HZ_SIZE_MINIMIZED,
                  HZ_MAKE_SIZE(W->Rect.Right - W->Rect.Left, W->Rect.Bottom - W->Rect.Top));
}

void WmMaximizeWindow(WINDOW *W)
{
    W->State &= ~HZ_WINDOW_STATE_MINIMIZED;
    if (!(W->State & HZ_WINDOW_STATE_MAXIMIZED)) {
        if (W->SnapState == WmSnapNone)
            W->RestoreRect = W->ResizePending ? W->PendingRect : W->Rect;
        W->SnapState = WmSnapNone;
        W->State |= HZ_WINDOW_STATE_MAXIMIZED;
        WmRequestResize(W, WmWorkArea());
        if (W->Visible)
            WmInvalidateWindowArea(W); /* square corners */
        WmShellNotify(HZ_SHELL_WINDOW_CHANGED, W);
    }
    WmReveal(W);
    WmActivate(W);
}

bool WmSnapWindow(WINDOW *W, uint32_t Snap)
{
    if (!W || Snap < WmSnapLeft || Snap > WmSnapBottomRight ||
        (W->State & HZ_WINDOW_STATE_MINIMIZED) || !(W->Style & HZ_WS_RESIZABLE) ||
        !(W->Style & HZ_WS_CAPTION) || (W->Style & (HZ_WS_POPUP | HZ_WS_OWNED)))
        return false;

    HZ_RECT Area = WmWorkArea();
    int32_t Width = Area.Right - Area.Left, Height = Area.Bottom - Area.Top;
    int32_t LeftWidth = Width / 2, TopHeight = Height / 2;
    int32_t MinWidth = HZ_MAX(HZ_MIN_WINDOW_WIDTH, W->MinWidth);
    int32_t MinHeight = HZ_MAX(HZ_MIN_WINDOW_HEIGHT, W->MinHeight);
    HZ_RECT R = Area;
    switch (Snap) {
    case WmSnapLeft:
        R.Right = Area.Left + LeftWidth;
        break;
    case WmSnapRight:
        R.Left = Area.Left + LeftWidth;
        break;
    case WmSnapTopLeft:
        R.Right = Area.Left + LeftWidth;
        R.Bottom = Area.Top + TopHeight;
        break;
    case WmSnapTopRight:
        R.Left = Area.Left + LeftWidth;
        R.Bottom = Area.Top + TopHeight;
        break;
    case WmSnapBottomLeft:
        R.Right = Area.Left + LeftWidth;
        R.Top = Area.Top + TopHeight;
        break;
    case WmSnapBottomRight:
        R.Left = Area.Left + LeftWidth;
        R.Top = Area.Top + TopHeight;
        break;
    default:
        return false;
    }
    if (R.Right - R.Left < MinWidth || R.Bottom - R.Top < MinHeight ||
        !WmValidSize(R.Right - R.Left, R.Bottom - R.Top))
        return false;

    if (W->SnapState == WmSnapNone && !(W->State & HZ_WINDOW_STATE_MAXIMIZED))
        W->RestoreRect = W->ResizePending ? W->PendingRect : W->Rect;
    W->State &= ~HZ_WINDOW_STATE_MAXIMIZED;
    W->SnapState = Snap;
    WmRequestResize(W, R);
    if (W->Visible)
        WmInvalidateWindowArea(W);
    WmShellNotify(HZ_SHELL_WINDOW_CHANGED, W);
    WmReveal(W);
    WmActivate(W);
    return true;
}

void WmRestoreWindow(WINDOW *W)
{
    if (W->State & HZ_WINDOW_STATE_MINIMIZED) {
        W->State &= ~HZ_WINDOW_STATE_MINIMIZED;
        WmReveal(W);
        WmActivate(W);
        WmPostMessage(W, HZ_WM_SIZE, WmSizeType(W),
                      HZ_MAKE_SIZE(W->Rect.Right - W->Rect.Left, W->Rect.Bottom - W->Rect.Top));
        return;
    }
    if (W->State & HZ_WINDOW_STATE_MAXIMIZED) {
        W->State &= ~HZ_WINDOW_STATE_MAXIMIZED;
        WmRequestResize(W, W->RestoreRect);
        if (W->Visible)
            WmInvalidateWindowArea(W);
        WmShellNotify(HZ_SHELL_WINDOW_CHANGED, W);
    } else if (W->SnapState != WmSnapNone) {
        W->SnapState = WmSnapNone;
        WmRequestResize(W, W->RestoreRect);
        if (W->Visible)
            WmInvalidateWindowArea(W);
        WmShellNotify(HZ_SHELL_WINDOW_CHANGED, W);
    }
}

void WmShowWindow(WINDOW *W, uint32_t Command)
{
    switch (Command) {
    case HZ_SW_HIDE:
        WmHide(W);
        break;
    case HZ_SW_MINIMIZE:
        WmMinimizeWindow(W);
        break;
    case HZ_SW_MAXIMIZE:
        WmMaximizeWindow(W);
        break;
    case HZ_SW_RESTORE:
        WmRestoreWindow(W);
        break;
    default: /* HZ_SW_SHOW, HZ_SW_SHOWNOACTIVATE */
        if (W->State & HZ_WINDOW_STATE_MINIMIZED) {
            WmRestoreWindow(W);
            break;
        }
        WmReveal(W);
        if (Command == HZ_SW_SHOW)
            WmActivate(W);
        break;
    }
    InpRefreshPointer();
}

void WmMoveWindow(WINDOW *W, int32_t X, int32_t Y)
{
    if (W->Rect.Left == X && W->Rect.Top == Y)
        return;
    if (W->Visible)
        WmInvalidateWindowArea(W);
    int32_t Width = W->Rect.Right - W->Rect.Left, Height = W->Rect.Bottom - W->Rect.Top;
    W->Rect = HZ_RECT{X, Y, X + Width, Y + Height};
    if (W->Visible)
        WmInvalidateWindowArea(W);
}

STATUS WmSetWindowPos(WINDOW *W, int32_t X, int32_t Y, int32_t Width, int32_t Height, uint32_t Flags)
{
    bool Requested = (Flags & HZ_SWP_PENDING) != 0; /* applying an interactive or state request */
    if (Flags & HZ_SWP_PENDING) {
        if (!W->ResizePending)
            return STATUS_SUCCESS;
        X = W->PendingRect.Left;
        Y = W->PendingRect.Top;
        Width = W->PendingRect.Right - W->PendingRect.Left;
        Height = W->PendingRect.Bottom - W->PendingRect.Top;
        W->ResizePending = false;
        Flags = 0;
    }
    if (Flags & HZ_SWP_NOMOVE) {
        X = W->Rect.Left;
        Y = W->Rect.Top;
    }
    if (Flags & HZ_SWP_NOSIZE) {
        Width = W->Rect.Right - W->Rect.Left;
        Height = W->Rect.Bottom - W->Rect.Top;
    }
    if (!WmValidSize(Width, Height))
        return STATUS_INVALID_PARAMETER;
    bool PositionChange = X != W->Rect.Left || Y != W->Rect.Top ||
                          Width != W->Rect.Right - W->Rect.Left || Height != W->Rect.Bottom - W->Rect.Top;
    if (Requested && PositionChange &&
        (W->SnapState != WmSnapNone || (W->State & HZ_WINDOW_STATE_MAXIMIZED))) {
        W->MotionFrom = W->Rect;
        W->MotionTo = HZ_RECT{X, Y, X + Width, Y + Height};
        W->MotionStartUs = KeQueryUptimeMicroseconds();
        W->MotionActive = true;
    }
    if (!Requested && PositionChange && W->SnapState != WmSnapNone) {
        W->SnapState = WmSnapNone;
        WmShellNotify(HZ_SHELL_WINDOW_CHANGED, W);
    }
    bool Resize = (uint32_t)Width != W->SurfaceWidth || (uint32_t)Height != W->SurfaceHeight;
    if (!Resize) {
        WmMoveWindow(W, X, Y);
        if (!(Flags & HZ_SWP_NOMOVE))
            WmPostMessage(W, HZ_WM_MOVE, 0, HZ_MAKE_POINT(X, Y));
        InpRefreshPointer();
        return STATUS_SUCCESS;
    }
    if (!Requested)
        W->State &= ~HZ_WINDOW_STATE_MAXIMIZED; /* the program chose a size of its own */

    MM_SECTION *Section;
    uint32_t *Pixels;
    uint64_t UserPixels;
    STATUS Status = WmCreateSurface(W, (uint32_t)Width, (uint32_t)Height, &Section, &Pixels, &UserPixels);
    if (!HZ_SUCCESS(Status))
        return Status;
    /* Keep the overlapping part so the window does not flash empty until it repaints. */
    uint32_t CopyWidth = HZ_MIN(W->SurfaceWidth, (uint32_t)Width), CopyHeight = HZ_MIN(W->SurfaceHeight, (uint32_t)Height);
    for (uint32_t y = 0; y < CopyHeight; y++)
        memcpy(Pixels + (size_t)y * Width, W->Pixels + (size_t)y * W->SurfaceWidth, (size_t)CopyWidth * 4);
    if (W->Visible)
        WmInvalidateWindowArea(W);
    MM_SECTION *OldSection = W->Surface;
    uint64_t OldUser = W->UserPixels;
    W->Surface = Section;
    W->Pixels = Pixels;
    W->UserPixels = UserPixels;
    W->SurfaceWidth = (uint32_t)Width;
    W->SurfaceHeight = (uint32_t)Height;
    W->Generation++;
    W->Rect = HZ_RECT{X, Y, X + Width, Y + Height};
    WmReleaseSurface(W, OldSection, OldUser);
    RGN_RECT Full = {0, 0, Width, Height};
    RgnSetRect(W->Update, &Full);
    if (W->Visible)
        WmInvalidateWindowArea(W);
    WmPostMessage(W, HZ_WM_SIZE, WmSizeType(W), HZ_MAKE_SIZE(Width, Height));
    KeSetEvent(&W->Owner->Wake); /* WM_PAINT is generated from the update region */
    InpRefreshPointer();
    return STATUS_SUCCESS;
}
