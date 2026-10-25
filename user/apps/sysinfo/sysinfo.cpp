/*
 * Nexora OS - user/apps/sysinfo/sysinfo.cpp
 * Live system counters for processor, memory, processes and uptime.
 * The counters are grouped into titled cards under three categories; every value comes from
 * NtQuerySystemInformation and the usage bar eases to each new reading instead of snapping.
 */
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

/* ---- Formatting -------------------------------------------------------------------------------- */

/* Digits with a thousands separator, e.g. 1245580 -> "1,245,580". */
hz::String FormatCount(uint64_t Value)
{
    char Digits[24];
    int Count = 0;
    do {
        Digits[Count++] = (char)('0' + (int)(Value % 10));
        Value /= 10;
    } while (Value && Count < (int)sizeof(Digits));
    hz::String Out;
    for (int i = Count - 1; i >= 0; i--) {
        Out.Append(Digits[i]);
        if (i > 0 && i % 3 == 0)
            Out.Append(',');
    }
    return Out;
}

/* A duration in microseconds as "1d 02h 03m", "02h 03m 04s", "03m 04s" or "04s". */
hz::String FormatDuration(uint64_t Microseconds)
{
    uint64_t Total = Microseconds / 1000000ULL;
    uint64_t Days = Total / 86400ULL;
    uint64_t Hours = Total / 3600ULL % 24ULL;
    uint64_t Minutes = Total / 60ULL % 60ULL;
    uint64_t Seconds = Total % 60ULL;
    if (Days)
        return hz::String::Format("%llud %02lluh %02llum", (unsigned long long)Days, (unsigned long long)Hours,
                                  (unsigned long long)Minutes);
    if (Hours)
        return hz::String::Format("%02lluh %02llum %02llus", (unsigned long long)Hours, (unsigned long long)Minutes,
                                  (unsigned long long)Seconds);
    return hz::String::Format("%02llum %02llus", (unsigned long long)Minutes, (unsigned long long)Seconds);
}

/* Multiplies a colour's alpha by Alpha (0..1): used to fade a category in. */
Color WithFade(Color C, float Alpha)
{
    return WithAlpha(C, (uint32_t)((float)((C >> 24) & 0xFF) * hz::Clamp(Alpha, 0.0f, 1.0f)));
}

/* ---- Elements ---------------------------------------------------------------------------------- */

/* An element that eases in from nothing; a selected category reveals its cards this way. */
class FadeElement : public Element {
public:
    FadeElement() { Fade_.Jump(1); }

    void Reveal(uint32_t DurationMs)
    {
        Fade_.Jump(0);
        Fade_.To(1, DurationMs);
        StartAnimation();
        Invalidate();
    }

protected:
    float FadeAlpha() const { return Fade_.Value(); }
    bool FadeRunning() const { return Fade_.Running(); }

private:
    Transition Fade_;
};

/* A card title with a hairline under it. */
class Header : public FadeElement {
public:
    explicit Header(const char *Text) : Text_(Text) { Height = 34; }

    Size MeasureOverride(Size Available) override
    {
        return Size{Available.Width >= Infinite ? 240 : Available.Width, Height};
    }

    void Paint(Painter &P) override
    {
        const Rect R = Bounds().Deflate(Thickness(12, 0, 12, 0));
        P.TextLine(GetFont(FontRole::BodyStrong), Rect{R.X, R.Y + 4, R.Width, 20}, Text_.CStr(),
                   WithFade(Theme::TextPrimary, FadeAlpha()));
        P.FillRect(Rect{R.X, R.Bottom() - 5, R.Width, 1}, WithFade(Theme::Divider, FadeAlpha()));
    }

private:
    hz::String Text_;
};

/* One labelled value; takes keyboard focus, hovers under the pointer and can say "not reported". */
class Row : public FadeElement {
public:
    explicit Row(const char *Label) : Label_(Label)
    {
        Height = 28;
        Focusable = true;
    }

    void Set(const char *Value, bool Reported = true)
    {
        hz::String Next(Reported ? Value : "not reported");
        if (Next == Value_ && Reported == Reported_)
            return;
        Value_ = Next;
        Reported_ = Reported;
        Invalidate();
    }

    Size MeasureOverride(Size Available) override
    {
        return Size{Available.Width >= Infinite ? 240 : Available.Width, Height};
    }

