/*
 * Nexora OS - user/apps/taskmgr/taskmgr.cpp
 * Task Manager. Processes: every process with its window title, CPU use over the last second,
 * memory, threads and handles; sortable by clicking a heading, filtered by the search box; End
 * task (or Delete) asks, then opens the process with PROCESS_TERMINATE and ends it (the kernel
 * refuses the System process). Performance: CPU and memory graphs over the last minute and the
 * system counters. Everything refreshes once a second. Ending a process is logged
 * ("taskmgr: ended clock.exe (process 12)") for the smoke tests.
 */
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

constexpr size_t kMaxProcesses = 128;
constexpr size_t kHistory = 60;

struct ProcessRow {
    uint32_t Pid = 0, Parent = 0, Threads = 0, Handles = 0;
    uint64_t Memory = 0, CpuUs = 0;
    uint32_t CpuTenths = 0; /* percent x 10 over the last interval */
    bool CpuKnown = false;  /* false on the first sample: no interval to subtract yet */
    hz::String Name, Window;
};

enum class SortKey : uint8_t { Name, Pid, Cpu, Memory, Threads, Handles };

struct Column {
    const char *Title;
    SortKey Key;
    int32_t Width; /* from the right edge; the name column takes the rest */
    bool Right;
};
const Column kColumns[] = {
    {"Name", SortKey::Name, 0, false},     {"PID", SortKey::Pid, 70, true},        {"CPU", SortKey::Cpu, 80, true},
    {"Memory", SortKey::Memory, 100, true}, {"Threads", SortKey::Threads, 80, true}, {"Handles", SortKey::Handles, 80, true},
};
constexpr int kColumnCount = 6;

/* Left edge and width of each column in a row of width W starting at X. */
void ColumnBounds(int32_t X, int32_t W, int32_t *Left, int32_t *Width)
{
    int32_t Right = X + W - 12;
    for (int i = kColumnCount - 1; i >= 1; i--) {
        Right -= kColumns[i].Width;
        Left[i] = Right;
        Width[i] = kColumns[i].Width - 12;
    }
    Left[0] = X + 12;
    Width[0] = Right - Left[0] - 12;
}

hz::String FormatPercent(uint32_t Tenths)
{
    return hz::String::Format("%u.%u%%", Tenths / 10, Tenths % 10);
}

/* Utilization of each processor over the last second, as horizontal bars (M11). The fill of each
   bar eases to the new value, so a busy second does not make them jump. */
class ProcessorBars final : public Element {
public:
    void SetValues(hz::Vector<uint32_t> Values)
    {
        bool CountChanged = Values.Size() != Tenths.Size();
        Tenths = hz::Move(Values);
        Level_.Resize(Tenths.Size());
        for (size_t i = 0; i < Tenths.Size(); i++)
            Level_[i].To((float)hz::Min<uint32_t>(Tenths[i], 1000), Motion::ControlStateMs);
        if (CountChanged)
            InvalidateLayout();
        StartAnimation();
        Invalidate();
    }

    Size MeasureOverride(Size Available) override
    {
        return Size{Available.Width, 30 + (int32_t)hz::Max<size_t>(Tenths.Size(), 1) * 26};
    }

    bool Animate() override
    {
        Invalidate();
        for (size_t i = 0; i < Level_.Size(); i++)
            if (Level_[i].Running())
                return true;
        return false;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillRoundRect(B, Theme::OverlayRadius, Theme::CardBackground);
        P.StrokeRoundRect(B, Theme::OverlayRadius, 1, Theme::CardStroke);
        P.TextLine(GetFont(FontRole::BodyStrong), Rect{B.X + 14, B.Y + 6, B.Width - 28, 22}, "Processors", Theme::TextPrimary);
        if (Tenths.Empty()) {
            P.TextLine(GetFont(FontRole::Body), Rect{B.X + 14, B.Y + 32, B.Width - 28, 22}, "not reported",
                       Theme::TextTertiary);
            return;
        }
        for (size_t i = 0; i < Tenths.Size(); i++) {
            int32_t Y = B.Y + 32 + (int32_t)i * 26;
            P.TextLine(GetFont(FontRole::Body), Rect{B.X + 14, Y, 70, 20}, hz::String::Format("CPU %zu", i).CStr(),
                       Theme::TextSecondary);
            Rect Track{B.X + 90, Y + 6, B.Width - 90 - 80, 8};
            P.FillRoundRect(Track, 4, Theme::ControlFill);
            float Level = hz::Clamp(Level_[i].Value(), 0.0f, 1000.0f);
            int32_t Fill = (int32_t)((int64_t)Track.Width * (int64_t)Level / 1000);
            if (Fill > 0)
                P.FillRoundRect(Rect{Track.X, Track.Y, hz::Max(Fill, 6), Track.Height}, 4, Theme::Accent);
            P.TextLine(GetFont(FontRole::Mono), Rect{B.Right() - 74, Y, 60, 20}, FormatPercent(Tenths[i]).CStr(),
                       Theme::TextSecondary, Align::End);
        }
    }

private:
    hz::Vector<uint32_t> Tenths; /* per processor */
    hz::Vector<Transition> Level_;
};

