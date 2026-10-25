/*
 * Nexora OS - user/apps/clock/clock.cpp
 * Clock: a Mica window with an analog face and a digital readout on the same square, switched by two
 * tabs with a short crossfade, plus the date and the uptime. The time is sampled every 50 ms and the
 * hands are eased towards it, so the second hand sweeps instead of jumping once a second. It is
 * shown in the time zone chosen in Settings (M11: shell.ini's UtcOffsetMinutes, read again every
 * 10 seconds).
 */
#include "hzfile.h"
#include "hzmath.h"
#include "hzui.h"

using namespace hzui;

namespace {

constexpr int32_t kFaceSize = 196; /* the square the dial and the readout share */
constexpr int32_t kWindowWidth = 340, kWindowHeight = 372;
constexpr uint32_t kSampleMs = 50; /* how often the true time is sampled for the sweeping hands */
constexpr unsigned kZoneRefreshTicks = 10000 / kSampleMs;

/* ---- The sampled time ---------------------------------------------------------------------------- */

struct TimeSample {
    hz::fs::CivilTime Civil;
    double Fraction; /* the fraction of the current second, 0 .. 1 */
    uint64_t UptimeUs;
};

TimeSample SampleTime(int Offset)
{
    uint64_t Now = 0, Uptime = 0;
    NtQuerySystemTime(&Now);
    NtQueryUptime(&Uptime);
    TimeSample S;
    S.Civil = hz::fs::ToCivil(Now, Offset);
    S.Fraction = (double)(Now % HZ_TIME_UNITS_PER_SECOND) / (double)HZ_TIME_UNITS_PER_SECOND;
    S.UptimeUs = Uptime;
    return S;
}

/* ---- Dial geometry ------------------------------------------------------------------------------- */

/* A point on the dial: 0 degrees is twelve o'clock and the angle grows clockwise. */
void DialPoint(float Cx, float Cy, float Angle, float Distance, float &X, float &Y)
{
    const double Rad = (double)Angle * (HZ_PI / 180.0);
    X = Cx + (float)(HzSin(Rad) * (double)Distance);
    Y = Cy - (float)(HzCos(Rad) * (double)Distance);
}

/* The representative of Target within 180 degrees of Base. A wrap past twelve is lifted by a whole
   turn, so an eased hand keeps going forward instead of spinning the long way back. */
float NearestTurn(float Target, float Base)
{
    while (Target - Base > 180.0f)
        Target -= 360.0f;
    while (Target - Base < -180.0f)
        Target += 360.0f;
    return Target;
}

/* One hand: a short tail behind the centre, then the shaft out to the tip. */
void DrawNeedle(Painter &P, float Cx, float Cy, float Angle, float Length, float Tail, float Width, Color C)
{
    float X0, Y0, X1, Y1;
    DialPoint(Cx, Cy, Angle + 180.0f, Tail, X0, Y0);
    DialPoint(Cx, Cy, Angle, Length, X1, Y1);
    P.Line(X0, Y0, X1, Y1, Width, C);
}

/* ---- The face ------------------------------------------------------------------------------------ */

/* The dial and the digital readout share one square and crossfade into each other; the hands chase
   the sampled time with an eased transition driven by Animate(). */
class ClockView : public Element {
public:
    ClockView()
    {
        Width = kFaceSize;
        Height = kFaceSize;
        HorizontalAlignment = Align::Center;
    }

    void SetDigital(bool Digital)
    {
        if (Digital_ == Digital)
            return;
        Digital_ = Digital;
        Fade_.To(Digital ? 1.0f : 0.0f, Motion::ControlStateMs);
        StartAnimation();
        Invalidate();
    }

