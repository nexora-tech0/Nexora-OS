/*
 * Nexora OS - win32k/usersvc.cpp
 * win32k system services (numbers from HZ_WIN32K_SERVICE_BASE, see uapi/hzuser.h) and the
 * subsystem's start-up.
 *
 * Operations that change a window's content or geometry, or its timers, are reserved for the
 * thread that created it (it is the only one that can use the new surface safely). Posting,
 * invalidating, activating and querying work on any window.
 */
#include "w32p.h"

namespace {

STATUS ReferenceWindow(uint64_t Handle, bool OwnerOnly, WINDOW **Out)
{
    WINDOW *W = WmFromHandle((HWND)Handle);
    if (!W)
        return STATUS_INVALID_HANDLE;
    if (OwnerOnly && W->Owner != WmGetThread(false))
        return STATUS_ACCESS_DENIED;
    *Out = W;
    return STATUS_SUCCESS;
}

STATUS Probe(uint64_t Pointer, size_t Length)
{
    static const uint8_t Zero[64] = {};
    return Length <= sizeof(Zero) ? MmCopyToUser((void *)Pointer, Zero, Length) : STATUS_INVALID_PARAMETER;
}

/* CreateWindow(const HZ_CREATE_WINDOW *Create, HWND *Window, HZ_SURFACE *Surface) */
STATUS NtUserCreateWindow(const uint64_t *Args)
{
    HZ_CREATE_WINDOW Create;
    STATUS Status = MmCopyFromUser(&Create, (const void *)Args[0], sizeof(Create));
    if (HZ_SUCCESS(Status))
        Status = Probe(Args[1], sizeof(HWND));
    if (HZ_SUCCESS(Status) && Args[2])
        Status = Probe(Args[2], sizeof(HZ_SURFACE));
    if (!HZ_SUCCESS(Status))
        return Status;
    Create.Title[sizeof(Create.Title) - 1] = '\0';
    HZ_SURFACE Surface;
    HWND Handle = 0;
    WmAcquire();
    WINDOW *W;
    Status = WmCreateWindow(&Create, &W);
    if (HZ_SUCCESS(Status)) {
        Handle = W->Handle;
        WmFillSurfaceInfo(W, &Surface);
    }
    WmRelease();
    if (!HZ_SUCCESS(Status))
        return Status;
    MmCopyToUser((void *)Args[1], &Handle, sizeof(Handle));
    if (Args[2])
        MmCopyToUser((void *)Args[2], &Surface, sizeof(Surface));
    return STATUS_SUCCESS;
}

/* DestroyWindow(HWND) */
STATUS NtUserDestroyWindow(const uint64_t *Args)
{
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status))
        WmDestroyWindow(W);
    WmRelease();
    return Status;
}

/* ShowWindow(HWND, uint32_t Command) */
STATUS NtUserShowWindow(const uint64_t *Args)
{
    if (Args[1] > HZ_SW_RESTORE)
        return STATUS_INVALID_PARAMETER;
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], !WmCallerIsShell(), &W); /* the shell may show any window */
    if (HZ_SUCCESS(Status))
        WmShowWindow(W, (uint32_t)Args[1]);
    WmRelease();
    return Status;
}

/* SetWindowPos(HWND, int32_t X, int32_t Y, int32_t Width, int32_t Height, uint32_t Flags) */
STATUS NtUserSetWindowPos(const uint64_t *Args)
{
    uint32_t Flags = (uint32_t)Args[5];
    if (Flags & ~(HZ_SWP_NOMOVE | HZ_SWP_NOSIZE | HZ_SWP_PENDING))
        return STATUS_INVALID_PARAMETER;
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status))
        Status = WmSetWindowPos(W, (int32_t)Args[1], (int32_t)Args[2], (int32_t)Args[3], (int32_t)Args[4], Flags);
    WmRelease();
    return Status;
}

