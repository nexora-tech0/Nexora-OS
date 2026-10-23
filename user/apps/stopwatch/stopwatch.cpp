/*
 * Nexora OS - user/apps/stopwatch/stopwatch.cpp
 * A small stopwatch driven by the system uptime clock.
 */
#include "hzui.h"

using namespace hzui;

namespace {

class Stopwatch {
public:
    bool Create()
    {
        Main_ = new Window("Stopwatch", 360, 220);
        Main_->SetMinimumSize(300, 190);
        StackPanel *Root = new StackPanel(Orientation::Vertical, 10);
        Root->Padding = Thickness(24, 14);
        Display_ = Root->Add<TextBlock>("00:00:00.0", FontRole::Title);
        Display_->HorizontalAlignment = Align::Center;
        Display_->Foreground = Theme::TextPrimary;
        Hint_ = Root->Add<TextBlock>("Ready", FontRole::Caption);
        Hint_->HorizontalAlignment = Align::Center;
        Hint_->Foreground = Theme::TextTertiary;
        StackPanel *Buttons = Root->Add<StackPanel>(Orientation::Horizontal, 8);
        Buttons->HorizontalAlignment = Align::Center;
        Toggle_ = Buttons->Add<Button>("Start", ButtonStyle::Accent);
        Toggle_->OnClick = [this]() { Toggle(); };
        Button *Reset = Buttons->Add<Button>("Reset", ButtonStyle::Standard);
        Reset->OnClick = [this]() { ResetTime(); };
        Main_->SetContent(Root);
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
        Main_->OnUnhandledKey = [this](KeyEvent &E) {
            if (E.Key == HZ_VK_SPACE) {
                E.Handled = true;
                Toggle();
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
            Hint_->SetText("Paused · Space to resume");
        } else {
            StartedAt_ = Now;
            Running_ = true;
            Toggle_->SetText("Pause");
            Hint_->SetText("Space to pause");
        }
        Update();
    }

    void ResetTime()
    {
        Elapsed_ = 0;
        if (Running_)
            StartedAt_ = NowUs();
        else
            Hint_->SetText("Ready");
        Update();
    }

    void Update()
    {
        uint64_t Us = Elapsed_ + (Running_ ? NowUs() - StartedAt_ : 0);
        uint64_t Tenths = Us / 100000;
        uint64_t Hours = Tenths / 36000;
        uint64_t Minutes = Tenths / 600 % 60;
        uint64_t Seconds = Tenths / 10 % 60;
        uint64_t Fraction = Tenths % 10;
        Display_->SetText(hz::String::Format("%02llu:%02llu:%02llu.%llu", (unsigned long long)Hours,
                                             (unsigned long long)Minutes, (unsigned long long)Seconds,
                                             (unsigned long long)Fraction)
                              .CStr());
    }

    Window *Main_ = nullptr;
    TextBlock *Display_ = nullptr, *Hint_ = nullptr;
    Button *Toggle_ = nullptr;
    uint64_t StartedAt_ = 0, Elapsed_ = 0;
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
