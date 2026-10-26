/*
 * Nexora OS - win32k/input.cpp
 * Input routing. Two system threads read \Device\Keyboard0 and \Device\Mouse0 through the I/O
 * manager; SendInput injects events through the same functions. Everything runs with WmLock held.
 *
 * Mouse: the cursor moves by the relative deltas (absolute for injected input) and is clamped to
 * the screen. A left press hit-tests the topmost window from fixed metrics:
 *   - resize border (HZ_WS_RESIZABLE, not maximized): a resize drag; each motion stores the new
 *     rectangle (bounded by the window's minimum size) and posts HZ_WM_SIZE_REQUEST once until the
 *     owner applies it (the owner reallocates its surface);
 *   - caption (HZ_WS_CAPTION, top HZ_CAPTION_HEIGHT rows): a move drag done here, WM_MOVE at the
 *     end; a double-click toggles maximize (HZ_WS_MAXIMIZEBOX); dragging a maximized window
 *     restores it under the cursor first;
 *   - caption buttons (right to left: close, maximize, minimize): the action on release over the
 *     same button; the owner hears hover and press changes through HZ_WM_CAPTIONSTATE;
 *   - client area: button messages, with capture until the release and double-click detection.
 * Pressing activates the window (unless HZ_WS_NOACTIVATE); pressing the desktop deactivates.
 * An explicit capture (SetCapture) takes every mouse message instead and suppresses activation
 * and caption handling; activation changes cancel it. The cursor shape follows the resize border
 * or drag under the cursor, otherwise the client cursor of the window under it (HZ_WA_CURSOR).
 * A mouse lock (HZ_WA_MOUSE_LOCK, for games) hides the cursor and holds it at the locking window's
 * centre: motion is posted as HZ_WM_MOUSEDELTA and buttons and the wheel go straight to that
 * window. Activation changes, hiding the window, the logo key and Alt+Tab end it
 * (HZ_WM_MOUSEUNLOCKED).
 * Keyboard: key and character messages go to the active window; Alt+F4 posts WM_CLOSE to it and
 * Alt+Tab activates the bottom-most window (cycling, minimized windows included). With a shell
 * registered (M8), the logo key alone and Alt+Tab (until Alt is released) go to the shell as
 * HZ_WM_SHELLNOTIFY events instead.
 * With hz.input=echo the PS/2 devices are left to the driver's echo diagnostic.
 */
#include "w32p.h"

#define INP_DOUBLE_CLICK_US 500000
#define INP_DOUBLE_CLICK_DISTANCE 4
#define INP_DRAG_THRESHOLD 4

#define INP_EDGE_LEFT 1u
#define INP_EDGE_TOP 2u
#define INP_EDGE_RIGHT 4u
#define INP_EDGE_BOTTOM 8u

