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

    MonthView() { Height = 310; }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillRoundRect(B, Theme::OverlayRadius, Theme::CardBackground);
        P.StrokeRoundRect(B, Theme::OverlayRadius, 1, Theme::CardStroke);
        static const char *const Weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        int32_t Cell = B.Width / 7;
        int32_t Top = B.Y + 10;
        for (unsigned C = 0; C < 7; C++)
            P.TextLine(GetFont(FontRole::Caption), Rect{B.X + (int32_t)C * Cell, Top, Cell, 26}, Weekdays[C],
                       Theme::TextTertiary, Align::Center);

        unsigned First = Weekday(Year, Month, 1);
        unsigned Count = DaysInMonth(Year, Month);
        for (unsigned Day = 1; Day <= Count; Day++) {
            unsigned Slot = First + Day - 1;
            unsigned Row = Slot / 7;
            unsigned Column = Slot % 7;
            int32_t X = B.X + (int32_t)Column * Cell;
            int32_t Y = Top + 32 + (int32_t)Row * 39;
            bool IsSelected = Day == SelectedDay;
            bool IsToday = Year == TodayYear && Month == TodayMonth && Day == TodayDay;
            if (IsSelected || IsToday) {
                float Radius = IsSelected ? 14.0f : 12.0f;
                Color Fill = IsSelected ? Theme::Accent : ScaleAlpha(Theme::Accent, 0.28f);
                P.FillCircle((float)(X + Cell / 2), (float)(Y + 14), Radius, Fill);
            }
            P.TextLine(GetFont(FontRole::Body), Rect{X, Y + 2, Cell, 25}, hz::String::Format("%u", Day).CStr(),
                       IsSelected ? Theme::TextOnAccent : Theme::TextPrimary, Align::Center);
        }
    }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button != HZ_MOUSE_LEFT)
            return;
        const Rect &B = Bounds();
        int32_t Cell = B.Width / 7;
        if (Cell <= 0)
            return;
        int32_t Column = (E.Position.X - B.X) / Cell;
        int32_t Row = (E.Position.Y - B.Y - 42) / 39;
        if (Column < 0 || Column >= 7 || Row < 0 || Row >= 6)
            return;
        unsigned Day = (unsigned)(Row * 7 + Column + 1) - Weekday(Year, Month, 1);
        if (Day == 0 || Day > DaysInMonth(Year, Month))
            return;
        SelectedDay = Day;
        Invalidate();
        if (OnSelected)
            OnSelected(Day);
        E.Handled = true;
    }
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
        Root->Padding = Thickness(18, 12);
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
        View_->Year = Year_;
        View_->Month = Month_;
        View_->SelectedDay = SelectedDay;
        View_->Invalidate();
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
