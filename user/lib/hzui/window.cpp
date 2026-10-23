/*
 * Nexora OS - user/lib/hzui/window.cpp
 * hzui windows: the window procedure shared by all of them, damage-driven painting and layout,
 * pointer routing (hover enter/leave, press capture, bubbling), keyboard focus and Tab order,
 * animation and program timers, tooltips, popups, and the message loop.
 */
#include "hzuip.h"

namespace hzui {

namespace {

hz::Vector<Window *> g_Graveyard;

bool IsButtonDown(uint32_t Message)
{
    return Message == HZ_WM_LBUTTONDOWN || Message == HZ_WM_LBUTTONDBLCLK || Message == HZ_WM_RBUTTONDOWN ||
           Message == HZ_WM_MBUTTONDOWN;
}

bool IsButtonUp(uint32_t Message)
{
    return Message == HZ_WM_LBUTTONUP || Message == HZ_WM_RBUTTONUP || Message == HZ_WM_MBUTTONUP;
}

uint32_t ButtonOf(uint32_t Message)
{
    switch (Message) {
    case HZ_WM_RBUTTONDOWN:
    case HZ_WM_RBUTTONUP:
        return HZ_MOUSE_RIGHT;
    case HZ_WM_MBUTTONDOWN:
    case HZ_WM_MBUTTONUP:
        return HZ_MOUSE_MIDDLE;
    default:
        return HZ_MOUSE_LEFT;
    }
}

void CollectFocusable(Element *E, hz::Vector<Element *> &Out)
{
    if (!E->IsVisible())
        return;
    if (E->Focusable && E->IsEnabled())
        Out.Push(E);
    for (size_t i = 0; i < E->ChildCount(); i++)
        CollectFocusable(E->ChildAt(i), Out);
}

}  // namespace

Rect WorkArea()
{
    HZ_DISPLAY_INFO Info;
    memset(&Info, 0, sizeof(Info));
    NtUserGetDisplayInfo(&Info);
    return Rect::FromHz(Info.WorkArea);
}

/* ---- Life cycle ----------------------------------------------------------------------------------- */

Window::Window(const char *Title, int32_t Width, int32_t Height, uint32_t Style)
    : Title_(Title), Width_(Width), Height_(Height), Style_(Style)
{
    Background = (Style & HZ_WS_MICA) ? 0x00000000 : Theme::SolidBackground;
}

Window::~Window()
{
    ClosePopups();
    OnClosed = nullptr; /* the program is tearing this down itself */
    if (Handle_)
        HzDestroyWindow(Handle_);
    if (Root_) {
        Element *Root = Root_;
        Root_ = nullptr;
        delete Root;
    }
    g_Graveyard.Remove(this);
}

void Window::SetContent(Element *Root)
{
    if (Root_)
        delete Root_;
    Root_ = Root;
    Focus_ = Hover_ = Pressed_ = nullptr;
    if (Root)
        Root->Attach(this);
    InvalidateLayout();
}

bool Window::Show()
{
    Rect Area = WorkArea();
    return ShowAt(Area.X + (Area.Width - Width_) / 2, Area.Y + (Area.Height - Height_) / 2);
}

bool Window::ShowAt(int32_t X, int32_t Y)
{
    if (Handle_)
        return true;
    Origin_ = Point{X, Y};
    Active_ = (Style_ & HZ_WS_VISIBLE) && !(Style_ & HZ_WS_NOACTIVATE);
    LayoutDirty_ = true;
    HWND Handle = HzCreateWindow(Title_.CStr(), HZ_RECT{X, Y, X + Width_, Y + Height_}, Style_, WindowProc, this);
    if (!Handle)
        return false;
    Handle_ = Handle;
    if (MinWidth_ || MinHeight_)
        HzSetMinimumSize(Handle_, MinWidth_, MinHeight_);
    if (!Animating_.Empty() && !AnimationTimer_) {
        NtUserSetTimer(Handle_, kAnimationTimer, 15);
        AnimationTimer_ = true;
    }
    if (Focus_) /* focus set before the window existed: let it start its timers (caret) now */
        Focus_->OnFocusChanged(true);
    return true;
}

void Window::Close()
{
    if (Handle_)
        HzDestroyWindow(Handle_); /* WM_DESTROY -> Destroyed() */
}

void Window::Destroyed()
{
    ClosePopups();
    Handle_ = 0;
    AnimationTimer_ = false;
    Timers_.Clear();
    Hover_ = Pressed_ = nullptr;
    if (OnClosed) {
        hz::Function<void()> Callback = OnClosed; /* it may delete this window */
        Callback();
    }
}

void Window::SetTitle(const char *Title)
{
    Title_ = Title;
    if (Handle_) {
        NtUserSetWindowText(Handle_, Title);
        HZ_RECT Caption = {0, 0, Width_, HZ_CAPTION_HEIGHT};
        HzInvalidate(Handle_, &Caption);
    }
}

void Window::SetMinimumSize(int32_t Width, int32_t Height)
{
    MinWidth_ = Width;
    MinHeight_ = Height;
    if (Handle_)
        HzSetMinimumSize(Handle_, Width, Height);
}

Rect Window::ClientRect() const
{
    int32_t Top = (Style_ & HZ_WS_CAPTION) ? HZ_CAPTION_HEIGHT : 0;
    return Rect{0, Top, Width_, hz::Max(Height_ - Top, 0)};
}

Point Window::ScreenOrigin() const
{
    return Origin_;
}

bool Window::IsActive() const
{
    return Active_ && Handle_;
}

/* ---- Messages ------------------------------------------------------------------------------------- */

int64_t Window::WindowProc(HWND Handle, uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    Window *Self = static_cast<Window *>(HzGetWindowData(Handle));
    if (!Self)
        return HzDefWindowProc(Handle, Message, WParam, LParam);
    if (Message == HZ_WM_CREATE)
        Self->Handle_ = Handle; /* HzCreateWindow has not returned yet */
    return Self->HandleMessage(Message, WParam, LParam);
}

int64_t Window::HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    switch (Message) {
    case HZ_WM_CREATE:
        return 0;
    case HZ_WM_PAINT:
        PaintWindow();
        return 0;
    case HZ_WM_SIZE:
        if (WParam != HZ_SIZE_MINIMIZED) {
            Width_ = HZ_POINT_X(LParam);
            Height_ = HZ_POINT_Y(LParam);
            HZ_RECT R = HzGetScreenRect(Handle_);
            Origin_ = Point{R.Left, R.Top};
            InvalidateLayout();
        } else {
            ClosePopups();
        }
        return 0;
    case HZ_WM_MOVE:
        Origin_ = Point{HZ_POINT_X(LParam), HZ_POINT_Y(LParam)};
        ClosePopups();
        if (OnMoved)
            OnMoved(Origin_.X, Origin_.Y);
        return 0;
    case HZ_WM_ACTIVATE:
        Active_ = WParam != 0;
        if (!Active_)
            ClosePopups();
        if (Focus_) {
            Focus_->OnWindowActivated(Active_);
            Focus_->Invalidate();
        }
        return 0;
    case HZ_WM_MOUSEMOVE:
    case HZ_WM_LBUTTONDOWN:
    case HZ_WM_LBUTTONUP:
    case HZ_WM_LBUTTONDBLCLK:
    case HZ_WM_RBUTTONDOWN:
    case HZ_WM_RBUTTONUP:
    case HZ_WM_MBUTTONDOWN:
    case HZ_WM_MBUTTONUP:
    case HZ_WM_MOUSEWHEEL:
        DispatchPointer(Message, WParam, LParam, nullptr);
        return 0;
    case HZ_WM_MOUSELEAVE:
        if (!Pressed_)
            UpdateHover(nullptr);
        HideToolTip();
        ToolTipCandidate_ = ToolTipSuppressed_ = nullptr;
        return 0;
    case HZ_WM_KEYDOWN:
    case HZ_WM_KEYUP:
    case HZ_WM_CHAR:
        HandleKey(Message, WParam, LParam);
        return 0;
    case HZ_WM_TIMER:
        if (WParam == kAnimationTimer) {
            AnimationTick();
        } else if (WParam == kToolTipTimer) {
            NtUserKillTimer(Handle_, kToolTipTimer);
            if (ToolTipCandidate_ && !ToolTip_ && Active_) {
                ToolTip_ = CreateToolTip(ToolTipCandidate_->ToolTip.CStr(), LastScreenPointer_);
                ToolTipOwner_ = ToolTipCandidate_;
            }
        } else {
            for (TimerSlot &Slot : Timers_) {
                if (Slot.Id == WParam) {
                    hz::Function<void()> Callback = Slot.Callback; /* it may kill its own timer */
                    Callback();
                    break;
                }
            }
        }
        return 0;
    case HZ_WM_CLOSE:
        if (OnCloseRequested && !OnCloseRequested())
            return 0;
        Close();
        return 0;
    case HZ_WM_DESTROY:
        Destroyed();
        return 0;
    case HZ_WM_CAPTURECHANGED:
        OnPopupPointerOutside();
        return 0;
    default:
        return HzDefWindowProc(Handle_, Message, WParam, LParam);
    }
}

