/*
 * Nexora OS - user/tests/wintest/wintest.cpp
 * Window manager self-test, run by the kernel at every boot before the desktop is shown
 * (win32k/w32test.cpp). It drives win32k through hzuser: windows, painting and composition
 * (reading composed pixels back with NtUserFlush + NtUserGetScreenPixel), rounded corners and
 * shadows, hit testing, activation, injected keyboard and mouse input (click, double-click, capture,
 * mouse leave, caption drag, border resize, close button, Alt+F4), timers, posted and quit
 * messages, ownership rules and invalid arguments, plus the hzuser heap and fonts. Since M7 also
 * caption button hover, minimize/maximize/restore (buttons, double-click, drag from maximized,
 * Alt+Tab), cursor shapes, minimum sizes and explicit capture; since M8 the shell services
 * (registration, notifications about another process's window, enumeration, shell control of
 * other windows, work area, logo key and Alt+Tab events, clipboard, wallpaper) and transparent
 * and frameless windows; since M9 GetShellWindow, requests to the shell window, the current
 * wallpaper in the display information and owned windows (not app windows). "wintest.exe child"
 * is the other process: it owns one app window. The
 * test leaves one window alive on purpose: the kernel checks that thread exit cleans it up.
 * The exit code is the number of failed checks.
 */
#include "hzuser.h"

