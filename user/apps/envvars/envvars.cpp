/* Nexora OS - persistent environment table used by new Terminal sessions. */
#include "hzfile.h"
#include "hzui.h"

using namespace hzui;

namespace {

bool ValidName(const hz::String &Name)
{
    if (Name.Empty() || Name.Length() > 64)
        return false;
    for (size_t Index = 0; Index < Name.Length(); Index++) {
        char C = Name[Index];
        bool Letter = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z');
        bool Digit = C >= '0' && C <= '9';
        if (!(Letter || C == '_' || (Index > 0 && Digit)))
            return false;
    }
    return true;
}

class EnvironmentApp {
public:
    EnvironmentApp() : Main_("Environment Variables", 820, 640)
    {
        Root_ = new Grid();
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::Stars(1));
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->RowSpacing = 12;
        Root_->Padding = Thickness(24, 20);

        Root_->Add<TextBlock>("Environment variables", FontRole::Title)->GridRow = 0;
        TextBlock *Info = Root_->Add<TextBlock>(
            "Values are saved for new Terminal sessions. PATH uses semicolon-separated folders; add C:\\Nexora\\System "
            "to keep built-in commands available.", FontRole::Body);
        Info->GridRow = 1;
        Info->Foreground = Theme::TextSecondary;
        Info->Wrap = true;

        StackPanel *Editor = Root_->Add<StackPanel>(Orientation::Horizontal, 8);
        Editor->GridRow = 2;
        Name_ = Editor->Add<TextBox>("Name");
        Name_->Width = 220;
        Value_ = Editor->Add<TextBox>("Value");
        Value_->Width = 340;
        Value_->OnSubmit = [this](const hz::String &) { SaveEntry(); };
        Button *Save = Editor->Add<Button>("Add / update", ButtonStyle::Accent);
        Save->OnClick = [this]() { SaveEntry(); };

        ScrollViewer *Scroll = Root_->Add<ScrollViewer>();
        Scroll->GridRow = 3;
        Rows_ = new StackPanel(Orientation::Vertical, 6);
        Scroll->SetContent(Rows_);
        Status_ = Root_->Add<TextBlock>("", FontRole::Caption);
        Status_->GridRow = 4;
        Status_->Foreground = Theme::TextSecondary;

        Main_.SetContent(Root_);
        Main_.OnClosed = []() { NtUserPostQuitMessage(0); };
    }

    bool Show()
    {
        if (!Main_.Show())
            return false;
        Main_.SetFocus(Name_);
        Reload();
        return true;
    }

private:
    void Reload()
    {
        Settings_.Load(hz::fs::SettingsPath("environment"));
        Rows_->ClearChildren();
        if (Settings_.Items().Empty()) {
            TextBlock *Empty = Rows_->Add<TextBlock>("No saved variables. Built-in defaults are provided by Terminal.",
                                                     FontRole::Body);
            Empty->Foreground = Theme::TextSecondary;
            return;
        }
        for (const hz::fs::Settings::Item &Item : Settings_.Items()) {
            StackPanel *Row = Rows_->Add<StackPanel>(Orientation::Horizontal, 10);
            Row->Padding = Thickness(12, 8);
            Row->Background = Theme::CardBackground;
            Row->BorderColor = Theme::CardStroke;
            Row->CornerRadius = 12;
            TextBlock *Label = Row->Add<TextBlock>(hz::String::Format("%s = %s", Item.Key.CStr(), Item.Value.CStr()).CStr(),
                                                   FontRole::Body);
            Label->Width = 570;
            Label->Foreground = Theme::TextPrimary;
            Label->Ellipsis = true;
            Button *Edit = Row->Add<Button>("Edit", ButtonStyle::Subtle);
            Edit->OnClick = [this, Key = Item.Key, Value = Item.Value]() {
                Name_->SetText(Key.CStr());
                Value_->SetText(Value.CStr());
                Main_.SetFocus(Value_);
            };
            Button *Remove = Row->Add<Button>("Remove", ButtonStyle::Subtle);
            Remove->OnClick = [this, Key = Item.Key]() {
                Settings_.Unset(Key.CStr());
                Persist("Variable removed. New Terminal sessions will use the default value.");
            };
        }
    }

    void SaveEntry()
    {
        hz::String Key = Name_->Text();
        hz::String Value = Value_->Text();
        if (!ValidName(Key)) {
            SetStatus("Use a name starting with a letter or underscore; letters, digits, and underscores are allowed.",
                      false);
            return;
        }
        Settings_.Set(Key.CStr(), Value.CStr());
        Persist("Variable saved for new Terminal sessions.");
        Name_->SetText("");
        Value_->SetText("");
        Main_.SetFocus(Name_);
    }

    void Persist(const char *Message)
    {
        STATUS Result = Settings_.Save(hz::fs::SettingsPath("environment"));
        if (!HZ_SUCCESS(Result)) {
            SetStatus(hz::String::Format("Could not save settings (0x%08X).", (uint32_t)Result).CStr(), false);
            return;
        }
        Reload();
        SetStatus(Message, true);
    }

    void SetStatus(const char *Message, bool Good)
    {
        Status_->Foreground = Good ? Theme::Accent : Theme::TextSecondary;
        Status_->SetText(Message);
    }

    Window Main_;
    Grid *Root_ = nullptr;
    TextBox *Name_ = nullptr, *Value_ = nullptr;
    StackPanel *Rows_ = nullptr;
    TextBlock *Status_ = nullptr;
    hz::fs::Settings Settings_;
};

}  // namespace

extern "C" int main(int, char **)
{
    EnvironmentApp App;
    if (!App.Show()) {
        HzPrint("envvars: cannot create the window");
        return 1;
    }
    HzPrint("envvars: started");
    return RunMessageLoop();
}
