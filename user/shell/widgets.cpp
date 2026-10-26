#include "widgets.h"
#include "hzsys.h"
#include "hzfile.h"
#include "shell.h"

namespace shell {

/* ---- CPU Widget ----------------------------------------------------------------------------- */

CPUWidget::CPUWidget() : Widget(WidgetSize::Small) {
    SetTimer(1000, [this]() { Invalidate(); });
}

void CPUWidget::Paint(Painter& P) {
    Widget::Paint(P);
    Rect R = Bounds().Deflate(Padding);
    
    HZ_SYSTEM_BASIC_INFORMATION Basic;
    if (HZ_SUCCESS(NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic), nullptr))) {
        uint64_t Idle = Basic.IdleTimeUs - LastIdle_;
        uint64_t Total = Basic.UptimeUs - LastUptime_;
        if (Total > 0) {
            float IdlePercent = (float)Idle / (float)Total;
            Usage_ = 1.0f - IdlePercent;
        }
        LastIdle_ = Basic.IdleTimeUs;
        LastUptime_ = Basic.UptimeUs;
    }

    HZ_FONT* F = GetFont(FontRole::Caption);
    HZ_FONT* Large = GetFont(FontRole::Title);
    
    P.TextLine(F, Rect{R.X, R.Y, R.Width, 20}, "CPU", Theme::TextSecondary, Align::Start);
    
    hz::String Percent = hz::String::Format("%d%%", (int)(Usage_ * 100.0f));
    P.TextLine(Large, Rect{R.X, R.Y + 20, R.Width, R.Height - 40}, Percent.CStr(), Theme::TextPrimary, Align::Center);
    
    /* Small graph at the bottom */
    Rect Graph{R.X, R.Bottom() - 10, R.Width, 4};
    P.FillRect(Graph, 0x30FFFFFF);
    P.FillRect(Rect{Graph.X, Graph.Y, (int32_t)(Graph.Width * Usage_), Graph.Height}, Theme::Accent);
}

bool CPUWidget::Animate() {
    return false;
}

/* ---- RAM Widget ----------------------------------------------------------------------------- */

RAMWidget::RAMWidget() : Widget(WidgetSize::Small) {
    SetTimer(2000, [this]() { Invalidate(); });
}

void RAMWidget::Paint(Painter& P) {
    Widget::Paint(P);
    Rect R = Bounds().Deflate(Padding);

    HZ_SYSTEM_BASIC_INFORMATION Basic;
    float Usage = 0.0f;
    uint64_t TotalMem = 0, UsedMem = 0;
    if (HZ_SUCCESS(NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic), nullptr))) {
        TotalMem = Basic.TotalPhysicalPages * Basic.PageSize;
        UsedMem = (Basic.TotalPhysicalPages - Basic.AvailablePages) * Basic.PageSize;
        if (TotalMem > 0) Usage = (float)UsedMem / (float)TotalMem;
    }

    HZ_FONT* F = GetFont(FontRole::Caption);
    HZ_FONT* Large = GetFont(FontRole::Title);

    P.TextLine(F, Rect{R.X, R.Y, R.Width, 20}, "RAM", Theme::TextSecondary, Align::Start);
    
    hz::String Percent = hz::String::Format("%d%%", (int)(Usage * 100.0f));
    P.TextLine(Large, Rect{R.X, R.Y + 20, R.Width, R.Height - 40}, Percent.CStr(), Theme::TextPrimary, Align::Center);

    P.TextLine(F, Rect{R.X, R.Bottom() - 20, R.Width, 20}, hz::fs::FormatSize(UsedMem).CStr(), Theme::TextTertiary, Align::Center);
}

bool RAMWidget::Animate() {
    return false;
}

/* ---- Calendar Widget ------------------------------------------------------------------------- */

CalendarWidget::CalendarWidget() : Widget(WidgetSize::Small) {}

void CalendarWidget::Paint(Painter& P) {
    Widget::Paint(P);
    Rect R = Bounds().Deflate(Padding);

    uint64_t Now = NowUs();
    hz::fs::CivilTime Civil = hz::fs::ToCivil(Now, g_Shell.UtcOffset);

    HZ_FONT* F = GetFont(FontRole::Caption);
    HZ_FONT* Large = GetFont(FontRole::Title);

    P.TextLine(F, Rect{R.X, R.Y, R.Width, 20}, hz::fs::MonthNames[Civil.Month - 1], 0xFFFF453A, Align::Center);
    
    hz::String Day = hz::String::Format("%u", Civil.Day);
    P.TextLine(Large, Rect{R.X, R.Y + 20, R.Width, R.Height - 40}, Day.CStr(), Theme::TextPrimary, Align::Center);
    
    P.TextLine(F, Rect{R.X, R.Bottom() - 20, R.Width, 20}, hz::fs::WeekdayNames[Civil.Weekday], Theme::TextSecondary, Align::Center);
}

/* ---- Battery Widget -------------------------------------------------------------------------- */

BatteryWidget::BatteryWidget() : Widget(WidgetSize::Small) {}

void BatteryWidget::Paint(Painter& P) {
    Widget::Paint(P);
    Rect R = Bounds().Deflate(Padding);

    HZ_FONT* F = GetFont(FontRole::Caption);
    
    P.TextLine(F, Rect{R.X, R.Y, R.Width, 20}, "BATTERY", Theme::TextSecondary, Align::Start);
    P.TextLine(F, R, "Unavailable", Theme::TextTertiary, Align::Center);
}

/* ---- Clock Widget ---------------------------------------------------------------------------- */

ClockWidget::ClockWidget() : Widget(WidgetSize::Small) {
    SetTimer(1000, [this]() { Invalidate(); });
}

void ClockWidget::Paint(Painter& P) {
    Widget::Paint(P);
    Rect R = Bounds().Deflate(Padding);
    
    uint64_t Now = NowUs();
    hz::fs::CivilTime Civil = hz::fs::ToCivil(Now, g_Shell.UtcOffset);

    HZ_FONT* Large = GetFont(FontRole::Title);
    
    hz::String Time = hz::String::Format("%02u:%02u", Civil.Hour, Civil.Minute);
    P.TextLine(Large, R, Time.CStr(), Theme::TextPrimary, Align::Center);
}

bool ClockWidget::Animate() {
    return false;
}

/* ---- WidgetHost ----------------------------------------------------------------------------- */

WidgetHost::WidgetHost() {
    /* Default widgets */
    AddWidget(new ClockWidget());
    AddWidget(new CalendarWidget());
    AddWidget(new CPUWidget());
    AddWidget(new RAMWidget());
}

void WidgetHost::AddWidget(Widget* W) {
    Widgets_.Push(W);
    AddChild(W);
    LayoutWidgets();
}

void WidgetHost::RemoveWidget(Widget* W) {
    /* TODO: implementation */
}

void WidgetHost::LayoutWidgets() {
    /* Simple grid layout for now */
    int32_t X = 0, Y = 0;
    int32_t Gap = 16;
    int32_t Size = 148; /* Small widget size */

    for (Widget* W : Widgets_) {
        W->SetFrame(Rect{X, Y, Size, Size});
        X += Size + Gap;
        if (X + Size > Bounds().Width) {
            X = 0;
            Y += Size + Gap;
        }
    }
}

void WidgetHost::Paint(Painter& P) {
    /* Children are painted automatically */
}

void WidgetHost::OnPointerDown(PointerEvent& E) {
    if (E.Button == HZ_MOUSE_RIGHT) {
        Editing_ = !Editing_;
        Invalidate();
        E.Handled = true;
    }
}

} // namespace shell
