/*
 * Nexora OS - user/include/hzuser.h
 * hzuser: the user-mode window library over the win32k services (uapi/hzuser.h).
 *
 *   - Windows have a window procedure (HZ_WNDPROC) and user data; hzuser keeps that table per
 *     process and calls the procedure from HzDispatchMessage (and directly for HZ_WM_CREATE and
 *     HZ_WM_DESTROY). HzDefWindowProc gives the standard behaviour: close destroys the window,
 *     resize requests are applied, paint validates.
 *   - Painting: HzBeginPaint returns a canvas over the window's surface (the whole window, caption
 *     included). For HZ_WS_CAPTION windows hzuser draws the caption itself (title and close button;
 *     transparent over Mica) before the application paints; HzGetClientRect is the area below it.
 *   - Drawing: rectangles, anti-aliased rounded rectangles and lines, and text with TrueType fonts
 *     (Inter from C:\Nexora\Fonts, rasterized here in user mode with stb_truetype and cached).
 *     Colors are 0xAARRGGBB (non-premultiplied); "blend" operations composite over the existing
 *     pixel, including its alpha, so text drawn on a transparent caption stays translucent.
 */
#ifndef HZUSER_H
#define HZUSER_H

#include "hzsys.h"
#include "uapi/hzuser.h"

HZ_EXTERN_C_BEGIN

/* ---- win32k system calls (user/lib/hzuser/hzusersys.asm) ------------------------------------- */

STATUS NtUserCreateWindow(const HZ_CREATE_WINDOW *Create, HWND *Window, HZ_SURFACE *Surface);
STATUS NtUserDestroyWindow(HWND Window);
STATUS NtUserShowWindow(HWND Window, uint32_t Command);
STATUS NtUserSetWindowPos(HWND Window, int32_t X, int32_t Y, int32_t Width, int32_t Height, uint32_t Flags);
STATUS NtUserGetWindowInfo(HWND Window, HZ_WINDOW_INFO *Info);
STATUS NtUserSetWindowText(HWND Window, const char *Title);
STATUS NtUserGetMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last);
STATUS NtUserPeekMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last, uint32_t Flags);
STATUS NtUserPostMessage(HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam);
STATUS NtUserPostQuitMessage(int64_t ExitCode);
STATUS NtUserInvalidateRect(HWND Window, const HZ_RECT *Rect);
STATUS NtUserBeginPaint(HWND Window, HZ_PAINT *Paint);
STATUS NtUserEndPaint(HWND Window, const HZ_RECT *Painted);
STATUS NtUserSetTimer(HWND Window, uint64_t Id, uint32_t PeriodMs);
STATUS NtUserKillTimer(HWND Window, uint64_t Id);
STATUS NtUserGetDisplayInfo(HZ_DISPLAY_INFO *Info);
STATUS NtUserGetCursorPos(HZ_POINT *Point);
STATUS NtUserSetForegroundWindow(HWND Window);
STATUS NtUserWindowFromPoint(int32_t X, int32_t Y, HWND *Window);
STATUS NtUserSendInput(const HZ_INPUT *Inputs, uint32_t Count);
STATUS NtUserFlush(uint32_t TimeoutMs);
STATUS NtUserGetScreenPixel(int32_t X, int32_t Y, uint32_t *Color);
STATUS NtUserSetCapture(HWND Window);
STATUS NtUserSetWindowAttribute(HWND Window, uint32_t Attribute, uint64_t Value);
STATUS NtUserRegisterShell(HWND Window);
STATUS NtUserEnumWindows(HWND *Buffer, uint32_t Capacity, uint32_t *Count);
STATUS NtUserSetWorkArea(const HZ_RECT *Rect);
STATUS NtUserSetClipboardText(const char *Text, uint32_t Length);
STATUS NtUserGetClipboardText(char *Buffer, uint32_t Capacity, uint32_t *Length);
STATUS NtUserSetWallpaper(uint32_t Index);
STATUS NtUserGetShellWindow(HWND *Window);

/* ---- Drawing ------------------------------------------------------------------------------------ */

typedef struct _HZ_CANVAS {
    uint32_t *Pixels;
    int32_t Width, Height;
    int32_t Stride;   /* pixels per row */
    HZ_RECT Clip;
} HZ_CANVAS;

typedef struct _HZ_FONT HZ_FONT;

#define HZ_FONT_UI 0          /* Inter Regular 14 px */
#define HZ_FONT_UI_SEMIBOLD 1 /* Inter SemiBold 14 px */
#define HZ_FONT_TITLE 2       /* Inter SemiBold 28 px */
#define HZ_FONT_CAPTION 3     /* Inter Regular 13 px */
#define HZ_FONT_SUBTITLE 4    /* Inter SemiBold 20 px */
#define HZ_FONT_SMALL 5       /* Inter Regular 12 px */
#define HZ_FONT_MONO 6        /* JetBrains Mono Regular 14 px (M9) */
#define HZ_FONT_BODY_LARGE 7  /* Inter Regular 18 px */
#define HZ_FONT_DISPLAY 8     /* Inter Display SemiBold 72 px */
#define HZ_FONT_COUNT 9