/* GetWindowInfo(HWND, HZ_WINDOW_INFO *Info) */
STATUS NtUserGetWindowInfo(const uint64_t *Args)
{
    HZ_WINDOW_INFO Info;
    memset(&Info, 0, sizeof(Info));
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], false, &W);
    if (HZ_SUCCESS(Status)) {
        Info.Rect = W->Rect;
        Info.Style = W->Style | (W->Visible ? HZ_WS_VISIBLE : 0);
        Info.State = (W->Visible ? HZ_WINDOW_STATE_VISIBLE : 0) | (W == WmActive ? HZ_WINDOW_STATE_ACTIVE : 0) |
                     W->State;
        Info.ThreadId = W->ThreadId;
        Info.ProcessId = W->Process->ProcessId;
        if (W->Process == PsGetCurrentProcess())
            WmFillSurfaceInfo(W, &Info.Surface);
        RtlStringCopy(Info.Title, sizeof(Info.Title), W->Title);
    }
    WmRelease();
    return HZ_SUCCESS(Status) ? MmCopyToUser((void *)Args[1], &Info, sizeof(Info)) : Status;
}

/* SetWindowText(HWND, const char *Title) */
STATUS NtUserSetWindowText(const uint64_t *Args)
{
    char Title[128];
    STATUS Status = MmCopyStringFromUser(Title, sizeof(Title), (const char *)Args[1]);
    if (Status == STATUS_NAME_TOO_LONG)
        Status = STATUS_SUCCESS; /* truncated */
    if (!HZ_SUCCESS(Status))
        return Status;
    WmAcquire();
    WINDOW *W;
    Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status)) {
        RtlStringCopy(W->Title, sizeof(W->Title), Title);
        WmShellNotify(HZ_SHELL_WINDOW_CHANGED, W);
    }
    WmRelease();
    return Status;
}

STATUS GetOrPeek(const uint64_t *Args, bool Remove, bool Wait)
{
    STATUS Status = Probe(Args[0], sizeof(HZ_MSG));
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_MSG Msg;
    Status = WmGetMessage(&Msg, (HWND)Args[1], (uint32_t)Args[2], (uint32_t)Args[3], Remove, Wait);
    if (Status == STATUS_SUCCESS)
        Status = MmCopyToUser((void *)Args[0], &Msg, sizeof(Msg));
    return Status;
}

/* GetMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last) */
STATUS NtUserGetMessage(const uint64_t *Args)
{
    return GetOrPeek(Args, true, true);
}

/* PeekMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last, uint32_t Flags) */
STATUS NtUserPeekMessage(const uint64_t *Args)
{
    return GetOrPeek(Args, (Args[4] & HZ_PM_REMOVE) != 0, false);
}

/* PostMessage(HWND, uint32_t Message, uint64_t WParam, uint64_t LParam) */
STATUS NtUserPostMessage(const uint64_t *Args)
{
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], false, &W);
    if (HZ_SUCCESS(Status))
        WmPostMessage(W, (uint32_t)Args[1], Args[2], Args[3]);
    WmRelease();
    return Status;
}

/* PostQuitMessage(int64_t ExitCode) */
STATUS NtUserPostQuitMessage(const uint64_t *Args)
{
    WmAcquire();
    W32THREAD *T = WmGetThread(true);
    if (T) {
        T->QuitPosted = true;
        T->QuitCode = (int64_t)Args[0];
        KeSetEvent(&T->Wake);
    }
    WmRelease();
    return T ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

/* InvalidateRect(HWND, const HZ_RECT *Rect) */
STATUS NtUserInvalidateRect(const uint64_t *Args)
{
    HZ_RECT Rect = {0, 0, HZ_MAX_WINDOW_SIZE, HZ_MAX_WINDOW_SIZE};
    if (Args[1]) {
        STATUS Status = MmCopyFromUser(&Rect, (const void *)Args[1], sizeof(Rect));
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], false, &W);
    if (HZ_SUCCESS(Status)) {
        HZ_RECT Surface = {0, 0, (int32_t)W->SurfaceWidth, (int32_t)W->SurfaceHeight};
        RGN_RECT R = WmToRgn(WmIntersect(Rect, Surface));
        if (R.Left < R.Right && R.Top < R.Bottom) {
            RgnCombineRect(W->Update, &R, RGN_OP_UNION);
            KeSetEvent(&W->Owner->Wake);
        }
    }
    WmRelease();
    return Status;
}

