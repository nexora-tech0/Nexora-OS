/*
 * Nexora OS - user/shell/flyouts.cpp
 * The Start menu (search, pinned programs, the user and the power menu) and quick settings
 * (toggles, brightness and volume, the wallpaper). Both are activated Mica popups above the
 * taskbar that close when they lose activation or on Escape.
 */
#include "shell.h"
#include "taskbar.h"
#include "hzfile.h"
#include "hzversion.h"

namespace shell {

namespace {

/* ---- Start ---------------------------------------------------------------------------------- */

constexpr int kTileColumns = 5;

/* Launches a program from Start (the menu closes first; the copy outlives it). */
void LaunchFromStart(const hz::apps::App &Program)
{
    hz::apps::App Copy = Program;
    if (g_Shell.Start)
        static_cast<Flyout *>(g_Shell.Start)->Dismiss();
    g_Shell.Launch(Copy);
}

class AppTile final : public Element {
public:
    explicit AppTile(const hz::apps::App &A) : Program_(A)
    {
        Width = 96;
        Height = 88;
        Focusable = true;
        ToolTip = A.Description;
    }

    const hz::apps::App &Target() const { return Program_; }
    int Rank = 0; /* search order: 0 name starts with the text, 1 name contains it, 2 description */

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        if (IsPressed()) {
            P.FillRoundRect(B, Theme::ControlRadius, Theme::SubtleFillPressed);
        } else if (Hover_.Value() > 0) {
            P.FillRoundRect(B, Theme::ControlRadius, ScaleAlpha(Theme::SubtleFillHover, Hover_.Value()));
        }
        PaintAppIcon(P, Rect{B.X + (B.Width - 36) / 2, B.Y + 12, 36, 36}, &Program_, Program_.Name.CStr());
        P.TextLine(GetFont(FontRole::Caption), Rect{B.X + 4, B.Y + 56, B.Width - 8, 20}, Program_.Name.CStr(),
                   Theme::TextPrimary, Align::Center);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds(), Theme::ControlRadius);
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

    bool Animate() override
    {
        Invalidate();
        return Hover_.Running();
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
        if (E.Button == HZ_MOUSE_LEFT)
            Open();
        else if (E.Button == HZ_MOUSE_RIGHT) {
            E.Handled = true;
            if (Window *Owner = GetWindow())
                ShowAppContextMenu(Owner, Program_, E.ScreenPosition);
        }
    }

    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_RETURN || E.Key == HZ_VK_SPACE) {
            E.Handled = true;
            Open();
        }
    }

    void Open() { LaunchFromStart(Program_); }

private:
    hz::apps::App Program_;
    Transition Hover_;
};

/* A recently opened program: icon, name and description on one row. */
class RecentRow final : public Element {
public:
    explicit RecentRow(const hz::apps::App &A) : Program_(A)
    {
        Height = 48;
        Focusable = true;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        if (IsPressed())
            P.FillRoundRect(B, Theme::ControlRadius, Theme::SubtleFillPressed);
        else if (IsHovered())
            P.FillRoundRect(B, Theme::ControlRadius, Theme::SubtleFillHover);
        PaintAppIcon(P, Rect{B.X + 8, B.Y + 8, 32, 32}, &Program_, Program_.Name.CStr());
        P.TextLine(GetFont(FontRole::Body), Rect{B.X + 52, B.Y + 5, B.Width - 60, 20}, Program_.Name.CStr(), Theme::TextPrimary);
        P.TextLine(GetFont(FontRole::Caption), Rect{B.X + 52, B.Y + 25, B.Width - 60, 18}, Program_.Description.CStr(),
                   Theme::TextTertiary);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds(), Theme::ControlRadius);
    }

    void OnPointerEnter() override { Invalidate(); }
    void OnPointerLeave() override { Invalidate(); }
    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }
    void OnPointerUp(PointerEvent &E) override
    {
        if (!Bounds().Contains(E.Position))
            return;
        if (E.Button == HZ_MOUSE_LEFT)
            LaunchFromStart(Program_);
        else if (E.Button == HZ_MOUSE_RIGHT) {
            E.Handled = true;
            if (Window *Owner = GetWindow())
                ShowAppContextMenu(Owner, Program_, E.ScreenPosition);
        }
    }
    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_RETURN || E.Key == HZ_VK_SPACE) {
            E.Handled = true;
            LaunchFromStart(Program_);
        }
    }