    void Paint(Painter &P) override
    {
        float A = FadeAlpha(), Hover = Hover_.Value();
        const Rect R = Bounds();
        if (Hover > 0.001f)
            P.FillRoundRect(R.Deflate(Thickness(6, 1, 6, 1)), Theme::ControlRadius,
                            WithFade(ScaleAlpha(Theme::HoverRow, Hover), A));
        const Rect Text = R.Deflate(Thickness(12, 0, 12, 0));
        P.TextLine(GetFont(FontRole::Body), Text, Label_.CStr(), WithFade(Theme::TextSecondary, A));
        P.TextLine(GetFont(FontRole::Mono), Text, Value_.CStr(),
                   WithFade(Reported_ ? Theme::TextPrimary : Theme::TextDisabled, A), Align::End);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds().Deflate(Thickness(6, 1, 6, 1)), Theme::ControlRadius);
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
        return Hover_.Running() || FadeRunning();
    }

private:
    hz::String Label_, Value_;
    bool Reported_ = false;
    Transition Hover_;
};

/* A percentage bar with a caption, eased to the value it is given. */
class Gauge : public FadeElement {
public:
    explicit Gauge(const char *Label) : Label_(Label) { Height = 46; }

    void Set(float Percent, const char *Caption, bool Reported)
    {
        Caption_ = Caption;
        Reported_ = Reported;
        float Target = Reported ? hz::Clamp(Percent, 0.0f, 100.0f) : 0.0f;
        if (Target != Target_) {
            Target_ = Target;
            Bar_.To(Target, Motion::ScrollMs);
            StartAnimation();
        }
        Invalidate();
    }

    Size MeasureOverride(Size Available) override
    {
        return Size{Available.Width >= Infinite ? 240 : Available.Width, Height};
    }

    void Paint(Painter &P) override
    {
        float A = FadeAlpha();
        const Rect R = Bounds().Deflate(Thickness(12, 0, 12, 0));
        const Rect Label{R.X, R.Y + 2, R.Width, 20};
        P.TextLine(GetFont(FontRole::Body), Label, Label_.CStr(), WithFade(Theme::TextSecondary, A));
        P.TextLine(GetFont(FontRole::Mono), Label, Caption_.CStr(),
                   WithFade(Reported_ ? Theme::TextPrimary : Theme::TextDisabled, A), Align::End);
        const Rect Track{R.X, R.Y + 28, R.Width, 8};
        P.FillRoundRect(Track, 4, WithFade(Theme::ControlFill, A));
        int32_t Fill = (int32_t)((float)Track.Width * Bar_.Value() / 100.0f + 0.5f);
        if (Fill > 0)
            P.FillRoundRect(Rect{Track.X, Track.Y, hz::Max(Fill, 6), Track.Height}, 4, WithFade(Theme::Accent, A));
    }

    bool Animate() override
    {
        Invalidate();
        return Bar_.Running() || FadeRunning();
    }

private:
    hz::String Label_, Caption_;
    float Target_ = 0;
    bool Reported_ = false;
    Transition Bar_;
};

/* A category in the navigation strip; hover, press and the selection mark all ease. */
class NavItem : public Element {
public:
    NavItem(const char *Text, hz::Function<void()> Select) : OnSelect(hz::Move(Select)), Text_(Text)
    {
        Height = 34;
        Width = 112;
        Focusable = true;
    }

    hz::Function<void()> OnSelect;

    void SetSelected(bool Selected)
    {
        if (Selected == Selected_)
            return;
        Selected_ = Selected;
        Mark_.To(Selected ? 1.0f : 0.0f, Motion::ControlStateMs);
        StartAnimation();
        Invalidate();
    }

    void Paint(Painter &P) override
    {
        const Rect R = Bounds();
        float Mark = Mark_.Value(), Hover = Hover_.Value();
        if (Hover > 0.001f && Mark < 0.999f)
            P.FillRoundRect(R, Theme::ControlRadius,
                            ScaleAlpha(IsPressed() ? Theme::ControlFillPressed : Theme::ControlFillHover, Hover));
        Color Text = MixColors(Theme::TextSecondary, Theme::TextPrimary, hz::Max(Mark, Hover * 0.6f));
        P.TextLine(GetFont(Mark > 0.5f ? FontRole::BodyStrong : FontRole::Body), R, Text_.CStr(), Text, Align::Center);
        float W = (float)(R.Width - 24) * Mark;
        if (W > 0.5f)
            P.FillRoundRect(Rect{R.X + (int32_t)((R.Width - W) / 2.0f), R.Bottom() - 3, (int32_t)W, 2}, 1, Theme::Accent);
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
    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }
    void OnPointerUp(PointerEvent &E) override
    {
        if (Bounds().Contains(E.Position) && OnSelect)
            OnSelect();
    }
    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_SPACE || E.Key == HZ_VK_RETURN) {
            E.Handled = true;
            if (OnSelect)
                OnSelect();
        }
    }
    bool Animate() override
    {
        Invalidate();
        return Mark_.Running() || Hover_.Running();
    }