/* A sparkline of the last kHistory samples (0..100) over a baseline grid, filled with a gradient.
   The newest sample eases in with a Transition so the leading edge never jumps. */
class Graph final : public Element {
public:
    Graph(const char *Title, Color Stroke) : Title_(Title), Stroke_(Stroke) { Height = 180; }

    hz::String Caption;

    void Push(float Value)
    {
        float V = hz::Clamp(Value, 0.0f, 100.0f);
        bool First = Samples_.Empty();
        Samples_.Push(V);
        if (Samples_.Size() > kHistory)
            Samples_.Erase(0);
        if (First)
            Current_.Jump(V);
        else
            Current_.To(V, Motion::ScrollMs);
        StartAnimation();
        Invalidate();
    }

    bool Animate() override
    {
        Invalidate();
        return Current_.Running();
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillRoundRect(B, Theme::OverlayRadius, Theme::CardBackground);
        P.StrokeRoundRect(B, Theme::OverlayRadius, 1, Theme::CardStroke);
        P.TextLine(GetFont(FontRole::BodyStrong), Rect{B.X + 14, B.Y + 8, B.Width / 2, 22}, Title_, Theme::TextPrimary);
        P.TextLine(GetFont(FontRole::Body), Rect{B.X + B.Width / 2, B.Y + 8, B.Width / 2 - 14, 22}, Caption.CStr(),
                   Theme::TextSecondary, Align::End);
        Rect Plot{B.X + 14, B.Y + 38, B.Width - 28, B.Height - 52};
        P.FillRoundRect(Plot, 3, Theme::InputActive);
        for (int i = 1; i < 4; i++) /* baseline grid */
            P.FillRect(Rect{Plot.X, Plot.Y + Plot.Height * i / 4, Plot.Width, 1}, Theme::Divider);
        P.StrokeRoundRect(Plot, 3, 1, ScaleAlpha(Stroke_, 0.35f));
        size_t N = Samples_.Size();
        if (N < 2)
            return;
        float Points[2 * kHistory];
        float Step = (float)Plot.Width / (float)(kHistory - 1);
        float X0 = (float)Plot.Right() - Step * (float)(N - 1);
        for (size_t i = 0; i < N; i++) {
            float V = i + 1 == N ? Current_.Value() : Samples_[i]; /* the tip follows the transition */
            Points[2 * i] = X0 + Step * (float)i;
            Points[2 * i + 1] = (float)Plot.Bottom() - 1 - V / 100.0f * (float)(Plot.Height - 2);
        }
        /* The area under the line, one column at a time so the edge follows the curve while each
           column still samples the one vertical gradient down the plot. */
        P.PushClip(Plot);
        Color Top = ScaleAlpha(Stroke_, 0.02f), Bottom = ScaleAlpha(Stroke_, 0.30f);
        for (size_t i = 0; i + 1 < N; i++) {
            int32_t Xa = (int32_t)Points[2 * i], Xb = (int32_t)Points[2 * i + 2];
            if (Xb <= Xa)
                continue;
            for (int32_t X = Xa; X < Xb; X++) {
                float T = (float)(X - Xa) / (float)(Xb - Xa);
                int32_t Y = (int32_t)(Points[2 * i + 1] + (Points[2 * i + 3] - Points[2 * i + 1]) * T);
                if (Y >= Plot.Bottom())
                    continue;
                P.PushClip(Rect{X, Y, 1, Plot.Bottom() - Y});
                P.FillVerticalGradient(Plot, 3, Top, Bottom);
                P.PopClip();
            }
        }
        P.PopClip();
        P.Polyline(Points, (int)N, 1.5f, Stroke_);
        P.FillCircle(Points[2 * (N - 1)], Points[2 * (N - 1) + 1], 2.5f, Stroke_);
    }

private:
    const char *Title_;
    Color Stroke_;
    hz::Vector<float> Samples_;
    Transition Current_;
};

/* The column headings: sorting by a click or with the arrow keys, a tinted hovered column and an
   indicator that fades in when the sort changes. */
class Headings final : public Element {
public:
    SortKey Key = SortKey::Name;
    bool Descending = false;
    hz::Function<void()> OnSortChanged;

    Headings()
    {
        Height = 32;
        Focusable = true;
        ToolTip = "Click a heading to sort; the arrow keys change the column";
        Indicator_.Jump(1.0f);
    }

    bool Animate() override
    {
        Invalidate();
        return Indicator_.Running() || Hover_.Running();
    }