private:
    hz::apps::App Program_;
};

/* Places the visible tiles row by row, best rank first (the manifest order within a rank).
   Returns the first one, or null. */
AppTile *FlowTiles(Grid *Tiles, const hz::Vector<AppTile *> &All)
{
    int Index = 0;
    AppTile *First = nullptr;
    for (int Rank = 0; Rank <= 2; Rank++) {
        for (AppTile *Tile : All) {
            if (!Tile->IsVisible() || Tile->Rank != Rank)
                continue;
            if (!First)
                First = Tile;
            Tile->GridRow = (uint8_t)(Index / kTileColumns);
            Tile->GridColumn = (uint8_t)(Index % kTileColumns);
            Index++;
        }
    }
    Tiles->InvalidateLayout();
    return First;
}

/* How well a program matches the search text, -1 if it does not. */
int MatchRank(const hz::apps::App &A, const hz::String &Text)
{
    if (A.Name.Length() >= Text.Length() && hz::fs::EqualNames(A.Name.Substring(0, Text.Length()).CStr(), Text.CStr()))
        return 0;
    if (A.Name.ContainsIgnoreCase(Text.CStr()))
        return 1;
    if (A.Description.ContainsIgnoreCase(Text.CStr()))
        return 2;
    return -1;
}

class Avatar final : public Element {
public:
    Avatar()
    {
        Width = 32;
        Height = 32;
        VerticalAlignment = Align::Center;
        HitTestVisible = false;
    }
    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillCircle(B.X + 16.0f, B.Y + 16.0f, 16, 0xFF5B4BB7);
        char Initial[2] = {g_Shell.DisplayName()[0], 0};
        if (Initial[0] >= 'a' && Initial[0] <= 'z')
            Initial[0] = (char)(Initial[0] - 32);
        P.TextLine(GetFont(FontRole::BodyStrong), B, Initial, 0xFFFFFFFF, Align::Center);
    }
};

/* ---- Quick settings ------------------------------------------------------------------------------ */

class ToggleTile final : public Element {
public:
    ToggleTile(const char *Label, Icon Glyph, bool *State) : Label_(Label), Glyph_(Glyph), State_(State)
    {
        Height = 64;
        Focusable = true;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        bool On = *State_;
        Color Fill = On ? (IsPressed() ? Theme::AccentPressed : (IsHovered() ? Theme::AccentHover : Theme::Accent))
                        : (IsPressed() ? Theme::ControlFillPressed : (IsHovered() ? Theme::ControlFillHover : Theme::ControlFill));
        P.FillRoundRect(B, Theme::ControlRadius, Fill);
        if (!On)
            P.StrokeRoundRect(B, Theme::ControlRadius, 1, Theme::ControlStroke);
        Color Ink = On ? Theme::TextOnAccent : Theme::TextPrimary;
        P.DrawIcon(Glyph_, Rect{B.X + 12, B.Y + 10, 18, 18}, Ink);
        P.TextLine(GetFont(FontRole::Caption), Rect{B.X + 12, B.Y + 36, B.Width - 20, 18}, Label_, Ink);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds(), Theme::ControlRadius);
    }

    void OnPointerEnter() override { Invalidate(); }
    void OnPointerLeave() override { Invalidate(); }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (Bounds().Contains(E.Position))
            Toggle();
    }

    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_SPACE || E.Key == HZ_VK_RETURN) {
            E.Handled = true;
            Toggle();
        }
    }