    void SetTime(const TimeSample &S)
    {
        const double Second = (double)S.Civil.Second + S.Fraction;
        const float SecAngle = (float)(Second / 60.0 * 360.0);
        const float MinAngle = (float)(((double)S.Civil.Minute + Second / 60.0) / 60.0 * 360.0);
        const float HourAngle =
            (float)(((double)(S.Civil.Hour % 12) + (double)S.Civil.Minute / 60.0 + Second / 3600.0) / 12.0 * 360.0);

        if (!Primed_) {
            Sec_.Jump(SecAngle);
            Min_.Jump(MinAngle);
            Hour_.Jump(HourAngle);
            Primed_ = true;
        } else {
            Fold(Sec_); /* the seconds angle grows without end: fold it back now and then */
            Sec_.To(NearestTurn(SecAngle, Sec_.Target()), Motion::ControlStateMs);
            Min_.To(NearestTurn(MinAngle, Min_.Target()), Motion::ControlStateMs);
            Hour_.To(NearestTurn(HourAngle, Hour_.Target()), Motion::ControlStateMs);
            StartAnimation();
        }

        HoursText_ = hz::String::Format("%02u:%02u", S.Civil.Hour, S.Civil.Minute);
        SecondsText_ = hz::String::Format("%02u", S.Civil.Second);
        Invalidate();
    }

    void Paint(Painter &P) override
    {
        const float Digital = Fade_.Value();
        if (Digital < 0.996f)
            PaintAnalog(P, 1.0f - Digital);
        if (Digital > 0.004f)
            PaintDigital(P, Digital);
    }

    bool Animate() override
    {
        if (!Digital_ || Fade_.Running())
            Invalidate(); /* the hands move every frame; the readout only when a sample changes it */
        return Fade_.Running() || Sec_.Running() || Min_.Running() || Hour_.Running();
    }

private:
    /* Keeps the unwrapped seconds angle near zero so the float keeps its precision. */
    static void Fold(Transition &T)
    {
        if (T.Target() > 3600.0f)
            T.Jump(T.Value() - 3600.0f);
    }

    void PaintAnalog(Painter &P, float A)
    {
        const Rect &B = Bounds();
        const float Cx = (float)B.X + (float)B.Width * 0.5f;
        const float Cy = (float)B.Y + (float)B.Height * 0.5f;
        const float R = (float)hz::Min(B.Width, B.Height) * 0.5f - 2.0f;
        if (R < 10.0f)
            return;

        /* a faint accent halo so the dial sits on the glass */
        P.FillCircle(Cx, Cy, R + 5.0f, ScaleAlpha(Theme::Accent, 0.05f * A));

        /* the soft inner disc, lighter at the top */
        P.FillVerticalGradient(Rect{(int32_t)(Cx - R), (int32_t)(Cy - R), (int32_t)(R * 2.0f), (int32_t)(R * 2.0f)}, R,
                               WithAlpha(Theme::ControlFill, (uint32_t)(34.0f * A)),
                               WithAlpha(Theme::ControlFill, (uint32_t)(7.0f * A)));
        P.StrokeCircle(Cx, Cy, R, 1.0f, ScaleAlpha(Theme::ControlStrongStroke, 0.35f * A));

        /* sixty minute ticks; every fifth is longer and brighter */
        for (int32_t I = 0; I < 60; I++) {
            const bool Major = (I % 5) == 0;
            const float Angle = (float)I * 6.0f;
            float X0, Y0, X1, Y1;
            DialPoint(Cx, Cy, Angle, R - (Major ? 13.0f : 7.0f), X0, Y0);
            DialPoint(Cx, Cy, Angle, R - 3.0f, X1, Y1);
            P.Line(X0, Y0, X1, Y1, Major ? 1.6f : 1.0f,
                   ScaleAlpha(Major ? Theme::TextSecondary : Theme::TextTertiary, (Major ? 0.85f : 0.45f) * A));
        }

        /* hour, minute, then the accent second hand on top */
        const Color Hand = ScaleAlpha(Theme::TextPrimary, 0.92f * A);
        DrawNeedle(P, Cx, Cy, Hour_.Value(), R * 0.52f, R * 0.14f, 4.5f, Hand);
        DrawNeedle(P, Cx, Cy, Min_.Value(), R * 0.76f, R * 0.16f, 2.6f, Hand);
        DrawNeedle(P, Cx, Cy, Sec_.Value(), R * 0.82f, R * 0.20f, 1.4f, ScaleAlpha(Theme::Accent, A));

        /* the centre cap */
        P.FillCircle(Cx, Cy, 5.5f, ScaleAlpha(Theme::TextPrimary, 0.95f * A));
        P.FillCircle(Cx, Cy, 2.2f, ScaleAlpha(Theme::Accent, A));
    }