namespace {

int g_Checks;
int g_Failures;

void Check(bool Ok, const char *What)
{
    g_Checks++;
    if (!Ok) {
        g_Failures++;
        HzPrint("wintest: FAIL %s", What);
    }
}

void CheckStatus(STATUS Got, STATUS Expected, const char *What)
{
    g_Checks++;
    if (Got != Expected) {
        g_Failures++;
        HzPrint("wintest: FAIL %s (status 0x%08X, expected 0x%08X)", What, (uint32_t)Got, (uint32_t)Expected);
    }
}

/* ---- Message log ------------------------------------------------------------------------------- */

struct LoggedMessage {
    HWND Window;
    uint32_t Message;
    uint64_t WParam, LParam;
};

LoggedMessage g_Log[256];
int g_LogCount;

void Log(HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (g_LogCount < 256)
        g_Log[g_LogCount++] = LoggedMessage{Window, Message, WParam, LParam};
}

void ClearLog()
{
    g_LogCount = 0;
}

const LoggedMessage *Find(HWND Window, uint32_t Message, int Nth = 0)
{
    for (int i = 0; i < g_LogCount; i++)
        if (g_Log[i].Window == Window && g_Log[i].Message == Message && Nth-- == 0)
            return &g_Log[i];
    return nullptr;
}

int CountOf(HWND Window, uint32_t Message)
{
    int Count = 0;
    for (int i = 0; i < g_LogCount; i++)
        Count += g_Log[i].Window == Window && g_Log[i].Message == Message;
    return Count;
}

/* Test windows paint their client area with one color (user data); they record every message and
   do not destroy themselves on WM_CLOSE so the test can observe it. */
struct TestWindow {
    uint32_t Color;
};

int64_t TestProc(HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    Log(Window, Message, WParam, LParam);
    switch (Message) {
    case HZ_WM_PAINT: {
        HZ_PAINTSTRUCT Paint;
        if (HzBeginPaint(Window, &Paint)) {
            TestWindow *T = (TestWindow *)HzGetWindowData(Window);
            HzFillRect(&Paint.Canvas, HzGetClientRect(Window), T ? T->Color : 0xFF000000);
            HzEndPaint(Window, &Paint);
        }
        return 0;
    }
    case HZ_WM_CLOSE:
        return 0;
    default:
        return HzDefWindowProc(Window, Message, WParam, LParam);
    }
}

/* Dispatches everything queued (including generated WM_PAINT); WM_QUIT is logged for window 0. */
void Pump()
{
    HZ_MSG Msg;
    while (HzPeekMessage(&Msg, 0, 0, 0, HZ_PM_REMOVE)) {
        if (Msg.Message == HZ_WM_QUIT)
            Log(0, HZ_WM_QUIT, Msg.WParam, 0);
        else
            HzDispatchMessage(&Msg);
    }
}

/* ---- Screen and input helpers ------------------------------------------------------------------ */

uint32_t Pixel(int32_t X, int32_t Y)
{
    uint32_t Color = 0xDEADBEEF;
    NtUserGetScreenPixel(X, Y, &Color);
    return Color & 0xFFFFFF;
}

void Settle()
{
    Pump();
    NtUserFlush(2000);
}

bool Near(uint32_t A, uint32_t B, int Tolerance)
{
    for (int Shift = 0; Shift <= 16; Shift += 8) {
        int D = (int)((A >> Shift) & 0xFF) - (int)((B >> Shift) & 0xFF);
        if (D < -Tolerance || D > Tolerance)
            return false;
    }
    return true;
}

int Luma(uint32_t C)
{
    return (int)((C >> 16) & 0xFF) + (int)((C >> 8) & 0xFF) + (int)(C & 0xFF);
}

STATUS Mouse(int32_t X, int32_t Y, uint16_t Buttons, int16_t Wheel = 0)
{
    HZ_INPUT In;
    memset(&In, 0, sizeof(In));
    In.Type = HZ_INPUT_MOUSE;
    In.Flags = HZ_INPUT_MOUSE_ABSOLUTE;
    In.Mouse.DeltaX = (int16_t)X;
    In.Mouse.DeltaY = (int16_t)Y;
    In.Mouse.Buttons = Buttons;
    In.Mouse.Wheel = Wheel;
    return NtUserSendInput(&In, 1);
}

void Click(int32_t X, int32_t Y)
{
    Mouse(X, Y, 0);
    Mouse(X, Y, HZ_MOUSE_LEFT);
    Mouse(X, Y, 0);
}

void Drag(int32_t X0, int32_t Y0, int32_t X1, int32_t Y1)
{
    Mouse(X0, Y0, 0);
    Mouse(X0, Y0, HZ_MOUSE_LEFT);
    Mouse((X0 + X1) / 2, (Y0 + Y1) / 2, HZ_MOUSE_LEFT);
    Mouse(X1, Y1, HZ_MOUSE_LEFT);
    Mouse(X1, Y1, 0);
}

void Key(uint8_t VirtualKey, uint8_t Character, uint16_t ScanCode, uint16_t Modifiers, bool Up)
{
    HZ_INPUT In;
    memset(&In, 0, sizeof(In));
    In.Type = HZ_INPUT_KEYBOARD;
    In.Key.ScanCode = ScanCode;
    In.Key.Flags = Up ? HZ_KEY_BREAK : 0;
    In.Key.Modifiers = Modifiers;
    In.Key.VirtualKey = VirtualKey;
    In.Key.Character = Up ? 0 : Character;
    NtUserSendInput(&In, 1);
}

HZ_RECT WindowRect(HWND Window)
{
    HZ_WINDOW_INFO Info;
    memset(&Info, 0, sizeof(Info));
    NtUserGetWindowInfo(Window, &Info);
    return Info.Rect;
}

bool RectIs(HZ_RECT R, int32_t L, int32_t T, int32_t Ri, int32_t B)
{
    return R.Left == L && R.Top == T && R.Right == Ri && R.Bottom == B;
}

/* ---- Tests --------------------------------------------------------------------------------------- */

constexpr uint32_t kRed = 0xFFE04040;
constexpr uint32_t kBlue = 0xFF3050E0;
constexpr uint32_t kCaption = 0x202020;

void TestRuntime()
{
    void *Small = malloc(24), *Medium = malloc(3000), *Large = malloc(100000);
    Check(Small && Medium && Large, "heap: malloc of 24, 3000 and 100000 bytes");
    Check(((uintptr_t)Small & 15) == 0 && ((uintptr_t)Large & 15) == 0, "heap: 16-byte alignment");
    memset(Large, 0x5A, 100000);
    char *Grown = (char *)realloc(Small, 500);
    Check(Grown != nullptr, "heap: realloc grows a block");
    uint8_t *Zeroed = (uint8_t *)calloc(64, 16);
    bool AllZero = Zeroed != nullptr;
    for (int i = 0; AllZero && i < 1024; i++)
        AllZero = Zeroed[i] == 0;
    Check(AllZero, "heap: calloc returns zeroed memory");
    free(Grown);
    free(Medium);
    free(Large);
    free(Zeroed);
    void *Again = malloc(24);
    Check(Again != nullptr, "heap: freed blocks are reused");
    free(Again);

    HZ_FONT *Ui = HzGetFont(HZ_FONT_UI), *Title = HzGetFont(HZ_FONT_TITLE);
    Check(Ui && Title && HzGetFont(HZ_FONT_UI_SEMIBOLD) && HzGetFont(HZ_FONT_CAPTION), "fonts: Inter loads");
    int32_t Width = HzMeasureText(Ui, "Horizon"), Wide = HzMeasureText(Title, "Horizon");
    Check(Width > 30 && Width < 80 && Wide > Width * 3 / 2, "fonts: text measurement scales with size");
    Check(HzFontLineHeight(Ui) >= 14 && HzFontLineHeight(Ui) <= 22 && HzFontAscent(Ui) > 8, "fonts: metrics");
    static uint32_t Pixels[64 * 24];
    HZ_CANVAS Canvas = {Pixels, 64, 24, 64, {0, 0, 64, 24}};
    for (uint32_t &P : Pixels)
        P = 0xFF000000;
    int32_t End = HzDrawText(&Canvas, Ui, 2, 17, "Hz\xC3\xA9", 0xFFFFFFFF); /* UTF-8 e-acute */
    int Lit = 0;
    for (uint32_t P : Pixels)
        Lit += (P & 0xFF) > 128;
    Check(End > 20 && Lit > 30, "fonts: text renders (UTF-8, cached and uncached glyphs)");
}

void TestDisplay(HZ_DISPLAY_INFO *Display)
{
    CheckStatus(NtUserGetDisplayInfo(Display), STATUS_SUCCESS, "display information");
    Check(Display->Width >= 800 && Display->Height >= 600, "display is at least 800x600");
    HzPrint("wintest: display %ux%u, composited %u", Display->Width, Display->Height, Display->Composited);
    /* Park the cursor in the bottom-right corner, away from every probe. */
    CheckStatus(Mouse((int32_t)Display->Width - 1, (int32_t)Display->Height - 1, 0), STATUS_SUCCESS, "absolute mouse");
    HZ_POINT Cursor = {};
    NtUserGetCursorPos(&Cursor);
    Check(Cursor.X == (int32_t)Display->Width - 1 && Cursor.Y == (int32_t)Display->Height - 1, "cursor position");
    Mouse(0x7FFF, 0x7FFF, 0);
    NtUserGetCursorPos(&Cursor);
    Check(Cursor.X == (int32_t)Display->Width - 1 && Cursor.Y == (int32_t)Display->Height - 1,
          "cursor is clamped to the screen");
    Check(NtUserFlush(2000) == STATUS_SUCCESS, "composition flush");
}

struct References {
    uint32_t Inside, Below, Beyond, Left;
};

void TestInvalidArguments()
{
    CheckStatus(NtUserDestroyWindow(0x00050007), STATUS_INVALID_HANDLE, "destroy an unknown window");
    CheckStatus(NtUserPostMessage(0, HZ_WM_USER, 0, 0), STATUS_INVALID_HANDLE, "post to window 0");
    HZ_CREATE_WINDOW Create;
    memset(&Create, 0, sizeof(Create));
    Create.Rect = HZ_RECT{50, 50, 50, 80};
    HWND Window = 0;
    CheckStatus(NtUserCreateWindow(&Create, &Window, nullptr), STATUS_INVALID_PARAMETER, "create an empty window");
    Create.Rect = HZ_RECT{0, 0, HZ_MAX_WINDOW_SIZE + 1, 100};
    CheckStatus(NtUserCreateWindow(&Create, &Window, nullptr), STATUS_INVALID_PARAMETER, "create an oversized window");
    Create.Rect = HZ_RECT{0, 0, 300, 200};
    Create.Style = 0x8000;
    Check(!HZ_SUCCESS(NtUserCreateWindow(&Create, &Window, nullptr)), "create a window with unknown style bits");
    CheckStatus(NtUserCreateWindow((const HZ_CREATE_WINDOW *)0x10, &Window, nullptr), STATUS_ACCESS_VIOLATION,
                "create from an invalid pointer");
    HZ_INPUT Bad;
    memset(&Bad, 0, sizeof(Bad));
    Bad.Type = 9;
    CheckStatus(NtUserSendInput(&Bad, 1), STATUS_INVALID_PARAMETER, "send input of an unknown type");
    CheckStatus(NtUserSendInput(&Bad, 0), STATUS_INVALID_PARAMETER, "send zero inputs");
}

uint32_t OtherThread(void *Parameter)
{
    HWND Window = (HWND)(uintptr_t)Parameter;
    STATUS Destroy = NtUserDestroyWindow(Window);
    STATUS Paint = NtUserSetWindowPos(Window, 0, 0, 0, 0, HZ_SWP_NOSIZE);
    STATUS Post = NtUserPostMessage(Window, HZ_WM_USER + 9, 99, 0);
    STATUS Capture = NtUserSetCapture(Window);
    STATUS Attribute = NtUserSetWindowAttribute(Window, HZ_WA_CURSOR, HZ_CURSOR_IBEAM);
    return Destroy == STATUS_ACCESS_DENIED && Paint == STATUS_ACCESS_DENIED && Post == STATUS_SUCCESS &&
                   Capture == STATUS_ACCESS_DENIED && Attribute == STATUS_ACCESS_DENIED
               ? 0
               : 1;
}

uint32_t CursorShape()
{
    HZ_DISPLAY_INFO Info;
    memset(&Info, 0, sizeof(Info));
    NtUserGetDisplayInfo(&Info);
    return Info.CursorShape;
}

HZ_WINDOW_INFO Info(HWND Window)
{
    HZ_WINDOW_INFO I;
    memset(&I, 0, sizeof(I));
    NtUserGetWindowInfo(Window, &I);
    return I;
}

void TestWindowStates(const HZ_DISPLAY_INFO &Display)
{
    const int32_t ScreenW = (int32_t)Display.Width, ScreenH = (int32_t)Display.Height;
    Check(RectIs(Display.WorkArea, 0, 0, ScreenW, ScreenH), "work area is the screen");
    TestWindow Data = {kRed};
    const uint32_t Style =
        HZ_WS_CAPTION | HZ_WS_RESIZABLE | HZ_WS_MINIMIZEBOX | HZ_WS_MAXIMIZEBOX | HZ_WS_VISIBLE;
    HWND W = HzCreateWindow("States", HZ_RECT{100, 100, 500, 400}, Style, TestProc, &Data);
    Check(W != 0, "create a window with minimize and maximize buttons");
    Settle();

    /* ---- Caption buttons: hover and pressed state ------------------------------------------- */
    ClearLog();
    Mouse(477, 116, 0); /* close: x 454..499 */
    Settle();
    Check(HzGetCaptionState(W) == HZ_CAPTION_BUTTON_CLOSE, "hovering the close button is reported");
    Check(Pixel(460, 104) == 0xC8342C, "hovered close button turns red");
    Mouse(431, 116, 0); /* maximize: 408..453 */
    Settle();
    Check(HzGetCaptionState(W) == HZ_CAPTION_BUTTON_MAXIMIZE && Pixel(460, 104) == kCaption,
          "hover moves to the maximize button");
    Check(Pixel(414, 104) != kCaption && Luma(Pixel(414, 104)) > Luma(kCaption), "hovered button is lighter");
    Mouse(385, 116, HZ_MOUSE_LEFT); /* minimize: 362..407, pressed */
    Settle();
    Check(HzGetCaptionState(W) == (HZ_CAPTION_BUTTON_MINIMIZE | (HZ_CAPTION_BUTTON_MINIMIZE << 8)),
          "pressing a caption button is reported");
    Mouse(300, 250, HZ_MOUSE_LEFT); /* dragged off: no action on release */
    Mouse(300, 250, 0);
    Settle();
    Check(HzGetCaptionState(W) == 0 && !HzIsMinimized(W) && (Info(W).State & HZ_WINDOW_STATE_VISIBLE),
          "caption button released elsewhere does nothing");

    /* ---- Maximize, restore ------------------------------------------------------------------------- */
    ClearLog();
    Click(431, 116);
    Settle();
    const LoggedMessage *Size = Find(W, HZ_WM_SIZE);
    Check(Find(W, HZ_WM_SIZE_REQUEST) && Size && Size->WParam == HZ_SIZE_MAXIMIZED &&
              HZ_POINT_X(Size->LParam) == ScreenW && HZ_POINT_Y(Size->LParam) == ScreenH,
          "maximize button: a size request, then WM_SIZE maximized");
    Check(RectIs(Info(W).Rect, 0, 0, ScreenW, ScreenH) && (Info(W).State & HZ_WINDOW_STATE_MAXIMIZED) &&
              HzIsMaximized(W),
          "maximized window fills the work area");
    Check(Pixel(1, 1) == kCaption && Pixel(ScreenW / 2, ScreenH - 2) == (kRed & 0xFFFFFF),
          "maximized windows have square corners and no border");
    Mouse(1, 300, 0);
    Check(CursorShape() == HZ_CURSOR_ARROW, "maximized windows have no resize border");
    ClearLog();
    Click(300, 10);
    Click(300, 10);
    Settle();
    Size = Find(W, HZ_WM_SIZE);
    Check(Size && Size->WParam == HZ_SIZE_RESTORED && RectIs(Info(W).Rect, 100, 100, 500, 400) && !HzIsMaximized(W),
          "double-clicking the caption restores");
    Click(300, 110);
    Click(300, 110);
    Settle();
    Check(HzIsMaximized(W) && RectIs(Info(W).Rect, 0, 0, ScreenW, ScreenH), "double-clicking the caption maximizes");

    /* Dragging a maximized window restores it under the cursor. */
    ClearLog();
    Drag(300, 10, 300, 200);
    Settle();
    int32_t Left = 300 - 300 * 400 / ScreenW;
    HZ_RECT R = Info(W).Rect;
    Check(!HzIsMaximized(W) && RectIs(R, Left, 190, Left + 400, 490), "dragging a maximized window restores it");
    const LoggedMessage *Moved = Find(W, HZ_WM_MOVE);
    Check(Moved && HZ_POINT_X(Moved->LParam) == Left && HZ_POINT_Y(Moved->LParam) == 190, "and reports the move");
    CheckStatus(NtUserSetWindowPos(W, 100, 100, 400, 300, 0), STATUS_SUCCESS, "move it back");
    Settle();

    /* ---- Minimize, Alt+Tab restores --------------------------------------------------------------- */
    ClearLog();
    Click(385, 116);
    Settle();
    Size = Find(W, HZ_WM_SIZE);
    HWND Hit = 1;
    NtUserWindowFromPoint(300, 250, &Hit);
    Check(Size && Size->WParam == HZ_SIZE_MINIMIZED && HzIsMinimized(W) &&
              (Info(W).State & (HZ_WINDOW_STATE_MINIMIZED | HZ_WINDOW_STATE_VISIBLE)) == HZ_WINDOW_STATE_MINIMIZED &&
              Hit == 0,
          "minimize button hides the window");
    ClearLog();
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, false);
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, true);
    Settle();
    Size = Find(W, HZ_WM_SIZE);
    Check(Size && Size->WParam == HZ_SIZE_RESTORED && !HzIsMinimized(W) && HzIsActiveWindow(W) &&
              Pixel(300, 250) == (kRed & 0xFFFFFF),
          "Alt+Tab restores a minimized window");
    CheckStatus(NtUserShowWindow(W, HZ_SW_MINIMIZE), STATUS_SUCCESS, "minimize by command");
    CheckStatus(NtUserShowWindow(W, HZ_SW_RESTORE), STATUS_SUCCESS, "restore by command");
    Pump();
    Check(!HzIsMinimized(W) && (Info(W).State & HZ_WINDOW_STATE_VISIBLE), "restored by command");
    CheckStatus(NtUserShowWindow(W, 6), STATUS_INVALID_PARAMETER, "unknown show command");

    /* ---- Cursor shapes ------------------------------------------------------------------------------- */
    Mouse(497, 250, 0);
    Check(CursorShape() == HZ_CURSOR_SIZE_WE, "cursor over the right border");
    Mouse(300, 397, 0);
    Check(CursorShape() == HZ_CURSOR_SIZE_NS, "cursor over the bottom border");
    Mouse(498, 398, 0);
    Check(CursorShape() == HZ_CURSOR_SIZE_NWSE, "cursor over the bottom-right corner");
    Mouse(498, 101, 0);
    Check(CursorShape() == HZ_CURSOR_SIZE_NESW, "cursor over the top-right corner");
    Mouse(300, 250, 0);
    Check(CursorShape() == HZ_CURSOR_ARROW, "arrow over the client area");
    CheckStatus(HzSetCursor(W, HZ_CURSOR_IBEAM), STATUS_SUCCESS, "set the client cursor");
    Check(CursorShape() == HZ_CURSOR_IBEAM, "client cursor applies at once");
    Settle();
    Check(Pixel(300, 250) == 0xFFFFFF && Pixel(299, 250) == 0x000000, "I-beam sprite at its hotspot");
    Mouse(300, 116, 0);
    Check(CursorShape() == HZ_CURSOR_ARROW, "arrow over the caption");
    CheckStatus(HzSetCursor(W, HZ_CURSOR_COUNT), STATUS_INVALID_PARAMETER, "unknown cursor shape");

    /* ---- Minimum size ---------------------------------------------------------------------------- */
    CheckStatus(HzSetMinimumSize(W, 300, 250), STATUS_SUCCESS, "set a minimum size");
    Drag(497, 250, 150, 250);
    Settle();
    R = Info(W).Rect;
    Check(R.Right - R.Left == 300 && R.Bottom - R.Top == 300, "interactive resizing stops at the minimum size");
    CheckStatus(HzSetMinimumSize(W, -1, 10), STATUS_INVALID_PARAMETER, "invalid minimum size");

    /* ---- Explicit capture ------------------------------------------------------------------------ */
    TestWindow OtherData = {kBlue};
    HWND Other = HzCreateWindow("Other", HZ_RECT{600, 100, 900, 300}, HZ_WS_CAPTION | HZ_WS_VISIBLE, TestProc, &OtherData);
    Settle();
    CheckStatus(NtUserSetForegroundWindow(W), STATUS_SUCCESS, "activate the first window");
    Pump();
    CheckStatus(HzSetCapture(W), STATUS_SUCCESS, "set the capture");
    ClearLog();
    Click(700, 200);
    Click(ScreenW - 30, ScreenH - 30);
    Pump();
    const LoggedMessage *Down = Find(W, HZ_WM_LBUTTONDOWN);
    Check(Down && HZ_POINT_X(Down->LParam) == 600 && HZ_POINT_Y(Down->LParam) == 100 && !Find(Other, HZ_WM_LBUTTONDOWN),
          "captured clicks go to the capture window in its coordinates");
    Check(HzIsActiveWindow(W) && !HzIsActiveWindow(Other), "captured clicks do not activate");
    Check(CountOf(W, HZ_WM_LBUTTONDOWN) == 2 && CountOf(W, HZ_WM_LBUTTONUP) == 2, "the capture outlives releases");
    HzReleaseCapture();
    Pump();
    Check(Find(W, HZ_WM_CAPTURECHANGED) != nullptr, "releasing reports the capture change");
    ClearLog();
    Click(700, 200);
    Pump();
    Check(Find(Other, HZ_WM_LBUTTONDOWN) && HzIsActiveWindow(Other), "after the release clicks go where they land");
    CheckStatus(HzSetCapture(W), STATUS_SUCCESS, "capture again");
    ClearLog();
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, false);
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, true);
    Pump();
    Check(Find(W, HZ_WM_CAPTURECHANGED) != nullptr, "an activation change cancels the capture");
    CheckStatus(HzSetCapture(0x00230011), STATUS_INVALID_HANDLE, "capture an unknown window");
    HzShowWindow(Other, HZ_SW_HIDE);
    CheckStatus(HzSetCapture(Other), STATUS_INVALID_PARAMETER, "capture a hidden window");
    HzReleaseCapture(); /* releasing nothing is harmless */

    HzDestroyWindow(Other);
    HzDestroyWindow(W);
    Pump();
}