/* BeginPaint(HWND, HZ_PAINT *Paint) */
STATUS NtUserBeginPaint(const uint64_t *Args)
{
    STATUS Status = Probe(Args[1], sizeof(HZ_PAINT));
    if (!HZ_SUCCESS(Status))
        return Status;
    HZ_PAINT Paint;
    memset(&Paint, 0, sizeof(Paint));
    WmAcquire();
    WINDOW *W;
    Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status)) {
        RGN_RECT Bounds;
        RgnGetBounds(W->Update, &Bounds);
        Paint.Update = HZ_RECT{Bounds.Left, Bounds.Top, Bounds.Right, Bounds.Bottom};
        RgnSetRect(W->Update, NULL);
        WmFillSurfaceInfo(W, &Paint.Surface);
    }
    WmRelease();
    return HZ_SUCCESS(Status) ? MmCopyToUser((void *)Args[1], &Paint, sizeof(Paint)) : Status;
}

/* EndPaint(HWND, const HZ_RECT *Painted) */
STATUS NtUserEndPaint(const uint64_t *Args)
{
    HZ_RECT Painted = {0, 0, HZ_MAX_WINDOW_SIZE, HZ_MAX_WINDOW_SIZE};
    if (Args[1]) {
        STATUS Status = MmCopyFromUser(&Painted, (const void *)Args[1], sizeof(Painted));
        if (!HZ_SUCCESS(Status))
            return Status;
    }
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status) && W->Visible) {
        HZ_RECT Window = {0, 0, W->Rect.Right - W->Rect.Left, W->Rect.Bottom - W->Rect.Top};
        HZ_RECT R = WmIntersect(Painted, Window);
        R = HZ_RECT{R.Left + W->Rect.Left, R.Top + W->Rect.Top, R.Right + W->Rect.Left, R.Bottom + W->Rect.Top};
        CmpInvalidateRect(&R);
    }
    WmRelease();
    return Status;
}

/* SetTimer(HWND, uint64_t Id, uint32_t PeriodMs) */
STATUS NtUserSetTimer(const uint64_t *Args)
{
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status))
        Status = WmSetTimer(W, Args[1], (uint32_t)Args[2]);
    WmRelease();
    return Status;
}

/* KillTimer(HWND, uint64_t Id) */
STATUS NtUserKillTimer(const uint64_t *Args)
{
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status))
        Status = WmKillTimer(W, Args[1]);
    WmRelease();
    return Status;
}

/* GetDisplayInfo(HZ_DISPLAY_INFO *Info) */
STATUS NtUserGetDisplayInfo(const uint64_t *Args)
{
    HZ_DISPLAY_INFO Info;
    memset(&Info, 0, sizeof(Info));
    Info.Width = (uint32_t)CmpScreenWidth();
    Info.Height = (uint32_t)CmpScreenHeight();
    Info.FrameIntervalUs = 16000;
    Info.Composited = CmpIsDesktopShown() ? 1u : 0u;
    WmAcquire();
    Info.WorkArea = WmWorkArea();
    Info.CursorShape = InpGetCursorShape();
    Info.Wallpaper = CmpGetWallpaper();
    WmRelease();
    return MmCopyToUser((void *)Args[0], &Info, sizeof(Info));
}

/* GetCursorPos(HZ_POINT *Point) */
STATUS NtUserGetCursorPos(const uint64_t *Args)
{
    WmAcquire();
    HZ_POINT P = InpGetCursor();
    WmRelease();
    return MmCopyToUser((void *)Args[0], &P, sizeof(P));
}

/* SetForegroundWindow(HWND) */
STATUS NtUserSetForegroundWindow(const uint64_t *Args)
{
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], false, &W);
    if (HZ_SUCCESS(Status)) {
        if (!W->Visible || (W->Style & HZ_WS_NOACTIVATE))
            Status = STATUS_INVALID_PARAMETER;
        else
            WmActivate(W);
    }
    WmRelease();
    return Status;
}

