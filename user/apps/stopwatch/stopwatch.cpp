/*
 * Nexora OS - user/apps/stopwatch/stopwatch.cpp
 * A small stopwatch driven by the system uptime clock, with laps and eased motion.
 */
#include "hzui.h"

using namespace hzui;

namespace {

/* The running readout: hours, minutes, seconds and tenths. */
hz::String FormatClock(uint64_t Us)
{
    uint64_t T = Us / 100000;
    return hz::String::Format("%02llu:%02llu:%02llu.%llu", (unsigned long long)(T / 36000),
                              (unsigned long long)(T / 600 % 60), (unsigned long long)(T / 10 % 60),
                              (unsigned long long)(T % 10));
}

/* A lap split, without the hours field the running clock shows. */
hz::String FormatSplit(uint64_t Us)
{
    uint64_t T = Us / 100000;
    return hz::String::Format("%02llu:%02llu.%llu", (unsigned long long)(T / 600),
                              (unsigned long long)(T / 10 % 60), (unsigned long long)(T % 10));
}

/* The change from the previous lap: '-' when the lap was faster, '+' when slower. */
hz::String FormatDelta(uint64_t Split, uint64_t Previous)
{
    bool Faster = Split < Previous;
    uint64_t Diff = Faster ? Previous - Split : Split - Previous;
    uint64_t T = Diff / 100000;
    return hz::String::Format("%c%llu.%llu", Faster ? '-' : '+', (unsigned long long)(T / 10),
                              (unsigned long long)(T % 10));
}

/* The elapsed readout. Fixed-width digit cells keep the number from jittering, and a ten-cell band
   eases its active cell in and out as the tenths advance, so the sub-second motion is smooth
   rather than a snap. */
class StopwatchFace final : public Element {
public:
    StopwatchFace() { Height = 96; }

    void SetReading(const hz::String &Text, int Tenth, bool Live)
    {
        Time_ = Text;
        bool Animating = false;
        for (int i = 0; i < 10; i++) {
            float Target = (Live && i == Tenth) ? 1.0f : 0.0f;
            Band_[i].To(Target, 90);
            if (Band_[i].Running())
                Animating = true;
        }
        Invalidate();
        if (Animating)
            StartAnimation(); /* only while a cell is actually easing, so an idle face stays still */
    }

    bool Animate() override
    {
        Invalidate();
        for (int i = 0; i < 10; i++)
            if (Band_[i].Running())
                return true;
        return false;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillRoundRect(B, Theme::OverlayRadius, Theme::CardBackground);
        P.StrokeRoundRect(B, Theme::OverlayRadius, 1, Theme::CardStroke);

        HZ_FONT *F = GetFont(FontRole::Title);
        int32_t Count = (int32_t)Time_.Length();
        if (F) {
            int32_t Cell = HzMeasureText(F, "0");
            if (Cell <= 0)
                Cell = 8;
            int32_t X = B.X + (B.Width - Cell * Count) / 2;
            for (int32_t i = 0; i < Count; i++) {
                char One[2] = {Time_[i], '\0'};
                P.TextLine(F, Rect{X + i * Cell, B.Y + 12, Cell, 44}, One, Theme::TextPrimary, Align::Center);
            }
        }

        float Pad = 16.0f, Gap = 3.0f;
        float BandW = (float)B.Width - Pad * 2;
        float CellW = (BandW - Gap * 9) / 10;
        float BandY = (float)(B.Bottom() - 20);
        for (int i = 0; i < 10; i++) {
            float X = (float)B.X + Pad + i * (CellW + Gap);
            Color C = MixColors(Theme::ControlFill, Theme::Accent, Band_[i].Value());
            P.FillRoundRect(Rect{(int32_t)X, (int32_t)BandY, (int32_t)CellW, 6}, 3, C);
        }
    }

private:
    hz::String Time_ = "00:00:00.0";
    Transition Band_[10];
};

/* The lap list. ListView supplies the selection, hover, scrolling and keyboard handling; the rows
   add the split, the delta from the previous lap and best/worst emphasis, and the newest row
   flashes with an eased highlight when it is recorded. */
class LapList final : public ListView {
public:
    LapList()
    {
        ItemHeight = 32;
        Height = 150;
        PaintItem = [this](Painter &P, const Rect &Row, size_t Index) { PaintRow(P, Row, Index); };
    }