    void SetSort(SortKey K, bool Desc)
    {
        Key = K;
        Descending = Desc;
        Indicator_.Jump(0.0f);
        Indicator_.To(1.0f, Motion::ControlStateMs);
        StartAnimation();
        Invalidate();
        if (OnSortChanged)
            OnSortChanged();
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        int32_t Left[kColumnCount], Width[kColumnCount];
        ColumnBounds(B.X, B.Width, Left, Width);
        float Indicator = Indicator_.Value();
        for (int i = 0; i < kColumnCount; i++) {
            bool Active = kColumns[i].Key == Key;
            Rect R{Left[i], B.Y, Width[i], B.Height};
            if (i == HotColumn_ && !Active)
                P.FillRoundRect(R.Inflate(-2), Theme::ControlRadius, ScaleAlpha(Theme::SubtleFillHover, Hover_.Value()));
            P.TextLine(GetFont(Active ? FontRole::BodyStrong : FontRole::Body), R, kColumns[i].Title,
                       Active ? Theme::TextPrimary : Theme::TextSecondary, kColumns[i].Right ? Align::End : Align::Start);
            if (Active) {
                HZ_FONT *F = GetFont(FontRole::BodyStrong);
                int32_t TextWidth = F ? HzMeasureText(F, kColumns[i].Title) : 40;
                int32_t X = kColumns[i].Right ? R.Right() - TextWidth - 14 : R.X + TextWidth + 4;
                P.DrawIcon(Descending ? Icon::ChevronDown : Icon::ChevronUp, Rect{X, B.Y + (B.Height - 10) / 2, 10, 10},
                           ScaleAlpha(Theme::TextSecondary, 0.3f + 0.7f * Indicator));
                int32_t Under = (int32_t)((float)Width[i] * (0.35f + 0.65f * Indicator));
                P.FillRoundRect(Rect{Left[i] + (Width[i] - Under) / 2, B.Bottom() - 3, Under, 2}, 1,
                                ScaleAlpha(Theme::Accent, Indicator));
            }
        }
        P.FillRect(Rect{B.X + 8, B.Bottom() - 1, B.Width - 16, 1}, Theme::Divider);
        if (ShowsFocusRing())
            P.FocusRing(ActiveRect(), Theme::ControlRadius);
    }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT)
            return;
        E.Handled = true;
        int32_t Index = ColumnAt(E.Position.X);
        if (Index < 0)
            return;
        bool Same = Key == kColumns[Index].Key;
        SetSort(kColumns[Index].Key, Same ? !Descending : (kColumns[Index].Key != SortKey::Name && kColumns[Index].Key != SortKey::Pid));
    }

    void OnPointerMove(PointerEvent &E) override
    {
        int32_t Index = ColumnAt(E.Position.X);
        if (Index != HotColumn_) {
            HotColumn_ = Index;
            Hover_.Jump(0.0f);
            Hover_.To(1.0f, Motion::HoverEnterMs);
            StartAnimation();
            Invalidate();
        }
    }

    void OnPointerLeave() override
    {
        if (HotColumn_ != -1) {
            HotColumn_ = -1;
            Hover_.To(0.0f, Motion::HoverLeaveMs);
            StartAnimation();
            Invalidate();
        }
    }

    void OnKeyDown(KeyEvent &E) override
    {
        int32_t Index = 0;
        for (int i = 0; i < kColumnCount; i++)
            if (kColumns[i].Key == Key)
                Index = i;
        if (E.Key == HZ_VK_LEFT) {
            SetSort(kColumns[Index > 0 ? Index - 1 : 0].Key, Descending);
            E.Handled = true;
        } else if (E.Key == HZ_VK_RIGHT) {
            SetSort(kColumns[Index < kColumnCount - 1 ? Index + 1 : kColumnCount - 1].Key, Descending);
            E.Handled = true;
        } else if (E.Key == HZ_VK_RETURN || E.Key == HZ_VK_SPACE) {
            SetSort(Key, !Descending);
            E.Handled = true;
        } else if (E.Key == HZ_VK_UP || E.Key == HZ_VK_DOWN) {
            SetSort(Key, E.Key == HZ_VK_DOWN);
            E.Handled = true;
        }
    }

private:
    int32_t ColumnAt(int32_t X) const
    {
        int32_t Left[kColumnCount], Width[kColumnCount];
        ColumnBounds(Bounds().X, Bounds().Width, Left, Width);
        for (int i = 0; i < kColumnCount; i++)
            if (X >= Left[i] - 6 && X < Left[i] + Width[i] + 6)
                return i;
        return -1;
    }

    Rect ActiveRect() const
    {
        int32_t Left[kColumnCount], Width[kColumnCount];
        ColumnBounds(Bounds().X, Bounds().Width, Left, Width);
        for (int i = 0; i < kColumnCount; i++)
            if (kColumns[i].Key == Key)
                return Rect{Left[i], Bounds().Y, Width[i], Bounds().Height};
        return Bounds();
    }

    Transition Indicator_, Hover_;
    int32_t HotColumn_ = -1;
};

/* The process list: a ListView that also drives the app's own animations each frame and draws the
   empty state and the "ended" notice. */
