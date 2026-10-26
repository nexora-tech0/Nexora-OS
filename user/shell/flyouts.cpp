/*
 * Nexora OS - user/shell/flyouts.cpp
 * The Start menu and Control Center. Both are activated Mica popups that close when they lose
 * activation or on Escape.
 */
#include "shell.h"
#include "taskbar.h"
#include "hzfile.h"
#include "hzversion.h"
#include "hzwallpaper.h"

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
        Hover_.To(1, Motion::HoverEnterMs);
        StartAnimation();
    }
    void OnPointerLeave() override
    {
        Hover_.To(0, Motion::HoverLeaveMs);
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
        else if (Hover_.Value() > 0)
            P.FillRoundRect(B, Theme::ControlRadius, ScaleAlpha(Theme::SubtleFillHover, Hover_.Value()));
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
    Transition Hover_;
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

/* Pinned and All apps are one section - the heading, the switch between the two views and the tile
   grid - so the view lives in the tree and is freed with the window. The search box and the button
   both reach it through this pointer, which is why the state is not a captured value. */
class AppSection final : public Grid
{
public:
    AppSection()
    {
        Rows.Push(GridLength::AutoSize());
        Rows.Push(GridLength::AutoSize());

        Grid *Header = Add<Grid>();
        Header->Columns.Push(GridLength::Stars(1));
        Header->Columns.Push(GridLength::AutoSize());
        Heading_ = Header->Add<TextBlock>("Pinned", FontRole::BodyStrong);
        Heading_->Margin = Thickness(8, 0, 0, 0); /* tiles sit at 24; the heading keeps Start's 32 */
        Heading_->VerticalAlignment = Align::Center;
        Button *Toggle = Header->Add<Button>("All apps", ButtonStyle::Subtle);
        Toggle->GridColumn = 1;
        Toggle->VerticalAlignment = Align::Center;
        Toggle->ToolTip = "Show every installed program";
        Toggle->OnClick = [this, Toggle]() {
            ShowAll_ = !ShowAll_;
            Toggle->SetText(ShowAll_ ? "Pinned" : "All apps");
            Toggle->ToolTip = ShowAll_ ? "Show only the pinned programs" : "Show every installed program";
            Apply();
        };

        Tiles_ = Add<Grid>();
        Tiles_->GridRow = 1;
        Tiles_->ColumnSpacing = 4;
        Tiles_->RowSpacing = 4;
        for (int i = 0; i < kTileColumns; i++)
            Tiles_->Columns.Push(GridLength::Pixels(96));
        for (size_t i = 0; i < (g_Shell.Apps.Size() + kTileColumns - 1) / kTileColumns; i++)
            Tiles_->Rows.Push(GridLength::AutoSize());
        for (const hz::apps::App &A : g_Shell.Apps)
            All_.Push(Tiles_->Add<AppTile>(A));
        Apply();
    }

    /* Narrows the section to what the search text matches; an empty text shows the current view. */
    void Filter(const hz::String &Text)
    {
        Filter_ = Text;
        Apply();
    }

    /* The best matching tile, or null; used by Return in the search box. */
    AppTile *Best() { return FlowTiles(Tiles_, All_); }

private:
    void Apply()
    {
        for (AppTile *Tile : All_) {
            Tile->Rank = Filter_.Empty() ? 0 : MatchRank(Tile->Target(), Filter_);
            Tile->SetVisible(Filter_.Empty() ? (ShowAll_ || Tile->Target().Pinned) : Tile->Rank >= 0);
        }
        FlowTiles(Tiles_, All_);
        /* Searching renames the section, exactly as Start did before this section owned the heading. */
        Heading_->SetText(!Filter_.Empty() ? "Results" : (ShowAll_ ? "All apps" : "Pinned"));
    }

    Grid *Tiles_ = nullptr;
    TextBlock *Heading_ = nullptr;
    hz::Vector<AppTile *> All_;
    hz::String Filter_;
    bool ShowAll_ = false;
};

class Avatar final : public Element {
public:
    Avatar()
    {
        Width = 32;
        Height = 32;
        VerticalAlignment = Align::Center;
        HitTestVisible = false;
    }
    ~Avatar() override { delete[] Picture_; }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        if (PictureReady() && Picture_)
            P.DrawBitmap(B, Picture_, kSize, kSize);
        else {
            P.FillCircle(B.X + 16.0f, B.Y + 16.0f, 16, kDisc);
            char Initial[2] = {g_Shell.DisplayName()[0], 0};
            if (Initial[0] >= 'a' && Initial[0] <= 'z')
                Initial[0] = (char)(Initial[0] - 32);
            P.TextLine(GetFont(FontRole::BodyStrong), B, Initial, 0xFFFFFFFF, Align::Center);
        }
    }

private:
    static constexpr int32_t kSize = 32;
    static constexpr Color kDisc = 0xFF5B4BB7; /* also the matte outside the picture's disc */

    /* Decodes once per chosen path and remembers the result; a broken file falls back to initials. */
    bool PictureReady()
    {
        if (Path_ == g_Shell.ProfilePicture)
            return Picture_;
        Path_ = g_Shell.ProfilePicture;
        delete[] Picture_;
        Picture_ = nullptr;
        if (Path_.Empty())
            return false;
        uint32_t *Buffer = new uint32_t[(size_t)kSize * kSize];
        if (HZ_SUCCESS(hz::wallpaper::MakeRoundPicture(Path_.CStr(), kSize, kDisc, Buffer)))
            Picture_ = Buffer;
        else
            delete[] Buffer;
        return Picture_;
    }

    hz::String Path_;
    uint32_t *Picture_ = nullptr;
};

