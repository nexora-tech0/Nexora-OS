/*
 * Nexora OS - user/lib/hzuser/window.cpp
 * hzuser windows: the per-process window table (procedure, user data, style, surface), message
 * dispatch, the default window procedure, and painting with the standard caption.
 *
 * The table is indexed by the slot part of the handle (HWND & 0xFFFF, 1..1024); an entry is valid
 * only while its stored handle matches, so stale handles of destroyed windows are ignored. Each
 * entry is used by the thread that created the window (win32k enforces the same rule for
 * painting and geometry), so the table needs no lock.
 *
 * Windows are created hidden, receive HZ_WM_CREATE and a first HZ_WM_PAINT synchronously, and
 * only then are shown if HZ_WS_VISIBLE was requested: the first composed frame already has the
 * application's content.
 */
#include "hzuser.h"
#include "hzui.h"

static_assert(HZ_WIN32K_SERVICE_COUNT == 31, "update user/lib/hzuser/hzusersys.asm with the service table");

namespace {

constexpr uint32_t kMaxWindows = 1024;

struct WindowEntry {
    HWND Handle;
    HZ_WNDPROC Proc;
    void *Data;
    uint32_t Style;
    bool Active;
    bool Maximized;
    bool Minimized;
    uint8_t CaptionHot, CaptionPressed; /* HZ_CAPTION_BUTTON_* (HZ_WM_CAPTIONSTATE) */
    HZ_SURFACE Surface;
};

WindowEntry g_Windows[kMaxWindows + 1];

WindowEntry *Lookup(HWND Window)
{
    uint32_t Slot = Window & 0xFFFF;
    if (Slot == 0 || Slot > kMaxWindows || g_Windows[Slot].Handle != Window || Window == 0)
        return nullptr;
    return &g_Windows[Slot];
}

void RefreshSurface(WindowEntry *E)
{
    HZ_WINDOW_INFO Info;
    if (HZ_SUCCESS(NtUserGetWindowInfo(E->Handle, &Info)))
        E->Surface = Info.Surface;
}

int64_t CallProc(WindowEntry *E, uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    HZ_WNDPROC Proc = E->Proc ? E->Proc : HzDefWindowProc;
    return Proc(E->Handle, Message, WParam, LParam);
}

constexpr uint32_t kCloseHot = 0xFFC8342C;
constexpr uint32_t kClosePressed = 0xFFA52B25;

int CaptionButtonCount(const WindowEntry *E)
{
    if (!(E->Style & HZ_WS_CAPTION))
        return 0;
    return 1 + ((E->Style & HZ_WS_MAXIMIZEBOX) ? 1 : 0) + ((E->Style & HZ_WS_MINIMIZEBOX) ? 1 : 0);
}

/* The button's rectangle for HZ_CAPTION_BUTTON_* (right to left: close, maximize, minimize). */
HZ_RECT CaptionButtonRect(const WindowEntry *E, int32_t Width, uint32_t Button)
{
    int Slot = 0;
    if (Button == HZ_CAPTION_BUTTON_MAXIMIZE)
        Slot = 1;
    else if (Button == HZ_CAPTION_BUTTON_MINIMIZE)
        Slot = (E->Style & HZ_WS_MAXIMIZEBOX) ? 2 : 1;
    int32_t Right = Width - Slot * HZ_CAPTION_BUTTON_WIDTH;
    return HZ_RECT{Right - HZ_CAPTION_BUTTON_WIDTH, 0, Right, HZ_CAPTION_HEIGHT};
}

void DrawCaptionButton(WindowEntry *E, HZ_CANVAS *C, uint32_t Button, uint32_t Glyph)
{
    HZ_RECT R = CaptionButtonRect(E, C->Width, Button);
    bool Hot = E->CaptionHot == Button, Pressed = E->CaptionPressed == Button && Hot;
    if (Hot || Pressed) {
        uint32_t Fill = Button == HZ_CAPTION_BUTTON_CLOSE ? (Pressed ? kClosePressed : kCloseHot)
                                                          : (Pressed ? hzui::Theme::ControlFillPressed
                                                                     : hzui::Theme::ControlFillHover);
        HzBlendRect(C, R, Fill);
        if (Button == HZ_CAPTION_BUTTON_CLOSE)
            Glyph = 0xFFFFFFFF;
    }
    double Cx = (R.Left + R.Right) / 2.0, Cy = HZ_CAPTION_HEIGHT / 2.0;
    switch (Button) {
    case HZ_CAPTION_BUTTON_CLOSE:
        HzDrawLine(C, Cx - 5, Cy - 5, Cx + 5, Cy + 5, 1.1, Glyph);
        HzDrawLine(C, Cx - 5, Cy + 5, Cx + 5, Cy - 5, 1.1, Glyph);
        break;
    case HZ_CAPTION_BUTTON_MINIMIZE:
        HzDrawLine(C, Cx - 5, Cy + 0.5, Cx + 5, Cy + 0.5, 1.0, Glyph);
        break;
    case HZ_CAPTION_BUTTON_MAXIMIZE:
        if (E->Maximized) { /* restore: two overlapping squares */
            HzDrawLine(C, Cx - 4.5, Cy - 2.5, Cx + 2.5, Cy - 2.5, 1.0, Glyph);
            HzDrawLine(C, Cx + 2.5, Cy - 2.5, Cx + 2.5, Cy + 4.5, 1.0, Glyph);
            HzDrawLine(C, Cx + 2.5, Cy + 4.5, Cx - 4.5, Cy + 4.5, 1.0, Glyph);
            HzDrawLine(C, Cx - 4.5, Cy + 4.5, Cx - 4.5, Cy - 2.5, 1.0, Glyph);
            HzDrawLine(C, Cx - 2.5, Cy - 2.5, Cx - 2.5, Cy - 4.5, 1.0, Glyph);
            HzDrawLine(C, Cx - 2.5, Cy - 4.5, Cx + 4.5, Cy - 4.5, 1.0, Glyph);
            HzDrawLine(C, Cx + 4.5, Cy - 4.5, Cx + 4.5, Cy + 2.5, 1.0, Glyph);
            HzDrawLine(C, Cx + 4.5, Cy + 2.5, Cx + 2.5, Cy + 2.5, 1.0, Glyph);
        } else {
            HzDrawLine(C, Cx - 4.5, Cy - 4.5, Cx + 4.5, Cy - 4.5, 1.0, Glyph);
            HzDrawLine(C, Cx + 4.5, Cy - 4.5, Cx + 4.5, Cy + 4.5, 1.0, Glyph);
            HzDrawLine(C, Cx + 4.5, Cy + 4.5, Cx - 4.5, Cy + 4.5, 1.0, Glyph);
            HzDrawLine(C, Cx - 4.5, Cy + 4.5, Cx - 4.5, Cy - 4.5, 1.0, Glyph);
        }
        break;
    default:
        break;
    }
}

void DrawCaption(WindowEntry *E, HZ_CANVAS *C)
{
    int32_t Width = C->Width;
    HZ_RECT Caption = {0, 0, Width, HZ_CAPTION_HEIGHT};
    if (C->Clip.Top >= Caption.Bottom)
        return;
    HzFillRect(C, Caption, (E->Style & HZ_WS_MICA) ? 0x00000000 : hzui::Theme::SolidBackground);
    uint32_t Text = E->Active ? hzui::Theme::TextPrimary : hzui::Theme::TextSecondary;
    HZ_WINDOW_INFO Info;
    HZ_FONT *Font = HzGetFont(HZ_FONT_CAPTION);
    if (Font && HZ_SUCCESS(NtUserGetWindowInfo(E->Handle, &Info))) {
        HZ_RECT SavedClip = C->Clip;
        C->Clip.Right = HZ_MIN(C->Clip.Right, Width - CaptionButtonCount(E) * HZ_CAPTION_BUTTON_WIDTH - 8);
        int32_t Baseline = (HZ_CAPTION_HEIGHT - HzFontLineHeight(Font)) / 2 + HzFontAscent(Font);
        HzDrawText(C, Font, 12, Baseline, Info.Title, Text);
        C->Clip = SavedClip;
    }

    DrawCaptionButton(E, C, HZ_CAPTION_BUTTON_CLOSE, Text);
    if (E->Style & HZ_WS_MAXIMIZEBOX)
        DrawCaptionButton(E, C, HZ_CAPTION_BUTTON_MAXIMIZE, Text);
    if (E->Style & HZ_WS_MINIMIZEBOX)
        DrawCaptionButton(E, C, HZ_CAPTION_BUTTON_MINIMIZE, Text);
}

void InvalidateCaption(WindowEntry *E)
{
    if (E->Style & HZ_WS_CAPTION) {
        HZ_RECT Caption = {0, 0, HZ_MAX_WINDOW_SIZE, HZ_CAPTION_HEIGHT};
        NtUserInvalidateRect(E->Handle, &Caption);
    }
}

}  // namespace

