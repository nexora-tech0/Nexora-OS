#include "widgets.h"
#include "hzfile.h"
#include "hzmath.h"
#include "hzsys.h"
#include "shell.h"

namespace shell {

namespace {

constexpr float kCorner = 26.0f;
constexpr int32_t kGap = 16;
const char *const kCatalog[] = {"Clock", "Calendar", "CPU", "RAM", "Battery", "GPU"};

Color GlassFill()
{
    /* Opaque surfaces keep wallpaper pixels from bleeding through as dark compositor blocks. */
    return Theme::Current() == Theme::Scheme::Light ? 0xFFF5F6F8 : 0xFF30333B;
}

Color GlassFillBottom()
{
    return Theme::Current() == Theme::Scheme::Light ? 0xFFEAEDF2 : 0xFF272A32;
}

Color GlassStroke()
{
    return Theme::Current() == Theme::Scheme::Light ? 0x26000000 : 0x38FFFFFF;
}

Color GlassHighlight()
{
    return Theme::Current() == Theme::Scheme::Light ? 0x65FFFFFF : 0x24FFFFFF;
}

} /* namespace */

int32_t WidgetPixelWidth(WidgetSize Size)
{
    switch (Size) {
    case WidgetSize::Small:
        return 164;
    case WidgetSize::Medium:
        return 300;
    default:
        return 320;
    }
}

int32_t WidgetPixelHeight(WidgetSize Size)
{
    switch (Size) {
    case WidgetSize::Small:
        return 156;
    case WidgetSize::Medium:
        return 168;
    default:
        return 280;
    }
}

WidgetSize NextWidgetSize(WidgetSize Size)
{
    if (Size == WidgetSize::Small)
        return WidgetSize::Medium;
    if (Size == WidgetSize::Medium)
        return WidgetSize::Large;
    return WidgetSize::Small;
}

Widget::Widget(WidgetSize Size, const char *Id, const char *Title, Icon Glyph)
    : Size_(Size), Id_(Id), Title_(Title), Glyph_(Glyph)
{
    Padding = Thickness(18, 36, 18, 16);
    SetSizeKind(Size);
}

void Widget::SetSizeKind(WidgetSize Size)
{
    Size_ = Size;
    Width = WidgetPixelWidth(Size);
    Height = WidgetPixelHeight(Size);
    Frame_.Width = Width;
    Frame_.Height = Height;
    InvalidateLayout();
}

void Widget::SetFrame(const Rect &Frame)
{
    Frame_ = Frame;
    Width = Frame.Width;
    Height = Frame.Height;
    InvalidateLayout();
}

void Widget::SetEditing(bool Editing)
{
    if (Editing_ == Editing)
        return;
    Editing_ = Editing;
    Invalidate();
}

Rect Widget::Body() const
{
    return Bounds().Deflate(Padding);
}

Rect Widget::RemoveButtonRect() const
{
    const Rect &B = Bounds();
    return Rect{B.X - 7, B.Y - 7, 24, 24};
}

Rect Widget::SizeHandleRect() const
{
    const Rect &B = Bounds();
    return Rect{B.Right() - 22, B.Bottom() - 22, 18, 18};
}

void Widget::Paint(Painter &P)
{
    const Rect &R = Bounds();
    P.FillRoundRect(Rect{R.X, R.Y + 4, R.Width, R.Height}, kCorner, 0x26000000);
    P.FillVerticalGradient(R, kCorner, GlassFill(), GlassFillBottom());
    P.FillRoundRect(Rect{R.X + 1, R.Y + 1, R.Width - 2, 32}, kCorner - 1, GlassHighlight());
    P.StrokeRoundRect(R, kCorner, 1.0f, GlassStroke());

    Rect Header{R.X + 18, R.Y + 13, R.Width - 36, 18};
    if (Glyph_ != Icon::None)
        P.DrawIcon(Glyph_, Rect{Header.X, Header.Y + 1, 15, 15}, Theme::TextSecondary, 1.25f);
    P.TextLine(GetFont(FontRole::Caption),
               Rect{Header.X + (Glyph_ != Icon::None ? 22 : 0), Header.Y, Header.Width - 22, 18},
               Title_.CStr(), Theme::TextSecondary);
}

void Widget::PaintOverlay(Painter &P)
{
    if (!Editing_)
        return;
    Rect Minus = RemoveButtonRect();
    P.FillCircle((float)(Minus.X + 11), (float)(Minus.Y + 11), 10, 0xFFFF3B30);
    P.FillRect(Rect{Minus.X + 6, Minus.Y + 10, 10, 2}, 0xFFFFFFFF);
    Rect Handle = SizeHandleRect();
    P.FillRect(Rect{Handle.X + 8, Handle.Y + 3, 2, 9}, Theme::TextSecondary);
    P.FillRect(Rect{Handle.X + 4, Handle.Y + 7, 2, 5}, Theme::TextSecondary);
    P.FillRect(Rect{Handle.X, Handle.Y + 11, 2, 1}, Theme::TextSecondary);
}

/* ---- CPU ------------------------------------------------------------------------------------ */

CPUWidget::CPUWidget() : Widget(WidgetSize::Small, "CPU", "CPU", Icon::Chart) {}

void CPUWidget::Paint(Painter &P)
{
    Widget::Paint(P);
    Rect R = Body();
    R.Y += 8;
    R.Height -= 8;

    HZ_SYSTEM_BASIC_INFORMATION Basic = {};
    if (HZ_SUCCESS(NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic), nullptr))) {
        uint64_t Idle = Basic.IdleTimeUs - LastIdle_;
        uint64_t Total = Basic.UptimeUs - LastUptime_;
        if (Total > 0) {
            float IdlePercent = (float)Idle / (float)Total;
            Usage_ = 1.0f - IdlePercent;
            if (Usage_ < 0)
                Usage_ = 0;
            if (Usage_ > 1)
                Usage_ = 1;
        }
        LastIdle_ = Basic.IdleTimeUs;
        LastUptime_ = Basic.UptimeUs;
        if (HistoryCount_ < 24)
            History_[HistoryCount_++] = Usage_;
        else {
            for (int i = 0; i < 23; i++)
                History_[i] = History_[i + 1];
            History_[23] = Usage_;
        }
    }

    hz::String Percent = hz::String::Format("%d%%", (int)(Usage_ * 100.0f + 0.5f));
    P.TextLine(GetFont(Size_ == WidgetSize::Small ? FontRole::Title : FontRole::Display),
               Rect{R.X, R.Y, R.Width, Size_ == WidgetSize::Small ? 48 : 56}, Percent.CStr(),
               Theme::TextPrimary, Align::Start);

    if (Size_ != WidgetSize::Small) {
        P.TextLine(GetFont(FontRole::Caption), Rect{R.X, R.Y + 58, R.Width, 18}, "Share of time not spent idle.",
                   Theme::TextSecondary);
        Rect Graph{R.X, R.Bottom() - 36, R.Width, 28};
        P.FillRoundRect(Graph, 8, ScaleAlpha(Theme::TextPrimary, 0.06f));
        int Count = HistoryCount_ > 0 ? HistoryCount_ : 1;
        float Step = (float)Graph.Width / 24.0f;
        for (int i = 0; i < Count; i++) {
            int32_t H = (int32_t)(History_[i] * (float)(Graph.Height - 4));
            if (H < 1)
                H = 1;
            P.FillRect(Rect{Graph.X + (int32_t)(i * Step) + 1, Graph.Bottom() - 2 - H, hz::Max((int32_t)Step - 2, 1), H},
                       Theme::Accent);
        }
    } else {
        Rect Graph{R.X, R.Bottom() - 10, R.Width, 7};
        P.FillRoundRect(Graph, 3.5f, ScaleAlpha(Theme::TextPrimary, 0.08f));
        P.FillRoundRect(Rect{Graph.X, Graph.Y, (int32_t)(Graph.Width * Usage_), Graph.Height}, 3.5f, Theme::Accent);
    }
}