/* ---- Control Center ----------------------------------------------------------------------------- */

/* How far the content travels while it slides into place. hzui has no opacity layer, so the motion
   is the content's own position: it starts this much lower and moves up, with the window's clip
   hiding the far edge until it arrives. */
constexpr int32_t kRevealSlide = 16;

class RevealHost final : public Element {
public:
    /* Starts the slide. The content must already be attached to its window. */
    void Begin()
    {
        Reveal_.Jump(0);
        Reveal_.To(1.0f, Motion::FlyoutRevealMs);
        StartAnimation();
    }

    void ArrangeOverride(const Rect &Content) override { Slide(Content); }

    bool Animate() override
    {
        if (ChildCount())
            Slide(ContentBounds()); /* the slide continues between layout passes */
        if (Window *W = GetWindow())
            W->InvalidateRect(W->ClientRect());
        return Reveal_.Running();
    }

private:
    void Slide(const Rect &Content)
    {
        int32_t Offset = (int32_t)((1.0f - Reveal_.Value()) * kRevealSlide);
        ChildAt(0)->Arrange(Rect{Content.X, Content.Y + Offset, Content.Width, Content.Height});
    }

    Transition Reveal_;
};

/* Compact fallback tints for the quick-settings wallpaper picker. */
struct Swatch {
    Color Top, Middle, Bottom, Glow;
};
const Swatch kFallbackSwatches[hz::wallpaper::Count] = {
    {0xFF153D2C, 0xFF285840, 0xFF102A36, 0xFF85D7A6},
    {0xFF24455D, 0xFF356D89, 0xFF162B42, 0xFF8DD2E8},
    {0xFF272638, 0xFF56516A, 0xFF242C42, 0xFFE4A68A},
    {0xFF344332, 0xFF65744A, 0xFF273841, 0xFFD7BA79},
    {0xFF25334A, 0xFF56677B, 0xFF28394B, 0xFFE9C4B0},
};

class WallpaperSwatch final : public Element {
public:
    WallpaperSwatch(uint32_t Index, int32_t SwatchWidth) : Index_(Index)
    {
        Width = SwatchWidth;
        ImageHeight_ = HZ_MAX(1, HZ_MIN(35, HZ_MAX(1, SwatchWidth - 6) * 9 / 16));
        Height = ImageHeight_ + 44;
        Focusable = true;
        const hz::wallpaper::Info *Info = hz::wallpaper::GetInfo(Index);
        if (!Info)
            return;
        ToolTip = Info->Name;
        STATUS LoadStatus = hz::wallpaper::LoadPreview(Index, &Preview_, &PreviewWidth_, &PreviewHeight_);
        if (!HZ_SUCCESS(LoadStatus))
            HzPrint("shell: cannot load wallpaper preview %u (0x%08X)", Index, (uint32_t)LoadStatus);
    }