/* ---- Painting and layout ------------------------------------------------------------------------- */

void Window::PaintWindow()
{
    HZ_PAINTSTRUCT Paint;
    if (!HzBeginPaint(Handle_, &Paint))
        return;
    UpdateLayout();
    Painter P(Paint.Canvas);
    OnPaint(P);
    HzEndPaint(Handle_, &Paint);
    PaintCount_++;
}

void Window::OnPaint(Painter &P)
{
    Rect Client = ClientRect();
    P.PushClip(Client);
    P.Clear(Client, Background);
    if (Root_)
        PaintTree(Root_, P);
    P.PopClip();
}

void Window::OnContextMenu(Point Screen)
{
    if (!IsOpen() || (Style_ & (HZ_WS_NOFRAME | HZ_WS_POPUP)))
        return;
    HWND Handle = Handle_;
    hz::Vector<MenuItem> Items;
    if (Style_ & HZ_WS_MINIMIZEBOX)
        Items.Push(MenuItem::Make("Minimize", [Handle]() { NtUserShowWindow(Handle, HZ_SW_MINIMIZE); }));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Close window", [Handle]() { NtUserPostMessage(Handle, HZ_WM_CLOSE, 0, 0); }));
    ShowMenu(this, Screen, hz::Move(Items));
}