    void Record(uint64_t Split)
    {
        Laps_.Push(Split);
        Recompute();
        SetItemCount(Laps_.Size());
        SetSelectedIndex(Laps_.Size() - 1);
        ScrollIntoView(Laps_.Size() - 1);
        Flash_.Jump(1.0f);
        Flash_.To(0.0f, 260);
        StartAnimation();
        Invalidate();
    }

    void ClearAll()
    {
        Laps_.Clear();
        Best_ = Worst_ = hz::NotFound;
        SetItemCount(0);
        SetSelectedIndex(hz::NotFound);
        Flash_.Jump(0.0f);
        Invalidate();
    }

    bool Animate() override
    {
        Invalidate();
        return Flash_.Running() || ListView::Animate();
    }

private:
    void Recompute()
    {
        Best_ = Worst_ = hz::NotFound;
        if (Laps_.Size() < 2)
            return;
        for (size_t i = 0; i < Laps_.Size(); i++) {
            if (Best_ == hz::NotFound || Laps_[i] < Laps_[Best_])
                Best_ = i;
            if (Worst_ == hz::NotFound || Laps_[i] > Laps_[Worst_])
                Worst_ = i;
        }
    }

    void PaintRow(Painter &P, const Rect &Row, size_t Index)
    {
        float Flash = Index + 1 == Laps_.Size() ? Flash_.Value() : 0.0f;
        if (Flash > 0.004f)
            P.FillRoundRect(Row, Theme::ControlRadius, ScaleAlpha(Theme::Accent, 0.30f * Flash));

        bool Best = Index == Best_, Worst = Index == Worst_;
        Color LabelColor = Best ? Theme::Accent : (Worst ? Theme::TextTertiary : Theme::TextSecondary);
        Color TimeColor = Best ? Theme::Accent : (Worst ? Theme::TextTertiary : Theme::TextPrimary);

        hz::String Name = hz::String::Format("Lap %llu", (unsigned long long)(Index + 1));
        Name.Append(Best ? " - best" : (Worst ? " - worst" : ""));
        P.TextLine(GetFont(FontRole::Body), Rect{Row.X, Row.Y, Row.Width / 2, Row.Height}, Name.CStr(), LabelColor,
                   Align::Start);

        if (Index > 0) {
            hz::String Delta = FormatDelta(Laps_[Index], Laps_[Index - 1]);
            P.TextLine(GetFont(FontRole::Caption),
                       Rect{Row.X + Row.Width / 2, Row.Y, Row.Width / 2 - 80, Row.Height}, Delta.CStr(),
                       Theme::TextTertiary, Align::End);
        }

        P.TextLine(GetFont(FontRole::Mono), Rect{Row.Right() - 72, Row.Y, 72, Row.Height},
                   FormatSplit(Laps_[Index]).CStr(), TimeColor, Align::End);
    }

    hz::Vector<uint64_t> Laps_;
    size_t Best_ = hz::NotFound, Worst_ = hz::NotFound;
    Transition Flash_;
};

/* A button whose pressed look eases in and out, on top of the toolkit's instant pressed fill and
   hover fade. */
class PressButton final : public Button {
public:
    PressButton(const char *Text, ButtonStyle Style = ButtonStyle::Standard) : Button(Text, Style) {}

    void OnPointerDown(PointerEvent &E) override
    {
        Button::OnPointerDown(E);
        if (E.Button == HZ_MOUSE_LEFT) {
            Press_.To(1.0f, 70);
            StartAnimation();
        }
    }

    void OnPointerUp(PointerEvent &E) override
    {
        Button::OnPointerUp(E);
        if (E.Button == HZ_MOUSE_LEFT) {
            Press_.To(0.0f, 180);
            StartAnimation();
        }
    }

    void OnKeyDown(KeyEvent &E) override
    {
        bool Activates = E.Key == HZ_VK_SPACE || E.Key == HZ_VK_RETURN;
        Button::OnKeyDown(E);
        if (Activates) {
            Press_.Jump(1.0f);
            Press_.To(0.0f, 180);
            StartAnimation();
        }
    }

    bool Animate() override
    {
        return Button::Animate() || Press_.Running();
    }