    ~WallpaperSwatch() override { free(Preview_); }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        if (Index_ >= hz::wallpaper::Count)
            return;
        const Swatch &S = kFallbackSwatches[Index_];
        const hz::wallpaper::Info *Info = hz::wallpaper::GetInfo(Index_);
        if (!Info)
            return;
        Rect Image{B.X + 3, B.Y + 3, B.Width - 6, ImageHeight_};
        P.FillRoundRect(B, Theme::ControlRadius, g_Shell.Wallpaper == Index_ ? Theme::ControlFillHover : Theme::ControlFill);
        P.PushClip(Image);
        if (Preview_) {
            P.DrawBitmap(Image, Preview_, PreviewWidth_, PreviewHeight_);
        } else {
            P.FillVerticalGradient(Rect{Image.X, Image.Y, Image.Width, Image.Height * 6 / 10}, 0, S.Top, S.Middle);
            P.FillVerticalGradient(Rect{Image.X, Image.Y + Image.Height * 6 / 10, Image.Width,
                                        Image.Height - Image.Height * 6 / 10},
                                   0, S.Middle, S.Bottom);
            P.FillCircle(Image.X + Image.Width / 2.0f, (float)Image.Bottom() + 10, 24, ScaleAlpha(S.Glow, 0.55f));
        }
        P.PopClip();
        if (g_Shell.Wallpaper == Index_)
            P.StrokeRoundRect(B, Theme::ControlRadius, 2, Theme::Accent);
        else if (IsHovered())
            P.StrokeRoundRect(B, Theme::ControlRadius, 1, Theme::ControlStrongStroke);
        char Lines[3][32] = {};
        size_t LineCount = 1;
        size_t MaxChars = HZ_MAX(1, (B.Width - 4) / 6);
        for (const char *Word = Info->Name; *Word;) {
            while (*Word == ' ')
                Word++;
            if (!*Word)
                break;
            const char *End = Word;
            while (*End && *End != ' ')
                End++;
            size_t WordLength = (size_t)(End - Word);
            char *Line = Lines[LineCount - 1];
            size_t CurrentLength = strlen(Line);
            if (CurrentLength && CurrentLength + 1 + WordLength > MaxChars && LineCount < 3) {
                Line = Lines[LineCount++];
                CurrentLength = 0;
            }
            if (CurrentLength && CurrentLength + 1 < sizeof(Lines[0]))
                Line[CurrentLength++] = ' ';
            size_t CopyLength = HZ_MIN(WordLength, sizeof(Lines[0]) - CurrentLength - 1);
            memcpy(Line + CurrentLength, Word, CopyLength);
            Line[CurrentLength + CopyLength] = 0;
            Word = End;
        }
        for (size_t Line = 0; Line < LineCount; Line++)
            P.TextLine(GetFont(FontRole::Caption), Rect{B.X + 2, B.Y + ImageHeight_ + 2 + (int32_t)Line * 13,
                                                        B.Width - 4, 13},
                       Lines[Line], Theme::TextPrimary, Align::Center);
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
    uint32_t *Preview_ = nullptr;
    int32_t PreviewWidth_ = 0, PreviewHeight_ = 0;
    int32_t ImageHeight_ = 0;
};

TextBlock *Heading(Element *Parent, const char *Text)
{
    TextBlock *T = Parent->Add<TextBlock>(Text, FontRole::BodyStrong);
    return T;
}

StackPanel *ControlCard(Element *Parent, const char *Title)
{
    StackPanel *Card = Parent->Add<StackPanel>(Orientation::Vertical, 8);
    Card->Padding = Thickness(12);
    Card->Background = Theme::FlyoutBackground;
    Card->BorderColor = Theme::CardStroke;
    Card->CornerRadius = 16;
    Heading(Card, Title);
    return Card;
}

class ControlGlyph final : public Element {
public:
    explicit ControlGlyph(Icon Glyph, bool Available = true) : Glyph_(Glyph), Available_(Available)
    {
        Width = Height = 38;
        VerticalAlignment = Align::Center;
        /* An unavailable service keeps a muted tile: dim grey on dark, dim grey on light. */
        Background = Available ? 0xFF566BD0
                               : (Theme::Current() == Theme::Scheme::Light ? 0xFFD9DBE1 : 0xFF454852);
        CornerRadius = 19;
        HitTestVisible = false;
    }

    void Paint(Painter &P) override
    {
        Element::Paint(P);
        const Rect &B = Bounds();
        P.DrawIcon(Glyph_, Rect{B.X + 10, B.Y + 10, B.Width - 20, B.Height - 20},
                   Available_ ? 0xFFF5F6FA : 0xFF999CA5);
    }

private:
    Icon Glyph_;
    bool Available_;
};