void TestWindows(const HZ_DISPLAY_INFO &Display)
{
    int32_t Right = (int32_t)Display.Width - 20, Bottom = (int32_t)Display.Height - 20;
    References Ref = {Pixel(300, 250), Pixel(300, 405), Pixel(650, 470), Pixel(110, 300)};

    /* ---- Red window: content, caption, rounded corner, shadow -------------------------------- */
    TestWindow RedData = {kRed}, BlueData = {kBlue};
    HWND Red = HzCreateWindow("Red", HZ_RECT{100, 100, 500, 400}, HZ_WS_CAPTION | HZ_WS_RESIZABLE | HZ_WS_VISIBLE,
                              TestProc, &RedData);
    Check(Red != 0, "create the red window");
    Check(Find(Red, HZ_WM_CREATE) && Find(Red, HZ_WM_PAINT), "WM_CREATE and the first WM_PAINT are synchronous");
    Settle();
    Check(Pixel(300, 250) == (kRed & 0xFFFFFF), "client area is composed");
    Check(Pixel(300, 110) == kCaption, "caption is drawn by hzuser");
    Check(Near(Pixel(104, 104), kCaption, 8), "inside the rounded corner");
    Check(Pixel(100, 100) != kCaption, "outside the rounded corner");
    uint32_t Shadow = Pixel(300, 405);
    Check(Luma(Shadow) < Luma(Ref.Below), "shadow below the window");
    HWND Hit = 0;
    NtUserWindowFromPoint(300, 250, &Hit);
    Check(Hit == Red, "WindowFromPoint finds the window");
    NtUserWindowFromPoint(50, 50, &Hit);
    Check(Hit == 0, "WindowFromPoint on the desktop");
    HZ_WINDOW_INFO Info;
    CheckStatus(NtUserGetWindowInfo(Red, &Info), STATUS_SUCCESS, "window information");
    Check(strcmp(Info.Title, "Red") == 0 && (Info.State & HZ_WINDOW_STATE_ACTIVE) && Info.Surface.Width == 400 &&
              Info.Surface.Height == 300 && Info.Surface.Pixels != 0,
          "title, active state and surface");
    CheckStatus(NtUserGetWindowInfo(Red, (HZ_WINDOW_INFO *)0x10), STATUS_ACCESS_VIOLATION,
                "window information to an invalid pointer");

    /* ---- Blue window above it; activation ---------------------------------------------------- */
    HWND Blue = HzCreateWindow("Blue", HZ_RECT{300, 200, 700, 500}, HZ_WS_CAPTION | HZ_WS_VISIBLE, TestProc, &BlueData);
    Check(Blue != 0, "create the blue window");
    ClearLog();
    Settle();
    Check(Pixel(400, 300) == (kBlue & 0xFFFFFF) && Pixel(200, 250) == (kRed & 0xFFFFFF), "stacking order");
    const LoggedMessage *Deactivated = Find(Red, HZ_WM_ACTIVATE), *Activated = Find(Blue, HZ_WM_ACTIVATE);
    Check(Deactivated && Deactivated->WParam == 0 && Activated && Activated->WParam == 1,
          "creating a visible window moves activation");
    Check(HzIsActiveWindow(Blue) && !HzIsActiveWindow(Red), "hzuser tracks activation");

    /* ---- Click activation and client mouse messages ------------------------------------------- */
    ClearLog();
    Click(150, 300);
    Settle();
    const LoggedMessage *Down = Find(Red, HZ_WM_LBUTTONDOWN), *Up = Find(Red, HZ_WM_LBUTTONUP);
    Check(Down && HZ_POINT_X(Down->LParam) == 50 && HZ_POINT_Y(Down->LParam) == 200 && (Down->WParam & HZ_MOUSE_LEFT),
          "button-down message with window coordinates");
    Check(Up != nullptr, "button-up message");
    Check(HzIsActiveWindow(Red) && !HzIsActiveWindow(Blue), "clicking activates");
    Check(Pixel(400, 300) == (kRed & 0xFFFFFF), "the activated window comes to the top");
    Check(Find(Red, HZ_WM_MOUSEMOVE) != nullptr, "mouse move message");

    /* ---- Keyboard ------------------------------------------------------------------------------ */
    ClearLog();
    Key('H', 'h', 0x23, 0, false);
    Key('H', 0, 0x23, 0, true);
    Pump();
    const LoggedMessage *KeyDown = Find(Red, HZ_WM_KEYDOWN), *Char = Find(Red, HZ_WM_CHAR);
    Check(KeyDown && KeyDown->WParam == 'H' && HZ_KEY_DATA_SCAN(KeyDown->LParam) == 0x23, "key-down to the active window");
    Check(Char && Char->WParam == 'h', "character message");
    Check(Find(Red, HZ_WM_KEYUP) && !Find(Blue, HZ_WM_KEYDOWN), "key-up, and nothing for inactive windows");

    /* ---- Wheel ---------------------------------------------------------------------------------- */
    ClearLog();
    Mouse(150, 300, 0, -2);
    Pump();
    const LoggedMessage *Wheel = Find(Red, HZ_WM_MOUSEWHEEL);
    Check(Wheel && (int64_t)Wheel->WParam == -240, "wheel message (120 per notch)");

    /* ---- Double-click ----------------------------------------------------------------------------- */
    ClearLog();
    Click(200, 300);
    Click(201, 301);
    Pump();
    Check(Find(Red, HZ_WM_LBUTTONDOWN) && Find(Red, HZ_WM_LBUTTONDBLCLK), "double-click");

    /* ---- Capture and mouse leave ------------------------------------------------------------- */
    ClearLog();
    Mouse(200, 300, 0);
    Mouse(200, 300, HZ_MOUSE_LEFT);
    Mouse(Right, Bottom, HZ_MOUSE_LEFT);
    Mouse(Right, Bottom, 0);
    Pump();
    const LoggedMessage *Captured = Find(Red, HZ_WM_MOUSEMOVE, CountOf(Red, HZ_WM_MOUSEMOVE) - 1);
    Check(Captured && HZ_POINT_X(Captured->LParam) == Right - 100 && HZ_POINT_Y(Captured->LParam) == Bottom - 100,
          "capture: moves outside the window still arrive");
    Check(Find(Red, HZ_WM_LBUTTONUP) != nullptr, "capture: the release arrives");
    ClearLog();
    Mouse(150, 300, 0);
    Mouse(50, 50, 0);
    Pump();
    Check(Find(Red, HZ_WM_MOUSELEAVE) != nullptr, "mouse leave");

    /* ---- Caption drag ------------------------------------------------------------------------------ */
    ClearLog();
    Drag(250, 115, 290, 155);
    Settle();
    const LoggedMessage *Moved = Find(Red, HZ_WM_MOVE);
    Check(Moved && HZ_POINT_X(Moved->LParam) == 140 && HZ_POINT_Y(Moved->LParam) == 140, "caption drag: WM_MOVE");
    Check(RectIs(WindowRect(Red), 140, 140, 540, 440), "caption drag moves the window");
    Check(Pixel(520, 420) == (kRed & 0xFFFFFF) && Pixel(110, 300) == Ref.Left, "moved window is recomposed");

    /* ---- Border resize ------------------------------------------------------------------------------ */
    ClearLog();
    Drag(537, 300, 597, 300);
    Settle();
    const LoggedMessage *Size = Find(Red, HZ_WM_SIZE);
    Check(CountOf(Red, HZ_WM_SIZE_REQUEST) == 1, "resize drag: one WM_SIZE_REQUEST");
    Check(Size && HZ_POINT_X(Size->LParam) == 460 && HZ_POINT_Y(Size->LParam) == 300, "resize drag: WM_SIZE");
    Check(RectIs(WindowRect(Red), 140, 140, 600, 440) && HzGetWindowRect(Red).Right == 460, "resized window and surface");
    Check(Pixel(580, 300) == (kRed & 0xFFFFFF), "resized window is repainted and composed");

    /* ---- Close button and Alt+F4 ---------------------------------------------------------------------- */
    ClearLog();
    Click(577, 156);
    Pump();
    Check(CountOf(Red, HZ_WM_CLOSE) == 1, "close button posts WM_CLOSE");
    ClearLog();
    Drag(577, 156, 400, 156); /* released away from the button: no close */
    Pump();
    Check(CountOf(Red, HZ_WM_CLOSE) == 0, "close button released elsewhere does nothing");
    Key(HZ_VK_F1 + 3, 0, 0x3E, HZ_MOD_ALT, false);
    Key(HZ_VK_F1 + 3, 0, 0x3E, HZ_MOD_ALT, true);
    Pump();
    Check(CountOf(Red, HZ_WM_CLOSE) == 1, "Alt+F4 posts WM_CLOSE to the active window");
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, false);
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, true);
    Pump();
    Check(HzIsActiveWindow(Blue), "Alt+Tab switches windows");

    /* ---- Painting protocol ------------------------------------------------------------------------- */
    ClearLog();
    HZ_RECT Part = {10, 40, 30, 60};
    HzInvalidate(Blue, &Part);
    HZ_MSG Msg;
    Check(HzPeekMessage(&Msg, Blue, HZ_WM_PAINT, HZ_WM_PAINT, HZ_PM_REMOVE), "invalidation generates WM_PAINT");
    Check(HzPeekMessage(&Msg, Blue, HZ_WM_PAINT, HZ_WM_PAINT, HZ_PM_REMOVE), "WM_PAINT repeats until painted");
    HzDispatchMessage(&Msg);
    Check(!HzPeekMessage(&Msg, Blue, HZ_WM_PAINT, HZ_WM_PAINT, HZ_PM_REMOVE), "BeginPaint validates");

    /* ---- Posted messages, other threads ------------------------------------------------------------- */
    ClearLog();
    CheckStatus(NtUserPostMessage(Blue, HZ_WM_USER + 5, 11, 22), STATUS_SUCCESS, "post a message");
    HANDLE Thread;
    CheckStatus(HzCreateThread(OtherThread, (void *)(uintptr_t)Blue, &Thread, nullptr), STATUS_SUCCESS, "start a thread");
    NtWaitForSingleObject(Thread, 0, nullptr);
    uint32_t ThreadResult = 0xFFFFFFFF;
    HZ_THREAD_BASIC_INFORMATION ThreadInfo;
    if (HZ_SUCCESS(NtQueryInformationThread(Thread, HZ_THREAD_BASIC_INFORMATION_CLASS, &ThreadInfo, sizeof(ThreadInfo))))
        ThreadResult = (uint32_t)ThreadInfo.ExitStatus;
    NtClose(Thread);
    Check(ThreadResult == 0, "other threads may post but not destroy, move, capture or set attributes");
    Pump();
    const LoggedMessage *User = Find(Blue, HZ_WM_USER + 5);
    Check(User && User->WParam == 11 && User->LParam == 22, "posted message arrives with its parameters");
    Check(Find(Blue, HZ_WM_USER + 9) != nullptr, "message posted by another thread arrives");

    /* ---- Timers ------------------------------------------------------------------------------------ */
    ClearLog();
    CheckStatus(NtUserSetTimer(Blue, 7, 20), STATUS_SUCCESS, "set a timer");
    uint64_t Start = 0, Now = 0;
    NtQueryUptime(&Start);
    int Ticks = 0;
    while (Ticks < 3 && HzGetMessage(&Msg, 0, 0, 0)) {
        if (Msg.Message == HZ_WM_TIMER && Msg.Window == Blue && Msg.WParam == 7)
            Ticks++;
        else
            HzDispatchMessage(&Msg);
    }
    NtQueryUptime(&Now);
    Check(Ticks == 3 && Now - Start >= 40000 && Now - Start < 1000000, "timer ticks at its period");
    CheckStatus(NtUserKillTimer(Blue, 7), STATUS_SUCCESS, "kill the timer");
    CheckStatus(NtUserKillTimer(Blue, 7), STATUS_NOT_FOUND, "kill it again");
    NtDelayExecution(60000);
    Check(!HzPeekMessage(&Msg, Blue, HZ_WM_TIMER, HZ_WM_TIMER, HZ_PM_REMOVE), "no ticks after KillTimer");

    /* ---- Hide and show ------------------------------------------------------------------------------- */
    NtUserShowWindow(Blue, HZ_SW_HIDE);
    Settle();
    Check(Pixel(650, 470) == Ref.Beyond, "hidden window is not composed");
    NtUserWindowFromPoint(650, 470, &Hit);
    Check(Hit == 0, "hidden window is not hit");
    NtUserShowWindow(Blue, HZ_SW_SHOW);
    Settle();
    Check(Pixel(650, 470) == (kBlue & 0xFFFFFF), "shown window is composed again");
    CheckStatus(NtUserShowWindow(Blue, 7), STATUS_INVALID_PARAMETER, "invalid show command");

    /* ---- Destroy: the wallpaper comes back ---------------------------------------------------------- */
    ClearLog();
    CheckStatus(HzDestroyWindow(Red), STATUS_SUCCESS, "destroy the red window");
    CheckStatus(HzDestroyWindow(Blue), STATUS_SUCCESS, "destroy the blue window");
    Check(Find(Red, HZ_WM_DESTROY) && Find(Blue, HZ_WM_DESTROY), "WM_DESTROY");
    CheckStatus(HzDestroyWindow(Red), STATUS_INVALID_HANDLE, "destroy a destroyed window");
    CheckStatus(NtUserPostMessage(Blue, HZ_WM_USER, 0, 0), STATUS_INVALID_HANDLE, "post to a destroyed window");
    Settle();
    Check(Pixel(300, 250) == Ref.Inside && Pixel(300, 405) == Ref.Below && Pixel(650, 470) == Ref.Beyond,
          "the desktop is restored");

    /* ---- Mica ------------------------------------------------------------------------------------------ */
    TestWindow Clear = {0x00000000};
    HWND Mica = HzCreateWindow("Mica", HZ_RECT{100, 100, 500, 400}, HZ_WS_CAPTION | HZ_WS_MICA | HZ_WS_VISIBLE, TestProc,
                               &Clear);
    Settle();
    uint32_t M = Pixel(300, 250);
    Check(M != Ref.Inside && Near(M, 0x1C1C24, 72), "Mica shows the tinted, blurred wallpaper");
    Check(Near(Pixel(300, 110), M, 40) && Pixel(300, 110) != kCaption, "Mica caption is transparent");
    HzDestroyWindow(Mica);

    /* ---- Quit ------------------------------------------------------------------------------------------ */
    CheckStatus(NtUserPostQuitMessage(42), STATUS_SUCCESS, "post quit");
    Check(!HzGetMessage(&Msg, 0, 0, 0) && Msg.Message == HZ_WM_QUIT && Msg.WParam == 42, "WM_QUIT ends the loop");
    Check(!HzPeekMessage(&Msg, 0, 0, 0, HZ_PM_REMOVE), "WM_QUIT is delivered once");
}