class ProcessList final : public ListView {
public:
    hz::Function<void()> OnDelete;
    hz::Function<bool()> OnPolish;          /* the app's animations; true while they run */
    hz::Function<float(uint32_t)> RowFlash; /* 1 -> 0 right after a process appears */
    hz::String ExitName;                    /* the last process that exited */
    float ExitAlpha = 0;

    void Kick() { StartAnimation(); }

    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_DELETE && OnDelete) {
            E.Handled = true;
            OnDelete();
            return;
        }
        ListView::OnKeyDown(E);
    }

    bool Animate() override
    {
        bool More = ListView::Animate();
        if (OnPolish && OnPolish())
            More = true;
        return More;
    }

    void Paint(Painter &P) override
    {
        ListView::Paint(P);
        Rect Content = ContentBounds();
        if (ItemCount() == 0) {
            P.TextLine(GetFont(FontRole::Body), Content, "no processes", Theme::TextTertiary, Align::Center);
            return;
        }
        if (ExitAlpha > 0.001f && !ExitName.Empty()) {
            HZ_FONT *F = GetFont(FontRole::Caption);
            hz::String Notice = hz::String::Format("%s ended", ExitName.CStr());
            int32_t W = hz::Min((F ? HzMeasureText(F, Notice.CStr()) : 80) + 24, Content.Width - 24);
            Rect Pill{Content.Right() - W - 12, Content.Bottom() - 34, W, 26};
            P.FillRoundRect(Pill, 13, WithAlpha(Theme::CardBackground, (uint32_t)(0xD0 * ExitAlpha)));
            P.StrokeRoundRect(Pill, 13, 1, WithAlpha(Theme::Accent, (uint32_t)(0x80 * ExitAlpha)));
            P.TextLine(F, Pill, Notice.CStr(), WithAlpha(Theme::TextPrimary, (uint32_t)(0xFF * ExitAlpha)), Align::Center);
        }
    }
};

/* The End task button: a short tint pulse when the confirmation is raised, so the press and the
   release read as one motion. */
class EndButton final : public Button {
public:
    EndButton() : Button("End task", ButtonStyle::Accent)
    {
        Glyph = Icon::Close;
        ToolTip = "End the selected task (Delete)";
    }

    void Pulse()
    {
        Tint_.Jump(1.0f);
        Tint_.To(0.0f, 600);
        StartAnimation();
        Invalidate();
    }

    bool Animate() override
    {
        Invalidate();
        return Tint_.Running() || Button::Animate();
    }

    void Paint(Painter &P) override
    {
        Button::Paint(P);
        if (Tint_.Value() > 0.0f)
            P.FillRoundRect(Bounds(), Theme::ControlRadius, WithAlpha(Theme::AccentHover, (uint32_t)(0x48 * Tint_.Value())));
    }

private:
    Transition Tint_;
};

class TaskManager {
public:
    bool Create();

private:
    /* A short tint that fades after a process appears, and one for the ended notice. */
    struct Flash {
        uint32_t Pid = 0;
        Transition Fade;
    };

    Element *BuildProcesses();
    Element *BuildPerformance();
    void Sample();
    void ApplyView();
    void EndSelected();
    void UpdateFlashes(const hz::Vector<ProcessRow> &Next);
    bool PolishStep();
    const ProcessRow *Selected() const;

    Window *Main_ = nullptr;
    TabView *Tabs_ = nullptr;
    TextBox *Filter_ = nullptr;
    Headings *Headings_ = nullptr;
    ProcessList *List_ = nullptr;
    EndButton *End_ = nullptr;
    TextBlock *Summary_ = nullptr;
    Graph *CpuGraph_ = nullptr, *MemoryGraph_ = nullptr;
    ProcessorBars *Bars_ = nullptr;
    HZ_SYSTEM_PROCESSOR_INFORMATION LastCpus_[8] = {};
    uint32_t LastCpuCount_ = 0;
    TextBlock *Stats_[8] = {};

    hz::Vector<ProcessRow> All_;    /* the last sample */
    hz::Vector<size_t> Visible_;    /* indexes into All_, filtered and sorted */
    hz::Vector<Flash> AppearFlashes_;
    hz::String ExitName_;
    Transition ExitFade_;
    uint32_t SelectedPid_ = 0;
    uint64_t LastSampleUs_ = 0, LastIdleUs_ = 0;
    uint32_t CpuTotalTenths_ = 0;
    bool CpuKnown_ = false;  /* an interval has been measured, so CPU is reported */
    bool HasSampled_ = false;
    HZ_SYSTEM_BASIC_INFORMATION Basic_ = {};
};

/* ---- Sampling --------------------------------------------------------------------------------- */

