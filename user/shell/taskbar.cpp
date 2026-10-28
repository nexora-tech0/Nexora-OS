/*
 * Nexora OS - user/shell/taskbar.cpp
 * The top dock, bottom taskbar (Start button and one button per app window), and Alt+Tab switcher.
 */
#include "taskbar.h"

namespace shell {

namespace {

enum class TaskKind : uint8_t { Start, App, PinnedApp, Quick };

uint64_t ClockMinuteKey()
{
    uint64_t Now = 0;
    NtQuerySystemTime(&Now);
    hz::fs::CivilTime T = hz::fs::ToCivil(Now, g_Shell.UtcOffset);
    return (((((uint64_t)T.Year * 13 + T.Month) * 32 + T.Day) * 24 + T.Hour) * 60 + T.Minute);
}

class TaskButton final : public Element {
public:
    TaskButton(TaskKind Kind, HWND Window, const char *Title, const char *AppId = "", bool Compact = false,
               int32_t ButtonSize = 42)
        : Kind_(Kind), Window_(Window), Title_(Title), AppId_(AppId), Compact_(Compact)
    {
        Width = Kind == TaskKind::Quick ? (Compact ? 30 : 64) : ButtonSize;
        Height = Compact ? 28 : (Kind == TaskKind::Quick ? 46 : ButtonSize);
        VerticalAlignment = Align::Center;
        ToolTip = Kind == TaskKind::Start ? "Start" : (Kind == TaskKind::Quick ? "Control Center" : Title);
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        const AppWindow *AppWindow_ = (Kind_ == TaskKind::App || Kind_ == TaskKind::PinnedApp)
                                          ? g_Shell.Find(Window_)
                                          : nullptr;
        const hz::apps::App *Program = Kind_ == TaskKind::PinnedApp ? g_Shell.FindApp(AppId_)
                                                                    : g_Shell.AppForWindow(Window_);
        bool Active = AppWindow_ && g_Shell.Active == Window_ && !AppWindow_->Minimized;
        bool Open = (Kind_ == TaskKind::Start && g_Shell.Start) || (Kind_ == TaskKind::Quick && g_Shell.Quick) ||
                    (Kind_ == TaskKind::PinnedApp && AppWindow_);
        if (Kind_ == TaskKind::App || Kind_ == TaskKind::PinnedApp) {
            if (!ActiveInitialized_ || Active != WasActive_) {
                ActiveInitialized_ = true;
                WasActive_ = Active;
                ActiveIndicator_.To(Active ? 1.0f : 0.0f, Motion::ControlStateMs);
                StartAnimation();
            }
        }
        Color BaseFill = Active || Open ? Theme::ControlFill : 0;
        Color Fill = IsPressed() ? Theme::SubtleFillPressed
                                 : MixColors(BaseFill, Theme::ControlFillHover, Hover_.Value());
        if (Fill)
            P.FillRoundRect(B, Theme::ControlRadius, Fill);
        int32_t IconSize = Compact_ ? 15 : 18;
        int32_t AppIconSize = B.Width < 36 ? 20 : 24;
        int32_t Cy = B.Y + (B.Height - AppIconSize) / 2 -
                     ((Kind_ == TaskKind::App || Kind_ == TaskKind::PinnedApp) ? 2 : 0);
        switch (Kind_) {
        case TaskKind::Start:
            PaintLogo(P, Rect{B.X + (B.Width - AppIconSize) / 2, Cy, AppIconSize, AppIconSize});
            break;
        case TaskKind::App:
        case TaskKind::PinnedApp:
            PaintAppIcon(P, Rect{B.X + (B.Width - AppIconSize) / 2, Cy, AppIconSize, AppIconSize}, Program,
                         Title_.CStr());
            if (AppWindow_) {
                float Indicator = ActiveIndicator_.Value();
                float IndicatorWidth = 4 + Indicator * 8;
                Color IndicatorColor = MixColors(Theme::TextTertiary, Theme::Accent, Indicator);
                P.FillRoundRect(B.X + (B.Width - IndicatorWidth) / 2, (float)B.Bottom() - 3, IndicatorWidth, 2, 1,
                                IndicatorColor);
            }
            break;
        case TaskKind::Quick:
            P.DrawIcon(Icon::Settings, Rect{B.X + (B.Width - IconSize) / 2, B.Y + (B.Height - IconSize) / 2,
                                            IconSize, IconSize},
                       Theme::TextPrimary);
            break;
        }
    }