bool CPUWidget::Animate()
{
    return false;
}

/* ---- RAM ------------------------------------------------------------------------------------ */

RAMWidget::RAMWidget() : Widget(WidgetSize::Small, "RAM", "Memory", Icon::Cube) {}

void RAMWidget::Paint(Painter &P)
{
    Widget::Paint(P);
    Rect R = Body();
    R.Y += 8;
    R.Height -= 8;

    HZ_SYSTEM_BASIC_INFORMATION Basic = {};
    float Usage = 0.0f;
    uint64_t TotalMem = 0, UsedMem = 0;
    if (HZ_SUCCESS(NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic), nullptr))) {
        TotalMem = Basic.TotalPhysicalPages * Basic.PageSize;
        UsedMem = (Basic.TotalPhysicalPages - Basic.AvailablePages) * Basic.PageSize;
        if (TotalMem > 0)
            Usage = (float)UsedMem / (float)TotalMem;
    }

    hz::String Percent = hz::String::Format("%d%%", (int)(Usage * 100.0f + 0.5f));
    P.TextLine(GetFont(FontRole::Title), Rect{R.X, R.Y, R.Width, 48}, Percent.CStr(), Theme::TextPrimary);
    P.TextLine(GetFont(FontRole::Caption), Rect{R.X, R.Y + 50, R.Width, 18},
               hz::String::Format("%s used", hz::fs::FormatSize(UsedMem).CStr()).CStr(), Theme::TextSecondary);
    if (Size_ != WidgetSize::Small)
        P.TextLine(GetFont(FontRole::Caption), Rect{R.X, R.Y + 70, R.Width, 18},
                   hz::String::Format("%s total", hz::fs::FormatSize(TotalMem).CStr()).CStr(), Theme::TextTertiary);

    Rect Graph{R.X, R.Bottom() - 10, R.Width, 7};
    P.FillRoundRect(Graph, 3.5f, ScaleAlpha(Theme::TextPrimary, 0.08f));
    P.FillRoundRect(Rect{Graph.X, Graph.Y, (int32_t)(Graph.Width * Usage), Graph.Height}, 3.5f, 0xFF34C759);
}