void TaskManager::Sample()
{
    static HZ_SYSTEM_PROCESS_INFORMATION Buffer[kMaxProcesses];
    uint32_t Returned = 0;
    if (!HZ_SUCCESS(NtQuerySystemInformation(HZ_SYSTEM_PROCESS_INFORMATION_CLASS, Buffer, sizeof(Buffer), &Returned)))
        return;
    HZ_SYSTEM_BASIC_INFORMATION Basic = {};
    NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic), nullptr);
    uint64_t Now = Basic.UptimeUs;
    uint64_t Interval = LastSampleUs_ && Now > LastSampleUs_ ? Now - LastSampleUs_ : 0;
    uint32_t Cpus = Basic.ProcessorCount ? Basic.ProcessorCount : 1;

    /* Window titles by process (the first window of each). */
    HWND Windows[64];
    uint32_t WindowCount = 0;
    NtUserEnumWindows(Windows, 64, &WindowCount);
    WindowCount = hz::Min<uint32_t>(WindowCount, 64);
    HZ_WINDOW_INFO Infos[64];
    for (uint32_t i = 0; i < WindowCount; i++)
        if (!HZ_SUCCESS(NtUserGetWindowInfo(Windows[i], &Infos[i])))
            Infos[i].ProcessId = 0;

    hz::Vector<ProcessRow> Next;
    size_t Count = hz::Min<size_t>(Returned / sizeof(HZ_SYSTEM_PROCESS_INFORMATION), kMaxProcesses);
    for (size_t i = 0; i < Count; i++) {
        const HZ_SYSTEM_PROCESS_INFORMATION &S = Buffer[i];
        ProcessRow R;
        R.Pid = S.ProcessId;
        R.Parent = S.ParentProcessId;
        R.Threads = S.ThreadCount;
        R.Handles = S.HandleCount;
        R.Memory = S.ResidentPages * 4096;
        R.CpuUs = S.CpuTimeUs;
        R.Name = S.ImageName;
        for (const ProcessRow &Old : All_) {
            if (Old.Pid == R.Pid && Interval && R.CpuUs >= Old.CpuUs) {
                uint64_t Tenths = (R.CpuUs - Old.CpuUs) * 1000 / (Interval * Cpus);
                R.CpuTenths = (uint32_t)hz::Min<uint64_t>(Tenths, 1000);
                R.CpuKnown = true;
            }
        }
        for (uint32_t w = 0; w < WindowCount; w++) {
            if (Infos[w].ProcessId == R.Pid && R.Window.Empty() && Infos[w].Title[0])
                R.Window = Infos[w].Title;
        }
        Next.Push(hz::Move(R));
    }
    if (Interval && Basic.IdleTimeUs >= LastIdleUs_) {
        uint64_t Idle = (Basic.IdleTimeUs - LastIdleUs_) * 1000 / (Interval * Cpus);
        CpuTotalTenths_ = Idle >= 1000 ? 0 : (uint32_t)(1000 - Idle);
    }
    /* Per processor: idle-time deltas over the same interval. */
    HZ_SYSTEM_PROCESSOR_INFORMATION CpuInfo[8];
    uint32_t CpuBytes = 0;
    NtQuerySystemInformation(HZ_SYSTEM_PROCESSOR_INFORMATION_CLASS, CpuInfo, sizeof(CpuInfo), &CpuBytes);
    uint32_t CpuCount = hz::Min<uint32_t>(CpuBytes / sizeof(CpuInfo[0]), 8);
    hz::Vector<uint32_t> Bars;
    if (Interval && CpuCount == LastCpuCount_) {
        for (uint32_t i = 0; i < CpuCount; i++) {
            uint64_t Idle = CpuInfo[i].IdleTimeUs >= LastCpus_[i].IdleTimeUs ? CpuInfo[i].IdleTimeUs - LastCpus_[i].IdleTimeUs : 0;
            uint64_t IdleTenths = Idle * 1000 / Interval;
            Bars.Push(IdleTenths >= 1000 ? 0 : (uint32_t)(1000 - IdleTenths));
        }
    }
    Bars_->SetValues(hz::Move(Bars)); /* empty until an interval is known: the panel says "not reported" */
    memcpy(LastCpus_, CpuInfo, sizeof(CpuInfo));
    LastCpuCount_ = CpuCount;
    LastSampleUs_ = Now;
    LastIdleUs_ = Basic.IdleTimeUs;
    Basic_ = Basic;
    CpuKnown_ = Interval != 0;
    UpdateFlashes(Next);
    All_ = hz::Move(Next);
    HasSampled_ = true;
    ApplyView();

    /* Performance */
    uint64_t Total = Basic.TotalPhysicalPages * Basic.PageSize, Free = Basic.AvailablePages * Basic.PageSize;
    if (Interval) {
        CpuGraph_->Push(CpuTotalTenths_ / 10.0f);
        CpuGraph_->Caption = FormatPercent(CpuTotalTenths_);
    } else {
        CpuGraph_->Caption = "not reported"; /* the first sample has no interval to measure over */
    }
    if (Total) {
        float MemoryPercent = (float)((Total - Free) * 1000 / Total) / 10.0f;
        MemoryGraph_->Push(MemoryPercent);
        MemoryGraph_->Caption = hz::String::Format("%s of %s in use", hz::fs::FormatSize(Total - Free).CStr(),
                                                   hz::fs::FormatSize(Total).CStr());
    } else {
        MemoryGraph_->Caption = "not reported";
    }
    uint64_t Up = Basic.UptimeUs / 1000000;
    const hz::String Values[8] = {
        CpuKnown_ ? FormatPercent(CpuTotalTenths_) : hz::String("not reported"),
        hz::String::Format("%u", Basic.ProcessorCount),
        hz::String::Format("%u", Basic.ProcessCount),
        hz::String::Format("%u", Basic.ThreadCount),
        hz::String::Format("%llu", (unsigned long long)Basic.ContextSwitches),
        hz::String::Format("%llu", (unsigned long long)Basic.InterruptCount),
        hz::fs::FormatSize(Free),
        hz::String::Format("%llu:%02llu:%02llu", (unsigned long long)(Up / 3600), (unsigned long long)(Up / 60 % 60),
                           (unsigned long long)(Up % 60)),
    };
    for (int i = 0; i < 8; i++)
        Stats_[i]->SetText(Values[i].CStr());
}