    void OnPointerEnter() override
    {
        Hover_.To(1, Motion::HoverEnterMs);
        StartAnimation();
    }
    void OnPointerLeave() override
    {
        Hover_.To(0, Motion::HoverLeaveMs);
        StartAnimation();
    }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (!Bounds().Contains(E.Position))
            return;
        if (E.Button == HZ_MOUSE_RIGHT) {
            E.Handled = true;
            ShowContextMenu(E.ScreenPosition);
            return;
        }
        if (E.Button != HZ_MOUSE_LEFT)
            return;
        switch (Kind_) {
        case TaskKind::Start:
            g_Shell.ToggleStart();
            break;
        case TaskKind::App:
            g_Shell.TaskbarClick(Window_);
            break;
        case TaskKind::PinnedApp:
            if (Window_)
                g_Shell.TaskbarClick(Window_);
            else if (const hz::apps::App *Program = g_Shell.FindApp(AppId_))
                g_Shell.Launch(*Program);
            break;
        case TaskKind::Quick:
            g_Shell.ToggleQuick();
            break;
        }
        if (Element *Root = GetWindow() ? GetWindow()->Content() : nullptr)
            Root->Invalidate();
    }

    bool Animate() override
    {
        Invalidate();
        return Hover_.Running() || ActiveIndicator_.Running();
    }

private:
    void ShowContextMenu(Point Screen)
    {
        Window *Owner = GetWindow();
        if (!Owner)
            return;
        if (Kind_ == TaskKind::PinnedApp || Kind_ == TaskKind::App) {
            const hz::apps::App *Program = Kind_ == TaskKind::PinnedApp ? g_Shell.FindApp(AppId_)
                                                                        : g_Shell.AppForWindow(Window_);
            if (Program) {
                ShowAppContextMenu(Owner, *Program, Screen, Window_);
                return;
            }
            hz::Vector<MenuItem> Items;
            if (Window_) {
                HWND Handle = Window_;
                Items.Push(MenuItem::Make("Minimize", [Handle]() { NtUserShowWindow(Handle, HZ_SW_MINIMIZE); }));
                Items.Push(MenuItem::MakeSeparator());
                Items.Push(MenuItem::Make("Close window", [Handle]() { NtUserPostMessage(Handle, HZ_WM_CLOSE, 0, 0); }));
            }
            ShowMenu(Owner, Screen, hz::Move(Items));
            return;
        }
        hz::Vector<MenuItem> Items;
        if (Kind_ == TaskKind::Start) {
            Items.Push(MenuItem::Make("Open Start", []() { g_Shell.ToggleStart(); }));
            if (const hz::apps::App *Settings = g_Shell.FindApp(hz::String("settings")))
                Items.Push(MenuItem::Make("Open Settings", [Program = *Settings]() { g_Shell.Launch(Program); }));
        } else {
            Items.Push(MenuItem::Make("Open Control Center", []() { g_Shell.ToggleQuick(); }));
        }
        ShowMenu(Owner, Screen, hz::Move(Items));
    }

    TaskKind Kind_;
    HWND Window_;
    hz::String Title_;
    hz::String AppId_;
    bool Compact_;
    Transition Hover_;
    Transition ActiveIndicator_;
    bool ActiveInitialized_ = false, WasActive_ = false;
};

class DockMark final : public Element {
public:
    DockMark()
    {
        Width = 22;
        Height = 22;
        VerticalAlignment = Align::Center;
        HitTestVisible = false;
        ToolTip = "Nexora OS desktop";
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.DrawIcon(Icon::Nexora, B, Theme::Accent, 2.2f);
    }
};

class DockClock final : public Element {
public:
    DockClock()
    {
        Width = 136;
        Height = 34;
        VerticalAlignment = Align::Center;
        HitTestVisible = false;
    }