/* ---- Shell services (M8) ------------------------------------------------------------------------ */

bool WaitForLogged(HWND Window, uint32_t Message, uint64_t WParam, uint32_t TimeoutMs)
{
    uint64_t Start = 0, Now = 0;
    NtQueryUptime(&Start);
    for (;;) {
        Pump();
        for (int i = 0; i < g_LogCount; i++)
            if (g_Log[i].Window == Window && g_Log[i].Message == Message && g_Log[i].WParam == WParam)
                return true;
        NtQueryUptime(&Now);
        if (Now - Start > (uint64_t)TimeoutMs * 1000)
            return false;
        NtDelayExecution(5000);
    }
}

const LoggedMessage *FindShell(HWND Shell, uint64_t Event)
{
    for (int i = 0; i < g_LogCount; i++)
        if (g_Log[i].Window == Shell && g_Log[i].Message == HZ_WM_SHELLNOTIFY && g_Log[i].WParam == Event)
            return &g_Log[i];
    return nullptr;
}

bool WaitForRect(HWND Window, HZ_RECT Expected, uint32_t TimeoutMs)
{
    for (uint32_t Waited = 0; Waited <= TimeoutMs; Waited += 10) {
        Pump();
        if (RectIs(Info(Window).Rect, Expected.Left, Expected.Top, Expected.Right, Expected.Bottom))
            return true;
        NtDelayExecution(10000);
    }
    return false;
}