extern "C" HWND HzCreateWindow(const char *Title, HZ_RECT Rect, uint32_t Style, HZ_WNDPROC Proc, void *UserData)
{
    HZ_CREATE_WINDOW Create;
    memset(&Create, 0, sizeof(Create));
    Create.Rect = Rect;
    Create.Style = Style & ~HZ_WS_VISIBLE;
    if (Title) {
        size_t Length = strlen(Title);
        if (Length >= sizeof(Create.Title))
            Length = sizeof(Create.Title) - 1;
        memcpy(Create.Title, Title, Length);
    }
    HWND Window = 0;
    HZ_SURFACE Surface;
    if (!HZ_SUCCESS(NtUserCreateWindow(&Create, &Window, &Surface)))
        return 0;
    uint32_t Slot = Window & 0xFFFF;
    if (Slot == 0 || Slot > kMaxWindows) {
        NtUserDestroyWindow(Window);
        return 0;
    }
    WindowEntry *E = &g_Windows[Slot];
    E->Handle = Window;
    E->Proc = Proc;
    E->Data = UserData;
    E->Style = Style & ~HZ_WS_VISIBLE;
    E->Active = (Style & HZ_WS_VISIBLE) && !(Style & HZ_WS_NOACTIVATE);
    E->Surface = Surface;

    if (CallProc(E, HZ_WM_CREATE, 0, (uint64_t)UserData) < 0) {
        HzDestroyWindow(Window);
        return 0;
    }
    if (!Lookup(Window)) /* destroyed during WM_CREATE */
        return 0;
    CallProc(E, HZ_WM_PAINT, 0, 0); /* the whole surface is invalid after creation */
    if (Style & HZ_WS_VISIBLE)
        NtUserShowWindow(Window, (Style & HZ_WS_NOACTIVATE) ? HZ_SW_SHOWNOACTIVATE : HZ_SW_SHOW);
    return Window;
}