    void Paint(Painter &P) override
    {
        uint64_t Now = 0;
        NtQuerySystemTime(&Now);
        hz::fs::CivilTime T = hz::fs::ToCivil(Now, g_Shell.UtcOffset);
        char Text[32], Weekday[4];
        memcpy(Weekday, hz::fs::WeekdayNames[T.Weekday], 3);
        Weekday[3] = 0;
        HzFormat(Text, sizeof(Text), "%s, %s %u", Weekday, hz::fs::MonthNames[(T.Month - 1) % 12], T.Day);
        HzFormat(Time_, sizeof(Time_), "%02u:%02u", T.Hour, T.Minute);
        const Rect &B = Bounds();
        const Color Ink = Theme::TextPrimary;
        P.TextLine(GetFont(FontRole::BodyStrong), Rect{B.X, B.Y, B.Width, 17}, Time_, Ink, Align::Center);
        P.TextLine(GetFont(FontRole::Caption), Rect{B.X, B.Y + 17, B.Width, 16}, Text,
                   Theme::TextSecondary, Align::Center);
    }

private:
    char Time_[8] = "";
};

class NotificationGlyph final : public Element {
public:
    explicit NotificationGlyph(Icon Glyph) : Glyph_(Glyph)
    {
        Width = 36;
        Height = 36;
    }
    void Paint(Painter &P) override
    {
        Rect R = Bounds();
        P.FillRoundRect(R, 12, Theme::Accent);
        P.DrawIcon(Glyph_, R.Deflate(Thickness(10)), 0xFFFFFFFF, 1.4f);
    }
private:
    Icon Glyph_;
};

class NotificationToast final : public Window {
public:
    NotificationToast(const char *Title, const char *Message, Icon Glyph)
        : Window("Notification", 380, 132,
                 HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_NOACTIVATE | HZ_WS_NOFRAME)
    {
        Background = 0;
        StackPanel *Card = new StackPanel(Orientation::Vertical, 9);
        Card->Padding = Thickness(16, 13, 16, 14);
        Card->Background = Theme::PopupBackground;
        Card->BorderColor = Theme::CardStroke;
        Card->CornerRadius = 20;
        Grid *Header = Card->Add<Grid>();
        Header->Columns.Push(GridLength::AutoSize());
        Header->Columns.Push(GridLength::Stars(1));
        Header->Columns.Push(GridLength::AutoSize());
        Header->ColumnSpacing = 10;
        Header->Add<NotificationGlyph>(Glyph);
        StackPanel *Heading = Header->Add<StackPanel>(Orientation::Vertical, 1);
        Heading->GridColumn = 1;
        Heading->VerticalAlignment = Align::Center;
        TextBlock *Source = Heading->Add<TextBlock>("Nexora", FontRole::Caption);
        Source->Foreground = Theme::TextTertiary;
        TextBlock *Name = Heading->Add<TextBlock>(Title, FontRole::BodyStrong);
        Name->Foreground = Theme::TextPrimary;
        Button *Dismiss = Header->Add<Button>("", ButtonStyle::Subtle);
        Dismiss->GridColumn = 2;
        Dismiss->Glyph = Icon::Close;
        Dismiss->ToolTip = "Dismiss";
        Dismiss->OnClick = [this]() { NtUserPostMessage(Handle(), HZ_WM_CLOSE, 0, 0); };
        TextBlock *Body = Card->Add<TextBlock>(Message, FontRole::Body);
        Body->Foreground = Theme::TextSecondary;
        Body->Wrap = true;
        SetContent(Card);
    }

    void Reveal()
    {
        Reveal_.Jump(0);
        Reveal_.To(1, Motion::FlyoutRevealMs);
        RevealTimer_ = SetTimer(16, [this]() {
            InvalidateRect(ClientRect());
            if (Reveal_.Value() >= 1.0f) {
                KillTimer(RevealTimer_);
                RevealTimer_ = 0;
            }
        });
        SetTimer(5200, [this]() { NtUserPostMessage(Handle(), HZ_WM_CLOSE, 0, 0); });
    }

protected:
    void OnPaint(Painter &P) override
    {
        float T = Reveal_.Value();
        if (T < 1.0f) {
            int32_t Offset = (int32_t)((1.0f - T) * -18.0f);
            Size S = WindowSize();
            P.PushClip(Rect{0, 0, S.Width, S.Height + Offset});
        }
        Window::OnPaint(P);
        if (T < 1.0f)
            P.PopClip();
    }

private:
    Transition Reveal_;
    uint64_t RevealTimer_ = 0;
};

}  // namespace

namespace {
NotificationToast *g_NotificationToast = nullptr;
}