namespace {

enum INP_DRAG { DragNone, DragMove, DragResize, DragButton };

struct {
    int32_t X, Y;
    uint16_t Buttons;
    uint16_t Modifiers;
    INP_DRAG Drag;
    WINDOW *DragWindow;
    uint32_t Edges;
    uint32_t DragButtonId;
    int32_t DragStartX, DragStartY;
    HZ_RECT DragStartRect;
    HZ_POINT DragOrigin;    /* window position when the drag started */
    WINDOW *Capture;        /* implicit (client press) or explicit (SetCapture) */
    bool ExplicitCapture;
    WINDOW *Lock;           /* mouse lock (HZ_WA_MOUSE_LOCK) */
    WINDOW *Hover;
    WINDOW *CaptionWindow;  /* last caption state reported */
    uint32_t CaptionHot, CaptionPressed;
    uint32_t Shape;
    uint64_t LastClickUs;
    HWND LastClickWindow;
    int32_t LastClickX, LastClickY;
    bool LastClickCaption;
    bool LogoDown, LogoUsed; /* the logo key alone opens Start (shell) */
    bool Switching;          /* Alt+Tab handed to the shell until Alt is released */
} Inp;

enum INP_HIT { HitClient, HitCaption, HitButton, HitBorder };

INP_HIT HitTest(const WINDOW *W, int32_t x, int32_t y, uint32_t *Edges, uint32_t *Button)
{
    int32_t Width = W->Rect.Right - W->Rect.Left, Height = W->Rect.Bottom - W->Rect.Top;
    *Edges = 0;
    *Button = HZ_CAPTION_BUTTON_NONE;
    if ((W->Style & HZ_WS_RESIZABLE) && !(W->State & HZ_WINDOW_STATE_MAXIMIZED)) {
        if (x < HZ_RESIZE_BORDER)
            *Edges |= INP_EDGE_LEFT;
        if (x >= Width - HZ_RESIZE_BORDER)
            *Edges |= INP_EDGE_RIGHT;
        if (y < HZ_RESIZE_BORDER)
            *Edges |= INP_EDGE_TOP;
        if (y >= Height - HZ_RESIZE_BORDER)
            *Edges |= INP_EDGE_BOTTOM;
        if (*Edges)
            return HitBorder;
    }
    if (!(W->Style & HZ_WS_CAPTION) || y >= HZ_CAPTION_HEIGHT)
        return HitClient;
    int32_t FromRight = Width - x; /* 1.. */
    int32_t Slot = 1;
    if (FromRight <= HZ_CAPTION_BUTTON_WIDTH) {
        *Button = HZ_CAPTION_BUTTON_CLOSE;
        return HitButton;
    }
    if (W->Style & HZ_WS_MAXIMIZEBOX) {
        if (FromRight <= 2 * HZ_CAPTION_BUTTON_WIDTH) {
            *Button = HZ_CAPTION_BUTTON_MAXIMIZE;
            return HitButton;
        }
        Slot++;
    }
    if ((W->Style & HZ_WS_MINIMIZEBOX) && FromRight <= (Slot + 1) * HZ_CAPTION_BUTTON_WIDTH) {
        *Button = HZ_CAPTION_BUTTON_MINIMIZE;
        return HitButton;
    }
    return HitCaption;
}

uint32_t EdgeShape(uint32_t Edges)
{
    bool Horizontal = Edges & (INP_EDGE_LEFT | INP_EDGE_RIGHT), Vertical = Edges & (INP_EDGE_TOP | INP_EDGE_BOTTOM);
    if (Horizontal && Vertical) {
        bool Main = (Edges & INP_EDGE_LEFT) ? (Edges & INP_EDGE_TOP) != 0 : (Edges & INP_EDGE_BOTTOM) != 0;
        return Main ? HZ_CURSOR_SIZE_NWSE : HZ_CURSOR_SIZE_NESW;
    }
    return Horizontal ? HZ_CURSOR_SIZE_WE : HZ_CURSOR_SIZE_NS;
}

uint64_t MouseWParam()
{
    return (uint64_t)Inp.Buttons | ((uint64_t)Inp.Modifiers << 16);
}

void PostMouse(WINDOW *W, uint32_t Message, uint64_t WParam)
{
    WmPostMessage(W, Message, WParam, HZ_MAKE_POINT(Inp.X - W->Rect.Left, Inp.Y - W->Rect.Top));
}

/* Tells the owner about hover/press changes of its caption buttons (one window at a time). */
void ReportCaption(WINDOW *W, uint32_t Hot, uint32_t Pressed)
{
    if (W != Inp.CaptionWindow) {
        if (Inp.CaptionWindow && (Inp.CaptionHot || Inp.CaptionPressed))
            WmPostMessage(Inp.CaptionWindow, HZ_WM_CAPTIONSTATE, 0, 0);
        Inp.CaptionWindow = W;
        Inp.CaptionHot = Inp.CaptionPressed = HZ_CAPTION_BUTTON_NONE;
    }
    if (W && (Hot != Inp.CaptionHot || Pressed != Inp.CaptionPressed)) {
        Inp.CaptionHot = Hot;
        Inp.CaptionPressed = Pressed;
        WmPostMessage(W, HZ_WM_CAPTIONSTATE, Hot | (Pressed << 8), 0);
    }
}

/* Recomputes the caption button state and the cursor shape for the current position. */
void UpdatePointerState()
{
    uint32_t Shape = HZ_CURSOR_ARROW, Hot = HZ_CAPTION_BUTTON_NONE, Pressed = HZ_CAPTION_BUTTON_NONE;
    WINDOW *CaptionWindow = nullptr;
    if (Inp.Lock) {
        Shape = HZ_CURSOR_HIDDEN;
    } else if (Inp.Drag == DragResize) {
        Shape = EdgeShape(Inp.Edges);
    } else if (Inp.Drag == DragButton) {
        WINDOW *W = Inp.DragWindow;
        uint32_t Edges, Button;
        CaptionWindow = W;
        Pressed = Inp.DragButtonId;
        if (WmWindowFromPoint(Inp.X, Inp.Y) == W &&
            HitTest(W, Inp.X - W->Rect.Left, Inp.Y - W->Rect.Top, &Edges, &Button) == HitButton &&
            Button == Pressed)
            Hot = Button;
    } else if (Inp.Drag == DragNone) {
        if (Inp.Capture) {
            Shape = Inp.Capture->Cursor;
        } else if (WINDOW *W = WmWindowFromPoint(Inp.X, Inp.Y)) {
            uint32_t Edges, Button;
            INP_HIT Hit = HitTest(W, Inp.X - W->Rect.Left, Inp.Y - W->Rect.Top, &Edges, &Button);
            if (Hit == HitBorder) {
                Shape = EdgeShape(Edges);
            } else if (Hit == HitClient) {
                Shape = W->Cursor;
            } else if (Hit == HitButton) {
                CaptionWindow = W;
                Hot = Button;
            }
        }
    }
    ReportCaption(CaptionWindow, Hot, Pressed);
    if (Shape != Inp.Shape) {
        Inp.Shape = Shape;
        CmpInvalidateCursor(Inp.X, Inp.Y, Inp.X, Inp.Y);
    }
}

void UpdateDrag()
{
    WINDOW *W = Inp.DragWindow;
    int32_t dx = Inp.X - Inp.DragStartX, dy = Inp.Y - Inp.DragStartY;
    if (Inp.Drag == DragMove) {
        if (W->State & HZ_WINDOW_STATE_MAXIMIZED) {
            if (dx > -INP_DRAG_THRESHOLD && dx < INP_DRAG_THRESHOLD && dy > -INP_DRAG_THRESHOLD && dy < INP_DRAG_THRESHOLD)
                return;
            /* Restore under the cursor, keeping its relative position along the caption. */
            int32_t RestoreWidth = W->RestoreRect.Right - W->RestoreRect.Left;
            int32_t RestoreHeight = W->RestoreRect.Bottom - W->RestoreRect.Top;
            int32_t Width = HZ_MAX(W->Rect.Right - W->Rect.Left, 1);
            int32_t OffsetX = (int32_t)((int64_t)(Inp.DragStartX - W->Rect.Left) * RestoreWidth / Width);
            int32_t OffsetY = HZ_MIN(Inp.DragStartY - W->Rect.Top, HZ_CAPTION_HEIGHT - 1);
            HZ_RECT R = {Inp.X - OffsetX, Inp.Y - OffsetY, Inp.X - OffsetX + RestoreWidth, Inp.Y - OffsetY + RestoreHeight};
            W->State &= ~HZ_WINDOW_STATE_MAXIMIZED;
            WmRequestResize(W, R);
            WmInvalidateWindowArea(W);
            Inp.DragStartRect = R;
            Inp.DragStartX = Inp.X;
            Inp.DragStartY = Inp.Y;
            return;
        }
        int32_t Left = Inp.DragStartRect.Left + dx, Top = Inp.DragStartRect.Top + dy;
        if (W->ResizePending) { /* restoring: the owner has not applied the size yet */
            int32_t Width = W->PendingRect.Right - W->PendingRect.Left, Height = W->PendingRect.Bottom - W->PendingRect.Top;
            W->PendingRect = HZ_RECT{Left, Top, Left + Width, Top + Height};
        } else {
            WmMoveWindow(W, Left, Top);
        }
        WmShellNotify(HZ_SHELL_WINDOW_MOVING, W);
        return;
    }
    if (Inp.Drag != DragResize)
        return;
    int32_t MinWidth = HZ_MAX(HZ_MIN_WINDOW_WIDTH, W->MinWidth), MinHeight = HZ_MAX(HZ_MIN_WINDOW_HEIGHT, W->MinHeight);
    HZ_RECT R = Inp.DragStartRect;
    if (Inp.Edges & INP_EDGE_LEFT)
        R.Left = HZ_MIN(R.Left + dx, R.Right - MinWidth);
    if (Inp.Edges & INP_EDGE_RIGHT)
        R.Right = HZ_MAX(R.Right + dx, R.Left + MinWidth);
    if (Inp.Edges & INP_EDGE_TOP)
        R.Top = HZ_MIN(R.Top + dy, R.Bottom - MinHeight);
    if (Inp.Edges & INP_EDGE_BOTTOM)
        R.Bottom = HZ_MAX(R.Bottom + dy, R.Top + MinHeight);
    R.Right = HZ_MIN(R.Right, R.Left + HZ_MAX_WINDOW_SIZE);
    R.Bottom = HZ_MIN(R.Bottom, R.Top + HZ_MAX_WINDOW_SIZE);
    WmRequestResize(W, R);
}

void StartDrag(INP_DRAG Kind, WINDOW *W, uint32_t Edges, uint32_t Button)
{
    Inp.Drag = Kind;
    Inp.DragWindow = W;
    Inp.Edges = Edges;
    Inp.DragButtonId = Button;
    Inp.DragStartX = Inp.X;
    Inp.DragStartY = Inp.Y;
    Inp.DragStartRect = W->Rect;
    Inp.DragOrigin = HZ_POINT{W->Rect.Left, W->Rect.Top};
}

void LeftPressed()
{
    if (Inp.ExplicitCapture) {
        PostMouse(Inp.Capture, HZ_WM_LBUTTONDOWN, MouseWParam());
        return;
    }
    WINDOW *W = WmWindowFromPoint(Inp.X, Inp.Y);
    if (!W) {
        WmActivate(nullptr); /* the desktop */
        return;
    }
    if (!(W->Style & HZ_WS_NOACTIVATE))
        WmActivate(W);
    uint32_t Edges, Button;
    INP_HIT Hit = HitTest(W, Inp.X - W->Rect.Left, Inp.Y - W->Rect.Top, &Edges, &Button);
    uint64_t Now = KeQueryUptimeMicroseconds();
    int32_t dx = Inp.X - Inp.LastClickX, dy = Inp.Y - Inp.LastClickY;
    bool Double = Inp.LastClickWindow == W->Handle && Now - Inp.LastClickUs < INP_DOUBLE_CLICK_US &&
                  dx >= -INP_DOUBLE_CLICK_DISTANCE && dx <= INP_DOUBLE_CLICK_DISTANCE &&
                  dy >= -INP_DOUBLE_CLICK_DISTANCE && dy <= INP_DOUBLE_CLICK_DISTANCE &&
                  Inp.LastClickCaption == (Hit == HitCaption);
    Inp.LastClickWindow = (Double || (Hit != HitClient && Hit != HitCaption)) ? 0 : W->Handle;
    Inp.LastClickUs = Now;
    Inp.LastClickX = Inp.X;
    Inp.LastClickY = Inp.Y;
    Inp.LastClickCaption = Hit == HitCaption;

    switch (Hit) {
    case HitCaption:
        if (Double && (W->Style & HZ_WS_MAXIMIZEBOX)) {
            if (W->State & HZ_WINDOW_STATE_MAXIMIZED)
                WmRestoreWindow(W);
            else
                WmMaximizeWindow(W);
            return;
        }
        StartDrag(DragMove, W, 0, 0);
        return;
    case HitBorder:
        StartDrag(DragResize, W, Edges, 0);
        UpdatePointerState();
        return;
    case HitButton:
        StartDrag(DragButton, W, 0, Button);
        UpdatePointerState();
        return;
    case HitClient:
        Inp.Capture = W;
        PostMouse(W, Double ? HZ_WM_LBUTTONDBLCLK : HZ_WM_LBUTTONDOWN, MouseWParam());
        return;
    }
}

void CaptionButtonAction(WINDOW *W, uint32_t Button)
{
    switch (Button) {
    case HZ_CAPTION_BUTTON_CLOSE:
        WmPostMessage(W, HZ_WM_CLOSE, 0, 0);
        break;
    case HZ_CAPTION_BUTTON_MAXIMIZE:
        if (W->State & HZ_WINDOW_STATE_MAXIMIZED)
            WmRestoreWindow(W);
        else
            WmMaximizeWindow(W);
        break;
    case HZ_CAPTION_BUTTON_MINIMIZE:
        WmMinimizeWindow(W);
        break;
    default:
        break;
    }
}

void LeftReleased()
{
    if (Inp.Drag != DragNone) {
        WINDOW *W = Inp.DragWindow;
        INP_DRAG Drag = Inp.Drag;
        uint32_t Button = Inp.DragButtonId;
        Inp.Drag = DragNone;
        Inp.DragWindow = nullptr;
        if (Drag == DragMove) {
            HZ_RECT R = W->ResizePending ? W->PendingRect : W->Rect;
            if (R.Left != Inp.DragOrigin.X || R.Top != Inp.DragOrigin.Y)
                WmPostMessage(W, HZ_WM_MOVE, 0, HZ_MAKE_POINT(R.Left, R.Top));
            WmShellNotify(HZ_SHELL_WINDOW_MOVE_END, W);
        } else if (Drag == DragButton) {
            uint32_t Edges, Under;
            if (WmWindowFromPoint(Inp.X, Inp.Y) == W &&
                HitTest(W, Inp.X - W->Rect.Left, Inp.Y - W->Rect.Top, &Edges, &Under) == HitButton && Under == Button)
                CaptionButtonAction(W, Button);
        }
        UpdatePointerState();
        return;
    }
    WINDOW *Target = Inp.Capture ? Inp.Capture : WmWindowFromPoint(Inp.X, Inp.Y);
    if (Target)
        PostMouse(Target, HZ_WM_LBUTTONUP, MouseWParam());
    if (!Inp.ExplicitCapture)
        Inp.Capture = nullptr;
    UpdatePointerState();
}

void OtherButton(uint16_t Changed, bool Pressed)
{
    WINDOW *Target = Inp.Capture ? Inp.Capture : WmWindowFromPoint(Inp.X, Inp.Y);
    if (!Target)
        return;
    uint32_t Message = Changed == HZ_MOUSE_RIGHT ? (Pressed ? HZ_WM_RBUTTONDOWN : HZ_WM_RBUTTONUP)
                                                 : (Pressed ? HZ_WM_MBUTTONDOWN : HZ_WM_MBUTTONUP);
    if (Pressed && !Inp.ExplicitCapture && !(Target->Style & HZ_WS_NOACTIVATE))
        WmActivate(Target);
    PostMouse(Target, Message, MouseWParam());
}

void InputThread(void *Context)
{
    bool Mouse = Context != nullptr;
    FILE_OBJECT *File;
    if (!HZ_SUCCESS(IoCreateFile(Mouse ? "\\Device\\Mouse0" : "\\Device\\Keyboard0", FILE_READ_DATA, FILE_OPEN, 0,
                                 KernelMode, &File, NULL)))
        return;
    for (;;) {
        uint8_t Buffer[16 * sizeof(HZ_KEY_EVENT)];
        uint32_t Size = Mouse ? sizeof(HZ_MOUSE_EVENT) : sizeof(HZ_KEY_EVENT);
        uint32_t Got = 0;
        if (!HZ_SUCCESS(IoReadFile(File, Buffer, 16 * Size, NULL, KernelMode, &Got)))
            break;
        WmAcquire();
        for (uint32_t i = 0; i < Got / Size; i++) {
            if (Mouse)
                InpProcessMouse((const HZ_MOUSE_EVENT *)(Buffer + i * Size), false);
            else
                InpProcessKey((const HZ_KEY_EVENT *)(Buffer + i * Size));
        }
        WmRelease();
    }
    ObDereferenceObject(File);
}

}  // namespace

