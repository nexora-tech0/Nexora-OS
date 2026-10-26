/*
 * Nexora OS - user/apps/calendar/calendar.cpp
 * A month calendar with local-time date selection.
 */
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

unsigned DaysInMonth(long long Year, unsigned Month)
{
    static const unsigned Days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (Month == 2 && (Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0)))
        return 29;
    return Days[Month - 1];
}

int64_t DaysBeforeYear(long long Year)
{
    long long Last = Year - 1;
    return 365 * (Year - 1970) + (Last / 4 - 1969 / 4) - (Last / 100 - 1969 / 100) +
           (Last / 400 - 1969 / 400);
}

unsigned Weekday(long long Year, unsigned Month, unsigned Day)
{
    int64_t Days = DaysBeforeYear(Year);
    for (unsigned M = 1; M < Month; M++)
        Days += DaysInMonth(Year, M);
    Days += Day - 1;
    int64_t Result = (Days + 4) % 7;
    return (unsigned)(Result < 0 ? Result + 7 : Result);
}

class MonthView final : public Element {
public:
    long long Year = 1970;
    unsigned Month = 1;
    unsigned TodayDay = 0;
    unsigned SelectedDay = 0;
    long long TodayYear = 0;
    unsigned TodayMonth = 0;
    hz::Function<void(unsigned)> OnSelected;

    MonthView()
    {
        Height = 310;
        Focusable = true;
        Grid_.Jump(1.0f);   /* the first month is drawn fully, no easing */
        Select_.Jump(1.0f);
    }

    /* Switches the shown month; Direction (-1 or +1) is the side the grid eases in from. */
    void SetMonth(long long Y, unsigned M, int Direction)
    {
        Year = Y;
        Month = M;
        Hot_ = 0;
        Hover_.Jump(0.0f);
        SlideDir_ = Direction;
        Grid_.Jump(0.0f);
        Grid_.To(1.0f, Motion::ControlStateMs);
        StartAnimation();
        Invalidate();
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillRoundRect(B, Theme::OverlayRadius, Theme::CardBackground);
        P.StrokeRoundRect(B, Theme::OverlayRadius, 1, Theme::CardStroke);

        const CellLayout L = Layout();
        const float Step = Grid_.Value();            /* 0 -> the new month is easing in */
        const float Alpha = 0.35f + 0.65f * Step;
        const int32_t Slide = (int32_t)((1.0f - Step) * 14.0f) * SlideDir_;

        static const char *const Weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        for (unsigned C = 0; C < 7; C++) {
            Rect R{B.X + (int32_t)((float)C * L.CellWidth), B.Y + HeaderTop, (int32_t)L.CellWidth, HeaderH};
            P.TextLine(GetFont(FontRole::Caption), R, Weekdays[C], Theme::TextTertiary, Align::Center);
        }
        P.FillRect(Rect{B.X + 10, B.Y + HeaderTop + HeaderH + 2, B.Width - 20, 1},
                   ScaleAlpha(Theme::Divider, Alpha));

        const int32_t First = (int32_t)Weekday(Year, Month, 1);
        const int32_t Count = (int32_t)DaysInMonth(Year, Month);
        const int32_t PrevMonth = Month == 1 ? 12 : (int32_t)Month - 1;
        const long long PrevYear = Month == 1 ? Year - 1 : Year;
        const int32_t PrevCount = (int32_t)DaysInMonth(PrevYear, (unsigned)PrevMonth);
        const float Hover = Hover_.Value();
        const float Grow = Select_.Value();

        P.PushClip(B);
        for (int32_t Slot = 0; Slot < 42; Slot++) {
            const int32_t Row = Slot / 7, Column = Slot % 7;
            const float Cx = (float)B.X + ((float)Column + 0.5f) * L.CellWidth + (float)Slide;
            const float Cy = (float)L.GridTop + ((float)Row + 0.5f) * L.RowHeight;
            const int32_t Day = Slot - First + 1;
            const bool InMonth = Day >= 1 && Day <= Count;
            /* The greyed neighbours: the tail of the previous month, then the head of the next. */
            const int32_t Number =
                InMonth ? Day
                        : (Slot < First ? PrevCount - (First - 1 - Slot) : Slot - First - Count + 1);
            const bool IsSelected = InMonth && (unsigned)Number == SelectedDay;
            const bool IsToday = InMonth && Year == TodayYear && Month == TodayMonth &&
                                 (unsigned)Number == TodayDay;
            const bool IsHot = InMonth && (unsigned)Number == Hot_;

            float Radius = hz::Min(L.CellWidth, L.RowHeight) * 0.5f - 3.0f;
            if (Radius < 8.0f)
                Radius = 8.0f;

            if (IsHot && !IsSelected)
                P.FillCircle(Cx, Cy, Radius, ScaleAlpha(Theme::Accent, 0.22f * Hover * Alpha));
            if (IsSelected)
                P.FillCircle(Cx, Cy, Radius * (0.7f + 0.3f * Grow), ScaleAlpha(Theme::Accent, Alpha));
            else if (IsToday)
                P.StrokeCircle(Cx, Cy, Radius - 1.0f, 1.5f, ScaleAlpha(Theme::Accent, 0.9f * Alpha));

            const Color Ink = IsSelected ? Theme::TextOnAccent
                            : !InMonth   ? Theme::TextDisabled
                            : IsToday    ? Theme::Accent
                                         : Theme::TextPrimary;
            const FontRole Role = (IsSelected || IsToday) ? FontRole::BodyStrong : FontRole::Body;
            Rect Text{(int32_t)(Cx - L.CellWidth * 0.5f), (int32_t)(Cy - L.RowHeight * 0.5f),
                      (int32_t)L.CellWidth, (int32_t)L.RowHeight};
            P.TextLine(GetFont(Role), Text, hz::String::Format("%d", Number).CStr(),
                       ScaleAlpha(Ink, InMonth ? Alpha : 0.55f * Alpha), Align::Center);
        }
        P.PopClip();
    }