/* ---- Calendar ------------------------------------------------------------------------------- */

CalendarWidget::CalendarWidget() : Widget(WidgetSize::Small, "Calendar", "Calendar", Icon::Calendar) {}

void CalendarWidget::Paint(Painter &P)
{
    Widget::Paint(P);
    Rect R = Body();
    R.Y += 6;
    R.Height -= 6;

    uint64_t Now = NowUs();
    hz::fs::CivilTime Civil = hz::fs::ToCivil(Now, g_Shell.UtcOffset);
    P.TextLine(GetFont(FontRole::Caption), Rect{R.X, R.Y, R.Width, 18}, hz::fs::MonthNames[Civil.Month - 1],
               0xFFFF453A, Align::Start);
    hz::String Day = hz::String::Format("%u", Civil.Day);
    P.TextLine(GetFont(FontRole::Display), Rect{R.X, R.Y + 16, Size_ == WidgetSize::Small ? R.Width : 72, 56},
               Day.CStr(), Theme::TextPrimary, Align::Start);
    P.TextLine(GetFont(FontRole::Body), Rect{R.X, R.Y + 74, R.Width, 20}, hz::fs::WeekdayNames[Civil.Weekday],
               Theme::TextSecondary);

    if (Size_ == WidgetSize::Large) {
        static const char *Heads[] = {"S", "M", "T", "W", "T", "F", "S"};
        int32_t Cell = hz::Min(R.Width / 7, 28);
        int32_t GridY = R.Y + 108;
        for (int i = 0; i < 7; i++)
            P.TextLine(GetFont(FontRole::Caption), Rect{R.X + i * Cell, GridY, Cell, 16}, Heads[i], Theme::TextTertiary,
                       Align::Center);
        /* First weekday of this month: Civil.Weekday is today; walk back Civil.Day-1 days. */
        int First = ((int)Civil.Weekday - ((int)Civil.Day - 1) % 7);
        while (First < 0)
            First += 7;
        int Days = 31;
        if (Civil.Month == 2)
            Days = 28;
        else if (Civil.Month == 4 || Civil.Month == 6 || Civil.Month == 9 || Civil.Month == 11)
            Days = 30;
        for (int D = 1; D <= Days; D++) {
            int Slot = First + D - 1;
            int Col = Slot % 7;
            int Row = Slot / 7;
            Rect CellR{R.X + Col * Cell, GridY + 20 + Row * 26, Cell, 24};
            if ((uint32_t)D == Civil.Day)
                P.FillCircle((float)(CellR.X + CellR.Width / 2), (float)(CellR.Y + 12), 11, 0xFFFF453A);
            P.TextLine(GetFont(FontRole::Caption), CellR, hz::String::Format("%d", D).CStr(),
                       (uint32_t)D == Civil.Day ? 0xFFFFFFFF : Theme::TextPrimary, Align::Center);
        }
    }
}