void InpInitialize()
{
    Inp.X = CmpScreenWidth() / 2;
    Inp.Y = CmpScreenHeight() / 2;
    /* The devices have one event queue each: two readers would split the events between them.
       The raw echo diagnostic (hz.input=echo) owns them; injected input still works. */
    if (BiCommandLineHasOption("hz.input=echo")) {
        DbgPrintEx(DPFLTR_INFO_LEVEL, "Desktop: input devices left to the echo diagnostic\n");
        return;
    }
    /* Devices that do not exist (no PS/2 mouse) simply end their thread. */
    PsCreateSystemThread(InputThread, nullptr, KI_THREAD_PRIORITY_DEFAULT + 4, "KeyboardInput", nullptr);
    PsCreateSystemThread(InputThread, (void *)1, KI_THREAD_PRIORITY_DEFAULT + 4, "MouseInput", nullptr);
}

HZ_POINT InpGetCursor()
{
    HZ_POINT P = {Inp.X, Inp.Y};
    return P;
}

uint32_t InpGetCursorShape()
{
    return Inp.Shape;
}

WINDOW *InpGetExplicitCapture()
{
    return Inp.ExplicitCapture ? Inp.Capture : nullptr;
}

void InpSetCapture(WINDOW *W)
{
    WINDOW *Old = Inp.ExplicitCapture ? Inp.Capture : nullptr;
    if (W == Old)
        return;
    if (Old)
        WmPostMessage(Old, HZ_WM_CAPTURECHANGED, 0, W ? W->Handle : 0);
    Inp.Capture = W;
    Inp.ExplicitCapture = W != nullptr;
    if (W) {
        Inp.Drag = DragNone; /* a window manager drag in progress ends here */
        Inp.DragWindow = nullptr;
    }
    UpdatePointerState();
}