    void PaintDigital(Painter &P, float A)
    {
        const Rect &B = Bounds();
        const int32_t Pad = 10;
        P.FillRoundRect(B, Theme::OverlayRadius, WithAlpha(Theme::CardBackground, (uint32_t)(28.0f * A)));
        P.StrokeRoundRect(B, Theme::OverlayRadius, 1, ScaleAlpha(Theme::CardStroke, A));

        Rect Hours{B.X + Pad, B.Y + B.Height / 5, B.Width - Pad * 2, B.Height * 2 / 5};
        P.TextLine(BigFont(Hours.Width), Hours, HoursText_.CStr(), ScaleAlpha(Theme::TextPrimary, A), Align::Center);

        Rect Seconds{B.X + Pad, B.Y + B.Height * 7 / 10, B.Width - Pad * 2, B.Height / 8};
        P.TextLine(GetFont(FontRole::Subtitle), Seconds, SecondsText_.CStr(), ScaleAlpha(Theme::Accent, A),
                   Align::Center);
    }

    /* Display is 72 px: fall back to Title when the readout would not fit the square. */
    HZ_FONT *BigFont(int32_t Width) const
    {
        HZ_FONT *F = GetFont(FontRole::Display);
        if (!F || HzMeasureTextN(F, HoursText_.CStr(), HoursText_.Length()) > Width)
            F = GetFont(FontRole::Title);
        return F;
    }

    Transition Fade_, Sec_, Min_, Hour_;
    bool Digital_ = false, Primed_ = false;
    hz::String HoursText_, SecondsText_;
};

/* ---- The mode tabs ------------------------------------------------------------------------------- */

/* A mode label: the active one is an accent pill, the others light up on hover. */
class ModeTab : public Element {
public:
    explicit ModeTab(const char *Text) : Label_(Text) { Focusable = true; }

    bool Active = false;
    hz::Function<void()> OnClick;

    Size MeasureOverride(Size) override
    {
        HZ_FONT *F = GetFont(FontRole::BodyStrong);
        const int32_t Width = F ? HzMeasureTextN(F, Label_.CStr(), Label_.Length()) : 0;
        return Size{Width + 28, 30};
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        const float Hover = Hover_.Value();
        const bool Pressed = IsPressed();
        if (Active) {
            P.FillRoundRect(B, Theme::ControlRadius + 3.0f,
                            Pressed ? Theme::AccentPressed : MixColors(Theme::Accent, Theme::AccentHover, Hover));
            P.TextLine(GetFont(FontRole::BodyStrong), B, Label_.CStr(), Theme::TextOnAccent, Align::Center);
            return;
        }
        P.FillRoundRect(B, Theme::ControlRadius + 3.0f, ScaleAlpha(Theme::ControlFillHover, Pressed ? 0.6f : Hover));
        P.TextLine(GetFont(FontRole::BodyStrong), B, Label_.CStr(),
                   MixColors(Theme::TextSecondary, Theme::TextPrimary, Pressed ? 1.0f : Hover), Align::Center);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds(), Theme::ControlRadius + 3.0f);
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
        if (E.Button == HZ_MOUSE_LEFT) {
            E.Handled = true;
            Invalidate(); /* the pressed look follows the pointer */
        }
    }

    void OnPointerMove(PointerEvent &) override { Invalidate(); }

    void OnPointerUp(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT && Bounds().Contains(E.Position) && OnClick)
            OnClick();
    }

    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key != HZ_VK_SPACE && E.Key != HZ_VK_RETURN)
            return;
        E.Handled = true;
        if (OnClick)
            OnClick();
    }

    uint32_t CursorShape() const override { return HZ_CURSOR_HAND; }

    bool Animate() override
    {
        Invalidate();
        return Hover_.Running();
    }