StackPanel *ControlStatusRow(Element *Parent, Icon Glyph, const char *Title, const char *Detail, bool Available = true)
{
    StackPanel *Row = Parent->Add<StackPanel>(Orientation::Horizontal, 10);
    Row->Padding = Thickness(7, 5);
    Row->Background = Theme::HoverRow;
    Row->CornerRadius = 13;
    Row->Add<ControlGlyph>(Glyph, Available);
    StackPanel *Text = Row->Add<StackPanel>(Orientation::Vertical, 1);
    Text->VerticalAlignment = Align::Center;
    Text->Add<TextBlock>(Title, FontRole::BodyStrong);
    TextBlock *Description = Text->Add<TextBlock>(Detail, FontRole::Caption);
    Description->Foreground = Theme::TextSecondary;
    Description->Wrap = true;
    return Row;
}

/* The same row, but the service really can be switched: the label is on the left and a switch sits
   on the right, so the row's own state is what the switch shows. */
Grid *ControlToggleRow(Element *Parent, Icon Glyph, const char *Title, const char *Detail, bool Value,
                       hz::Function<void(bool)> OnChanged)
{
    Grid *Row = Parent->Add<Grid>();
    Row->Padding = Thickness(7, 5);
    Row->Background = Theme::HoverRow;
    Row->CornerRadius = 13;
    Row->Columns.Push(GridLength::Stars(1));
    Row->Columns.Push(GridLength::AutoSize());
    StackPanel *Text = Row->Add<StackPanel>(Orientation::Horizontal, 10);
    Text->Add<ControlGlyph>(Glyph);
    StackPanel *Labels = Text->Add<StackPanel>(Orientation::Vertical, 1);
    Labels->VerticalAlignment = Align::Center;
    Labels->Add<TextBlock>(Title, FontRole::BodyStrong);
    TextBlock *Description = Labels->Add<TextBlock>(Detail, FontRole::Caption);
    Description->Foreground = Theme::TextSecondary;
    ToggleSwitch *Switch = new ToggleSwitch("", "");
    Switch->SetOn(Value, false); /* set before it is attached: the first state does not animate in */
    Switch->OnToggled = hz::Move(OnChanged);
    Switch->VerticalAlignment = Align::Center;
    Switch->HorizontalAlignment = Align::End;
    Switch->GridColumn = 1;
    Row->AddChild(Switch);
    return Row;
}

class QuickAction final : public Element {
public:
    QuickAction(const char *Title, Icon Glyph, hz::Function<void()> Action)
        : Title_(Title), Glyph_(Glyph), Action_(hz::Move(Action))
    {
        Width = 58;
        Height = 68;
        Focusable = true;
        ToolTip = Title;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        float Hover = Hover_.Value();
        float Radius = 22.0f + Hover * 2.0f;
        float CenterX = B.X + B.Width / 2.0f;
        float CenterY = B.Y + 23.0f;
        P.FillCircle(CenterX, CenterY, Radius, MixColors(0xFF424651, 0xFF5A6272, Hover));
        P.StrokeCircle(CenterX, CenterY, Radius, 1.0f, 0x26FFFFFF);
        P.DrawIcon(Glyph_, Rect{(int32_t)CenterX - 8, (int32_t)CenterY - 8, 16, 16}, 0xFFF1F2F6);
        P.TextLine(GetFont(FontRole::Caption), Rect{B.X - 4, B.Y + 48, B.Width + 8, 18}, Title_.CStr(),
                   Theme::TextSecondary, Align::Center);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds(), 10);
    }

    void OnPointerEnter() override
    {
        Hover_.To(1.0f, Motion::HoverEnterMs);
        StartAnimation();
    }

    void OnPointerLeave() override
    {
        Hover_.To(0.0f, Motion::HoverLeaveMs);
        StartAnimation();
    }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT && Bounds().Contains(E.Position) && Action_)
            Action_();
    }

    void OnKeyDown(KeyEvent &E) override
    {
        if ((E.Key == HZ_VK_RETURN || E.Key == HZ_VK_SPACE) && Action_) {
            E.Handled = true;
            Action_();
        }
    }

    bool Animate() override
    {
        Invalidate();
        return Hover_.Running();
    }

private:
    hz::String Title_;
    Icon Glyph_;
    hz::Function<void()> Action_;
    Transition Hover_;
};