extern "C" STATUS HzDestroyWindow(HWND Window)
{
    WindowEntry *E = Lookup(Window);
    if (!E)
        return NtUserDestroyWindow(Window); /* foreign or stale: let win32k decide */
    CallProc(E, HZ_WM_DESTROY, 0, 0);
    STATUS Status = NtUserDestroyWindow(Window);
    if (HZ_SUCCESS(Status) || Status == STATUS_INVALID_HANDLE)
        memset(E, 0, sizeof(*E));
    return Status;
}

extern "C" void *HzGetWindowData(HWND Window)
{
    WindowEntry *E = Lookup(Window);
    return E ? E->Data : nullptr;
}

extern "C" bool HzGetMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last)
{
    for (;;) {
        STATUS Status = NtUserGetMessage(Msg, Filter, First, Last);
        if (Status == STATUS_ALERTED)
            continue;
        if (!HZ_SUCCESS(Status)) {
            memset(Msg, 0, sizeof(*Msg));
            Msg->Message = HZ_WM_QUIT;
            Msg->WParam = (uint64_t)(int64_t)Status;
            return false;
        }
        return Msg->Message != HZ_WM_QUIT;
    }
}

extern "C" bool HzPeekMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last, uint32_t Flags)
{
    return NtUserPeekMessage(Msg, Filter, First, Last, Flags) == STATUS_SUCCESS;
}

extern "C" int64_t HzDispatchMessage(const HZ_MSG *Msg)
{
    WindowEntry *E = Lookup(Msg->Window);
    if (!E)
        return 0;
    switch (Msg->Message) {
    case HZ_WM_ACTIVATE:
        E->Active = Msg->WParam != 0;
        InvalidateCaption(E);
        break;
    case HZ_WM_CAPTIONSTATE:
        E->CaptionHot = (uint8_t)(Msg->WParam & 0xFF);
        E->CaptionPressed = (uint8_t)((Msg->WParam >> 8) & 0xFF);
        InvalidateCaption(E);
        break;
    case HZ_WM_SIZE:
        E->Minimized = Msg->WParam == HZ_SIZE_MINIMIZED;
        if (!E->Minimized)
            E->Maximized = Msg->WParam == HZ_SIZE_MAXIMIZED;
        RefreshSurface(E);
        InvalidateCaption(E);
        break;
    default:
        break;
    }
    return CallProc(E, Msg->Message, Msg->WParam, Msg->LParam);
}