void Window::UpdateLayout()
{
    if (!LayoutDirty_ || !Root_)
        return;
    LayoutDirty_ = false;
    LayoutTree(Root_, ClientRect());
}

void Window::InvalidateRect(const Rect &R)
{
    if (!Handle_)
        return;
    Rect Area = R.Intersect(ClientRect());
    if (Area.Empty())
        return;
    HZ_RECT H = Area.ToHz();
    HzInvalidate(Handle_, &H);
}

void Window::InvalidateLayout()
{
    if (LayoutDirty_)
        return;
    LayoutDirty_ = true;
    InvalidateRect(ClientRect());
}

void Window::RequestAnimation(Element *E)
{
    if (!Animating_.Contains(E))
        Animating_.Push(E);
    if (!AnimationTimer_ && Handle_) {
        NtUserSetTimer(Handle_, kAnimationTimer, 15);
        AnimationTimer_ = true;
    }
}

void Window::AnimationTick()
{
    hz::Vector<Element *> Current = Animating_;
    for (Element *E : Current) {
        if (!Animating_.Contains(E)) /* removed by an earlier callback */
            continue;
        if (!E->Animate())
            Animating_.Remove(E);
    }
    if (Animating_.Empty() && AnimationTimer_) {
        NtUserKillTimer(Handle_, kAnimationTimer);
        AnimationTimer_ = false;
    }
}

uint64_t Window::SetTimer(uint32_t PeriodMs, hz::Function<void()> Callback)
{
    if (!Handle_)
        return 0;
    uint64_t Id = NextTimerId_++;
    if (!HZ_SUCCESS(NtUserSetTimer(Handle_, Id, PeriodMs)))
        return 0;
    Timers_.Push(TimerSlot{Id, hz::Move(Callback)});
    return Id;
}

void Window::KillTimer(uint64_t Id)
{
    for (size_t i = 0; i < Timers_.Size(); i++) {
        if (Timers_[i].Id == Id) {
            Timers_.Erase(i);
            if (Handle_)
                NtUserKillTimer(Handle_, Id);
            return;
        }
    }
}

/* ---- Pointer --------------------------------------------------------------------------------------- */

void Window::UpdateHover(Element *Target)
{
    if (Target == Hover_)
        return;
    Element *Old = Hover_;
    Hover_ = Target;
    for (Element *E = Old; E; E = E->Parent())
        if (E != Target && !E->IsAncestorOf(Target))
            E->OnPointerLeave();
    for (Element *E = Target; E; E = E->Parent())
        if (E != Old && !E->IsAncestorOf(Old))
            E->OnPointerEnter();
}

void Window::UpdateCursor()
{
    Element *Source = Pressed_ ? Pressed_ : Hover_;
    uint32_t Shape = Source && Source->IsEnabled() ? Source->CursorShape() : HZ_CURSOR_ARROW;
    if (Shape != CursorShape_ && Handle_) {
        CursorShape_ = Shape;
        HzSetCursor(Handle_, Shape);
    }
}