private:
    hz::String Label_;
    Transition Hover_;
};

/* ---- The window's content ------------------------------------------------------------------------ */

TextBlock *Line(Element *Parent, FontRole Role, Color C)
{
    TextBlock *T = Parent->Add<TextBlock>("", Role);
    T->Foreground = C;
    T->TextAlignment = Align::Center;
    return T;
}

struct ClockFace {
    ClockView *View = nullptr;
    TextBlock *Date = nullptr, *Up = nullptr;
    ModeTab *AnalogTab = nullptr, *DigitalTab = nullptr;
    int Offset = 0;
    unsigned Ticks = 0, LastSecond = 60;

    void SetDigital(bool On)
    {
        View->SetDigital(On);
        AnalogTab->Active = !On;
        DigitalTab->Active = On;
        AnalogTab->Invalidate();
        DigitalTab->Invalidate();
    }

    void Refresh()
    {
        if (Ticks++ % kZoneRefreshTicks == 0)
            Offset = hz::fs::UtcOffsetMinutes();

        const TimeSample S = SampleTime(Offset);
        View->SetTime(S);

        if (S.Civil.Second == LastSecond)
            return; /* the date and the uptime change once a second, not once a sample */
        LastSecond = S.Civil.Second;
        Date->SetText(hz::String::Format("%s %u %s %lld (%s)", hz::fs::WeekdayNames[S.Civil.Weekday], S.Civil.Day,
                                         hz::fs::MonthNames[(S.Civil.Month - 1) % 12], S.Civil.Year,
                                         hz::fs::FormatUtcOffset(Offset).CStr())
                          .CStr());
        const uint64_t UpSeconds = S.UptimeUs / 1000000;
        Up->SetText(hz::String::Format("Up %llu:%02llu:%02llu", (unsigned long long)(UpSeconds / 3600),
                                       (unsigned long long)(UpSeconds / 60 % 60), (unsigned long long)(UpSeconds % 60))
                        .CStr());
    }
};

}  // namespace

extern "C" int main(int argc, char **argv)
{
    HZ_DISPLAY_INFO Display;
    memset(&Display, 0, sizeof(Display));
    NtUserGetDisplayInfo(&Display);

    ClockFace Face;
    Window Clock("Clock", kWindowWidth, kWindowHeight,
                 HZ_WS_CAPTION | HZ_WS_MICA | HZ_WS_MINIMIZEBOX | HZ_WS_VISIBLE);

    StackPanel *Root = new StackPanel(Orientation::Vertical, 10);
    Root->Padding = Thickness(20, 16);

    StackPanel *Tabs = Root->Add<StackPanel>(Orientation::Horizontal, 6);
    Tabs->HorizontalAlignment = Align::Center;
    Face.AnalogTab = Tabs->Add<ModeTab>("Analog");
    Face.DigitalTab = Tabs->Add<ModeTab>("Digital");
    Face.AnalogTab->Active = true;
    Face.AnalogTab->OnClick = [&Face]() { Face.SetDigital(false); };
    Face.DigitalTab->OnClick = [&Face]() { Face.SetDigital(true); };

    Face.View = Root->Add<ClockView>();
    Face.Date = Line(Root, FontRole::Body, Theme::TextSecondary);
    Face.Up = Line(Root, FontRole::Caption, Theme::TextTertiary);

    Clock.SetContent(Root);
    Face.Refresh();
    Clock.OnClosed = []() { NtUserPostQuitMessage(0); };

    const HZ_RECT &Work = Display.WorkArea;
    if (!Clock.ShowAt(Work.Right - kWindowWidth - 24, Work.Top + 24)) {
        HzPrint("clock: cannot create the window");
        return 1;
    }
    Clock.SetFocus(Face.AnalogTab);
    Clock.SetTimer(kSampleMs, [&Face]() { Face.Refresh(); });
    HzPrint("clock: started");
    return RunMessageLoop();
}