    void Paint(Painter &P) override
    {
        Button::Paint(P);
        float K = Press_.Value();
        if (K <= 0.004f)
            return;
        const Rect &B = Bounds();
        P.FillRoundRect(B.Deflate(Thickness(2)), Theme::ControlRadius - 1, ScaleAlpha(Theme::Accent, 0.18f * K));
        P.StrokeRoundRect(B.Deflate(Thickness(1)), Theme::ControlRadius, 1.5f, ScaleAlpha(Theme::Accent, 0.55f * K));
    }

private:
    Transition Press_;
};

class Stopwatch {
public:
    bool Create()
    {
        Main_ = new Window("Stopwatch", 400, 400);
        Main_->SetMinimumSize(340, 340);

        StackPanel *Root = new StackPanel(Orientation::Vertical, 12);
        Root->Padding = Thickness(20, 16);

        Face_ = Root->Add<StopwatchFace>();

        Hint_ = Root->Add<TextBlock>("Press Start", FontRole::Caption);
        Hint_->HorizontalAlignment = Align::Center;
        Hint_->Foreground = Theme::TextTertiary;

        StackPanel *Buttons = Root->Add<StackPanel>(Orientation::Horizontal, 8);
        Buttons->HorizontalAlignment = Align::Center;

        Toggle_ = Buttons->Add<PressButton>("Start", ButtonStyle::Accent);
        Toggle_->ToolTip = "Start or pause (Space)";
        Toggle_->OnClick = [this]() { Toggle(); };

        Lap_ = Buttons->Add<PressButton>("Lap", ButtonStyle::Standard);
        Lap_->ToolTip = "Record a lap (L)";
        Lap_->OnClick = [this]() { RecordLap(); };

        Reset_ = Buttons->Add<PressButton>("", ButtonStyle::Standard);
        Reset_->Glyph = Icon::Refresh;
        Reset_->ToolTip = "Reset (R)";
        Reset_->OnClick = [this]() { ResetTime(); };

        Laps_ = Root->Add<LapList>();
        Laps_->Background = Theme::CardBackground;
        Laps_->BorderColor = Theme::CardStroke;
        Laps_->CornerRadius = Theme::OverlayRadius;
        Laps_->Padding = Thickness(0, 6);

        Main_->SetContent(Root);
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
        Main_->OnUnhandledKey = [this](KeyEvent &E) {
            if (E.Key == HZ_VK_SPACE) {
                E.Handled = true;
                Toggle();
            } else if (E.Key == 'L') {
                E.Handled = true;
                RecordLap();
            } else if (E.Key == 'R') {
                E.Handled = true;
                ResetTime();
            }
        };
        if (!Main_->Show())
            return false;
        Main_->SetFocus(Toggle_);
        Main_->SetTimer(100, [this]() { Update(); });
        HzPrint("stopwatch: started");
        return true;
    }

private:
    void Toggle()
    {
        uint64_t Now = NowUs();
        if (Running_) {
            Elapsed_ += Now - StartedAt_;
            Running_ = false;
            Toggle_->SetText("Start");
            Hint_->SetText("Paused - Space to resume");
        } else {
            StartedAt_ = Now;
            Running_ = true;
            Toggle_->SetText("Pause");
            Hint_->SetText("Space to pause");
        }
        Update();
    }

    void RecordLap()
    {
        uint64_t Cumulative = Elapsed_ + (Running_ ? NowUs() - StartedAt_ : 0);
        if (Cumulative <= LapMarkUs_)
            return; /* nothing has run since the last lap */
        Laps_->Record(Cumulative - LapMarkUs_);
        LapMarkUs_ = Cumulative;
    }

    void ResetTime()
    {
        Elapsed_ = 0;
        LapMarkUs_ = 0;
        if (Running_)
            StartedAt_ = NowUs();
        else
            Hint_->SetText("Ready");
        Laps_->ClearAll();
        Update();
    }

    void Update()
    {
        uint64_t Us = Elapsed_ + (Running_ ? NowUs() - StartedAt_ : 0);
        Face_->SetReading(FormatClock(Us), (int)(Us / 100000 % 10), Running_);
    }

    Window *Main_ = nullptr;
    StopwatchFace *Face_ = nullptr;
    TextBlock *Hint_ = nullptr;
    PressButton *Toggle_ = nullptr, *Lap_ = nullptr, *Reset_ = nullptr;
    LapList *Laps_ = nullptr;
    uint64_t StartedAt_ = 0, Elapsed_ = 0, LapMarkUs_ = 0;
    bool Running_ = false;
};

}  // namespace

extern "C" int main(int argc, char **argv)
{
    Stopwatch App;
    if (!App.Create()) {
        HzPrint("stopwatch: cannot create the window");
        return 1;
    }
    return RunMessageLoop();
}