void Window::DispatchPointer(uint32_t Message, uint64_t WParam, uint64_t LParam, const HZ_MSG *)
{
    PointerEvent E;
    E.Position = Point{HZ_POINT_X(LParam), HZ_POINT_Y(LParam)};
    E.ScreenPosition = Point{Origin_.X + E.Position.X, Origin_.Y + E.Position.Y};
    if (Message == HZ_WM_MOUSEWHEEL) {
        E.WheelDelta = (int32_t)(int64_t)WParam;
    } else {
        E.Buttons = (uint32_t)(WParam & 0x7);
        E.Modifiers = (uint16_t)((WParam >> 16) & 0xFFFF);
    }
    if (IsButtonDown(Message) && !Rect{0, 0, Width_, Height_}.Contains(E.Position)) {
        OnPopupPointerOutside(); /* only captured popups see presses outside themselves */
        return;
    }
    UpdateLayout();
    Element *Target = Root_ ? Root_->HitTest(E.Position) : nullptr;

    if (Message == HZ_WM_MOUSEMOVE) {
        UpdateHover(Target);
        if (Pressed_) {
            Pressed_->OnPointerMove(E);
        } else {
            for (Element *El = Target; El && !E.Handled; El = El->Parent())
                if (El->IsEnabled())
                    El->OnPointerMove(E);
        }
        UpdateToolTip(E.ScreenPosition);
        UpdateCursor();
        return;
    }
    if (Message == HZ_WM_MOUSEWHEEL) {
        HideToolTip();
        for (Element *El = Target; El && !E.Handled; El = El->Parent())
            if (El->IsEnabled())
                El->OnPointerWheel(E);
        return;
    }
    E.Button = ButtonOf(Message);
    if (IsButtonDown(Message)) {
        HideToolTip();
        ToolTipSuppressed_ = ToolTipCandidate_;
        E.ClickCount = Message == HZ_WM_LBUTTONDBLCLK ? 2 : 1;
        UpdateHover(Target);
        if (E.Button == HZ_MOUSE_LEFT || E.Button == HZ_MOUSE_RIGHT) {
            Element *Focusable = Target;
            while (Focusable && !(Focusable->Focusable && Focusable->IsEnabled()))
                Focusable = Focusable->Parent();
            if (Focusable)
                SetFocus(Focusable);
            else if (FocusVisible_ && Focus_)
                Focus_->Invalidate();
            FocusVisible_ = false;
        }
        for (Element *El = Target; El; El = El->Parent()) {
            if (El->IsEnabled())
                El->OnPointerDown(E);
            if (E.Handled) {
                if (E.Button == HZ_MOUSE_LEFT) {
                    Pressed_ = El;
                    El->Invalidate();
                }
                break;
            }
        }
        UpdateCursor();
        return;
    }
    if (IsButtonUp(Message)) {
        if (E.Button == HZ_MOUSE_LEFT && Pressed_) {
            Element *Was = Pressed_;
            Pressed_ = nullptr;
            Was->OnPointerUp(E);
            Was->Invalidate();
        } else {
            for (Element *El = Target; El && !E.Handled; El = El->Parent())
                if (El->IsEnabled())
                    El->OnPointerUp(E);
        }
        if (E.Button == HZ_MOUSE_RIGHT && !E.Handled)
            OnContextMenu(E.ScreenPosition);
        UpdateHover(Target);
        UpdateCursor();
    }
}

/* ---- Keyboard and focus ------------------------------------------------------------------------- */

void Window::HandleKey(uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    KeyEvent E;
    E.Modifiers = HZ_KEY_DATA_MODIFIERS(LParam);
    if (Message == HZ_WM_KEYUP)
        return;
    if (Message == HZ_WM_KEYDOWN) {
        DropChar_ = false;
        E.Key = (uint32_t)WParam;
        HideToolTip();
    } else {
        E.Character = (uint32_t)WParam;
    }
    if (Menu_) { /* an open menu takes the keyboard */
        if (Message == HZ_WM_KEYDOWN)
            Menu_->HandlePopupKey(E);
        return;
    }
    if (Message == HZ_WM_CHAR) {
        if (DropChar_) {
            DropChar_ = false;
            return;
        }
        for (Element *El = Focus_; El && !E.Handled; El = El->Parent())
            if (El->IsEnabled())
                El->OnChar(E);
        return;
    }
    for (Element *El = Focus_; El && !E.Handled; El = El->Parent())
        if (El->IsEnabled())
            El->OnKeyDown(E);
    if (!E.Handled && E.Key == HZ_VK_TAB && !E.Ctrl() && !E.Alt()) {
        MoveFocus(!E.Shift());
        E.Handled = true;
    }
    /* Not while the content is disabled (a dialog of this window is open). */
    if (!E.Handled && OnUnhandledKey && (!Root_ || Root_->IsEnabled()))
        OnUnhandledKey(E);
}

