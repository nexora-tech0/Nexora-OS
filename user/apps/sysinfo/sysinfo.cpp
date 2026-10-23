/*
 * Nexora OS - user/apps/sysinfo/sysinfo.cpp
 * Live system counters for processor, memory, processes and uptime.
 */
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

class SystemInfo {
public:
    bool Create()
    {
        Main_ = new Window("System Information", 460, 340);
        Main_->SetMinimumSize(360, 280);
        StackPanel *Root = new StackPanel(Orientation::Vertical, 6);
        Root->Padding = Thickness(22, 14);
        Root->Add<TextBlock>("Nexora OS · live system counters", FontRole::Subtitle);
        Processor_ = AddLine(Root);
        Memory_ = AddLine(Root);
        Available_ = AddLine(Root);
        Processes_ = AddLine(Root);
        Threads_ = AddLine(Root);
        Uptime_ = AddLine(Root);
        Status_ = Root->Add<TextBlock>("Refreshing every second", FontRole::Caption);
        Status_->Foreground = Theme::TextTertiary;
        Main_->SetContent(Root);
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
        if (!Main_->Show())
            return false;
        Refresh();
        Main_->SetTimer(1000, [this]() { Refresh(); });
        HzPrint("sysinfo: started");
        return true;
    }

private:
    static TextBlock *AddLine(Element *Parent)
    {
        TextBlock *Line = Parent->Add<TextBlock>("", FontRole::Body);
        Line->Foreground = Theme::TextPrimary;
        return Line;
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
        uint64_t TotalBytes = Info.TotalPhysicalPages * Info.PageSize;
        uint64_t AvailableBytes = Info.AvailablePages * Info.PageSize;
        Processor_->SetText(hz::String::Format("Processors     %u", Info.ProcessorCount).CStr());
        Memory_->SetText(hz::String::Format("Physical memory     %s", hz::fs::FormatSize(TotalBytes).CStr()).CStr());
        Available_->SetText(hz::String::Format("Available memory    %s", hz::fs::FormatSize(AvailableBytes).CStr()).CStr());
        Processes_->SetText(hz::String::Format("Processes      %u", Info.ProcessCount).CStr());
        Threads_->SetText(hz::String::Format("Threads        %u", Info.ThreadCount).CStr());
        uint64_t Seconds = Info.UptimeUs / 1000000;
        Uptime_->SetText(hz::String::Format("Uptime         %llu:%02llu:%02llu",
                                            (unsigned long long)(Seconds / 3600),
                                            (unsigned long long)(Seconds / 60 % 60),
                                            (unsigned long long)(Seconds % 60))
                             .CStr());
        Status_->SetText("Refreshing every second");
    }

    Window *Main_ = nullptr;
    TextBlock *Processor_ = nullptr, *Memory_ = nullptr, *Available_ = nullptr, *Processes_ = nullptr,
              *Threads_ = nullptr, *Uptime_ = nullptr, *Status_ = nullptr;
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