/* The appear/exit flashes, by the difference between this sample and the one before it. */
void TaskManager::UpdateFlashes(const hz::Vector<ProcessRow> &Next)
{
    if (!HasSampled_)
        return; /* nothing to compare the first sample against */
    hz::Vector<Flash> Keep;
    for (const ProcessRow &R : Next) {
        bool Seen = false;
        for (const ProcessRow &Old : All_) {
            if (Old.Pid == R.Pid) {
                Seen = true;
                break;
            }
        }
        if (!Seen) {
            Flash F;
            F.Pid = R.Pid;
            F.Fade.Jump(1.0f);
            F.Fade.To(0.0f, 900);
            Keep.Push(hz::Move(F));
        }
    }
    for (Flash &F : AppearFlashes_)
        if (F.Fade.Running())
            Keep.Push(hz::Move(F));
    AppearFlashes_ = hz::Move(Keep);

    for (const ProcessRow &Old : All_) {
        bool Still = false;
        for (const ProcessRow &R : Next) {
            if (R.Pid == Old.Pid) {
                Still = true;
                break;
            }
        }
        if (!Still) {
            ExitName_ = Old.Name;
            ExitFade_.Jump(1.0f);
            ExitFade_.To(0.0f, 1000);
            break;
        }
    }
    if (List_ && (!AppearFlashes_.Empty() || ExitFade_.Running()))
        List_->Kick();
}

/* Runs on every animation frame: keeps the list and the ended notice repainting while they fade. */
bool TaskManager::PolishStep()
{
    bool Running = ExitFade_.Running();
    for (size_t i = 0; i < AppearFlashes_.Size(); i++)
        if (AppearFlashes_[i].Fade.Running())
            Running = true;
    if (!List_)
        return Running;
    if (Running) {
        List_->ExitName = ExitName_;
        List_->ExitAlpha = ExitFade_.Value();
        List_->Invalidate();
    } else if (!List_->ExitName.Empty()) {
        List_->ExitName.Clear();
        List_->ExitAlpha = 0;
        List_->Invalidate();
    }
    return Running;
}

void TaskManager::ApplyView()
{
    const hz::String &Filter = Filter_->Text();
    Visible_.Clear();
    for (size_t i = 0; i < All_.Size(); i++) {
        const ProcessRow &R = All_[i];
        if (!Filter.Empty() && !R.Name.ContainsIgnoreCase(Filter.CStr()) && !R.Window.ContainsIgnoreCase(Filter.CStr()) &&
            hz::String::Format("%u", R.Pid) != Filter)
            continue;
        Visible_.Push(i);
    }
    SortKey Key = Headings_->Key;
    bool Descending = Headings_->Descending;
    auto Less = [this, Key](size_t A, size_t B) {
        const ProcessRow &X = All_[A], &Y = All_[B];
        switch (Key) {
        case SortKey::Pid: return X.Pid < Y.Pid;
        case SortKey::Cpu: return X.CpuTenths < Y.CpuTenths || (X.CpuTenths == Y.CpuTenths && X.Pid < Y.Pid);
        case SortKey::Memory: return X.Memory < Y.Memory || (X.Memory == Y.Memory && X.Pid < Y.Pid);
        case SortKey::Threads: return X.Threads < Y.Threads || (X.Threads == Y.Threads && X.Pid < Y.Pid);
        case SortKey::Handles: return X.Handles < Y.Handles || (X.Handles == Y.Handles && X.Pid < Y.Pid);
        default: {
            int C = strcmp(X.Name.CStr(), Y.Name.CStr());
            return C < 0 || (C == 0 && X.Pid < Y.Pid);
        }
        }
    };
    for (size_t i = 1; i < Visible_.Size(); i++)
        for (size_t j = i; j > 0; j--) {
            size_t A = Visible_[j - 1], B = Visible_[j];
            bool Swap = Descending ? Less(A, B) : Less(B, A);
            if (!Swap)
                break;
            Visible_[j - 1] = B;
            Visible_[j] = A;
        }
    List_->SetItemCount(Visible_.Size());
    size_t Selection = hz::NotFound;
    for (size_t i = 0; i < Visible_.Size(); i++)
        if (All_[Visible_[i]].Pid == SelectedPid_)
            Selection = i;
    List_->SetSelectedIndex(Selection, false);
    List_->Invalidate();
    End_->SetEnabled(Selected() != nullptr);
    hz::String Cpu = CpuKnown_ ? FormatPercent(CpuTotalTenths_) : hz::String("not reported");
    Summary_->SetText(hz::String::Format("%zu of %zu processes  |  CPU %s", Visible_.Size(), All_.Size(), Cpu.CStr()).CStr());
}