private:
    hz::String Text_;
    bool Selected_ = false;
    Transition Mark_, Hover_;
};

/* ---- App --------------------------------------------------------------------------------------- */

class SystemInfo {
public:
    bool Create()
    {
        Main_ = new Window("System Information", 520, 430);
        Main_->SetMinimumSize(430, 350);
        StackPanel *Root = new StackPanel(Orientation::Vertical, 10);
        Root->Padding = Thickness(20, 16);

        TextBlock *Subtitle = Root->Add<TextBlock>("Live counters reported by the kernel", FontRole::Caption);
        Subtitle->Foreground = Theme::TextTertiary;

        StackPanel *Nav = Root->Add<StackPanel>(Orientation::Horizontal, 6);
        Nav_[0] = Nav->Add<NavItem>("Overview", [this]() { Select(0); });
        Nav_[1] = Nav->Add<NavItem>("Memory", [this]() { Select(1); });
        Nav_[2] = Nav->Add<NavItem>("Processor", [this]() { Select(2); });

        StackPanel *Host = Root->Add<StackPanel>(Orientation::Vertical, 10);
        for (size_t i = 0; i < 3; i++)
            Pages_[i] = Host->Add<StackPanel>(Orientation::Vertical, 10);
        BuildOverview(Pages_[0]);
        BuildMemory(Pages_[1]);
        BuildProcessor(Pages_[2]);
        for (size_t i = 1; i < 3; i++)
            Pages_[i]->SetVisible(false);

        Status_ = Root->Add<TextBlock>("Refreshing every second", FontRole::Caption);
        Status_->Foreground = Theme::TextTertiary;

        Main_->SetContent(Root);
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
        if (!Main_->Show())
            return false;
        Select(0);
        Refresh();
        Main_->SetTimer(1000, [this]() { Refresh(); });
        HzPrint("sysinfo: started");
        return true;
    }

private:
    StackPanel *AddCard(StackPanel *Page)
    {
        StackPanel *Card = Page->Add<StackPanel>(Orientation::Vertical, 2);
        Card->Background = Theme::CardBackground;
        Card->BorderColor = Theme::CardStroke;
        Card->CornerRadius = Theme::OverlayRadius;
        Card->Padding = Thickness(0, 8, 0, 10);
        return Card;
    }

    void AddHeader(StackPanel *Card, size_t Page, const char *Text)
    {
        Fade_[Page].Push(Card->Add<Header>(Text));
    }

    Row *AddRow(StackPanel *Card, size_t Page, const char *Label, const char *Tooltip = nullptr)
    {
        Row *R = Card->Add<Row>(Label);
        if (Tooltip)
            R->ToolTip = Tooltip;
        Fade_[Page].Push(R);
        return R;
    }

    Gauge *AddGauge(StackPanel *Card, size_t Page, const char *Label, const char *Tooltip = nullptr)
    {
        Gauge *G = Card->Add<Gauge>(Label);
        if (Tooltip)
            G->ToolTip = Tooltip;
        Fade_[Page].Push(G);
        return G;
    }

    void BuildOverview(StackPanel *Page)
    {
        StackPanel *Card = AddCard(Page);
        AddHeader(Card, 0, "Summary");
        OverviewGauge_ = AddGauge(Card, 0, "Physical memory used", "Share of physical memory currently in use");
        Processors_ = AddRow(Card, 0, "Logical processors", "Online processors reported by the kernel");
        Processes_ = AddRow(Card, 0, "Processes");
        Threads_ = AddRow(Card, 0, "Threads");
        Uptime_ = AddRow(Card, 0, "Uptime", "Time since the kernel booted");
    }

    void BuildMemory(StackPanel *Page)
    {
        StackPanel *Card = AddCard(Page);
        AddHeader(Card, 1, "Physical memory");
        MemoryGauge_ = AddGauge(Card, 1, "In use", "Share of physical memory currently in use");
        Total_ = AddRow(Card, 1, "Total");
        Available_ = AddRow(Card, 1, "Available");
        Used_ = AddRow(Card, 1, "Used");
        PageSize_ = AddRow(Card, 1, "Page size", "Memory granularity reported by the kernel");
    }