hz::String NetworkStatus()
{
    HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION Interfaces[HZ_MAX_NETWORK_INTERFACES] = {};
    uint32_t Returned = 0;
    STATUS Status = NtQuerySystemInformation(HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS, Interfaces,
                                             sizeof(Interfaces), &Returned);
    if (!HZ_SUCCESS(Status))
        return hz::String::Format("Status unavailable (0x%08X)", (uint32_t)Status);
    if (Returned > sizeof(Interfaces) || Returned % sizeof(Interfaces[0])) {
        HzPrint("shell: malformed network information length %u", Returned);
        return "Adapter information unavailable";
    }
    uint32_t Count = Returned / sizeof(Interfaces[0]);
    if (!Count)
        return "No supported adapter detected";
    const HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION &Interface = Interfaces[0];
    return hz::String::Format("%s - %s", Interface.Name, Interface.LinkUp ? "Connected" : "Disconnected");
}

void ChangeWallpaper(int Direction)
{
    if (!hz::wallpaper::Count)
        return;
    uint32_t Current = g_Shell.Wallpaper % hz::wallpaper::Count;
    uint32_t Next = Direction < 0 ? (Current + hz::wallpaper::Count - 1) % hz::wallpaper::Count
                                 : (Current + 1) % hz::wallpaper::Count;
    g_Shell.SetWallpaper(Next);
    if (g_Shell.Wallpaper == Next)
        HzPrint("shell: wallpaper %u", Next);
}

}  // namespace

Window *CreateStartMenu()
{
    const Rect &S = g_Shell.Screen;
    const int32_t Width = HZ_MIN(560, HZ_MAX(1, S.Width - 24));
    const int32_t Height = HZ_MIN(600, HZ_MAX(1, g_Shell.Work.Height));
    Flyout *Start = new Flyout("Start", Width, Height, &g_Shell.Start);
    Grid *Root = new Grid();
    Root->Rows.Push(GridLength::AutoSize()); /* the search box */
    Root->Rows.Push(GridLength::Stars(1));   /* scrollable apps and recent sections */
    Root->Rows.Push(GridLength::Pixels(64)); /* the user and power band */

    TextBox *Search = Root->Add<TextBox>("Search for programs");
    Search->Margin = Thickness(24, 24, 24, 0);
    ScrollViewer *Content = Root->Add<ScrollViewer>();
    Content->GridRow = 1;
    StackPanel *Sections = new StackPanel(Orientation::Vertical);
    Content->SetContent(Sections);
    AppSection *Apps = Sections->Add<AppSection>();
    Apps->Margin = Thickness(24, 18, 24, 8);
    StackPanel *Recommended = Sections->Add<StackPanel>(Orientation::Vertical, 2);
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
    Bottom->GridRow = 3;
    Bottom->Background = Theme::FooterBand;
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

    Search->OnTextChanged = [Apps](const hz::String &Text) { Apps->Filter(Text); };
    Search->OnSubmit = [Apps](const hz::String &) {
        if (AppTile *Best = Apps->Best())
            Best->Open();
    };
    /* The tiles slide into place once the window - and so its clip - exists. */
    RevealHost *Host = new RevealHost();
    Host->AddChild(Root);
    Start->SetContent(Host);
    Start->SetFocus(Search);
    if (!Start->ShowAt((S.Width - Width) / 2, g_Shell.Work.Bottom() - 12 - Height)) {
        g_Shell.Start = nullptr;
        delete Start;
        return nullptr;
    }
    Host->Begin();
    return Start;
}