/* The mouse lock (HZ_WA_MOUSE_LOCK, for games): only the active window may take it; the cursor is
   hidden and parked; motion arrives as deltas and buttons go to the window; activating another
   window ends it with HZ_WM_MOUSEUNLOCKED. */
void TestMouseLock()
{
    TestWindow AData = {kRed}, BData = {kBlue};
    HWND A = HzCreateWindow("Lock A", HZ_RECT{100, 100, 400, 300}, HZ_WS_CAPTION | HZ_WS_VISIBLE, TestProc, &AData);
    HWND B = HzCreateWindow("Lock B", HZ_RECT{450, 100, 750, 300}, HZ_WS_CAPTION | HZ_WS_VISIBLE, TestProc, &BData);
    NtUserSetForegroundWindow(B);
    Settle();
    CheckStatus(NtUserSetWindowAttribute(A, HZ_WA_MOUSE_LOCK, 1), STATUS_ACCESS_DENIED,
                "only the active window may lock the mouse");
    CheckStatus(NtUserSetWindowAttribute(B, HZ_WA_MOUSE_LOCK, 1), STATUS_SUCCESS, "the active window locks the mouse");
    HZ_DISPLAY_INFO Now;
    memset(&Now, 0, sizeof(Now));
    NtUserGetDisplayInfo(&Now);
    Check(Now.CursorShape == HZ_CURSOR_HIDDEN, "the locked cursor is hidden");
    HZ_POINT P = {}, Q = {};
    NtUserGetCursorPos(&P);
    HZ_RECT R = WindowRect(B);
    Check(P.X == (R.Left + R.Right) / 2 && P.Y == (R.Top + R.Bottom) / 2, "and parked at the window's centre");
    ClearLog();
    Mouse(P.X + 30, P.Y - 20, 0);
    Mouse(P.X + 30, P.Y - 20, HZ_MOUSE_LEFT);
    Mouse(P.X + 30, P.Y - 20, 0);
    Pump();
    const LoggedMessage *Delta = Find(B, HZ_WM_MOUSEDELTA);
    Check(Delta && HZ_POINT_X(Delta->LParam) == 30 && HZ_POINT_Y(Delta->LParam) == -20, "motion arrives as a delta");
    NtUserGetCursorPos(&Q);
    Check(Q.X == P.X && Q.Y == P.Y, "the locked cursor does not move");
    Check(Find(B, HZ_WM_LBUTTONDOWN) && Find(B, HZ_WM_LBUTTONUP) && !Find(B, HZ_WM_MOUSEMOVE) &&
              !Find(A, HZ_WM_LBUTTONDOWN),
          "buttons go to the locked window");
    ClearLog();
    NtUserSetForegroundWindow(A);
    Pump();
    Check(Find(B, HZ_WM_MOUSEUNLOCKED) != nullptr, "activating another window ends the lock");
    NtUserGetDisplayInfo(&Now);
    Check(Now.CursorShape != HZ_CURSOR_HIDDEN, "and shows the cursor again");
    CheckStatus(NtUserSetWindowAttribute(A, HZ_WA_MOUSE_LOCK, 1), STATUS_SUCCESS, "lock again");
    ClearLog();
    CheckStatus(NtUserSetWindowAttribute(A, HZ_WA_MOUSE_LOCK, 0), STATUS_SUCCESS, "and unlock");
    Pump();
    NtUserGetDisplayInfo(&Now);
    Check(!Find(A, HZ_WM_MOUSEUNLOCKED) && Now.CursorShape != HZ_CURSOR_HIDDEN, "unlocking by request sends no message");
    HzDestroyWindow(A);
    HzDestroyWindow(B);
    Settle();
}