private:
    void Toggle()
    {
        *State_ = !*State_;
        HzPrint("shell: %s %s", Label_, *State_ ? "on" : "off");
        Invalidate();
        if (g_Shell.Bar && g_Shell.Bar->Content())
            g_Shell.Bar->Content()->Invalidate();
    }

    const char *Label_;
    Icon Glyph_;
    bool *State_;
};

class IconView final : public Element {
public:
    explicit IconView(Icon Glyph) : Glyph_(Glyph)
    {
        Width = 20;
        Height = 20;
        VerticalAlignment = Align::Center;
        HitTestVisible = false;
    }
    void Paint(Painter &P) override { P.DrawIcon(Glyph_, Bounds().Deflate(Thickness(2)), Theme::TextSecondary); }

private:
    Icon Glyph_;
};

/* The kernel's wallpaper palettes (win32k/compose.cpp), for the previews. */
struct Swatch {
    Color Top, Middle, Bottom, Glow;
    const char *Name;
};
const Swatch kSwatches[HZ_MAX_WALLPAPER + 1] = {
    {0xFF071A3D, 0xFF164B8C, 0xFF09255A, 0xFF74C8FF, "Nexora blue"},
    {0xFF050D22, 0xFF0F2A52, 0xFF1C4E74, 0xFF4CC8FF, "Blue hour"},
    {0xFF161028, 0xFF4E2E58, 0xFF9A5466, 0xFFFFC27A, "Dawn"},
};

class WallpaperSwatch final : public Element {
public:
    explicit WallpaperSwatch(uint32_t Index) : Index_(Index)
    {
        Width = 96;
        Height = 56;
        Focusable = true;
        ToolTip = kSwatches[Index].Name;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        const Swatch &S = kSwatches[Index_];
        Rect Inner = B.Deflate(Thickness(3));
        P.FillVerticalGradient(Rect{Inner.X, Inner.Y, Inner.Width, Inner.Height * 6 / 10}, 0, S.Top, S.Middle);
        P.FillVerticalGradient(Rect{Inner.X, Inner.Y + Inner.Height * 6 / 10, Inner.Width, Inner.Height - Inner.Height * 6 / 10},
                               0, S.Middle, S.Bottom);
        P.PushClip(Inner);
        P.FillCircle(Inner.X + Inner.Width / 2.0f, (float)Inner.Bottom() + 10, 24, ScaleAlpha(S.Glow, 0.55f));
        P.PopClip();
        if (g_Shell.Wallpaper == Index_)
            P.StrokeRoundRect(B, 6, 2, Theme::Accent);
        else if (IsHovered())
            P.StrokeRoundRect(B, 6, 1, Theme::ControlStrongStroke);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds(), 6);
    }

    void OnPointerEnter() override { Invalidate(); }
    void OnPointerLeave() override { Invalidate(); }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (Bounds().Contains(E.Position))
            Choose();
    }

    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_SPACE || E.Key == HZ_VK_RETURN) {
            E.Handled = true;
            Choose();
        }
    }

private:
    void Choose()
    {
        g_Shell.SetWallpaper(Index_);
        if (g_Shell.Wallpaper != Index_)
            return;
        HzPrint("shell: wallpaper %u", Index_);
        if (Parent())
            Parent()->Invalidate();
    }

    uint32_t Index_;
};

TextBlock *Heading(Element *Parent, const char *Text)
{
    TextBlock *T = Parent->Add<TextBlock>(Text, FontRole::BodyStrong);
    return T;
}

}  // namespace