/* ---- Battery / GPU (honest) ----------------------------------------------------------------- */

BatteryWidget::BatteryWidget() : Widget(WidgetSize::Small, "Battery", "Battery", Icon::Power) {}

void BatteryWidget::Paint(Painter &P)
{
    Widget::Paint(P);
    Rect R = Body();
    P.TextLine(GetFont(FontRole::Title), Rect{R.X, R.Y + 18, R.Width, 40}, "—", Theme::TextPrimary, Align::Center);
    P.TextLine(GetFont(FontRole::Caption), Rect{R.X, R.Y + 58, R.Width, 36},
               "No battery sensor is available on this hardware.", Theme::TextSecondary, Align::Center);
}

GPUWidget::GPUWidget() : Widget(WidgetSize::Small, "GPU", "GPU", Icon::Palette) {}

void GPUWidget::Paint(Painter &P)
{
    Widget::Paint(P);
    Rect R = Body();
    P.TextLine(GetFont(FontRole::Title), Rect{R.X, R.Y + 18, R.Width, 40}, "—", Theme::TextPrimary, Align::Center);
    P.TextLine(GetFont(FontRole::Caption), Rect{R.X, R.Y + 58, R.Width, 36},
               "No GPU load counter is exposed yet.", Theme::TextSecondary, Align::Center);
}

/* ---- Clock ---------------------------------------------------------------------------------- */

ClockWidget::ClockWidget() : Widget(WidgetSize::Small, "Clock", "Clock", Icon::Clock) {}

void ClockWidget::Paint(Painter &P)
{
    Widget::Paint(P);
    Rect R = Body();
    uint64_t Now = NowUs();
    hz::fs::CivilTime Civil = hz::fs::ToCivil(Now, g_Shell.UtcOffset);
    hz::String Time = hz::String::Format("%02u:%02u", Civil.Hour, Civil.Minute);
    hz::String Date = hz::String::Format("%s %u", hz::fs::WeekdayNames[Civil.Weekday], Civil.Day);

    if (Size_ == WidgetSize::Large) {
        float Cx = (float)(R.X + R.Width / 2);
        float Cy = (float)(R.Y + 110);
        float Radius = 78.0f;
        P.FillCircle(Cx, Cy, Radius, ScaleAlpha(Theme::TextPrimary, 0.06f));
        P.StrokeCircle(Cx, Cy, Radius, 2.0f, ScaleAlpha(Theme::TextPrimary, 0.25f));
        double MinuteAngle = ((double)Civil.Minute / 60.0) * 2.0 * HZ_PI - HZ_PI / 2;
        double HourAngle = (((double)(Civil.Hour % 12) + (double)Civil.Minute / 60.0) / 12.0) * 2.0 * HZ_PI - HZ_PI / 2;
        P.Line(Cx, Cy, Cx + (float)(HzCos(HourAngle) * 42.0), Cy + (float)(HzSin(HourAngle) * 42.0), 3.0f,
               Theme::TextPrimary);
        P.Line(Cx, Cy, Cx + (float)(HzCos(MinuteAngle) * 62.0), Cy + (float)(HzSin(MinuteAngle) * 62.0), 2.0f,
               Theme::Accent);
        P.FillCircle(Cx, Cy, 4.0f, Theme::Accent);
        P.TextLine(GetFont(FontRole::Subtitle), Rect{R.X, R.Bottom() - 48, R.Width, 24}, Time.CStr(), Theme::TextPrimary,
                   Align::Center);
        P.TextLine(GetFont(FontRole::Caption), Rect{R.X, R.Bottom() - 24, R.Width, 18}, Date.CStr(), Theme::TextSecondary,
                   Align::Center);
        return;
    }

    P.TextLine(GetFont(FontRole::Display), Rect{R.X, R.Y + 8, R.Width, 64}, Time.CStr(), Theme::TextPrimary,
               Size_ == WidgetSize::Small ? Align::Center : Align::Start);
    P.TextLine(GetFont(FontRole::Body), Rect{R.X, R.Y + 72, R.Width, 22}, Date.CStr(), Theme::TextSecondary,
               Size_ == WidgetSize::Small ? Align::Center : Align::Start);
}

