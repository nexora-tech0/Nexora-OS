/*
 * Nexora OS - user/apps/run/run.cpp
 * Run dialog for Nexora programs and files. .nxe is a command suffix for a native Nexora tool;
 * the resolver maps it to an implemented image, while ordinary .exe images go through the PE
 * loader and its currently limited Win32 compatibility surface.
 */
#include "hzapps.h"
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

constexpr const char *kSystemFolder = "C:\\Nexora\\System";

bool HasPath(const hz::String &Text)
{
    return Text.Find("\\") != hz::NotFound || Text.Find(":") != hz::NotFound;
}

hz::String SystemPath(const hz::String &Name)
{
    return HasPath(Name) ? Name : hz::fs::Join(hz::String(kSystemFolder), Name.CStr());
}

bool ParseCommand(const hz::String &Text, hz::String &Program, hz::String &Arguments)
{
    size_t At = 0;
    while (At < Text.Length() && (Text[At] == ' ' || Text[At] == '\t'))
        At++;
    if (At == Text.Length())
        return false;

    size_t Begin = At;
    char Quote = 0;
    if (Text[At] == '"') {
        Quote = '"';
        Begin = ++At;
        while (At < Text.Length() && Text[At] != Quote)
            At++;
        if (At == Text.Length())
            return false;
        Program = Text.Substring(Begin, At - Begin);
        At++;
    } else {
        while (At < Text.Length() && Text[At] != ' ' && Text[At] != '\t')
            At++;
        Program = Text.Substring(Begin, At - Begin);
    }
    while (At < Text.Length() && (Text[At] == ' ' || Text[At] == '\t'))
        At++;
    Arguments = Text.Substring(At);
    return !Program.Empty();
}

class RunDialog {
public:
    RunDialog() : Window_("Run", 560, 292, HZ_WS_CAPTION | HZ_WS_MICA | HZ_WS_MINIMIZEBOX | HZ_WS_VISIBLE)
    {
        StackPanel *Root = new StackPanel(Orientation::Vertical, 12);
        Root->Padding = Thickness(22, 20);
        Root->Add<TextBlock>("Open a program, folder, or document", FontRole::Subtitle);
        Root->Add<TextBlock>("Enter a Nexora tool such as taskmgr.nxe, or an executable path.", FontRole::Body);

        Input_ = Root->Add<TextBox>("Program or path");
        Input_->Height = Theme::ControlHeight + 4;
        Input_->MaxLength = 512;
        Input_->OnSubmit = [this](const hz::String &) { Launch(); };

        StackPanel *Actions = Root->Add<StackPanel>(Orientation::Horizontal, 8);
        Actions->HorizontalAlignment = Align::End;
        Button *Cancel = Actions->Add<Button>("Cancel", ButtonStyle::Subtle);
        Cancel->OnClick = [this]() { Window_.Close(); };
        Button *Open = Actions->Add<Button>("Run", ButtonStyle::Accent);
        Open->OnClick = [this]() { Launch(); };

        Status_ = Root->Add<TextBlock>("Tools with no Nexora implementation yet are reported as unavailable.",
                                      FontRole::Caption);
        Status_->Foreground = Theme::TextSecondary;
        Window_.SetContent(Root);
        Window_.OnClosed = []() { NtUserPostQuitMessage(0); };
    }

    bool Show()
    {
        if (!Window_.Show())
            return false;
        Window_.SetFocus(Input_);
        return true;
    }

    void Launch()
    {
        hz::String Program, Arguments;
        if (!ParseCommand(Input_->Text(), Program, Arguments)) {
            SetStatus("Enter a program or file to run.");
            return;
        }

        hz::String Extension = hz::fs::Extension(Program);
        hz::String Image;
        if (hz::fs::EqualNames(Extension.CStr(), "nxe")) {
            Image = SystemPath(hz::apps::ResolveToolImageName(Program));
        } else if (Extension.Empty() || hz::fs::EqualNames(Extension.CStr(), "exe")) {
            if (Extension.Empty())
                Program.Append(".exe");
            Image = SystemPath(Program);
        } else {
            uint32_t Pid = 0;
            hz::String OpenedBy;
            STATUS Status = hz::apps::OpenPath(Program, &Pid, &OpenedBy);
            if (HZ_SUCCESS(Status))
                SetStatus(hz::String::Format("Opened with %s (process %u).", OpenedBy.CStr(), Pid));
            else
                SetStatus(hz::String::Format("Cannot open this item (0x%08X).", (uint32_t)Status));
            return;
        }

        bool Directory = false;
        if (!hz::fs::Exists(Image, &Directory) || Directory) {
            SetStatus(hz::String::Format("Nexora could not find %s.", Image.CStr()));
            return;
        }
        hz::String CommandLine = hz::fs::FileName(Image);
        if (!Arguments.Empty()) {
            CommandLine.Append(' ');
            CommandLine.Append(Arguments);
        }
        HANDLE Process = 0;
        uint32_t Pid = 0;
        STATUS Status = NtCreateProcess(Image.CStr(), CommandLine.CStr(), &Process, &Pid);
        if (HZ_SUCCESS(Status)) {
            NtClose(Process);
            SetStatus(hz::String::Format("Started %s (process %u).", hz::fs::FileName(Image).CStr(), Pid));
            HzPrint("run: started %s (process %u)", hz::fs::FileName(Image).CStr(), Pid);
            return;
        }

        if (Status == STATUS_DLL_NOT_FOUND || Status == STATUS_NOT_SUPPORTED ||
            Status == STATUS_INVALID_IMAGE_NOT_MZ || Status == STATUS_INVALID_IMAGE_FORMAT) {
            hz::String Report;
            if (HZ_SUCCESS(hz::apps::QueryCompatibilityReport(Image, Report))) {
                SetStatus(Report);
                return;
            }
        }
        SetStatus(hz::String::Format("Could not start this program (0x%08X).", (uint32_t)Status));
    }

private:
    void SetStatus(const hz::String &Text)
    {
        Status_->SetText(Text.CStr());
        HzPrint("run: %s", Text.CStr());
    }

    Window Window_;
    TextBox *Input_ = nullptr;
    TextBlock *Status_ = nullptr;
};

}  // namespace

extern "C" int main(int, char **)
{
    RunDialog Dialog;
    if (!Dialog.Show()) {
        HzPrint("run: cannot create the window");
        return 1;
    }
    HzPrint("run: started");
    return RunMessageLoop();
}