Window *CreateStartMenu()
{
    const int32_t Width = 560, Height = 600;
    Flyout *Start = new Flyout("Start", Width, Height, &g_Shell.Start);
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize());
    Root->Rows.Push(GridLength::AutoSize());
    Root->Rows.Push(GridLength::AutoSize());
    Root->Rows.Push(GridLength::Stars(1));
    Root->Rows.Push(GridLength::Pixels(64));

    TextBox *Search = Root->Add<TextBox>("Search for programs");
    Search->Margin = Thickness(24, 24, 24, 0);
    TextBlock *Pinned = Heading(Root, "Pinned");
    Pinned->GridRow = 1;
    Pinned->Margin = Thickness(32, 20, 24, 8);
    Grid *Tiles = Root->Add<Grid>();
    Tiles->GridRow = 2;
    Tiles->Margin = Thickness(24, 0);
    Tiles->ColumnSpacing = 4;
    Tiles->RowSpacing = 4;
    for (int i = 0; i < kTileColumns; i++)
        Tiles->Columns.Push(GridLength::Pixels(96));
    for (size_t i = 0; i < (g_Shell.Apps.Size() + kTileColumns - 1) / kTileColumns; i++)
        Tiles->Rows.Push(GridLength::AutoSize());
    hz::Vector<AppTile *> AllTiles;
    for (const hz::apps::App &A : g_Shell.Apps) {
        AppTile *Tile = Tiles->Add<AppTile>(A);
        Tile->SetVisible(A.Pinned);
        AllTiles.Push(Tile);
    }
    FlowTiles(Tiles, AllTiles);
    StackPanel *Recommended = Root->Add<StackPanel>(Orientation::Vertical, 2);
    Recommended->GridRow = 3;
    Recommended->Margin = Thickness(32, 16, 32, 0);
    Heading(Recommended, "Recent")->Margin = Thickness(0, 0, 0, 6);
    size_t RecentCount = 0;
    for (const hz::String &Id : g_Shell.Recent) {
        if (const hz::apps::App *A = g_Shell.FindApp(Id)) {
            Recommended->Add<RecentRow>(*A);
            RecentCount++;
        }
    }
    if (!RecentCount) {
        TextBlock *Hint = Recommended->Add<TextBlock>("Programs you open will appear here.", FontRole::Body);
        Hint->Wrap = true;
        Hint->Foreground = Theme::TextTertiary;
    }

    Grid *Bottom = Root->Add<Grid>();
    Bottom->GridRow = 4;
    Bottom->Background = 0x26000000;
    Bottom->Padding = Thickness(32, 0, 24, 0);
    Bottom->Columns.Push(GridLength::Stars(1));
    Bottom->Columns.Push(GridLength::AutoSize());
    StackPanel *User = Bottom->Add<StackPanel>(Orientation::Horizontal, 12);
    User->VerticalAlignment = Align::Center;
    User->Add<Avatar>();
    TextBlock *Name = User->Add<TextBlock>(g_Shell.DisplayName(), FontRole::Body);
    Name->VerticalAlignment = Align::Center;
    Button *Power = Bottom->Add<Button>("", ButtonStyle::Subtle);
    Power->GridColumn = 1;
    Power->Glyph = Icon::Power;
    Power->ToolTip = "Power";
    Power->VerticalAlignment = Align::Center;
    Power->OnClick = [Start, Power]() {
        hz::Vector<MenuItem> Items;
        Items.Push(MenuItem::Make("Lock", []() { g_Shell.LockNow(); }));
        Items.Push(MenuItem::Make("Restart", []() { g_Shell.RequestShutdown(HZ_SHUTDOWN_REBOOT); }));
        Items.Push(MenuItem::Make("Shut down", []() { g_Shell.RequestShutdown(HZ_SHUTDOWN_POWER_OFF); }));
        Point O = Start->ScreenOrigin();
        ShowMenu(Start, Point{O.X + Power->Bounds().X - 80, O.Y + Power->Bounds().Y - 3 * 32 - 16}, hz::Move(Items), 140);
    };

    Search->OnTextChanged = [AllTiles, Tiles, Pinned](const hz::String &Text) {
        for (AppTile *Tile : AllTiles) {
            Tile->Rank = Text.Empty() ? 0 : MatchRank(Tile->Target(), Text);
            Tile->SetVisible(Text.Empty() ? Tile->Target().Pinned : Tile->Rank >= 0);
        }
        Pinned->SetText(Text.Empty() ? "Pinned" : "Results");
        FlowTiles(Tiles, AllTiles);
    };
    Search->OnSubmit = [AllTiles, Tiles](const hz::String &) {
        if (AppTile *Best = FlowTiles(Tiles, AllTiles))
            Best->Open();
    };
    Start->SetContent(Root);
    Start->SetFocus(Search);
    const Rect &S = g_Shell.Screen;
    if (!Start->ShowAt((S.Width - Width) / 2, g_Shell.Work.Bottom() - 12 - Height)) {
        g_Shell.Start = nullptr;
        delete Start;
        return nullptr;
    }
    return Start;
}