bool ClockWidget::Animate()
{
    return false;
}

/* ---- Host ----------------------------------------------------------------------------------- */

WidgetHost::WidgetHost(WidgetSurface Surface) : Surface_(Surface)
{
    HitTestVisible = true;
    RefreshPrefs();
}

Widget *WidgetHost::MakeWidget(const char *Id) const
{
    if (hz::fs::EqualNames(Id, "Clock"))
        return new ClockWidget();
    if (hz::fs::EqualNames(Id, "Calendar"))
        return new CalendarWidget();
    if (hz::fs::EqualNames(Id, "CPU"))
        return new CPUWidget();
    if (hz::fs::EqualNames(Id, "RAM"))
        return new RAMWidget();
    if (hz::fs::EqualNames(Id, "Battery"))
        return new BatteryWidget();
    if (hz::fs::EqualNames(Id, "GPU"))
        return new GPUWidget();
    return nullptr;
}

Widget *WidgetHost::FindWidget(const char *Id) const
{
    for (Widget *W : Widgets_)
        if (hz::fs::EqualNames(W->Id(), Id))
            return W;
    return nullptr;
}

int WidgetHost::DefaultEnabled(const char *Id) const
{
    if (Surface_ == WidgetSurface::Lock)
        return (hz::fs::EqualNames(Id, "Clock") || hz::fs::EqualNames(Id, "Calendar")) ? 1 : 0;
    if (hz::fs::EqualNames(Id, "Battery") || hz::fs::EqualNames(Id, "GPU"))
        return 0;
    return 1;
}

void WidgetHost::AddWidget(Widget *W, bool PlaceInFreeArea)
{
    if (!W)
        return;
    if (PlaceInFreeArea) {
        int32_t Width = WidgetPixelWidth(W->GetSize());
        int32_t Height = WidgetPixelHeight(W->GetSize());
        int32_t MaxX = hz::Max(Bounds().Width - Width, 0);
        int32_t MaxY = hz::Max(Bounds().Height - Height, 0);
        bool Placed = false;
        for (int32_t Y = 0; Y <= MaxY && !Placed; Y += Height + kGap) {
            for (int32_t X = 0; X <= MaxX; X += Width + kGap) {
                Rect Candidate{X, Y, Width, Height};
                bool Occupied = false;
                for (Widget *Existing : Widgets_)
                    if (!Candidate.Intersect(Existing->Frame_).Empty()) {
                        Occupied = true;
                        break;
                    }
                if (!Occupied) {
                    W->SetFrame(Candidate);
                    Placed = true;
                    break;
                }
            }
        }
        if (!Placed)
            W->SetFrame(Rect{MaxX, MaxY, Width, Height});
    }
    if (W->Frame_.Width <= 0) {
        int32_t Col = (int32_t)(Widgets_.Size() % 2);
        int32_t Row = (int32_t)(Widgets_.Size() / 2);
        W->SetFrame(Rect{Col * (WidgetPixelWidth(WidgetSize::Small) + kGap),
                         Row * (WidgetPixelHeight(WidgetSize::Small) + kGap), WidgetPixelWidth(W->GetSize()),
                         WidgetPixelHeight(W->GetSize())});
    }
    Widgets_.Push(W);
    AddChild(W);
    InvalidateLayout();
    StartRefreshTimer();
}