/* WindowFromPoint(int32_t X, int32_t Y, HWND *Window) */
STATUS NtUserWindowFromPoint(const uint64_t *Args)
{
    WmAcquire();
    WINDOW *W = WmWindowFromPoint((int32_t)Args[0], (int32_t)Args[1]);
    HWND Handle = W ? W->Handle : 0;
    WmRelease();
    return MmCopyToUser((void *)Args[2], &Handle, sizeof(Handle));
}

/* SendInput(const HZ_INPUT *Inputs, uint32_t Count) */
STATUS NtUserSendInput(const uint64_t *Args)
{
    const uint32_t Max = 64;
    uint32_t Count = (uint32_t)Args[1];
    if (Count == 0 || Count > Max)
        return STATUS_INVALID_PARAMETER;
    HZ_INPUT *Inputs = (HZ_INPUT *)ExAllocatePoolWithTag(NonPagedPool, Count * sizeof(HZ_INPUT), W32_TAG);
    if (!Inputs)
        return STATUS_NO_MEMORY;
    STATUS Status = MmCopyFromUser(Inputs, (const void *)Args[0], Count * sizeof(HZ_INPUT));
    if (HZ_SUCCESS(Status)) {
        WmAcquire();
        for (uint32_t i = 0; i < Count; i++) {
            if (Inputs[i].Type == HZ_INPUT_MOUSE)
                InpProcessMouse(&Inputs[i].Mouse, (Inputs[i].Flags & HZ_INPUT_MOUSE_ABSOLUTE) != 0);
            else if (Inputs[i].Type == HZ_INPUT_KEYBOARD)
                InpProcessKey(&Inputs[i].Key);
            else
                Status = STATUS_INVALID_PARAMETER;
        }
        WmRelease();
    }
    ExFreePoolWithTag(Inputs, W32_TAG);
    return Status;
}

/* Flush(uint32_t TimeoutMs) */
STATUS NtUserFlush(const uint64_t *Args)
{
    return CmpFlush(HZ_MIN((uint32_t)Args[0], 10000u));
}

/* GetScreenPixel(int32_t X, int32_t Y, uint32_t *Color) */
STATUS NtUserGetScreenPixel(const uint64_t *Args)
{
    uint32_t Color;
    STATUS Status = CmpGetPixel((int32_t)Args[0], (int32_t)Args[1], &Color);
    return HZ_SUCCESS(Status) ? MmCopyToUser((void *)Args[2], &Color, sizeof(Color)) : Status;
}

/* SetCapture(HWND): 0 releases the caller's own explicit capture. */
STATUS NtUserSetCapture(const uint64_t *Args)
{
    WmAcquire();
    STATUS Status = STATUS_SUCCESS;
    if (Args[0] == 0) {
        WINDOW *Current = InpGetExplicitCapture();
        if (Current && Current->Owner == WmGetThread(false))
            InpSetCapture(nullptr);
    } else {
        WINDOW *W;
        Status = ReferenceWindow(Args[0], true, &W);
        if (HZ_SUCCESS(Status)) {
            if (W->Visible)
                InpSetCapture(W);
            else
                Status = STATUS_INVALID_PARAMETER;
        }
    }
    WmRelease();
    return Status;
}