void TestShell(const HZ_DISPLAY_INFO &Display)
{
    const int32_t ScreenW = (int32_t)Display.Width, ScreenH = (int32_t)Display.Height;
    HZ_RECT Strip = {0, 0, ScreenW, ScreenH - 48};
    CheckStatus(NtUserSetWorkArea(&Strip), STATUS_ACCESS_DENIED, "only the shell sets the work area");
    CheckStatus(NtUserSetWallpaper(1), STATUS_ACCESS_DENIED, "only the shell sets the wallpaper");
    HWND Found = (HWND)1;
    CheckStatus(NtUserGetShellWindow(&Found), STATUS_SUCCESS, "ask for the shell window");
    Check(Found == 0, "there is no shell yet");
    CheckStatus(NtUserGetShellWindow(nullptr), STATUS_ACCESS_VIOLATION, "GetShellWindow with a bad pointer");

    TestWindow ShellData = {kBlue};
    HWND Shell = HzCreateWindow("Shell", HZ_RECT{0, 0, 200, 100}, HZ_WS_POPUP | HZ_WS_NOACTIVATE, TestProc, &ShellData);
    CheckStatus(NtUserRegisterShell(Shell), STATUS_SUCCESS, "register as the shell");
    NtUserGetShellWindow(&Found);
    Check(Found == Shell, "GetShellWindow finds the registered shell window");
    ClearLog();
    CheckStatus(NtUserPostMessage(Found, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_WALLPAPER, 2), STATUS_SUCCESS,
                "post a request to the shell window");
    Check(WaitForLogged(Shell, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_WALLPAPER, 2000), "the shell window receives it");
    ClearLog();

    HANDLE Process = 0;
    CheckStatus(NtCreateProcess("C:\\Nexora\\System\\wintest.exe", "wintest.exe child", &Process, nullptr),
                STATUS_SUCCESS, "start another process with a window");
    Check(WaitForLogged(Shell, HZ_WM_SHELLNOTIFY, HZ_SHELL_WINDOW_CREATED, 5000), "the shell hears about its window");
    const LoggedMessage *Created = FindShell(Shell, HZ_SHELL_WINDOW_CREATED);
    HWND Child = Created ? (HWND)Created->LParam : 0;
    Check(WaitForLogged(Shell, HZ_WM_SHELLNOTIFY, HZ_SHELL_WINDOW_ACTIVATED, 3000) &&
              FindShell(Shell, HZ_SHELL_WINDOW_ACTIVATED)->LParam == Child && FindShell(Shell, HZ_SHELL_WINDOW_CHANGED),
          "it is shown and activated");
    HWND List[8] = {};
    uint32_t Count = 0;
    CheckStatus(NtUserEnumWindows(List, 8, &Count), STATUS_SUCCESS, "enumerate app windows");
    Check(Count == 1 && List[0] == Child && strcmp(Info(Child).Title, "Child") == 0,
          "only the other process's captioned window is an app window");

    ClearLog();
    CheckStatus(NtUserShowWindow(Child, HZ_SW_MINIMIZE), STATUS_SUCCESS, "the shell minimizes another process's window");
    Pump();
    Check((Info(Child).State & HZ_WINDOW_STATE_MINIMIZED) && FindShell(Shell, HZ_SHELL_WINDOW_CHANGED),
          "minimized, and the shell is told");
    CheckStatus(NtUserEnumWindows(List, 8, &Count), STATUS_SUCCESS, "enumerate again");
    Check(Count == 1, "minimized windows stay in the list");
    CheckStatus(NtUserShowWindow(Child, HZ_SW_RESTORE), STATUS_SUCCESS, "and restores it");
    Check(Info(Child).State & HZ_WINDOW_STATE_VISIBLE, "restored");

    CheckStatus(NtUserSetWorkArea(&Strip), STATUS_SUCCESS, "the shell sets the work area");
    HZ_DISPLAY_INFO Now;
    NtUserGetDisplayInfo(&Now);
    Check(RectIs(Now.WorkArea, 0, 0, ScreenW, ScreenH - 48), "display information reports it");
    CheckStatus(NtUserShowWindow(Child, HZ_SW_MAXIMIZE), STATUS_SUCCESS, "maximize the other window");
    Check(WaitForRect(Child, Strip, 3000), "maximized windows fill the work area");
    HZ_RECT Smaller = {0, 0, ScreenW, ScreenH - 100};
    NtUserSetWorkArea(&Smaller);
    Check(WaitForRect(Child, Smaller, 3000), "maximized windows follow work area changes");
    HZ_RECT Outside = {0, 0, ScreenW + 10, ScreenH};
    CheckStatus(NtUserSetWorkArea(&Outside), STATUS_INVALID_PARAMETER, "a work area beyond the screen");

    ClearLog();
    Key(HZ_VK_LWIN, 0, 0x5B, 0, false);
    Key(HZ_VK_LWIN, 0, 0x5B, 0, true);
    Pump();
    Check(FindShell(Shell, HZ_SHELL_START_KEY) != nullptr, "the logo key alone goes to the shell");
    ClearLog();
    Key(HZ_VK_LWIN, 0, 0x5B, 0, false);
    Key('E', 'e', 0x12, 0, false);
    Key('E', 0, 0x12, 0, true);
    Key(HZ_VK_LWIN, 0, 0x5B, 0, true);
    Pump();
    Check(FindShell(Shell, HZ_SHELL_START_KEY) == nullptr, "not when combined with another key");

    ClearLog();
    Key(HZ_VK_MENU, 0, 0x38, HZ_MOD_ALT, false);
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, false);
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, true);
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT | HZ_MOD_SHIFT, false);
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT | HZ_MOD_SHIFT, true);
    Key(HZ_VK_MENU, 0, 0x38, 0, true);
    Pump();
    int Next = 0;
    for (int i = 0; i < g_LogCount; i++)
        Next += g_Log[i].Message == HZ_WM_SHELLNOTIFY && g_Log[i].WParam == HZ_SHELL_SWITCH_NEXT;
    const LoggedMessage *End = FindShell(Shell, HZ_SHELL_SWITCH_END);
    Check(Next == 2 && End && End->LParam == 1, "Alt+Tab goes to the shell until Alt is released");
    Check(HzIsActiveWindow(Shell) == false && (Info(Child).State & HZ_WINDOW_STATE_ACTIVE), "the kernel does not switch itself");
    ClearLog();
    Key(HZ_VK_TAB, 0, 0x0F, HZ_MOD_ALT, false);
    Key(HZ_VK_ESCAPE, 0x1B, 0x01, HZ_MOD_ALT, false);
    Key(HZ_VK_MENU, 0, 0x38, 0, true);
    Pump();
    End = FindShell(Shell, HZ_SHELL_SWITCH_END);
    Check(End && End->LParam == 0, "Escape cancels the switch");

    ClearLog();
    NtUserPostMessage(Child, HZ_WM_USER + 1, 0, 0); /* the child renames its window */
    Check(WaitForLogged(Shell, HZ_WM_SHELLNOTIFY, HZ_SHELL_WINDOW_CHANGED, 3000) &&
              strcmp(Info(Child).Title, "Child renamed") == 0,
          "title changes are reported");
    ClearLog();
    NtUserPostMessage(Child, HZ_WM_CLOSE, 0, 0);
    Check(WaitForLogged(Shell, HZ_WM_SHELLNOTIFY, HZ_SHELL_WINDOW_DESTROYED, 3000) &&
              FindShell(Shell, HZ_SHELL_WINDOW_DESTROYED)->LParam == Child,
          "destruction is reported");
    uint64_t Timeout = 5000000;
    Check(NtWaitForSingleObject(Process, 0, &Timeout) == STATUS_SUCCESS, "the other process ends");
    NtClose(Process);
    CheckStatus(NtUserEnumWindows(List, 8, &Count), STATUS_SUCCESS, "enumerate after it ended");
    Check(Count == 0, "no app windows left");

    /* Clipboard */
    CheckStatus(NtUserSetClipboardText("hello", 5), STATUS_SUCCESS, "set clipboard text");
    char Text[16];
    uint32_t Length = 0;
    CheckStatus(NtUserGetClipboardText(Text, sizeof(Text), &Length), STATUS_SUCCESS, "get clipboard text");
    Check(Length == 5 && strcmp(Text, "hello") == 0, "clipboard round trip");
    CheckStatus(NtUserGetClipboardText(Text, 5, &Length), STATUS_BUFFER_TOO_SMALL, "clipboard buffer too small");
    Check(Length == 5, "the length is reported anyway");
    CheckStatus(NtUserSetClipboardText(nullptr, 0), STATUS_SUCCESS, "empty the clipboard");
    NtUserGetClipboardText(Text, sizeof(Text), &Length);
    Check(Length == 0 && Text[0] == '\0', "empty clipboard");
    CheckStatus(NtUserSetClipboardText(Text, HZ_MAX_CLIPBOARD + 1), STATUS_INVALID_PARAMETER, "oversized clipboard text");

    /* Wallpaper (after the closed child's area has been recomposed) */
    NtUserFlush(2000);
    uint32_t Before = Pixel(5, 5);
    CheckStatus(NtUserSetWallpaper(1), STATUS_SUCCESS, "change the wallpaper");
    NtUserFlush(2000);
    Check(Pixel(5, 5) != Before, "the desktop shows it");
    NtUserGetDisplayInfo(&Now);
    Check(Now.Wallpaper == 1, "display information reports the wallpaper");
    CheckStatus(NtUserSetWallpaper(HZ_WALLPAPER_AUTO), STATUS_SUCCESS, "restore the boot wallpaper");
    NtUserFlush(2000);
    Check(Pixel(5, 5) == Before, "the boot wallpaper is back");
    NtUserGetDisplayInfo(&Now);
    Check(Now.Wallpaper == HZ_WALLPAPER_AUTO, "and reports it");
    CheckStatus(NtUserSetWallpaper(HZ_WALLPAPER_AUTO + 1), STATUS_INVALID_PARAMETER, "unknown wallpaper");

    /* Unregistering gives the screen back. */
    CheckStatus(NtUserRegisterShell(0), STATUS_SUCCESS, "unregister the shell");
    NtUserGetDisplayInfo(&Now);
    Check(RectIs(Now.WorkArea, 0, 0, ScreenW, ScreenH), "the work area is the screen again");
    CheckStatus(NtUserSetWallpaper(0), STATUS_ACCESS_DENIED, "no longer the shell");
    NtUserGetShellWindow(&Found);
    Check(Found == 0, "GetShellWindow after unregistering");
    /* Owned windows (dialogs) are not app windows. Without a shell this process's windows count. */
    TestWindow MainData = {kBlue}, DialogData = {kRed};
    HWND Main = HzCreateWindow("Main", HZ_RECT{100, 100, 400, 300}, HZ_WS_CAPTION | HZ_WS_VISIBLE, TestProc, &MainData);
    HWND Owned = HzCreateWindow("Owned", HZ_RECT{150, 150, 350, 250}, HZ_WS_CAPTION | HZ_WS_OWNED | HZ_WS_VISIBLE, TestProc,
                                &DialogData);
    Pump();
    CheckStatus(NtUserEnumWindows(List, 8, &Count), STATUS_SUCCESS, "enumerate with an owned window");
    Check(Count == 1 && List[0] == Main, "an owned window is not an app window");
    Check(Owned && (Info(Owned).State & HZ_WINDOW_STATE_ACTIVE), "an owned window is activated like others");
    HZ_CREATE_WINDOW Bad = {};
    Bad.Rect = HZ_RECT{0, 0, 100, 100};
    Bad.Style = HZ_WS_VALID + 1;
    HWND BadWindow = 0;
    HZ_SURFACE BadSurface;
    CheckStatus(NtUserCreateWindow(&Bad, &BadWindow, &BadSurface), STATUS_INVALID_PARAMETER, "unknown style bits");
    HzDestroyWindow(Owned);
    HzDestroyWindow(Main);
    Pump();
    CheckStatus(NtUserRegisterShell(Shell), STATUS_SUCCESS, "register again");
    HzDestroyWindow(Shell);
    CheckStatus(NtUserSetWorkArea(&Strip), STATUS_ACCESS_DENIED, "destroying the shell window unregisters");
    ClearLog();
    Key(HZ_VK_LWIN, 0, 0x5B, 0, false);
    Key(HZ_VK_LWIN, 0, 0x5B, 0, true);
    Pump();
}