void ShowNotification(const char *Title, const char *Message, Icon Glyph)
{
    if (g_NotificationToast)
        NtUserPostMessage(g_NotificationToast->Handle(), HZ_WM_CLOSE, 0, 0);
    NotificationToast *Toast = new NotificationToast(Title, Message, Glyph);
    g_NotificationToast = Toast;
    Toast->OnClosed = [Toast]() {
        if (g_NotificationToast == Toast)
            g_NotificationToast = nullptr;
    };
    if (!Toast->ShowAt(hz::Max(g_Shell.Screen.Width - 404, 8), kTopDockTop + kTopDockHeight + 12)) {
        if (g_NotificationToast == Toast)
            g_NotificationToast = nullptr;
        delete Toast;
        return;
    }
    Toast->Reveal();
}

Taskbar::Taskbar(int32_t ScreenWidth)
    : Window("Nexora Dock", hz::Min(hz::Max(ScreenWidth - 32, 1), 58), kTaskbarHeight,
             HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_NOACTIVATE | HZ_WS_TRANSPARENT | HZ_WS_NOFRAME),
      ScreenWidth_(ScreenWidth) /* shown after unlocking */
{
    Grid *Root = new Grid();
    Root->Columns.Push(GridLength::AutoSize());
    Root->Padding = Thickness(8, 4);
    Root->Background = Theme::Glass;
    Root->BorderColor = Theme::CardStroke;
    Root->CornerRadius = 23;
    StackPanel *Center = Root->Add<StackPanel>(Orientation::Horizontal, 0);
    Start_ = Center->Add<TaskButton>(TaskKind::Start, (HWND)0, "Start");
    Apps_ = Center->Add<StackPanel>(Orientation::Horizontal, 4);
    Surface_ = Root;
    Background = 0;
    SetContent(Root);
}

void Taskbar::ApplyTheme()
{
    if (Surface_) {
        Surface_->Background = Theme::Glass;
        Surface_->BorderColor = Theme::CardStroke;
        Surface_->Invalidate();
    }
    InvalidateRect(ClientRect());
}

TopDock::TopDock(int32_t ScreenWidth)
    : Window("Nexora Dock", hz::Min(ScreenWidth > 32 ? ScreenWidth - 32 : ScreenWidth, 460), kTopDockHeight,
             HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_NOACTIVATE | HZ_WS_TRANSPARENT | HZ_WS_NOFRAME)
{
    Background = 0;
    Grid *Root = new Grid();
    Root->Background = Theme::PopupBackground;
    Root->BorderColor = Theme::CardStroke;
    Root->CornerRadius = 22;
    Root->Columns.Push(GridLength::AutoSize());
    Root->Columns.Push(GridLength::Stars(1));
    Root->Columns.Push(GridLength::AutoSize());
    Root->Padding = Thickness(12, 0);

    StackPanel *Brand = Root->Add<StackPanel>(Orientation::Horizontal, 8);
    Brand->VerticalAlignment = Align::Center;
    Brand->Padding = Thickness(6, 0);
    Brand->Add<DockMark>();
    TextBlock *Name = Brand->Add<TextBlock>("Nexora OS", FontRole::BodyStrong);
    Name->VerticalAlignment = Align::Center;
    Name->Foreground = Theme::TextPrimary;
    Name_ = Name;

    StackPanel *Tray = Root->Add<StackPanel>(Orientation::Horizontal, 5);
    Tray->GridColumn = 2;
    Tray->HorizontalAlignment = Align::End;
    Tray->VerticalAlignment = Align::Center;
    Tray->Padding = Thickness(6, 0);
    Tray->Background = Theme::ControlFill;
    Tray->CornerRadius = 14;
    Clock_ = Tray->Add<DockClock>();
    Tray->Add<TaskButton>(TaskKind::Quick, (HWND)0, "", "", true);
    Surface_ = Root;
    Brand_ = Brand;
    Tray_ = Tray;
    SetContent(Root);
}

void Taskbar::OnPaint(Painter &P)
{
    float T = Reveal_.Value();
    if (T < 1.0f) {
        int32_t Offset = (int32_t)((1.0f - T) * kTaskbarHeight);
        Size S = WindowSize();
        P.PushClip(Rect{0, Offset, S.Width, S.Height - Offset});
    }
    Window::OnPaint(P);
    if (T < 1.0f)
        P.PopClip();
}