void WidgetHost::RemoveWidget(Widget *W)
{
    if (!W)
        return;
    for (size_t I = 0; I < Widgets_.Size(); I++) {
        if (Widgets_[I] == W) {
            Widgets_.Erase(I);
            if (ActiveWidget_ == W)
                ActiveWidget_ = nullptr;
            RemoveChild(W);
            SaveWidgetPrefs();
            InvalidateLayout();
            return;
        }
    }
}

void WidgetHost::StartRefreshTimer()
{
    Window *Owner = GetWindow();
    if (RefreshTimer_ || !Owner)
        return;
    RefreshTimer_ = Owner->SetTimer(1000, [this]() {
        for (Widget *W : Widgets_)
            W->Invalidate();
    });
    if (!RefreshTimer_)
        HzPrint("shell: could not start widget refresh timer");
}

void WidgetHost::RefreshPrefs()
{
    hz::fs::Settings Prefs;
    STATUS Status = Prefs.Load(hz::fs::SettingsPath("desktop"));
    if (!HZ_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
        HzPrint("shell: cannot read widget settings (0x%08X)", (uint32_t)Status);
        return;
    }

    Widgets_.Clear();
    ClearChildren();
    int Index = 0;
    for (const char *Id : kCatalog) {
        hz::String EnabledKey = hz::String::Format("%s%sEnabled", KeyPrefix(), Id);
        if (Prefs.GetInt(EnabledKey.CStr(), DefaultEnabled(Id)) == 0)
            continue;
        Widget *W = MakeWidget(Id);
        if (!W)
            continue;
        hz::String SizeKey = hz::String::Format("%s%sSize", KeyPrefix(), Id);
        int SizeValue = Prefs.GetInt(SizeKey.CStr(), (int)W->GetSize());
        W->SetSizeKind(SizeValue <= 0 ? WidgetSize::Small : SizeValue == 1 ? WidgetSize::Medium : WidgetSize::Large);
        hz::String XKey = hz::String::Format("%s%sX", KeyPrefix(), Id);
        hz::String YKey = hz::String::Format("%s%sY", KeyPrefix(), Id);
        int32_t X = Prefs.GetInt(XKey.CStr(), (Index % 2) * (WidgetPixelWidth(WidgetSize::Small) + kGap));
        int32_t Y = Prefs.GetInt(YKey.CStr(), (Index / 2) * (WidgetPixelHeight(WidgetSize::Small) + kGap));
        W->SetFrame(Rect{X, Y, WidgetPixelWidth(W->GetSize()), WidgetPixelHeight(W->GetSize())});
        AddWidget(W, true);
        Index++;
    }
    InvalidateLayout();
}

void WidgetHost::Paint(Painter &) {}

Rect WidgetHost::PlusRect() const
{
    const Rect &B = Bounds();
    if (Surface_ == WidgetSurface::Lock)
        return Rect{B.Right() - 52, B.Y + (B.Height - 44) / 2, 44, 44};
    return Rect{B.Right() - 56, B.Y + 8, 44, 44};
}

void WidgetHost::PaintOverlay(Painter &P)
{
    if (!Editing_)
        return;
    Rect Plus = PlusRect();
    P.StrokeRoundRect(Plus, 12, 1.5f, Theme::TextTertiary);
    P.DrawIcon(Icon::Add, Plus.Deflate(Thickness(12)), Theme::TextSecondary, 1.4f);
}