    void PaintOverlay(Painter &P) override
    {
        if (!ShowsFocusRing() || SelectedDay == 0)
            return;
        const CellLayout L = Layout();
        const int32_t Slot = (int32_t)Weekday(Year, Month, 1) + (int32_t)SelectedDay - 1;
        const int32_t Row = Slot / 7, Column = Slot % 7;
        float Radius = hz::Min(L.CellWidth, L.RowHeight) * 0.5f - 3.0f;
        if (Radius < 8.0f)
            Radius = 8.0f;
        const float Cx = (float)Bounds().X + ((float)Column + 0.5f) * L.CellWidth;
        const float Cy = (float)L.GridTop + ((float)Row + 0.5f) * L.RowHeight;
        P.FocusRing(Rect{(int32_t)(Cx - Radius), (int32_t)(Cy - Radius), (int32_t)(Radius * 2.0f),
                         (int32_t)(Radius * 2.0f)},
                    Radius);
    }

    void OnPointerMove(PointerEvent &E) override { SetHot(DayAt(E.Position)); }

    void OnPointerLeave() override { SetHot(0); }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT)
            return;
        const unsigned Day = DayAt(E.Position);
        if (Day == 0)
            return;
        Focus();
        SetHot(Day);
        SelectDay(Day);
        E.Handled = true;
    }

    void OnKeyDown(KeyEvent &E) override
    {
        int32_t Delta = 0;
        if (E.Key == HZ_VK_LEFT)
            Delta = -1;
        else if (E.Key == HZ_VK_RIGHT)
            Delta = 1;
        else if (E.Key == HZ_VK_UP)
            Delta = -7;
        else if (E.Key == HZ_VK_DOWN)
            Delta = 7;
        else
            return;
        const int32_t Count = (int32_t)DaysInMonth(Year, Month);
        const int32_t Current = SelectedDay == 0 ? 1 : (int32_t)SelectedDay;
        const int32_t Next = hz::Clamp(Current + Delta, 1, Count);
        E.Handled = true;
        if ((unsigned)Next != SelectedDay)
            SelectDay((unsigned)Next);
    }

    bool Animate() override
    {
        Invalidate();
        return Grid_.Running() || Hover_.Running() || Select_.Running();
    }

private:
    struct CellLayout {
        float CellWidth = 1.0f, RowHeight = 1.0f;
        int32_t GridTop = 0;
    };

    static constexpr int32_t HeaderTop = 12;
    static constexpr int32_t HeaderH = 28;

    /* The grid geometry, shared by painting and hit testing so they cannot drift apart. */
    CellLayout Layout() const
    {
        const Rect &B = Bounds();
        const int32_t GridTop = B.Y + HeaderTop + HeaderH + 4;
        const int32_t GridH = hz::Max(B.Bottom() - GridTop - 10, 1);
        return CellLayout{(float)B.Width / 7.0f, (float)GridH / 6.0f, GridTop};
    }

    /* The day (1-based) of the current month under P, or 0 for a neighbour or a blank cell. */
    unsigned DayAt(Point P) const
    {
        const Rect &B = Bounds();
        const CellLayout L = Layout();
        const int32_t Column = (int32_t)((float)(P.X - B.X) / L.CellWidth);
        const int32_t Row = (int32_t)((float)(P.Y - L.GridTop) / L.RowHeight);
        if (Column < 0 || Column >= 7 || Row < 0 || Row >= 6)
            return 0;
        const int32_t Day = Row * 7 + Column - (int32_t)Weekday(Year, Month, 1) + 1;
        if (Day < 1 || Day > (int32_t)DaysInMonth(Year, Month))
            return 0;
        return (unsigned)Day;
    }

    void SetHot(unsigned Day)
    {
        if (Day == Hot_)
            return;
        Hot_ = Day;
        Hover_.To(Day ? 1.0f : 0.0f, Day ? Motion::HoverEnterMs : Motion::HoverLeaveMs);
        StartAnimation();
        Invalidate();
    }

    void SelectDay(unsigned Day)
    {
        SelectedDay = Day;
        Select_.Jump(0.0f);
        Select_.To(1.0f, Motion::ControlStateMs);
        StartAnimation();
        Invalidate();
        if (OnSelected)
            OnSelected(Day);
    }

    unsigned Hot_ = 0;
    int SlideDir_ = 1;
    Transition Grid_, Hover_, Select_;
};