void Window::SetFocus(Element *E, bool FromKeyboard)
{
    if (FromKeyboard)
        FocusVisible_ = true;
    if (E == Focus_) {
        if (E)
            E->Invalidate();
        return;
    }
    Element *Old = Focus_;
    Focus_ = E;
    if (Old) {
        Old->OnFocusChanged(false);
        Old->Invalidate();
    }
    if (E) {
        E->OnFocusChanged(true);
        E->Invalidate();
        if (FromKeyboard)
            E->BringIntoView(E->Bounds());
    }
}

bool Window::MoveFocus(bool Forward)
{
    if (!Root_)
        return false;
    hz::Vector<Element *> Order;
    CollectFocusable(Root_, Order);
    if (Order.Empty())
        return false;
    size_t Index = Focus_ ? Order.Find(Focus_) : hz::NotFound;
    size_t Next;
    if (Index == hz::NotFound)
        Next = Forward ? 0 : Order.Size() - 1;
    else
        Next = Forward ? (Index + 1) % Order.Size() : (Index + Order.Size() - 1) % Order.Size();
    SetFocus(Order[Next], true);
    return true;
}

void Window::ForgetElement(Element *E)
{
    auto Gone = [E](Element *X) { return X && (X == E || E->IsAncestorOf(X)); };
    if (Gone(Focus_))
        Focus_ = nullptr;
    if (Gone(Hover_))
        Hover_ = nullptr;
    if (Gone(Pressed_))
        Pressed_ = nullptr;
    if (Gone(ToolTipCandidate_))
        ToolTipCandidate_ = nullptr;
    if (Gone(ToolTipSuppressed_))
        ToolTipSuppressed_ = nullptr;
    if (Gone(ToolTipOwner_))
        HideToolTip();
    Animating_.Remove(E);
}

/* ---- Popups and tooltips -------------------------------------------------------------------------- */

void Window::SetMenu(Window *Menu)
{
    Menu_ = Menu;
}

void Window::ClosePopups()
{
    HideToolTip();
    if (Menu_) {
        Window *Menu = Menu_;
        Menu_ = nullptr;
        Menu->Close();
        DeferDelete(Menu);
    }
}

void Window::UpdateToolTip(Point Screen)
{
    LastScreenPointer_ = Screen;
    Element *Owner = Hover_;
    while (Owner && Owner->ToolTip.Empty())
        Owner = Owner->Parent();
    if (Owner == ToolTipCandidate_)
        return;
    ToolTipCandidate_ = Owner;
    if (ToolTip_ && ToolTipOwner_ != Owner)
        HideToolTip();
    if (ToolTipSuppressed_ && ToolTipSuppressed_ != Owner)
        ToolTipSuppressed_ = nullptr;
    if (Owner && Owner != ToolTipSuppressed_ && Handle_ && !Menu_)
        NtUserSetTimer(Handle_, kToolTipTimer, kToolTipDelayMs);
    else if (Handle_)
        NtUserKillTimer(Handle_, kToolTipTimer);
}

void Window::HideToolTip()
{
    if (Handle_)
        NtUserKillTimer(Handle_, kToolTipTimer);
    if (ToolTip_) {
        Window *Tip = ToolTip_;
        ToolTip_ = nullptr;
        ToolTipOwner_ = nullptr;
        Tip->Close();
        DeferDelete(Tip);
    }
}

PopupWindow::PopupWindow(int32_t Width, int32_t Height)
    : Window("", Width, Height, HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_NOACTIVATE | HZ_WS_VISIBLE)
{
    Background = Theme::PopupBackground;
}

/* ---- Message loop -------------------------------------------------------------------------------- */

void DeferDelete(Window *W)
{
    if (W && !g_Graveyard.Contains(W))
        g_Graveyard.Push(W);
}

void CollectClosedWindows()
{
    while (!g_Graveyard.Empty()) {
        Window *W = g_Graveyard.Back();
        g_Graveyard.Pop();
        delete W;
    }
}

int RunMessageLoop()
{
    HZ_MSG Msg;
    while (HzGetMessage(&Msg, 0, 0, 0)) {
        HzDispatchMessage(&Msg);
        CollectClosedWindows();
    }
    CollectClosedWindows();
    return (int)Msg.WParam;
}

void PumpMessages()
{
    HZ_MSG Msg;
    while (HzPeekMessage(&Msg, 0, 0, 0, HZ_PM_REMOVE)) {
        if (Msg.Message == HZ_WM_QUIT) {
            NtUserPostQuitMessage((int64_t)Msg.WParam); /* leave it for the real loop */
            break;
        }
        HzDispatchMessage(&Msg);
        CollectClosedWindows();
    }
}

}  // namespace hzui