/* SetWindowAttribute(HWND, uint32_t Attribute, uint64_t Value) */
STATUS NtUserSetWindowAttribute(const uint64_t *Args)
{
    WmAcquire();
    WINDOW *W;
    STATUS Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status)) {
        uint64_t Value = Args[2];
        switch ((uint32_t)Args[1]) {
        case HZ_WA_CURSOR:
            if (Value >= HZ_CURSOR_COUNT) {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            W->Cursor = (uint32_t)Value;
            InpRefreshPointer();
            break;
        case HZ_WA_MOUSE_LOCK:
            Status = InpSetMouseLock(W, Value != 0);
            break;
        case HZ_WA_MIN_SIZE: {
            int32_t Width = HZ_POINT_X(Value), Height = HZ_POINT_Y(Value);
            if (Width < 0 || Height < 0 || Width > HZ_MAX_WINDOW_SIZE || Height > HZ_MAX_WINDOW_SIZE) {
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            W->MinWidth = Width;
            W->MinHeight = Height;
            break;
        }
        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
    }
    WmRelease();
    return Status;
}

/* RegisterShell(HWND): 0 unregisters. */
STATUS NtUserRegisterShell(const uint64_t *Args)
{
    WmAcquire();
    STATUS Status = STATUS_SUCCESS;
    WINDOW *W = nullptr;
    if (Args[0])
        Status = ReferenceWindow(Args[0], true, &W);
    if (HZ_SUCCESS(Status))
        Status = WmRegisterShell(W);
    WmRelease();
    return Status;
}

/* EnumWindows(HWND *Buffer, uint32_t Capacity, uint32_t *Count) */
STATUS NtUserEnumWindows(const uint64_t *Args)
{
    uint32_t Capacity = (uint32_t)Args[1];
    if (Capacity > W32_MAX_WINDOWS)
        Capacity = W32_MAX_WINDOWS;
    HWND *Buffer = nullptr;
    if (Capacity) {
        Buffer = (HWND *)ExAllocatePoolWithTag(NonPagedPool, Capacity * sizeof(HWND), W32_TAG);
        if (!Buffer)
            return STATUS_NO_MEMORY;
    }
    WmAcquire();
    uint32_t Count = WmEnumAppWindows(Buffer, Capacity);
    WmRelease();
    STATUS Status = MmCopyToUser((void *)Args[2], &Count, sizeof(Count));
    if (HZ_SUCCESS(Status) && Capacity)
        Status = MmCopyToUser((void *)Args[0], Buffer, HZ_MIN(Count, Capacity) * sizeof(HWND));
    if (Buffer)
        ExFreePoolWithTag(Buffer, W32_TAG);
    return Status;
}

/* SetWorkArea(const HZ_RECT *Rect) */
STATUS NtUserSetWorkArea(const uint64_t *Args)
{
    HZ_RECT Rect;
    STATUS Status = MmCopyFromUser(&Rect, (const void *)Args[0], sizeof(Rect));
    if (!HZ_SUCCESS(Status))
        return Status;
    WmAcquire();
    Status = WmCallerIsShell() ? WmSetWorkArea(&Rect) : STATUS_ACCESS_DENIED;
    WmRelease();
    return Status;
}

char *WmClipboard;
uint32_t WmClipboardLength;

/* SetClipboardText(const char *Text, uint32_t Length) */
STATUS NtUserSetClipboardText(const uint64_t *Args)
{
    uint32_t Length = (uint32_t)Args[1];
    if (Length > HZ_MAX_CLIPBOARD)
        return STATUS_INVALID_PARAMETER;
    char *Text = nullptr;
    if (Length) {
        Text = (char *)ExAllocatePoolWithTag(NonPagedPool, Length, W32_TAG);
        if (!Text)
            return STATUS_NO_MEMORY;
        STATUS Status = MmCopyFromUser(Text, (const void *)Args[0], Length);
        if (!HZ_SUCCESS(Status)) {
            ExFreePoolWithTag(Text, W32_TAG);
            return Status;
        }
    }
    WmAcquire();
    char *Old = WmClipboard;
    WmClipboard = Text;
    WmClipboardLength = Length;
    WmRelease();
    if (Old)
        ExFreePoolWithTag(Old, W32_TAG);
    return STATUS_SUCCESS;
}

/* GetClipboardText(char *Buffer, uint32_t Capacity, uint32_t *Length) */
STATUS NtUserGetClipboardText(const uint64_t *Args)
{
    uint32_t Capacity = (uint32_t)Args[1];
    WmAcquire();
    uint32_t Length = WmClipboardLength;
    STATUS Status = MmCopyToUser((void *)Args[2], &Length, sizeof(Length));
    if (HZ_SUCCESS(Status)) {
        if (Capacity <= Length) {
            Status = STATUS_BUFFER_TOO_SMALL;
        } else {
            if (Length)
                Status = MmCopyToUser((void *)Args[0], WmClipboard, Length);
            static const char Nul = '\0';
            if (HZ_SUCCESS(Status))
                Status = MmCopyToUser((char *)Args[0] + Length, &Nul, 1);
        }
    }
    WmRelease();
    return Status;
}

/* SetWallpaper(uint32_t Index) */
STATUS NtUserSetWallpaper(const uint64_t *Args)
{
    WmAcquire();
    bool Shell = WmCallerIsShell();
    WmRelease();
    if (!Shell)
        return STATUS_ACCESS_DENIED;
    return CmpSetWallpaper((uint32_t)Args[0]);
}

/* GetShellWindow(HWND *Window) */
STATUS NtUserGetShellWindow(const uint64_t *Args)
{
    WmAcquire();
    HWND Handle = WmShellWindow ? WmShellWindow->Handle : 0;
    WmRelease();
    return MmCopyToUser((void *)Args[0], &Handle, sizeof(Handle));
}

/* In HZ_WIN32K_SERVICE order (checked by the count; keep it in step with uapi/hzuser.h). */
const KSYSTEM_SERVICE W32kServiceTable[] = {
    {NtUserCreateWindow, 3, "UserCreateWindow"},
    {NtUserDestroyWindow, 1, "UserDestroyWindow"},
    {NtUserShowWindow, 2, "UserShowWindow"},
    {NtUserSetWindowPos, 6, "UserSetWindowPos"},
    {NtUserGetWindowInfo, 2, "UserGetWindowInfo"},
    {NtUserSetWindowText, 2, "UserSetWindowText"},
    {NtUserGetMessage, 4, "UserGetMessage"},
    {NtUserPeekMessage, 5, "UserPeekMessage"},
    {NtUserPostMessage, 4, "UserPostMessage"},
    {NtUserPostQuitMessage, 1, "UserPostQuitMessage"},
    {NtUserInvalidateRect, 2, "UserInvalidateRect"},
    {NtUserBeginPaint, 2, "UserBeginPaint"},
    {NtUserEndPaint, 2, "UserEndPaint"},
    {NtUserSetTimer, 3, "UserSetTimer"},
    {NtUserKillTimer, 2, "UserKillTimer"},
    {NtUserGetDisplayInfo, 1, "UserGetDisplayInfo"},
    {NtUserGetCursorPos, 1, "UserGetCursorPos"},
    {NtUserSetForegroundWindow, 1, "UserSetForegroundWindow"},
    {NtUserWindowFromPoint, 3, "UserWindowFromPoint"},
    {NtUserSendInput, 2, "UserSendInput"},
    {NtUserFlush, 1, "UserFlush"},
    {NtUserGetScreenPixel, 3, "UserGetScreenPixel"},
    {NtUserSetCapture, 1, "UserSetCapture"},
    {NtUserSetWindowAttribute, 3, "UserSetWindowAttribute"},
    {NtUserRegisterShell, 1, "UserRegisterShell"},
    {NtUserEnumWindows, 3, "UserEnumWindows"},
    {NtUserSetWorkArea, 1, "UserSetWorkArea"},
    {NtUserSetClipboardText, 2, "UserSetClipboardText"},
    {NtUserGetClipboardText, 3, "UserGetClipboardText"},
    {NtUserSetWallpaper, 1, "UserSetWallpaper"},
    {NtUserGetShellWindow, 1, "UserGetShellWindow"},
};

static_assert(sizeof(W32kServiceTable) / sizeof(W32kServiceTable[0]) == HZ_WIN32K_SERVICE_COUNT,
              "win32k service table and enum HZ_WIN32K_SERVICE differ");

}  // namespace

extern "C" void W32kInitialize(void)
{
    WmInitialize();
    CmpInitialize();
    InpInitialize();
    KeSetWin32ServiceTable(W32kServiceTable, HZ_WIN32K_SERVICE_COUNT);
    PsSetWin32ThreadCallout(WmThreadCleanup);
}

extern "C" void W32kShowDesktop(void)
{
    CmpShowDesktop();
}

extern "C" uint32_t W32kQueryWindowCount(void)
{
    return WmWindowCount;
}