void WidgetHost::ArrangeOverride(const Rect &Content)
{
    StartRefreshTimer();
    if (Surface_ == WidgetSurface::Lock) {
        int32_t Total = 0;
        for (Widget *W : Widgets_)
            Total += WidgetPixelWidth(W->GetSize()) + kGap;
        if (Total > 0)
            Total -= kGap;
        int32_t X = hz::Max((Content.Width - Total) / 2, 0);
        int32_t MaxH = 0;
        for (Widget *W : Widgets_) {
            int32_t Ww = WidgetPixelWidth(W->GetSize());
            int32_t Wh = WidgetPixelHeight(W->GetSize());
            if (Wh > Content.Height)
                Wh = Content.Height;
            W->SetFrame(Rect{X - Content.X, hz::Max((Content.Height - Wh) / 2, 0), Ww, Wh});
            W->Arrange(Rect{Content.X + W->Frame_.X, Content.Y + W->Frame_.Y, Ww, Wh});
            X += Ww + kGap;
            MaxH = hz::Max(MaxH, Wh);
        }
        (void)MaxH;
        return;
    }
    for (Widget *W : Widgets_) {
        int32_t Ww = WidgetPixelWidth(W->GetSize());
        int32_t Wh = WidgetPixelHeight(W->GetSize());
        W->Frame_.Width = Ww;
        W->Frame_.Height = Wh;
        W->Frame_.X = hz::Clamp(W->Frame_.X, 0, hz::Max(Content.Width - Ww, 0));
        W->Frame_.Y = hz::Clamp(W->Frame_.Y, 0, hz::Max(Content.Height - Wh, 0));

        /* Saved widget positions can overlap after a display-size or widget-size change.
           Resolve collisions into the nearest free 16 px grid cell before painting. */
        auto Collides = [this, W](const Rect &Candidate) {
            for (Widget *Placed : Widgets_) {
                if (Placed == W)
                    break;
                if (!Candidate.Intersect(Placed->Frame_).Empty())
                    return true;
            }
            return false;
        };
        Rect Candidate = W->Frame_;
        if (Collides(Candidate)) {
            bool Found = false;
            int32_t MaxX = hz::Max(Content.Width - Ww, 0);
            int32_t MaxY = hz::Max(Content.Height - Wh, 0);
            for (int32_t Y = 0; Y <= MaxY && !Found; Y += kGap) {
                for (int32_t X = 0; X <= MaxX; X += kGap) {
                    Candidate = Rect{X, Y, Ww, Wh};
                    if (!Collides(Candidate)) {
                        Found = true;
                        break;
                    }
                }
            }
            if (Found)
                W->Frame_ = Candidate;
        }
        W->Width = Ww;
        W->Height = Wh;
        W->Arrange(Rect{Content.X + W->Frame_.X, Content.Y + W->Frame_.Y, Ww, Wh});
    }
}

Widget *WidgetHost::WidgetAt(Point Position) const
{
    for (size_t I = Widgets_.Size(); I > 0; I--) {
        Widget *W = Widgets_[I - 1];
        if (W->Bounds().Contains(Position) || (Editing_ && W->RemoveButtonRect().Contains(Position)))
            return W;
    }
    return nullptr;
}

Element *WidgetHost::HitTest(Point P)
{
    if (!IsVisible() || !Bounds().Contains(P))
        return nullptr;
    if (Editing_ && PlusRect().Contains(P))
        return this;
    /* The host owns pointer routing so edit, drag and the gallery run even when the hit is a child. */
    if (WidgetAt(P))
        return this;
    return nullptr;
}