Window *CreateQuickSettings()
{
    const int32_t Width = 360, Height = 396;
    Flyout *Quick = new Flyout("Quick settings", Width, Height, &g_Shell.Quick);
    StackPanel *Root = new StackPanel(Orientation::Vertical, 12);
    Root->Padding = Thickness(16);

    Grid *Toggles = Root->Add<Grid>();
    Toggles->Columns.Push(GridLength::Stars(1));
    Toggles->Columns.Push(GridLength::Stars(1));
    Toggles->Rows.Push(GridLength::AutoSize());
    Toggles->Rows.Push(GridLength::AutoSize());
    Toggles->ColumnSpacing = 8;
    Toggles->RowSpacing = 8;
    struct {
        const char *Label;
        Icon Glyph;
        bool *State;
    } const Tiles[4] = {{"Night light", Icon::Moon, &g_Shell.NightLight},
                        {"Focus", Icon::Clock, &g_Shell.Focus},
                        {"Energy saver", Icon::Leaf, &g_Shell.EnergySaver},
                        {"Airplane mode", Icon::Plane, &g_Shell.Airplane}};
    for (int i = 0; i < 4; i++) {
        ToggleTile *Tile = Toggles->Add<ToggleTile>(Tiles[i].Label, Tiles[i].Glyph, Tiles[i].State);
        Tile->GridColumn = (uint8_t)(i % 2);
        Tile->GridRow = (uint8_t)(i / 2);
    }

    struct {
        Icon Glyph;
        float *Value;
        const char *Tip;
    } const Sliders[2] = {{Icon::Sun, &g_Shell.Brightness, "Brightness"}, {Icon::Volume, &g_Shell.Volume, "Volume"}};
    for (const auto &S : Sliders) {
        Grid *Row = Root->Add<Grid>();
        Row->Columns.Push(GridLength::Pixels(28));
        Row->Columns.Push(GridLength::Stars(1));
        Row->Add<IconView>(S.Glyph);
        Slider *Bar = Row->Add<Slider>(0, 100, *S.Value);
        Bar->GridColumn = 1;
        Bar->ToolTip = S.Tip;
        float *Value = S.Value;
        Bar->OnValueChanged = [Value](float V) { *Value = V; };
    }

    Heading(Root, "Wallpaper");
    StackPanel *Swatches = Root->Add<StackPanel>(Orientation::Horizontal, 8);
    for (uint32_t i = 0; i <= HZ_MAX_WALLPAPER; i++)
        Swatches->Add<WallpaperSwatch>(i);
    TextBlock *Footer = Root->Add<TextBlock>("Nexora OS " HZ_VERSION_STRING " (" HZ_MILESTONE ")", FontRole::Caption);
    Footer->Foreground = Theme::TextTertiary;

    Quick->SetContent(Root);
    const Rect &S = g_Shell.Screen;
    if (!Quick->ShowAt(S.Width - Width - 12, g_Shell.Work.Bottom() - 12 - Height)) {
        g_Shell.Quick = nullptr;
        delete Quick;
        return nullptr;
    }
    return Quick;
}

}  // namespace shell
