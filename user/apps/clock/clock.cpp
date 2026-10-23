/*
 * Nexora OS - user/apps/clock/clock.cpp
 * Clock: a small Mica window with the time, the date and the uptime, updated every second. The
 * time is shown in the time zone chosen in Settings (M11: shell.ini's UtcOffsetMinutes, read
 * again every 10 seconds).
 */
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

struct ClockFace {
    TextBlock *Time, *Date, *Up;
    int Offset = 0;
    unsigned Ticks = 0;

    void Update()
    {
        if (Ticks++ % 10 == 0)
            Offset = hz::fs::UtcOffsetMinutes();
        uint64_t Now = 0, Uptime = 0;
        NtQuerySystemTime(&Now);
        NtQueryUptime(&Uptime);
        hz::fs::CivilTime T = hz::fs::ToCivil(Now, Offset);
        Time->SetText(hz::String::Format("%02u:%02u:%02u", T.Hour, T.Minute, T.Second).CStr());
        Date->SetText(hz::String::Format("%s %u %s %lld (%s)", hz::fs::WeekdayNames[T.Weekday], T.Day,
                                         hz::fs::MonthNames[(T.Month - 1) % 12], T.Year, hz::fs::FormatUtcOffset(Offset).CStr())
                          .CStr());
        uint64_t UpSeconds = Uptime / 1000000;
        Up->SetText(hz::String::Format("Up %llu:%02llu:%02llu", (unsigned long long)(UpSeconds / 3600),
                                       (unsigned long long)(UpSeconds / 60 % 60), (unsigned long long)(UpSeconds % 60))
                        .CStr());
    }
};

TextBlock *Line(Element *Parent, FontRole Role, Color C)
{
    TextBlock *T = Parent->Add<TextBlock>("", Role);
    T->Foreground = C;
    return T;
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    HZ_DISPLAY_INFO Display;
    memset(&Display, 0, sizeof(Display));
    NtUserGetDisplayInfo(&Display);

    Window Clock("Clock", 300, 170, HZ_WS_CAPTION | HZ_WS_MICA | HZ_WS_MINIMIZEBOX | HZ_WS_VISIBLE);
    ClockFace Face;
    StackPanel *Root = new StackPanel(Orientation::Vertical, 2);
    Root->Padding = Thickness(24, 12);
    Face.Time = Line(Root, FontRole::Title, Theme::TextPrimary);
    Face.Date = Line(Root, FontRole::Body, Theme::TextSecondary);
    Face.Up = Line(Root, FontRole::Caption, Theme::TextTertiary);
    Clock.SetContent(Root);
    Face.Update();
    Clock.OnClosed = []() { NtUserPostQuitMessage(0); };

    const HZ_RECT &Work = Display.WorkArea;
    if (!Clock.ShowAt(Work.Right - 300 - 24, Work.Top + 24)) {
        HzPrint("clock: cannot create the window");
        return 1;
    }
    Clock.SetTimer(1000, [&Face]() { Face.Update(); });
    HzPrint("clock: started");
    return RunMessageLoop();
}
