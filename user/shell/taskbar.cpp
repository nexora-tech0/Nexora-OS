/*
 * Nexora OS - user/shell/taskbar.cpp
 * The taskbar (Start button and one button per app window in the middle, quick settings and the
 * clock on the right) and the Alt+Tab switcher.
 */
#include "taskbar.h"

namespace shell {

namespace {

enum class TaskKind : uint8_t { Start, App, PinnedApp, Quick };

class TaskButton final : public Element {
public:
    TaskButton(TaskKind Kind, HWND Window, const char *Title, const char *AppId = "")
        : Kind_(Kind), Window_(Window), Title_(Title), AppId_(AppId)
    {
        Width = Kind == TaskKind::Quick ? 72 : 44;
        Height = 40;
        VerticalAlignment = Align::Center;
        ToolTip = Kind == TaskKind::Start ? "Start" : (Kind == TaskKind::Quick ? "Quick settings" : Title);
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
                ActiveIndicator_.To(Active ? 1.0f : 0.0f, 160);
                StartAnimation();
            }
        }
        Color BaseFill = Active || Open ? Theme::ControlFill : 0;
        Color Fill = IsPressed() ? Theme::SubtleFillPressed
                                 : MixColors(BaseFill, Theme::ControlFillHover, Hover_.Value());
        if (Fill)
            P.FillRoundRect(B, Theme::ControlRadius, Fill);
        int32_t Cy = B.Y + (B.Height - 24) / 2 - ((Kind_ == TaskKind::App || Kind_ == TaskKind::PinnedApp) ? 2 : 0);
        switch (Kind_) {
        case TaskKind::Start:
            PaintLogo(P, Rect{B.X + (B.Width - 24) / 2, Cy, 24, 24});
            break;
        case TaskKind::App:
        case TaskKind::PinnedApp:
            PaintAppIcon(P, Rect{B.X + (B.Width - 24) / 2, Cy, 24, 24}, Program, Title_.CStr());
            if (AppWindow_) {
                float Indicator = ActiveIndicator_.Value();
                float IndicatorWidth = 6 + Indicator * 10;
                Color IndicatorColor = MixColors(Theme::TextTertiary, Theme::Accent, Indicator);
                P.FillRoundRect(B.X + (B.Width - IndicatorWidth) / 2, (float)B.Bottom() - 4, IndicatorWidth, 3, 1.5f,
                                IndicatorColor);
            }
            break;
        case TaskKind::Quick:
            P.DrawIcon(Icon::Volume, Rect{B.X + 14, B.Y + 12, 16, 16}, Theme::TextPrimary);
            P.DrawIcon(g_Shell.Airplane ? Icon::Plane : Icon::Sun, Rect{B.X + 42, B.Y + 12, 16, 16}, Theme::TextPrimary);
            break;
        }
    }

    void OnPointerEnter() override
    {
        Hover_.To(1, 110);
        StartAnimation();
    }
    void OnPointerLeave() override
    {
        Hover_.To(0, 140);
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
            Items.Push(MenuItem::Make("Open quick settings", []() { g_Shell.ToggleQuick(); }));
        }
        ShowMenu(Owner, Screen, hz::Move(Items));
    }

    TaskKind Kind_;
    HWND Window_;
    hz::String Title_;
    hz::String AppId_;
    Transition Hover_;
    Transition ActiveIndicator_;
    bool ActiveInitialized_ = false, WasActive_ = false;
};

class ClockView final : public Element {
public:
    ClockView()
    {
        Width = 84;
        Height = 40;
        VerticalAlignment = Align::Center;
    }