Window *CreateQuickSettings()
{
    const Rect &S = g_Shell.Screen;
    const int32_t Width = HZ_MIN(660, HZ_MAX(1, S.Width - 24));
    /* The Appearance card (three real switches) made the right column the taller one, so the flyout
       is a little taller than the network/audio-only version was. */
    const int32_t Height = HZ_MIN(640, HZ_MAX(1, g_Shell.Work.Height));
    Flyout *Quick = new Flyout("Control Center", Width, Height, &g_Shell.Quick);
    Grid *Root = new Grid();
    Root->Columns.Push(GridLength::Stars(1));
    Root->Columns.Push(GridLength::Stars(1));
    Root->Rows.Push(GridLength::Stars(1));
    Root->Rows.Push(GridLength::AutoSize());
    Root->ColumnSpacing = 10;
    Root->RowSpacing = 10;
    Root->Padding = Thickness(14);

    StackPanel *Left = Root->Add<StackPanel>(Orientation::Vertical, 10);
    Left->GridColumn = 0;
    StackPanel *Network = ControlCard(Left, "Network");
    ControlStatusRow(Network, Icon::Wifi, "Ethernet", NetworkStatus().CStr());
    /* Both of these really are missing: no 802.11/Bluetooth driver exists, and audio has no mixer.
       The rows stay, but they say so rather than pretending to switch something. */
    ControlStatusRow(Network, Icon::Bluetooth, "Bluetooth", "Not available in this build", false);

    StackPanel *Devices = ControlCard(Left, "Audio & display");
    ControlStatusRow(Devices, Icon::Volume, "Volume", "Audio controls are not implemented", false);
    ControlStatusRow(Devices, Icon::Sun, "Brightness", "Display brightness is not adjustable", false);

    StackPanel *Focus = ControlCard(Left, "Focus");
    ControlStatusRow(Focus, Icon::Moon, "Focus mode", "Not available in this build", false);

    StackPanel *Right = Root->Add<StackPanel>(Orientation::Vertical, 10);
    Right->GridColumn = 1;
    /* These three switches really work: each one applies the change to the shell at once and saves
       it to shell.ini, so it survives a restart. */
    StackPanel *Appearance = ControlCard(Right, "Appearance");
    ControlToggleRow(Appearance, Icon::Sun, "Light mode", "Applies to the whole shell", g_Shell.LightMode,
                     [](bool On) { g_Shell.SetLightMode(On); });
    ControlToggleRow(Appearance, Icon::AppWindow, "Top dock", "Show the dock along the top", g_Shell.ShowTopDock,
                     [](bool On) { g_Shell.SetTopDock(On); });
    ControlToggleRow(Appearance, Icon::Folder, "Desktop icons", "Show icons on the desktop", g_Shell.ShowDesktopIcons,
                     [](bool On) { g_Shell.SetDesktopIcons(On); });

    StackPanel *Actions = ControlCard(Right, "Quick actions");
    StackPanel *ActionRow = Actions->Add<StackPanel>(Orientation::Horizontal, 4);
    ActionRow->HorizontalAlignment = Align::Center;
    ActionRow->Add<QuickAction>("Previous", Icon::ChevronLeft, []() { ChangeWallpaper(-1); });
    ActionRow->Add<QuickAction>("Next", Icon::ChevronRight, []() { ChangeWallpaper(1); });
    ActionRow->Add<QuickAction>("Settings", Icon::Settings, []() {
        if (g_Shell.Quick)
            static_cast<Flyout *>(g_Shell.Quick)->Dismiss();
        if (const hz::apps::App *Settings = g_Shell.FindApp(hz::String("settings")))
            g_Shell.Launch(*Settings);
        else
            HzPrint("shell: Settings is not installed");
    });
    ActionRow->Add<QuickAction>("Lock", Icon::Person, []() { g_Shell.LockNow(); });
    TextBlock *PowerInfo = Actions->Add<TextBlock>("Battery and power status unavailable", FontRole::Caption);
    PowerInfo->Foreground = Theme::TextTertiary;

    StackPanel *Wallpaper = ControlCard(Root, "Desktop wallpaper");
    Wallpaper->GridRow = 1;
    Wallpaper->GridColumnSpan = 2;
    StackPanel *Swatches = Wallpaper->Add<StackPanel>(Orientation::Horizontal, 6);
    int32_t SwatchWidth = HZ_MIN(88, HZ_MAX(38, (Width - 58 - 24) / (int32_t)hz::wallpaper::Count));
    for (uint32_t i = 0; i < hz::wallpaper::Count; i++)
        Swatches->Add<WallpaperSwatch>(i, SwatchWidth);
    TextBlock *Hint = Wallpaper->Add<TextBlock>("Choose a wallpaper to apply it to the desktop.", FontRole::Caption);
    Hint->Foreground = Theme::TextSecondary;

    /* The content slides into place once the window - and so its clip - exists, so the reveal is
       started after ShowAt, not before. */
    RevealHost *Host = new RevealHost();
    Host->AddChild(Root);
    Quick->SetContent(Host);
    int32_t X = HZ_MAX(0, S.Width - Width - 12);
    int32_t Y = HZ_MIN(kTopDockTop + kTopDockHeight + 8, S.Height - kTaskbarHeight - Height);
    if (!Quick->ShowAt(X, Y)) {
        g_Shell.Quick = nullptr;
        delete Quick;
        return nullptr;
    }
    Host->Begin();
    return Quick;
}

}  // namespace shell