const ProcessRow *TaskManager::Selected() const
{
    size_t I = List_->SelectedIndex();
    return I < Visible_.Size() ? &All_[Visible_[I]] : nullptr;
}

/* ---- Ending a process ------------------------------------------------------------------------ */

void TaskManager::EndSelected()
{
    const ProcessRow *R = Selected();
    if (!R)
        return;
    uint32_t Pid = R->Pid;
    hz::String Name = R->Name;
    if (Pid == (uint32_t)HzCurrentPeb()->ProcessId) {
        Main_->Close();
        return;
    }
    hz::String Question = hz::String::Format(
        "End %s (process %u)? Unsaved data in it will be lost.%s", Name.CStr(), Pid,
        hz::fs::EqualNames(Name.CStr(), "shell.exe") ? " Ending the shell removes the taskbar." : "");
    End_->Pulse(); /* the press and the release read as one motion */
    MessageBox(Main_, "End task", Question.CStr(), "End task|Cancel", [this, Pid, Name](int Choice) {
        if (Choice != 0)
            return;
        HANDLE Process = 0;
        STATUS Status = NtOpenProcess(&Process, PROCESS_TERMINATE, Pid);
        if (HZ_SUCCESS(Status)) {
            Status = NtTerminateProcess(Process, 1);
            NtClose(Process);
        }
        if (HZ_SUCCESS(Status)) {
            HzPrint("taskmgr: ended %s (process %u)", Name.CStr(), Pid);
            Sample();
            return;
        }
        HzPrint("taskmgr: cannot end %s (process %u): 0x%08X", Name.CStr(), Pid, (uint32_t)Status);
        const char *Why = Status == STATUS_ACCESS_DENIED ? "Access is denied: the system process cannot be ended."
                          : Status == STATUS_INVALID_CID ? "The process has already ended."
                                                         : "The operation failed.";
        MessageBox(Main_, "End task", hz::String::Format("%s could not be ended. %s", Name.CStr(), Why).CStr(), nullptr, nullptr);
        Sample();
    });
}

/* ---- Layout ----------------------------------------------------------------------------------- */