void HzCanvasFromSurface(HZ_CANVAS *Canvas, const HZ_SURFACE *Surface);
void HzFillRect(HZ_CANVAS *Canvas, HZ_RECT Rect, uint32_t Color);   /* replaces pixels */
void HzBlendRect(HZ_CANVAS *Canvas, HZ_RECT Rect, uint32_t Color);  /* composites over */
void HzFillRoundRect(HZ_CANVAS *Canvas, HZ_RECT Rect, int32_t Radius, uint32_t Color);
void HzDrawLine(HZ_CANVAS *Canvas, double X0, double Y0, double X1, double Y1, double Width, uint32_t Color);
void HzBlendPixel(HZ_CANVAS *Canvas, int32_t X, int32_t Y, uint32_t Color, uint32_t Coverage);

/* Loads a TrueType font at a pixel height (NULL on failure). */
HZ_FONT *HzLoadFont(const char *Path, int32_t PixelHeight);
/* The standard fonts (loaded on first use; NULL if C:\Nexora\Fonts is missing). */
HZ_FONT *HzGetFont(int Which);
int32_t HzFontAscent(const HZ_FONT *Font);
int32_t HzFontLineHeight(const HZ_FONT *Font);
int32_t HzMeasureText(HZ_FONT *Font, const char *Text);
/* Draws UTF-8 text with its baseline at Baseline; returns the pen position after it. */
int32_t HzDrawText(HZ_CANVAS *Canvas, HZ_FONT *Font, int32_t X, int32_t Baseline, const char *Text, uint32_t Color);
/* The same for the first Length bytes. HzMeasureTextN is exact in 1/64 pixel (HzMeasureTextFixed). */
int32_t HzMeasureTextN(HZ_FONT *Font, const char *Text, size_t Length);
int32_t HzMeasureTextFixed(HZ_FONT *Font, const char *Text, size_t Length); /* 26.6 fixed point */
int32_t HzDrawTextN(HZ_CANVAS *Canvas, HZ_FONT *Font, int32_t X, int32_t Baseline, const char *Text, size_t Length,
                    uint32_t Color);
int32_t HzFontDescent(const HZ_FONT *Font);

/* ---- Windows -------------------------------------------------------------------------------------- */

typedef int64_t (*HZ_WNDPROC)(HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam);

typedef struct _HZ_PAINTSTRUCT {
    HZ_CANVAS Canvas;   /* the whole window */
    HZ_RECT Update;     /* window coordinates */
} HZ_PAINTSTRUCT;

/* Creates a window (sends HZ_WM_CREATE before returning). Rect is in screen coordinates. */
HWND HzCreateWindow(const char *Title, HZ_RECT Rect, uint32_t Style, HZ_WNDPROC Proc, void *UserData);
/* Sends HZ_WM_DESTROY, then destroys it. */
STATUS HzDestroyWindow(HWND Window);
void *HzGetWindowData(HWND Window);
/* False for WM_QUIT (Msg->WParam = exit code). */
bool HzGetMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last);
bool HzPeekMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last, uint32_t Flags);
int64_t HzDispatchMessage(const HZ_MSG *Msg);
int64_t HzDefWindowProc(HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam);
/* Runs the message loop until WM_QUIT; returns its exit code. */
int64_t HzRunMessageLoop(void);

bool HzBeginPaint(HWND Window, HZ_PAINTSTRUCT *Paint);
void HzEndPaint(HWND Window, const HZ_PAINTSTRUCT *Paint);
void HzInvalidate(HWND Window, const HZ_RECT *Rect);
/* Window-relative rectangle of the whole window and of the client area (below the caption). */
HZ_RECT HzGetWindowRect(HWND Window);
HZ_RECT HzGetClientRect(HWND Window);
bool HzIsActiveWindow(HWND Window);
bool HzIsMaximized(HWND Window);
bool HzIsMinimized(HWND Window);
/* Last HZ_WM_CAPTIONSTATE: hot button | pressed button << 8. */
uint32_t HzGetCaptionState(HWND Window);
STATUS HzShowWindow(HWND Window, uint32_t Command); /* HZ_SW_* */
STATUS HzSetCursor(HWND Window, uint32_t Shape);   /* client-area cursor, HZ_CURSOR_* */
STATUS HzSetMinimumSize(HWND Window, int32_t Width, int32_t Height);
STATUS HzSetCapture(HWND Window);
void HzReleaseCapture(void);
HZ_RECT HzGetScreenRect(HWND Window);

HZ_EXTERN_C_END

#endif /* HZUSER_H */