    void BuildProcessor(StackPanel *Page)
    {
        StackPanel *Card = AddCard(Page);
        AddHeader(Card, 2, "Processors");
        ProcessorCount_ = AddRow(Card, 2, "Logical processors", "Online processors reported by the kernel");

        StackPanel *Activity = AddCard(Page);
        AddHeader(Activity, 2, "Activity");
        ContextSwitches_ = AddRow(Activity, 2, "Context switches", "Total switches since boot");
        Interrupts_ = AddRow(Activity, 2, "Interrupts handled", "Hardware interrupts handled since boot");
        IdleTime_ = AddRow(Activity, 2, "Idle time", "Total idle time of every processor");
    }

    void Select(size_t Index)
    {
        if (Index == Selected_)
            return;
        Selected_ = Index;
        for (size_t i = 0; i < 3; i++) {
            Nav_[i]->SetSelected(i == Index);
            Pages_[i]->SetVisible(i == Index);
        }
        for (FadeElement *E : Fade_[Index])
            E->Reveal(Motion::ControlStateMs);
    }

    void Refresh()
    {
        HZ_SYSTEM_BASIC_INFORMATION Info{};
        uint32_t Returned = 0;
        STATUS Result = NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Info, sizeof(Info), &Returned);
        if (!HZ_SUCCESS(Result)) {
            Status_->SetText(hz::String::Format("System query failed (0x%08X)", (uint32_t)Result).CStr());
            HzPrint("sysinfo: system query failed (0x%08X)", (uint32_t)Result);
            return;
        }

        bool PageSizeReported = Info.PageSize != 0;
        bool MemoryReported = PageSizeReported && Info.TotalPhysicalPages != 0 &&
                              Info.AvailablePages <= Info.TotalPhysicalPages;
        uint64_t TotalBytes = PageSizeReported ? Info.TotalPhysicalPages * Info.PageSize : 0;
        uint64_t AvailableBytes = PageSizeReported ? Info.AvailablePages * Info.PageSize : 0;
        uint64_t UsedBytes = MemoryReported ? TotalBytes - AvailableBytes : 0;
        uint64_t UsedTenths = MemoryReported ? UsedBytes * 1000ULL / TotalBytes : 0;
        float UsedPercent = (float)UsedTenths / 10.0f;
        hz::String Percent = hz::String::Format("%llu.%llu%%", (unsigned long long)(UsedTenths / 10),
                                                (unsigned long long)(UsedTenths % 10));

        OverviewGauge_->Set(UsedPercent, Percent.CStr(), MemoryReported);
        MemoryGauge_->Set(UsedPercent, Percent.CStr(), MemoryReported);

        bool ProcessorsReported = Info.ProcessorCount != 0;
        hz::String Processors = hz::String::Format("%u", Info.ProcessorCount);
        Processors_->Set(Processors.CStr(), ProcessorsReported);
        ProcessorCount_->Set(Processors.CStr(), ProcessorsReported);
        Processes_->Set(FormatCount(Info.ProcessCount).CStr());
        Threads_->Set(FormatCount(Info.ThreadCount).CStr());
        Uptime_->Set(FormatDuration(Info.UptimeUs).CStr());
        Total_->Set(hz::fs::FormatSize(TotalBytes).CStr(), MemoryReported);
        Available_->Set(hz::fs::FormatSize(AvailableBytes).CStr(), MemoryReported);
        Used_->Set(hz::fs::FormatSize(UsedBytes).CStr(), MemoryReported);
        PageSize_->Set(hz::fs::FormatSize(Info.PageSize).CStr(), PageSizeReported);
        ContextSwitches_->Set(FormatCount(Info.ContextSwitches).CStr());
        Interrupts_->Set(FormatCount(Info.InterruptCount).CStr());
        IdleTime_->Set(FormatDuration(Info.IdleTimeUs).CStr());
        Status_->SetText("Refreshing every second");
    }

    Window *Main_ = nullptr;
    NavItem *Nav_[3] = {};
    StackPanel *Pages_[3] = {};
    hz::Vector<FadeElement *> Fade_[3];
    size_t Selected_ = hz::NotFound;
    TextBlock *Status_ = nullptr;

    Gauge *OverviewGauge_ = nullptr, *MemoryGauge_ = nullptr;
    Row *Processors_ = nullptr, *Processes_ = nullptr, *Threads_ = nullptr, *Uptime_ = nullptr;
    Row *Total_ = nullptr, *Available_ = nullptr, *Used_ = nullptr, *PageSize_ = nullptr;
    Row *ProcessorCount_ = nullptr, *ContextSwitches_ = nullptr, *Interrupts_ = nullptr, *IdleTime_ = nullptr;
};

}  // namespace

extern "C" int main(int argc, char **argv)
{
    SystemInfo App;
    if (!App.Create()) {
        HzPrint("sysinfo: cannot create the window");
        return 1;
    }
    return RunMessageLoop();
}