    void Paint(Painter &P) override
    {
        uint64_t Now = 0;
        NtQuerySystemTime(&Now);
        hz::fs::CivilTime T = hz::fs::ToCivil(Now, g_Shell.UtcOffset);
        char Time[16], Date[24];
        HzFormat(Time, sizeof(Time), "%02u:%02u", T.Hour, T.Minute);
        HzFormat(Date, sizeof(Date), "%u/%u/%lld", T.Day, T.Month, T.Year);
        const Rect &B = Bounds();
        HZ_FONT *F = GetFont(FontRole::Caption);
        P.TextLine(F, Rect{B.X, B.Y + 3, B.Width - 8, 17}, Time, Theme::TextPrimary, Align::End);
        P.TextLine(F, Rect{B.X, B.Y + 20, B.Width - 8, 17}, Date, Theme::TextPrimary, Align::End);
        HzFormat(Tip_, sizeof(Tip_), "%s, %u/%u/%lld %s (%s)", hz::fs::WeekdayNames[T.Weekday], T.Day, T.Month, T.Year, Time,
                 hz::fs::FormatUtcOffset(g_Shell.UtcOffset).CStr());
        ToolTip = Tip_;
    }

private:
    char Tip_[64] = "";
};

}  // namespace

Taskbar::Taskbar(int32_t ScreenWidth)
    : Window("Taskbar", ScreenWidth, kTaskbarHeight,
             HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_NOACTIVATE | HZ_WS_MICA | HZ_WS_NOFRAME) /* shown after unlocking */
{
    Grid *Root = new Grid();
    Root->Columns.Push(GridLength::Stars(1));
    Root->Columns.Push(GridLength::AutoSize());
    Root->Columns.Push(GridLength::Stars(1));
    Root->Padding = Thickness(12, 0);
    StackPanel *Center = Root->Add<StackPanel>(Orientation::Horizontal, 4);
    Center->GridColumn = 1;
    Center->Add<TaskButton>(TaskKind::Start, (HWND)0, "Start");
    Apps_ = Center->Add<StackPanel>(Orientation::Horizontal, 4);
    StackPanel *Tray = Root->Add<StackPanel>(Orientation::Horizontal, 4);
    Tray->GridColumn = 2;
    Tray->HorizontalAlignment = Align::End;
    Tray->Add<TaskButton>(TaskKind::Quick, (HWND)0, "");
    Clock_ = Tray->Add<ClockView>();
    SetContent(Root);
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

void Taskbar::OnPaint(Painter &P)
{
    Window::OnPaint(P);
    P.FillRect(Rect{0, 0, WindowSize().Width, 1}, 0x1AFFFFFF);
}

void Taskbar::OnContextMenu(Point Screen)
{
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make("Open Start", []() { g_Shell.ToggleStart(); }));
    Items.Push(MenuItem::Make("Quick settings", []() { g_Shell.ToggleQuick(); }));
    if (const hz::apps::App *Settings = g_Shell.FindApp(hz::String("settings")))
        Items.Push(MenuItem::Make("Open Settings", [Program = *Settings]() { g_Shell.Launch(Program); }));
    ShowMenu(this, Screen, hz::Move(Items));
}

void Taskbar::Rebuild()
{
    Apps_->ClearChildren();
    for (const hz::String &Id : g_Shell.TaskbarPins) {
        const hz::apps::App *Program = g_Shell.FindApp(Id);
        if (!Program)
            continue;
        HWND Window = 0;
        for (const AppWindow &W : g_Shell.Windows)
            if (W.AppId == Id && (!Window || W.Handle == g_Shell.Active))
                Window = W.Handle;
        Apps_->Add<TaskButton>(TaskKind::PinnedApp, Window, Program->Name.CStr(), Program->Id.CStr());
    }
    for (const AppWindow &W : g_Shell.Windows)
        if (!g_Shell.AppForWindow(W.Handle) || !g_Shell.IsTaskbarPinned(W.AppId))
            Apps_->Add<TaskButton>(TaskKind::App, W.Handle, W.Title.CStr());
    if (!ClockTimer_ && IsOpen()) /* once the window exists */
        ClockTimer_ = SetTimer(1000, [this]() { Clock_->Invalidate(); });
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