Element *TaskManager::BuildProcesses()
{
    Grid *Page = new Grid();
    Page->Padding = Thickness(12, 8, 12, 12);
    Page->RowSpacing = 6;
    Page->Rows.Push(GridLength::AutoSize());
    Page->Rows.Push(GridLength::AutoSize());
    Page->Rows.Push(GridLength::Stars(1));

    Grid *Bar = Page->Add<Grid>();
    Bar->ColumnSpacing = 10;
    Bar->Columns.Push(GridLength::Pixels(260));
    Bar->Columns.Push(GridLength::Stars(1));
    Bar->Columns.Push(GridLength::AutoSize());
    Filter_ = Bar->Add<TextBox>("Filter by name or PID");
    Filter_->ToolTip = "Type a name or a PID to filter the list";
    Filter_->OnTextChanged = [this](const hz::String &) { ApplyView(); };
    Filter_->OnSubmit = [this](const hz::String &) { /* Enter: select the first match */
        if (!Visible_.Empty()) {
            List_->SetSelectedIndex(0);
            Main_->SetFocus(List_, true);
        }
    };
    Summary_ = Bar->Add<TextBlock>("", FontRole::Body);
    Summary_->GridColumn = 1;
    Summary_->VerticalAlignment = Align::Center;
    Summary_->Foreground = Theme::TextSecondary;
    End_ = Bar->Add<EndButton>();
    End_->GridColumn = 2;
    End_->OnClick = [this]() { EndSelected(); };

    Grid *Table = Page->Add<Grid>();
    Table->GridRow = 2;
    Table->Background = Theme::CardBackground;
    Table->CornerRadius = Theme::OverlayRadius;
    Table->Rows.Push(GridLength::AutoSize());
    Table->Rows.Push(GridLength::Stars(1));
    Headings_ = Table->Add<Headings>();
    Headings_->OnSortChanged = [this]() { ApplyView(); };

    ProcessList *List = Table->Add<ProcessList>();
    List_ = List;
    List->GridRow = 1;
    List->ItemHeight = 30;
    List->Margin = Thickness(4);
    List->OnDelete = [this]() { EndSelected(); };
    List->OnPolish = [this]() { return PolishStep(); };
    List->RowFlash = [this](uint32_t Pid) -> float {
        for (const Flash &F : AppearFlashes_)
            if (F.Pid == Pid)
                return F.Fade.Value();
        return 0.0f;
    };
    List_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
        if (I >= Visible_.Size())
            return;
        const ProcessRow &Row = All_[Visible_[I]];
        int32_t Left[kColumnCount], Width[kColumnCount];
        ColumnBounds(R.X - 4, R.Width + 8, Left, Width);
        float Flash = List_->RowFlash ? List_->RowFlash(Row.Pid) : 0.0f;
        if (Flash > 0.0f) /* a process that just appeared */
            P.FillRoundRect(Rect{R.X - 12, R.Y, R.Width + 20, R.Height}, Theme::ControlRadius,
                            WithAlpha(Theme::Accent, (uint32_t)(0x38 * Flash)));
        HZ_FONT *F = GetFont(FontRole::Body);
        hz::String Name = Row.Window.Empty() ? Row.Name : hz::String::Format("%s  -  %s", Row.Name.CStr(), Row.Window.CStr());
        P.TextLine(F, Rect{Left[0], R.Y, Width[0], R.Height}, Name.CStr(), Theme::TextPrimary);
        hz::String Cells[5] = {
            hz::String::Format("%u", Row.Pid),
            Row.CpuKnown ? FormatPercent(Row.CpuTenths) : hz::String("not reported"),
            hz::fs::FormatSize(Row.Memory),
            hz::String::Format("%u", Row.Threads),
            hz::String::Format("%u", Row.Handles)};
        HZ_FONT *M = GetFont(FontRole::Mono); /* the numbers line up in a monospaced font */
        for (int c = 1; c < kColumnCount; c++)
            P.TextLine(M, Rect{Left[c], R.Y, Width[c], R.Height}, Cells[c - 1].CStr(),
                       c == 2 && Row.CpuKnown && Row.CpuTenths >= 100 ? Theme::Accent : Theme::TextSecondary, Align::End);
    };
    List_->OnSelectionChanged = [this](size_t I) {
        SelectedPid_ = I < Visible_.Size() ? All_[Visible_[I]].Pid : 0;
        End_->SetEnabled(SelectedPid_ != 0);
    };
    return Page;
}

Element *TaskManager::BuildPerformance()
{
    StackPanel *Page = new StackPanel(Orientation::Vertical, 12);
    Page->Padding = Thickness(16, 12);
    CpuGraph_ = Page->Add<Graph>("CPU", Theme::Accent);
    Bars_ = Page->Add<ProcessorBars>();
    MemoryGraph_ = Page->Add<Graph>("Memory", 0xFF4CC8FF);
    CpuGraph_->Height = 120;
    MemoryGraph_->Height = 120;
    Grid *Stats = Page->Add<Grid>();
    Stats->ColumnSpacing = 16;
    Stats->RowSpacing = 4;
    for (int i = 0; i < 4; i++)
        Stats->Columns.Push(GridLength::Stars(1));
    for (int i = 0; i < 4; i++)
        Stats->Rows.Push(GridLength::AutoSize());
    static const char *const Labels[8] = {"Utilization", "Processors", "Processes", "Threads",
                                          "Context switches", "Interrupts", "Memory available", "Up time"};
    for (int i = 0; i < 8; i++) {
        TextBlock *Label = Stats->Add<TextBlock>(Labels[i], FontRole::Caption);
        Label->Foreground = Theme::TextTertiary;
        Label->GridColumn = (uint8_t)(i % 4);
        Label->GridRow = (uint8_t)(i / 4 * 2);
        Stats_[i] = Stats->Add<TextBlock>("", FontRole::Subtitle);
        Stats_[i]->GridColumn = (uint8_t)(i % 4);
        Stats_[i]->GridRow = (uint8_t)(i / 4 * 2 + 1);
    }
    return Page;
}

bool TaskManager::Create()
{
    Main_ = new Window("Task Manager", 900, 620);
    Main_->SetMinimumSize(620, 400);
    Tabs_ = new TabView();
    Tabs_->Margin = Thickness(4, 0, 4, 0);
    Tabs_->AddTab("Processes", BuildProcesses());
    Tabs_->AddTab("Performance", BuildPerformance());
    Main_->SetContent(Tabs_);
    Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    Sample();
    if (!Main_->Show())
        return false;
    Main_->SetFocus(Filter_);
    Main_->SetTimer(1000, [this]() { Sample(); });
    return true;
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    TaskManager *App = new TaskManager();
    if (!App->Create()) {
        HzPrint("taskmgr: cannot create the window");
        return 1;
    }
    HzPrint("taskmgr: started");
    return RunMessageLoop();
}