void WidgetHost::ShowGallery(Point Screen)
{
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make(Editing_ ? "Done editing widgets" : "Edit widgets", [this]() {
        Editing_ = !Editing_;
        for (Widget *W : Widgets_)
            W->SetEditing(Editing_);
        Invalidate();
    }));
    Items.Push(MenuItem::MakeSeparator());
    for (const char *Id : kCatalog) {
        bool Present = FindWidget(Id) != nullptr;
        hz::String Label = hz::String::Format(Present ? "Remove %s" : "Add %s", Id);
        Items.Push(MenuItem::Make(Label.CStr(), [this, Id, Present]() {
            if (Present) {
                if (Widget *W = FindWidget(Id))
                    RemoveWidget(W);
                return;
            }
            Widget *W = MakeWidget(Id);
            if (!W)
                return;
            AddWidget(W);
            W->SetEditing(Editing_);
            SaveWidgetPrefs();
        }));
    }
    ShowMenu(GetWindow(), Screen, hz::Move(Items), 220);
}

void WidgetHost::OnPointerDown(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_RIGHT) {
        ShowGallery(E.ScreenPosition);
        E.Handled = true;
        return;
    }
    if (E.Button != HZ_MOUSE_LEFT)
        return;
    if (Editing_ && PlusRect().Contains(E.Position)) {
        ShowGallery(E.ScreenPosition);
        E.Handled = true;
        return;
    }
    Widget *Hit = WidgetAt(E.Position);
    if (!Hit)
        return;
    if (Editing_ && Hit->RemoveButtonRect().Contains(E.Position)) {
        RemoveWidget(Hit);
        E.Handled = true;
        return;
    }
    if (Editing_ && Hit->SizeHandleRect().Contains(E.Position)) {
        Hit->SetSizeKind(NextWidgetSize(Hit->GetSize()));
        SaveWidgetPrefs();
        InvalidateLayout();
        E.Handled = true;
        return;
    }
    if (Editing_) {
        ActiveWidget_ = Hit;
        Rect Frame = Hit->Bounds();
        DragOffset_ = Point{E.Position.X - Frame.X, E.Position.Y - Frame.Y};
        E.Handled = true;
    }
}

void WidgetHost::OnPointerMove(PointerEvent &E)
{
    if (!Editing_ || !ActiveWidget_ || Surface_ == WidgetSurface::Lock)
        return;
    Rect Frame = ActiveWidget_->Frame_;
    Frame.X = E.Position.X - Bounds().X - DragOffset_.X;
    Frame.Y = E.Position.Y - Bounds().Y - DragOffset_.Y;
    ActiveWidget_->SetFrame(Frame);
    InvalidateLayout();
    E.Handled = true;
}

void WidgetHost::OnPointerUp(PointerEvent &E)
{
    if (E.Button != HZ_MOUSE_LEFT || !ActiveWidget_)
        return;
    ActiveWidget_ = nullptr;
    SaveWidgetPrefs();
    E.Handled = true;
}

void WidgetHost::SaveWidgetPrefs()
{
    hz::fs::Settings Prefs;
    STATUS Status = Prefs.Load(hz::fs::SettingsPath("desktop"));
    if (!HZ_SUCCESS(Status) && Status != STATUS_OBJECT_NAME_NOT_FOUND) {
        HzPrint("shell: cannot load widget settings for save (0x%08X)", (uint32_t)Status);
        return;
    }
    for (const char *Id : kCatalog) {
        Widget *W = FindWidget(Id);
        hz::String EnabledKey = hz::String::Format("%s%sEnabled", KeyPrefix(), Id);
        Prefs.SetInt(EnabledKey.CStr(), W ? 1 : 0);
        if (!W)
            continue;
        hz::String XKey = hz::String::Format("%s%sX", KeyPrefix(), Id);
        hz::String YKey = hz::String::Format("%s%sY", KeyPrefix(), Id);
        hz::String SizeKey = hz::String::Format("%s%sSize", KeyPrefix(), Id);
        Prefs.SetInt(XKey.CStr(), W->Frame_.X);
        Prefs.SetInt(YKey.CStr(), W->Frame_.Y);
        Prefs.SetInt(SizeKey.CStr(), (int)W->GetSize());
    }
    Status = Prefs.Save(hz::fs::SettingsPath("desktop"));
    if (!HZ_SUCCESS(Status))
        HzPrint("shell: cannot save widget positions (0x%08X)", (uint32_t)Status);
}

} /* namespace shell */