void TestFrames()
{
    uint32_t Wallpaper = Pixel(320, 520), Below = Pixel(550, 566);
    TestWindow GlassData = {0x80FF0000}, FlatData = {0xFF00A000};
    HWND Glass = HzCreateWindow("Glass", HZ_RECT{300, 500, 400, 560}, HZ_WS_TRANSPARENT | HZ_WS_POPUP | HZ_WS_VISIBLE,
                                TestProc, &GlassData);
    HWND Flat = HzCreateWindow("Flat", HZ_RECT{500, 500, 600, 560}, HZ_WS_NOFRAME | HZ_WS_POPUP | HZ_WS_VISIBLE, TestProc,
                               &FlatData);
    Settle();
    uint32_t Mixed = Pixel(320, 520);
    int R = (int)((Mixed >> 16) & 0xFF), G = (int)((Mixed >> 8) & 0xFF);
    int WallR = (int)((Wallpaper >> 16) & 0xFF), WallG = (int)((Wallpaper >> 8) & 0xFF);
    Check(R - (WallR * 127 + 255 * 128) / 255 <= 3 && (WallR * 127 + 255 * 128) / 255 - R <= 3 &&
              G - WallG * 127 / 255 <= 3 && WallG * 127 / 255 - G <= 3,
          "transparent windows blend their pixels over what is below");
    Check(Pixel(500, 500) == 0x00A000 && Pixel(599, 559) == 0x00A000, "frameless windows have square corners and no border");
    Check(Pixel(550, 566) == Below, "frameless windows cast no shadow");
    HzDestroyWindow(Glass);
    HzDestroyWindow(Flat);
    Pump();
}