/* Ends the mouse lock; Notify tells the window it lost it without asking. */
static void ReleaseMouseLock(bool Notify)
{
    WINDOW *W = Inp.Lock;
    if (!W)
        return;
    Inp.Lock = nullptr;
    if (Notify)
        WmPostMessage(W, HZ_WM_MOUSEUNLOCKED, 0, 0);
    UpdatePointerState();
}

STATUS InpSetMouseLock(WINDOW *W, bool Lock)
{
    if (!Lock) {
        if (Inp.Lock == W)
            ReleaseMouseLock(false);
        return STATUS_SUCCESS;
    }
    if (W != WmActive || !W->Visible)
        return STATUS_ACCESS_DENIED;
    if (Inp.Lock == W)
        return STATUS_SUCCESS;
    ReleaseMouseLock(true);
    InpCancelCapture();
    Inp.Capture = nullptr;
    Inp.Drag = DragNone;
    Inp.DragWindow = nullptr;
    if (Inp.Hover) {
        WmPostMessage(Inp.Hover, HZ_WM_MOUSELEAVE, 0, 0);
        Inp.Hover = nullptr;
    }
    /* Park the (hidden) cursor at the window's centre: button messages carry that point. */
    int32_t OldX = Inp.X, OldY = Inp.Y;
    Inp.X = (W->Rect.Left + W->Rect.Right) / 2;
    Inp.Y = (W->Rect.Top + W->Rect.Bottom) / 2;
    CmpInvalidateCursor(OldX, OldY, Inp.X, Inp.Y);
    Inp.Lock = W;
    UpdatePointerState();
    return STATUS_SUCCESS;
}