class CalendarApp {
public:
    bool Create()
    {
        uint64_t Now = 0;
        NtQuerySystemTime(&Now);
        int Offset = hz::fs::UtcOffsetMinutes();
        Today_ = hz::fs::ToCivil(Now, Offset);
        Year_ = Today_.Year;
        Month_ = Today_.Month;

        Main_ = new Window("Calendar", 520, 420);
        Main_->SetMinimumSize(380, 360);
        StackPanel *Root = new StackPanel(Orientation::Vertical, 4);
        Root->Padding = Thickness(20, 16);
        Grid *Header = Root->Add<Grid>();
        Header->Columns.Push(GridLength::AutoSize());
        Header->Columns.Push(GridLength::Stars(1));
        Header->Columns.Push(GridLength::AutoSize());
        Button *Previous = Header->Add<Button>("‹", ButtonStyle::Subtle);
        Previous->GridColumn = 0;
        Previous->ToolTip = "Previous month";
        Previous->OnClick = [this]() { MoveMonth(-1); };
        MonthLabel_ = Header->Add<TextBlock>("", FontRole::Subtitle);
        MonthLabel_->GridColumn = 1;
        MonthLabel_->HorizontalAlignment = Align::Center;
        Button *Next = Header->Add<Button>("›", ButtonStyle::Subtle);
        Next->GridColumn = 2;
        Next->ToolTip = "Next month";
        Next->OnClick = [this]() { MoveMonth(1); };

        View_ = Root->Add<MonthView>();
        View_->Year = Year_;
        View_->Month = Month_;
        View_->TodayYear = Today_.Year;
        View_->TodayMonth = Today_.Month;
        View_->TodayDay = Today_.Day;
        View_->SelectedDay = Today_.Day;
        View_->OnSelected = [this](unsigned Day) { UpdateSelection(Day); };
        Selected_ = Root->Add<TextBlock>("", FontRole::Body);
        Selected_->Foreground = Theme::TextSecondary;
        Main_->SetContent(Root);
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
        if (!Main_->Show())
            return false;
        UpdateMonth();
        UpdateSelection(Today_.Day);
        HzPrint("calendar: started");
        return true;
    }

private:
    void MoveMonth(int Delta)
    {
        int Month = (int)Month_ + Delta;
        if (Month < 1) {
            Month = 12;
            Year_--;
        } else if (Month > 12) {
            Month = 1;
            Year_++;
        }
        Month_ = (unsigned)Month;
        unsigned Limit = DaysInMonth(Year_, Month_);
        unsigned SelectedDay = hz::Min<unsigned>(View_->SelectedDay, Limit);
        View_->SelectedDay = SelectedDay;
        View_->SetMonth(Year_, Month_, Delta);
        UpdateMonth();
        UpdateSelection(SelectedDay);
    }

    void UpdateMonth()
    {
        MonthLabel_->SetText(hz::String::Format("%s %lld", hz::fs::MonthNames[Month_ - 1], Year_).CStr());
    }

    void UpdateSelection(unsigned Day)
    {
        Selected_->SetText(hz::String::Format("%s %u %s %lld", hz::fs::WeekdayNames[Weekday(Year_, Month_, Day)],
                                              Day, hz::fs::MonthNames[Month_ - 1], Year_)
                               .CStr());
    }

    Window *Main_ = nullptr;
    MonthView *View_ = nullptr;
    TextBlock *MonthLabel_ = nullptr, *Selected_ = nullptr;
    hz::fs::CivilTime Today_{};
    long long Year_ = 1970;
    unsigned Month_ = 1;
};

}  // namespace

extern "C" int main(int argc, char **argv)
{
    CalendarApp App;
    if (!App.Create()) {
        HzPrint("calendar: cannot create the window");
        return 1;
    }
    return RunMessageLoop();
}