/* "wintest.exe child": one app window for the shell tests; WM_USER+1 renames it, closing ends the process. */
int64_t ChildProc(HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (Message == HZ_WM_USER + 1) {
        NtUserSetWindowText(Window, "Child renamed");
        return 0;
    }
    if (Message == HZ_WM_DESTROY) {
        NtUserPostQuitMessage(0);
        return 0;
    }
    return HzDefWindowProc(Window, Message, WParam, LParam);
}

int RunChild()
{
    const uint32_t Style = HZ_WS_CAPTION | HZ_WS_RESIZABLE | HZ_WS_MINIMIZEBOX | HZ_WS_MAXIMIZEBOX | HZ_WS_VISIBLE;
    if (!HzCreateWindow("Child", HZ_RECT{300, 200, 700, 500}, Style, ChildProc, nullptr))
        return 1;
    return (int)HzRunMessageLoop();
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return RunChild();
    HZ_POINT Cursor = {};
    NtUserGetCursorPos(&Cursor); /* restored at the end: the desktop starts with the cursor as it was */
    TestRuntime();
    HZ_DISPLAY_INFO Display;
    memset(&Display, 0, sizeof(Display));
    TestDisplay(&Display);
    TestInvalidArguments();
    if (Display.Width >= 800 && Display.Height >= 600) {
        TestWindows(Display);
        TestWindowStates(Display);
        TestMouseLock();
        TestShell(Display);
        TestFrames();
    }

    /* Left alive on purpose: thread exit must destroy it (checked by the kernel). */
    TestWindow Leftover = {0xFF808080};
    HWND Last = HzCreateWindow("Leftover", HZ_RECT{200, 200, 400, 300}, HZ_WS_VISIBLE, TestProc, &Leftover);
    Check(Last != 0, "create a window left for process exit");
    Mouse(Cursor.X, Cursor.Y, 0);

    HzPrint("wintest: %d checks, %d failed", g_Checks, g_Failures);
    return g_Failures;
}