void InpCancelCapture()
{
    if (Inp.ExplicitCapture)
        InpSetCapture(nullptr);
    ReleaseMouseLock(true);
}

void InpRefreshPointer()
{
    UpdatePointerState();
}

void InpWindowHidden(WINDOW *W)
{
    if (Inp.Lock == W)
        ReleaseMouseLock(true);
    if (Inp.Capture == W) {
        if (Inp.ExplicitCapture)
            InpSetCapture(nullptr);
        else
            Inp.Capture = nullptr;
    }
    if (Inp.DragWindow == W) {
        Inp.DragWindow = nullptr;
        Inp.Drag = DragNone;
    }
    if (Inp.Hover == W) {
        WmPostMessage(W, HZ_WM_MOUSELEAVE, 0, 0);
        Inp.Hover = nullptr;
    }
    if (Inp.CaptionWindow == W)
        ReportCaption(nullptr, 0, 0);
    UpdatePointerState();
}

void InpWindowDestroyed(WINDOW *W)
{
    if (Inp.Lock == W) {
        Inp.Lock = nullptr;
        UpdatePointerState();
    }
    if (Inp.Capture == W) {
        Inp.Capture = nullptr;
        Inp.ExplicitCapture = false;
    }
    if (Inp.Hover == W)
        Inp.Hover = nullptr;
    if (Inp.DragWindow == W) {
        Inp.DragWindow = nullptr;
        Inp.Drag = DragNone;
    }
    if (Inp.CaptionWindow == W) {
        Inp.CaptionWindow = nullptr;
        Inp.CaptionHot = Inp.CaptionPressed = HZ_CAPTION_BUTTON_NONE;
    }
    if (Inp.LastClickWindow == W->Handle)
        Inp.LastClickWindow = 0;
}