extern "C" int64_t HzDefWindowProc(HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    switch (Message) {
    case HZ_WM_CLOSE:
        HzDestroyWindow(Window);
        return 0;
    case HZ_WM_PAINT: {
        HZ_PAINTSTRUCT Paint;
        if (HzBeginPaint(Window, &Paint)) {
            WindowEntry *E = Lookup(Window);
            HZ_RECT Client = HzGetClientRect(Window);
            HzFillRect(&Paint.Canvas, Client,
                       (E && (E->Style & HZ_WS_MICA)) ? 0x00000000 : hzui::Theme::SolidBackground);
            HzEndPaint(Window, &Paint);
        }
        return 0;
    }
    case HZ_WM_SIZE_REQUEST:
        NtUserSetWindowPos(Window, 0, 0, 0, 0, HZ_SWP_PENDING);
        return 0;
    default:
        return 0;
    }
}

extern "C" int64_t HzRunMessageLoop(void)
{
    HZ_MSG Msg;
    while (HzGetMessage(&Msg, 0, 0, 0))
        HzDispatchMessage(&Msg);
    return (int64_t)Msg.WParam;
}

extern "C" bool HzBeginPaint(HWND Window, HZ_PAINTSTRUCT *Paint)
{
    WindowEntry *E = Lookup(Window);
    HZ_PAINT Raw;
    if (!E || !HZ_SUCCESS(NtUserBeginPaint(Window, &Raw)))
        return false;
    E->Surface = Raw.Surface;
    HzCanvasFromSurface(&Paint->Canvas, &Raw.Surface);
    Paint->Update = Raw.Update;
    Paint->Canvas.Clip = HZ_RECT{HZ_MAX(Raw.Update.Left, 0), HZ_MAX(Raw.Update.Top, 0),
                                 HZ_MIN(Raw.Update.Right, Paint->Canvas.Width),
                                 HZ_MIN(Raw.Update.Bottom, Paint->Canvas.Height)};
    if (E->Style & HZ_WS_CAPTION)
        DrawCaption(E, &Paint->Canvas);
    return true;
}

extern "C" void HzEndPaint(HWND Window, const HZ_PAINTSTRUCT *Paint)
{
    NtUserEndPaint(Window, &Paint->Update);
}

extern "C" void HzInvalidate(HWND Window, const HZ_RECT *Rect)
{
    NtUserInvalidateRect(Window, Rect);
}

extern "C" HZ_RECT HzGetWindowRect(HWND Window)
{
    WindowEntry *E = Lookup(Window);
    if (!E)
        return HZ_RECT{0, 0, 0, 0};
    return HZ_RECT{0, 0, (int32_t)E->Surface.Width, (int32_t)E->Surface.Height};
}

extern "C" HZ_RECT HzGetClientRect(HWND Window)
{
    WindowEntry *E = Lookup(Window);
    if (!E)
        return HZ_RECT{0, 0, 0, 0};
    int32_t Top = (E->Style & HZ_WS_CAPTION) ? HZ_CAPTION_HEIGHT : 0;
    return HZ_RECT{0, Top, (int32_t)E->Surface.Width, (int32_t)E->Surface.Height};
}

extern "C" bool HzIsActiveWindow(HWND Window)
{
    WindowEntry *E = Lookup(Window);
    return E && E->Active;
}

extern "C" bool HzIsMaximized(HWND Window)
{
    WindowEntry *E = Lookup(Window);
    return E && E->Maximized;
}

extern "C" bool HzIsMinimized(HWND Window)
{
    WindowEntry *E = Lookup(Window);
    return E && E->Minimized;
}

extern "C" uint32_t HzGetCaptionState(HWND Window)
{
    WindowEntry *E = Lookup(Window);
    return E ? (uint32_t)E->CaptionHot | ((uint32_t)E->CaptionPressed << 8) : 0;
}

extern "C" STATUS HzShowWindow(HWND Window, uint32_t Command)
{
    return NtUserShowWindow(Window, Command);
}

extern "C" STATUS HzSetCursor(HWND Window, uint32_t Shape)
{
    return NtUserSetWindowAttribute(Window, HZ_WA_CURSOR, Shape);
}

extern "C" STATUS HzSetMinimumSize(HWND Window, int32_t Width, int32_t Height)
{
    return NtUserSetWindowAttribute(Window, HZ_WA_MIN_SIZE, HZ_MAKE_SIZE(Width, Height));
}

extern "C" STATUS HzSetCapture(HWND Window)
{
    return NtUserSetCapture(Window);
}

extern "C" void HzReleaseCapture(void)
{
    NtUserSetCapture(0);
}

extern "C" HZ_RECT HzGetScreenRect(HWND Window)
{
    HZ_WINDOW_INFO Info;
    if (!HZ_SUCCESS(NtUserGetWindowInfo(Window, &Info)))
        return HZ_RECT{0, 0, 0, 0};
    return Info.Rect;
}