void Taskbar::Reveal()
{
    Reveal_.Jump(0);
    Reveal_.To(1, Motion::FlyoutRevealMs);
    if (RevealTimer_)
        KillTimer(RevealTimer_);
    RevealTimer_ = SetTimer(16, [this]() {
        InvalidateRect(ClientRect());
        if (Reveal_.Value() >= 1.0f) {
            KillTimer(RevealTimer_);
            RevealTimer_ = 0;
        }
    });
    if (!RevealTimer_)
        HzPrint("shell: could not start taskbar reveal animation");
}

void TopDock::Reveal()
{
    Reveal_.Jump(0);
    Reveal_.To(1, Motion::FlyoutRevealMs);
    if (RevealTimer_)
        KillTimer(RevealTimer_);
    RevealTimer_ = SetTimer(16, [this]() {
        InvalidateRect(ClientRect());
        if (Reveal_.Value() >= 1.0f) {
            KillTimer(RevealTimer_);
            RevealTimer_ = 0;
        }
    });
    if (!RevealTimer_)
        HzPrint("shell: could not start top-dock reveal animation");
}

void TopDock::ApplyTheme()
{
    if (Surface_) {
        Surface_->Background = Theme::PopupBackground;
        Surface_->BorderColor = Theme::CardStroke;
    }
    if (Brand_) {
        Brand_->Background = 0;
        Brand_->Invalidate();
    }
    if (Tray_) {
        Tray_->Background = Theme::ControlFill;
        Tray_->Invalidate();
    }
    if (Name_) {
        Name_->Foreground = Theme::TextPrimary;
        Name_->Invalidate();
    }
    InvalidateRect(ClientRect());
}

void TopDock::OnPaint(Painter &P)
{
    float T = Reveal_.Value();
    if (T < 1.0f) {
        int32_t Offset = (int32_t)((1.0f - T) * -kTopDockHeight);
        Size S = WindowSize();
        P.PushClip(Rect{0, 0, S.Width, S.Height + Offset});
    }
    Window::OnPaint(P);
    if (T < 1.0f)
        P.PopClip();
    if (!ClockTimer_ && IsOpen())
        ClockTimer_ = SetTimer(1000, [this, LastMinute = ClockMinuteKey()]() mutable {
            uint64_t Minute = ClockMinuteKey();
            if (Minute != LastMinute) {
                LastMinute = Minute;
                Clock_->Invalidate();
            }
        });
}

int64_t Taskbar::HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (Message == HZ_WM_SHELLNOTIFY) {
        g_Shell.OnShellEvent(WParam, LParam);
        return 0;
    }
    if (Message == HZ_WM_SHELLREQUEST) {
        g_Shell.OnShellRequest(WParam, LParam);
        return 0;
    }
    return Window::HandleMessage(Message, WParam, LParam);
}

void Taskbar::OnContextMenu(Point Screen)
{
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("Open Start", []() { g_Shell.ToggleStart(); }));
    Items.Push(MenuItem::Make("Control Center", []() { g_Shell.ToggleQuick(); }));
    if (const hz::apps::App *Settings = g_Shell.FindApp(hz::String("settings")))
        Items.Push(MenuItem::Make("Open Settings", [Program = *Settings]() { g_Shell.Launch(Program); }));
    ShowMenu(this, Screen, hz::Move(Items));
}