void InpProcessMouse(const HZ_MOUSE_EVENT *E, bool Absolute)
{
    if (WINDOW *L = Inp.Lock) {
        /* Locked: the cursor stays; the window gets the motion and every button and wheel. */
        int32_t Dx = Absolute ? E->DeltaX - Inp.X : E->DeltaX;
        int32_t Dy = Absolute ? E->DeltaY - Inp.Y : E->DeltaY;
        uint16_t Previous = Inp.Buttons;
        Inp.Buttons = E->Buttons & (HZ_MOUSE_LEFT | HZ_MOUSE_RIGHT | HZ_MOUSE_MIDDLE);
        uint16_t Pressed = Inp.Buttons & ~Previous, Released = Previous & ~Inp.Buttons;
        if (Dx || Dy)
            WmPostMessage(L, HZ_WM_MOUSEDELTA, MouseWParam(), HZ_MAKE_POINT(Dx, Dy));
        static const struct {
            uint16_t Button;
            uint32_t Down, Up;
        } Buttons[] = {{HZ_MOUSE_LEFT, HZ_WM_LBUTTONDOWN, HZ_WM_LBUTTONUP},
                       {HZ_MOUSE_RIGHT, HZ_WM_RBUTTONDOWN, HZ_WM_RBUTTONUP},
                       {HZ_MOUSE_MIDDLE, HZ_WM_MBUTTONDOWN, HZ_WM_MBUTTONUP}};
        for (const auto &B : Buttons) {
            if (Pressed & B.Button)
                PostMouse(L, B.Down, MouseWParam());
            if (Released & B.Button)
                PostMouse(L, B.Up, MouseWParam());
        }
        if (E->Wheel)
            PostMouse(L, HZ_WM_MOUSEWHEEL, (uint64_t)((int64_t)E->Wheel * 120));
        return;
    }
    int32_t OldX = Inp.X, OldY = Inp.Y;
    int32_t MaxX = HZ_MAX(CmpScreenWidth() - 1, 0), MaxY = HZ_MAX(CmpScreenHeight() - 1, 0);
    int32_t X = Absolute ? E->DeltaX : Inp.X + E->DeltaX;
    int32_t Y = Absolute ? E->DeltaY : Inp.Y + E->DeltaY;
    Inp.X = HZ_MIN(HZ_MAX(X, 0), MaxX);
    Inp.Y = HZ_MIN(HZ_MAX(Y, 0), MaxY);
    bool Moved = Inp.X != OldX || Inp.Y != OldY;
    if (Moved)
        CmpInvalidateCursor(OldX, OldY, Inp.X, Inp.Y);

    uint16_t Previous = Inp.Buttons;
    Inp.Buttons = E->Buttons & (HZ_MOUSE_LEFT | HZ_MOUSE_RIGHT | HZ_MOUSE_MIDDLE);
    uint16_t Pressed = Inp.Buttons & ~Previous, Released = Previous & ~Inp.Buttons;

    if (Moved) {
        if (Inp.Drag != DragNone) {
            UpdateDrag();
        } else {
            WINDOW *Under = WmWindowFromPoint(Inp.X, Inp.Y);
            WINDOW *Target = Inp.Capture ? Inp.Capture : Under;
            if (!Inp.Capture && Inp.Hover && Inp.Hover != Under)
                WmPostMessage(Inp.Hover, HZ_WM_MOUSELEAVE, 0, 0);
            if (!Inp.Capture)
                Inp.Hover = Under;
            if (Target)
                PostMouse(Target, HZ_WM_MOUSEMOVE, MouseWParam());
        }
        UpdatePointerState();
    }
    if (Pressed & HZ_MOUSE_LEFT)
        LeftPressed();
    if (Released & HZ_MOUSE_LEFT)
        LeftReleased();
    static const uint16_t OtherButtons[] = {HZ_MOUSE_RIGHT, HZ_MOUSE_MIDDLE};
    for (uint16_t Button : OtherButtons) {
        if ((Pressed | Released) & Button)
            OtherButton(Button, (Pressed & Button) != 0);
    }
    if (E->Wheel) {
        WINDOW *Target = Inp.Capture ? Inp.Capture : WmWindowFromPoint(Inp.X, Inp.Y);
        if (Target)
            PostMouse(Target, HZ_WM_MOUSEWHEEL, (uint64_t)((int64_t)E->Wheel * 120));
    }
}

void InpProcessKey(const HZ_KEY_EVENT *E)
{
    Inp.Modifiers = E->Modifiers;
    bool Down = !(E->Flags & HZ_KEY_BREAK);
    bool Alt = (E->Modifiers & HZ_MOD_ALT) != 0;
    bool Shift = (E->Modifiers & HZ_MOD_SHIFT) != 0;
    bool Shell = WmShellWindow != nullptr;

    /* The shell's keys take the mouse back from a locked window. */
    if (Shell && Down && (E->VirtualKey == HZ_VK_LWIN || (Alt && E->VirtualKey == HZ_VK_TAB)))
        ReleaseMouseLock(true);
    /* With a shell, Alt+Tab drives its switcher until Alt is released (keys are swallowed). */
    if (Inp.Switching) {
        if (!Alt || (E->VirtualKey == HZ_VK_MENU && !Down)) {
            Inp.Switching = false;
            WmShellEvent(HZ_SHELL_SWITCH_END, 1);
        } else if (Down && E->VirtualKey == HZ_VK_TAB) {
            WmShellEvent(HZ_SHELL_SWITCH_NEXT, Shift ? 1 : 0);
        } else if (Down && E->VirtualKey == HZ_VK_ESCAPE) {
            Inp.Switching = false;
            WmShellEvent(HZ_SHELL_SWITCH_END, 0);
        }
        return;
    }
    /* The logo key alone (pressed and released with nothing in between) opens Start. */
    if (E->VirtualKey == HZ_VK_LWIN && Shell) {
        if (Down && !Inp.LogoDown) {
            Inp.LogoDown = true;
            Inp.LogoUsed = false;
        } else if (!Down) {
            if (Inp.LogoDown && !Inp.LogoUsed)
                WmShellEvent(HZ_SHELL_START_KEY, 0);
            Inp.LogoDown = false;
        }
        return;
    }
    if (Down && Inp.LogoDown)
        Inp.LogoUsed = true;
    if (Down && Alt && E->VirtualKey == HZ_VK_TAB && Shell) {
        Inp.Switching = true;
        WmShellEvent(HZ_SHELL_SWITCH_NEXT, Shift ? 1 : 0);
        return;
    }
    if (Down && Alt && E->VirtualKey == HZ_VK_F1 + 3) { /* Alt+F4 */
        if (WmActive)
            WmPostMessage(WmActive, HZ_WM_CLOSE, 0, 0);
        return;
    }
    if (Down && Alt && E->VirtualKey == HZ_VK_TAB) { /* Alt+Tab: the bottom-most window comes up */
        for (WINDOW *W = WmBottom; W; W = W->Above) {
            bool Candidate = W->Visible || (W->State & HZ_WINDOW_STATE_MINIMIZED);
            if (W != WmActive && Candidate && !(W->Style & (HZ_WS_NOACTIVATE | HZ_WS_POPUP | HZ_WS_TOPMOST))) {
                if (W->State & HZ_WINDOW_STATE_MINIMIZED)
                    WmRestoreWindow(W);
                else
                    WmActivate(W);
                break;
            }
        }
        return;
    }
    if (!WmActive)
        return;
    uint64_t Data = HZ_KEY_DATA(E->ScanCode, E->Flags, E->Modifiers);
    WmPostMessage(WmActive, Down ? HZ_WM_KEYDOWN : HZ_WM_KEYUP, E->VirtualKey, Data);
    if (Down && E->Character)
        WmPostMessage(WmActive, HZ_WM_CHAR, E->Character, Data);
}