void Taskbar::Rebuild()
{
    Apps_->ClearChildren();
    size_t AppCount = 0;
    for (const hz::String &Id : g_Shell.TaskbarPins)
        if (g_Shell.FindApp(Id))
            AppCount++;
    for (const AppWindow &W : g_Shell.Windows)
        if (!g_Shell.AppForWindow(W.Handle) || !g_Shell.IsTaskbarPinned(W.AppId))
            AppCount++;

    const int32_t Gap = 4;
    const int32_t ButtonCount = (int32_t)AppCount + 1;
    const int32_t MaximumWidth = hz::Max(ScreenWidth_ - 32, 1);
    const int32_t ButtonSize =
        hz::Min(42, hz::Max(20, (MaximumWidth - 16 - Gap * (int32_t)AppCount) / ButtonCount));
    Start_->Width = ButtonSize;
    Start_->Height = ButtonSize;
    Apps_->Padding = AppCount ? Thickness(Gap, 0, 0, 0) : Thickness(0);
    Apps_->Spacing = Gap;
    for (const hz::String &Id : g_Shell.TaskbarPins) {
        const hz::apps::App *Program = g_Shell.FindApp(Id);
        if (!Program)
            continue;
        HWND Window = 0;
        for (const AppWindow &W : g_Shell.Windows)
            if (W.AppId == Id && (!Window || W.Handle == g_Shell.Active))
                Window = W.Handle;
        Apps_->Add<TaskButton>(TaskKind::PinnedApp, Window, Program->Name.CStr(), Program->Id.CStr(), false,
                               ButtonSize);
    }
    for (const AppWindow &W : g_Shell.Windows)
        if (!g_Shell.AppForWindow(W.Handle) || !g_Shell.IsTaskbarPinned(W.AppId))
            Apps_->Add<TaskButton>(TaskKind::App, W.Handle, W.Title.CStr(), "", false, ButtonSize);
    const int32_t Width = hz::Min(MaximumWidth, 16 + ButtonCount * ButtonSize + (ButtonCount - 1) * Gap);
    Size Current = WindowSize();
    int32_t X = (ScreenWidth_ - Width) / 2;
    int32_t Y = g_Shell.Screen.Height - kTaskbarHeight - kTaskbarBottomInset;
    if (Handle() && (Current.Width != Width || ScreenOrigin().X != X || ScreenOrigin().Y != Y)) {
        STATUS Status = NtUserSetWindowPos(Handle(), X, Y, Width, kTaskbarHeight, 0);
        if (!HZ_SUCCESS(Status))
            HzPrint("shell: could not resize taskbar (0x%08X)", (uint32_t)Status);
    }
    Refresh();
}

void Taskbar::Refresh()
{
    if (Content())
        Content()->Invalidate();
}

/* ---- Alt+Tab switcher ---------------------------------------------------------------------------- */

namespace {

constexpr int32_t kTileWidth = 148, kTileHeight = 112, kTileGap = 8, kSwitcherPadding = 16;

class SwitcherView final : public Element {
public:
    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        for (size_t i = 0; i < g_Shell.SwitchList.Size(); i++) {
            Rect Tile{B.X + kSwitcherPadding + (int32_t)i * (kTileWidth + kTileGap), B.Y + kSwitcherPadding, kTileWidth,
                      kTileHeight};
            if (Tile.Right() > B.Right())
                break;
            bool Selected = i == g_Shell.SwitchIndex;
            if (Selected) {
                P.FillRoundRect(Tile, Theme::OverlayRadius, Theme::ControlFillHover);
                P.StrokeRoundRect(Tile, Theme::OverlayRadius, 2, Theme::Accent);
            }
            hz::String Title = g_Shell.TitleOf(g_Shell.SwitchList[i]);
            PaintAppIcon(P, Rect{Tile.X + (kTileWidth - 48) / 2, Tile.Y + 16, 48, 48},
                         g_Shell.AppForWindow(g_Shell.SwitchList[i]), Title.CStr());
            P.TextLine(GetFont(Selected ? FontRole::BodyStrong : FontRole::Body), Rect{Tile.X + 8, Tile.Y + 74, kTileWidth - 16, 24},
                       Title.CStr(), Theme::TextPrimary, Align::Center);
        }
    }
};

}  // namespace

Window *CreateSwitcher()
{
    const Rect &S = g_Shell.Screen;
    int32_t Fit = hz::Max<int32_t>((S.Width - 64 - 2 * kSwitcherPadding + kTileGap) / (kTileWidth + kTileGap), 1);
    int32_t Count = hz::Min<int32_t>((int32_t)g_Shell.SwitchList.Size(), Fit);
    int32_t Width = 2 * kSwitcherPadding + Count * kTileWidth + (Count - 1) * kTileGap;
    int32_t Height = 2 * kSwitcherPadding + kTileHeight;
    Window *Switcher = new Window("Switcher", Width, Height,
                                  HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_NOACTIVATE | HZ_WS_MICA | HZ_WS_VISIBLE);
    Switcher->SetContent(new SwitcherView());
    const Rect &W = g_Shell.Work;
    if (!Switcher->ShowAt(W.X + (W.Width - Width) / 2, W.Y + (W.Height - Height) / 2)) {
        delete Switcher;
        return nullptr;
    }
    g_Shell.Switcher = Switcher;
    return Switcher;
}

}  // namespace shell
