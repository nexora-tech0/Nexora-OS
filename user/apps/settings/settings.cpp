/*
 * Nexora OS - user/apps/settings/settings.cpp
 * Settings: a navigation list on the left and one page on the right.
 *   - Personalization: the three wallpapers. The choice is sent to the shell (HZ_WM_SHELLREQUEST
 *     to NtUserGetShellWindow) so the shell's quick settings stay in step; the current wallpaper
 *     comes from HZ_DISPLAY_INFO.Wallpaper.
 *   - Display: resolution, refresh interval, the work area.
 *   - Storage: every volume with its file system and use.
 *   - Accounts (M11): the user name (C:\Users\<name> follows a change) and the password (a salted
 *     hash, hzcrypto.h; the current one is needed to change or remove it).
 *   - Time: the date and time in the chosen time zone (M11: a UTC offset in 30-minute steps; the
 *     clock itself keeps UTC), the uptime.
 *   - About: version, processors, memory, processes and threads.
 * Accounts and the time zone are saved to shell.ini, and the shell is told to read it again
 * (HZ_SHELL_REQUEST_SETTINGS); the live system cannot save them. Pages that show live values
 * refresh every second.
 */
#include "hzcrypto.h"
#include "hzfile.h"
#include "hzui.h"
#include "hzversion.h"

using namespace hzui;

namespace {

/* The kernel's wallpaper palettes (win32k/compose.cpp), for the previews. */
struct Swatch {
    Color Top, Middle, Bottom, Glow;
    const char *Name;
    const char *Description;
};
const Swatch kSwatches[HZ_MAX_WALLPAPER + 1] = {
    {0xFF0B0E26, 0xFF261A4A, 0xFF4A2448, 0xFFFF8A4C, "Nexora dusk", "Violet sky, the sun setting"},
    {0xFF050D22, 0xFF0F2A52, 0xFF1C4E74, 0xFF4CC8FF, "Blue hour", "Deep blue, a cool glow"},
    {0xFF161028, 0xFF4E2E58, 0xFF9A5466, 0xFFFFC27A, "Dawn", "Warm rose, the sun rising"},
};

bool CurrentWallpaper(uint32_t &Wallpaper)
{
    HZ_DISPLAY_INFO Info;
    memset(&Info, 0, sizeof(Info));
    STATUS Status = NtUserGetDisplayInfo(&Info);
    if (!HZ_SUCCESS(Status) || Info.Wallpaper > HZ_MAX_WALLPAPER)
        return false;
    Wallpaper = Info.Wallpaper;
    return true;
}

class WallpaperCard final : public Element {
public:
    WallpaperCard(uint32_t Index, uint32_t *Selected, TextBlock *Status)
        : Index_(Index), Selected_(Selected), Status_(Status)
    {
        Height = 150;
        Focusable = true;
        ToolTip = kSwatches[Index].Description;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        const Swatch &S = kSwatches[Index_];
        Rect Image{B.X + 4, B.Y + 4, B.Width - 8, 112 - 8};
        P.PushClip(Image);
        P.FillVerticalGradient(Rect{Image.X, Image.Y, Image.Width, Image.Height * 6 / 10}, 0, S.Top, S.Middle);
        P.FillVerticalGradient(Rect{Image.X, Image.Y + Image.Height * 6 / 10, Image.Width, Image.Height - Image.Height * 6 / 10},
                               0, S.Middle, S.Bottom);
        P.FillCircle(Image.X + Image.Width / 2.0f, (float)Image.Bottom() + 16, 44, ScaleAlpha(S.Glow, 0.55f));
        P.FillRect(Rect{Image.X, Image.Bottom() - 1, Image.Width, 1}, ScaleAlpha(S.Glow, 0.8f));
        P.PopClip();
        bool Chosen = *Selected_ == Index_;
        if (Chosen)
            P.StrokeRoundRect(Rect{B.X, B.Y, B.Width, 112}, 8, 3, Theme::Accent);
        else if (IsHovered())
            P.StrokeRoundRect(Rect{B.X, B.Y, B.Width, 112}, 8, 1, Theme::ControlStrongStroke);
        P.TextLine(GetFont(Chosen ? FontRole::BodyStrong : FontRole::Body), Rect{B.X + 4, B.Y + 118, B.Width - 8, 24}, S.Name,
                   Theme::TextPrimary);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Rect{Bounds().X, Bounds().Y, Bounds().Width, 112}, 8);
    }

    void OnPointerEnter() override { Invalidate(); }
    void OnPointerLeave() override { Invalidate(); }
    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT) {
            Pressed_ = true;
            E.Handled = true;
        }
    }
    void OnPointerUp(PointerEvent &E) override
    {
        bool WasPressed = Pressed_;
        Pressed_ = false;
        if (WasPressed && E.Button == HZ_MOUSE_LEFT && Bounds().Contains(E.Position))
            Choose();
    }
    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_RETURN || E.Key == HZ_VK_SPACE) {
            E.Handled = true;
            Choose();
        }
    }

    void Choose()
    {
        HWND Shell = 0;
        if (!HZ_SUCCESS(NtUserGetShellWindow(&Shell)) || !Shell) {
            HzPrint("settings: no shell to change the wallpaper");
            Status_->Foreground = Theme::Accent;
            Status_->SetText("The shell is not available to change the wallpaper.");
            return;
        }
        STATUS Status = NtUserPostMessage(Shell, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_WALLPAPER, Index_);
        if (!HZ_SUCCESS(Status)) {
            HzPrint("settings: wallpaper request failed (0x%08X)", (uint32_t)Status);
            Status_->Foreground = Theme::Accent;
            Status_->SetText(hz::String::Format("The wallpaper request failed (0x%08X).", (uint32_t)Status).CStr());
            return;
        }
        *Selected_ = Index_;
        HzPrint("settings: wallpaper %u requested", Index_);
        Status_->Foreground = Theme::TextSecondary;
        Status_->SetText("Wallpaper change requested.");
        if (Parent())
            Parent()->Invalidate();
    }

private:
    uint32_t Index_;
    uint32_t *Selected_;
    TextBlock *Status_;
    bool Pressed_ = false;
};

/* A titled card with label/value rows. */
class InfoCard final : public StackPanel {
public:
    explicit InfoCard(const char *Title) : StackPanel(Orientation::Vertical, 4)
    {
        Background = Theme::CardBackground;
        BorderColor = Theme::CardStroke;
        CornerRadius = Theme::OverlayRadius;
        Padding = Thickness(16, 12);
        if (Title && *Title)
            Add<TextBlock>(Title, FontRole::BodyStrong)->Margin = Thickness(0, 0, 0, 6);
    }

    TextBlock *Row(const char *Label)
    {
        Grid *G = Add<Grid>();
        G->Columns.Push(GridLength::Pixels(132));
        G->Columns.Push(GridLength::Stars(1));
        TextBlock *L = G->Add<TextBlock>(Label, FontRole::Body);
        L->Foreground = Theme::TextSecondary;
        TextBlock *V = G->Add<TextBlock>("", FontRole::Body);
        V->GridColumn = 1;
        return V;
    }
};

struct Page {
    const char *Title;
    Icon Glyph;
    Element *Content;
    const char *Keywords;
    hz::Function<void()> Update;
    bool Periodic = false;
};

class SettingsApp {
public:
    bool Create(const char *Initial);

private:
    StackPanel *NewPage(const char *Title, const char *Subtitle);
    void AddPersonalization();
    void AddAccounts();
    void ApplyName();
    void ApplyPassword();
    void ChangeOffset(int Minutes);
    void AddDisplay();
    void AddStorage();
    void AddNetwork();
    void AddTime();
    void AddAbout();
    void AddHome();
    void AddUnavailablePages();
    void Show(size_t Index);
    void OpenPage(const char *Title);
    void FilterNavigation(const hz::String &Query);

    Window *Main_ = nullptr;
    ListView *Nav_ = nullptr;
    TextBox *Search_ = nullptr;
    ScrollViewer *Host_ = nullptr;
    Grid *Pages_ = nullptr;
    hz::Vector<Page> List_;
    hz::Vector<size_t> NavigationPages_;
    size_t Current_ = hz::NotFound;
    uint32_t Wallpaper_ = 0;
    /* Accounts */
    TextBox *AccountName_ = nullptr, *OldPassword_ = nullptr, *NewPassword_ = nullptr, *Confirm_ = nullptr;
    TextBlock *NameNote_ = nullptr, *PasswordNote_ = nullptr, *PasswordState_ = nullptr;
    TextBlock *TimeNote_ = nullptr;
    Button *ChangePassword_ = nullptr;
};

/* shell.ini, and the shell told to read it again. */
STATUS SaveShellSettings(const hz::fs::Settings &S, bool *ShellNotified = nullptr)
{
    if (hz::fs::IsLiveSystem())
        return STATUS_MEDIA_WRITE_PROTECTED;
    STATUS Status = S.Save(hz::fs::SettingsPath("shell"));
    bool Notified = false;
    if (HZ_SUCCESS(Status)) {
        HWND Shell = 0;
        STATUS NotifyStatus = NtUserGetShellWindow(&Shell);
        if (HZ_SUCCESS(NotifyStatus) && Shell)
            NotifyStatus = NtUserPostMessage(Shell, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_SETTINGS, 0);
        else if (HZ_SUCCESS(NotifyStatus))
            NotifyStatus = STATUS_NOT_FOUND;
        Notified = HZ_SUCCESS(NotifyStatus);
        if (!Notified)
            HzPrint("settings: saved shell settings but notification failed (0x%08X)", (uint32_t)NotifyStatus);
    }
    if (ShellNotified)
        *ShellNotified = Notified;
    return Status;
}

STATUS LoadShellSettings(hz::fs::Settings &S)
{
    return S.Load(hz::fs::SettingsPath("shell"));
}

StackPanel *SettingsApp::NewPage(const char *Title, const char *Subtitle)
{
    StackPanel *P = Pages_->Add<StackPanel>(Orientation::Vertical, 12);
    P->Padding = Thickness(28, 12, 28, 24);
    P->Add<TextBlock>(Title, FontRole::Title);
    TextBlock *Sub = P->Add<TextBlock>(Subtitle, FontRole::Body);
    Sub->Foreground = Theme::TextSecondary;
    Sub->Wrap = true;
    Sub->Margin = Thickness(0, 0, 0, 8);
    P->SetVisible(false);
    return P;
}

void SettingsApp::AddPersonalization()
{
    StackPanel *P = NewPage("Personalization", "Choose the desktop wallpaper. The change applies right away.");
    TextBlock *Status = P->Add<TextBlock>("", FontRole::Caption);
    Status->Wrap = true;
    Grid *Cards = P->Add<Grid>();
    Cards->Columns.Push(GridLength::Stars(1));
    Cards->Columns.Push(GridLength::Stars(1));
    Cards->Rows.Push(GridLength::AutoSize());
    Cards->Rows.Push(GridLength::AutoSize());
    Cards->ColumnSpacing = 8;
    Cards->RowSpacing = 8;
    for (uint32_t i = 0; i <= HZ_MAX_WALLPAPER; i++) {
        WallpaperCard *Card = Cards->Add<WallpaperCard>(i, &Wallpaper_, Status);
        Card->GridColumn = i % 2;
        Card->GridRow = i / 2;
    }
    InfoCard *Colors = P->Add<InfoCard>("Colors");
    Colors->Row("Mode")->SetText("Dark");
    Colors->Row("Accent color")->SetText("Nexora orange");
    Colors->Row("Transparency")->SetText("Wallpaper-backed surfaces (blur is unavailable)");
    List_.Push(Page{"Personalization", Icon::Sun, P, "wallpaper background colors theme appearance", [this, Cards, Status]() {
                        uint32_t Now = 0;
                        if (!CurrentWallpaper(Now)) {
                            Status->Foreground = Theme::Accent;
                            Status->SetText("The active wallpaper could not be queried.");
                            return;
                        }
                        Status->SetText("");
                        if (Now != Wallpaper_) {
                            Wallpaper_ = Now;
                            Cards->Invalidate();
                        }
                    }});
}

void SettingsApp::AddAccounts()
{
    bool Live = hz::fs::IsLiveSystem();
    StackPanel *P = NewPage("Accounts", Live ? "The live system does not keep settings: install Nexora OS to choose a "
                                               "name and a password."
                                             : "Your name, and the password the lock screen asks for.");
    InfoCard *Who = P->Add<InfoCard>("Your name");
    StackPanel *NameRow = Who->Add<StackPanel>(Orientation::Vertical, 8);
    AccountName_ = NameRow->Add<TextBox>("Your name");
    AccountName_->MaxLength = 32;
    hz::fs::Settings InitialSettings;
    STATUS LoadStatus = LoadShellSettings(InitialSettings);
    AccountName_->SetText(HZ_SUCCESS(LoadStatus) ? InitialSettings.Get("UserName").CStr() : "", false);
    AccountName_->OnSubmit = [this](const hz::String &) { ApplyName(); };
    Button *SaveName = NameRow->Add<Button>("Save name");
    SaveName->OnClick = [this]() { ApplyName(); };
    NameNote_ = Who->Add<TextBlock>("Letters, digits, spaces, '-', '_' and '.' (up to 32 characters). Your documents "
                                    "folder is renamed with it.",
                                    FontRole::Caption);
    NameNote_->Foreground = Theme::TextTertiary;
    NameNote_->Wrap = true;

    InfoCard *Pw = P->Add<InfoCard>("Password");
    PasswordState_ = Pw->Row("Status");
    OldPassword_ = Pw->Add<TextBox>("Current password");
    NewPassword_ = Pw->Add<TextBox>("New password (empty: no password)");
    Confirm_ = Pw->Add<TextBox>("Type the new password again");
    TextBox *Boxes[3] = {OldPassword_, NewPassword_, Confirm_};
    for (TextBox *Box : Boxes) {
        Box->Password = true;
        Box->Width = 280;
        Box->MaxLength = 64;
        Box->HorizontalAlignment = Align::Start;
        Box->Margin = Thickness(0, 4, 0, 0);
    }
    OldPassword_->OnSubmit = [this](const hz::String &) { Main_->SetFocus(NewPassword_); };
    NewPassword_->OnSubmit = [this](const hz::String &) { Main_->SetFocus(Confirm_); };
    Confirm_->OnSubmit = [this](const hz::String &) { ApplyPassword(); };
    ChangePassword_ = Pw->Add<Button>("Change password", ButtonStyle::Accent);
    ChangePassword_->HorizontalAlignment = Align::Start;
    ChangePassword_->Margin = Thickness(0, 8, 0, 0);
    ChangePassword_->OnClick = [this]() { ApplyPassword(); };
    PasswordNote_ = Pw->Add<TextBlock>("", FontRole::Caption);
    PasswordNote_->Foreground = Theme::TextSecondary;
    PasswordNote_->Wrap = true;
    if (Live) {
        Element *Disabled[6] = {AccountName_, SaveName, OldPassword_, NewPassword_, Confirm_, ChangePassword_};
        for (Element *E : Disabled)
            E->SetEnabled(false);
    }
    if (!HZ_SUCCESS(LoadStatus)) {
        NameNote_->Foreground = Theme::Accent;
        NameNote_->SetText(hz::String::Format("Could not load account settings: %s.",
                                              hz::fs::StatusText(LoadStatus)).CStr());
    }
    List_.Push(Page{"Accounts", Icon::Person, P, "account user username password lock screen", [this]() {
                        hz::fs::Settings S;
                        STATUS Status = LoadShellSettings(S);
                        if (!HZ_SUCCESS(Status)) {
                            PasswordState_->SetText(hz::String::Format("Settings unavailable (0x%08X).",
                                                                       (uint32_t)Status).CStr());
                            return;
                        }
                        bool Has = !S.Get("PasswordHash").Empty();
                        const char *State = Has ? "Set: the lock screen asks for it" : "Not set";
                        if (PasswordState_->Text() != State) {
                            PasswordState_->SetText(State);
                            OldPassword_->SetVisible(Has);
                            ChangePassword_->SetText(Has ? "Change password" : "Set password");
                            Main_->InvalidateLayout();
                        }
                    }});
}

void SettingsApp::ApplyName()
{
    hz::String Name = AccountName_->Text();
    if (!hz::fs::IsValidUserName(Name.CStr())) {
        NameNote_->SetText("That name cannot be used: letters, digits, spaces, '-', '_' and '.' (up to 32 characters).");
        NameNote_->Foreground = Theme::Accent;
        return;
    }
    hz::fs::Settings S;
    STATUS Status = LoadShellSettings(S);
    if (!HZ_SUCCESS(Status)) {
        NameNote_->Foreground = Theme::Accent;
        NameNote_->SetText(hz::String::Format("Account settings could not be loaded: %s.",
                                              hz::fs::StatusText(Status)).CStr());
        return;
    }
    hz::String Old = S.Get("UserName");
    if (Old == Name) {
        NameNote_->SetText("That is already your name.");
        return;
    }
    /* The documents folder follows the name. */
    hz::String OldHome = hz::String::Format("C:\\Users\\%s", Old.CStr());
    hz::String NewHome = hz::String::Format("C:\\Users\\%s", Name.CStr());
    if (Old.Empty() || !hz::fs::Exists(OldHome)) {
        NameNote_->Foreground = Theme::Accent;
        NameNote_->SetText("The existing user folder is missing; use Setup to create an account safely.");
        return;
    }
    if (hz::fs::Exists(NewHome)) {
        NameNote_->Foreground = Theme::Accent;
        NameNote_->SetText("A user folder with that name already exists. No files were changed.");
        return;
    }
    Status = hz::fs::MoveTree(OldHome, NewHome);
    if (!HZ_SUCCESS(Status)) {
        NameNote_->Foreground = Theme::Accent;
        NameNote_->SetText(hz::String::Format("The user folder was not renamed: %s.",
                                              hz::fs::StatusText(Status)).CStr());
        return;
    }
    S.Set("UserName", Name.CStr());
    bool ShellNotified = false;
    Status = SaveShellSettings(S, &ShellNotified);
    if (!HZ_SUCCESS(Status)) {
        STATUS Rollback = hz::fs::MoveTree(NewHome, OldHome);
        if (!HZ_SUCCESS(Rollback)) {
            NameNote_->Foreground = Theme::Accent;
            NameNote_->SetText(hz::String::Format("Settings save failed (%s), and the folder could not be restored (%s).",
                                                  hz::fs::StatusText(Status), hz::fs::StatusText(Rollback)).CStr());
            return;
        }
    }
    HzPrint("settings: user name '%s' (0x%08X)", Name.CStr(), (uint32_t)Status);
    NameNote_->Foreground = HZ_SUCCESS(Status) ? Theme::TextSecondary : Theme::Accent;
    NameNote_->SetText(HZ_SUCCESS(Status)
                           ? (ShellNotified ? "Saved. Start and the lock screen show the new name."
                                            : "Saved to disk; the shell did not refresh this session.")
                           : hz::String::Format("The name could not be saved: %s. The folder was restored.",
                                                hz::fs::StatusText(Status)).CStr());
}

void SettingsApp::ApplyPassword()
{
    hz::fs::Settings S;
    STATUS LoadStatus = LoadShellSettings(S);
    if (!HZ_SUCCESS(LoadStatus)) {
        PasswordNote_->Foreground = Theme::Accent;
        PasswordNote_->SetText(hz::String::Format("Account settings could not be loaded: %s.",
                                                  hz::fs::StatusText(LoadStatus)).CStr());
        OldPassword_->SetText("");
        NewPassword_->SetText("");
        Confirm_->SetText("");
        return;
    }
    hz::String Hash = S.Get("PasswordHash");
    PasswordNote_->Foreground = Theme::Accent;
    if (!Hash.Empty() && !hz::crypto::CheckPassword(OldPassword_->Text(), S.Get("PasswordSalt"), Hash)) {
        HzPrint("settings: the current password is wrong");
        PasswordNote_->SetText("The current password is not right.");
        OldPassword_->SetText("");
        NewPassword_->SetText("");
        Confirm_->SetText("");
        Main_->SetFocus(OldPassword_);
        return;
    }
    if (NewPassword_->Text() != Confirm_->Text()) {
        PasswordNote_->SetText("The two new passwords are not the same.");
        OldPassword_->SetText("");
        NewPassword_->SetText("");
        Confirm_->SetText("");
        Main_->SetFocus(Confirm_);
        return;
    }
    bool Remove = NewPassword_->Text().Empty();
    if (Remove && Hash.Empty()) {
        PasswordNote_->SetText("Type a new password twice to set one.");
        OldPassword_->SetText("");
        NewPassword_->SetText("");
        Confirm_->SetText("");
        return;
    }
    if (Remove) {
        S.Set("PasswordSalt", "");
        S.Set("PasswordHash", "");
    } else {
        hz::String Salt = hz::crypto::NewSalt();
        S.Set("PasswordSalt", Salt.CStr());
        S.Set("PasswordHash", hz::crypto::HashPassword(NewPassword_->Text(), Salt).CStr());
    }
    bool ShellNotified = false;
    STATUS Status = SaveShellSettings(S, &ShellNotified);
    HzPrint("settings: password %s (0x%08X)", Remove ? "removed" : "set", (uint32_t)Status);
    OldPassword_->SetText("");
    NewPassword_->SetText("");
    Confirm_->SetText("");
    PasswordNote_->Foreground = HZ_SUCCESS(Status) ? Theme::TextSecondary : Theme::Accent;
    PasswordNote_->SetText(!HZ_SUCCESS(Status) ? hz::String::Format("The password cannot be saved: %s.",
                                                                    hz::fs::StatusText(Status)).CStr()
                           : !ShellNotified   ? "Saved to disk; the shell did not refresh this session."
                           : Remove            ? "The password is removed: the lock screen opens with any key."
                                               : "The password is saved. The lock screen asks for it.");
    if (Current_ < List_.Size() && List_[Current_].Update)
        List_[Current_].Update();
}

void SettingsApp::ChangeOffset(int Minutes)
{
    hz::fs::Settings S;
    STATUS Status = LoadShellSettings(S);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("settings: cannot load time-zone settings (0x%08X)", (uint32_t)Status);
        if (TimeNote_) {
            TimeNote_->Foreground = Theme::Accent;
            TimeNote_->SetText(hz::String::Format("Time-zone settings could not be loaded: %s.",
                                                  hz::fs::StatusText(Status)).CStr());
        }
        return;
    }
    Minutes = hz::fs::ClampUtcOffset(Minutes);
    S.SetInt("UtcOffsetMinutes", Minutes);
    bool ShellNotified = false;
    Status = SaveShellSettings(S, &ShellNotified);
    HzPrint("settings: time zone %s (0x%08X)", hz::fs::FormatUtcOffset(Minutes).CStr(), (uint32_t)Status);
    if (HZ_SUCCESS(Status) && !ShellNotified)
        HzPrint("settings: time-zone settings saved but shell refresh failed");
    if (TimeNote_) {
        TimeNote_->Foreground = HZ_SUCCESS(Status) ? Theme::TextSecondary : Theme::Accent;
        TimeNote_->SetText(!HZ_SUCCESS(Status)
                               ? hz::String::Format("Time-zone settings could not be saved: %s.",
                                                    hz::fs::StatusText(Status)).CStr()
                               : ShellNotified ? "Time zone saved."
                                               : "Time zone saved to disk; the shell did not refresh this session.");
    }
    if (Current_ < List_.Size() && List_[Current_].Update)
        List_[Current_].Update();
}

void SettingsApp::AddDisplay()
{
    StackPanel *P =
        NewPage("Display", "Information about the active screen. Resolution and scaling controls are not available.");
    InfoCard *Card = P->Add<InfoCard>("Screen");
    TextBlock *Resolution = Card->Row("Resolution");
    TextBlock *Refresh = Card->Row("Frame interval");
    TextBlock *Work = Card->Row("Work area");
    TextBlock *Compositor = Card->Row("Compositor");
    List_.Push(Page{"Display", Icon::Home, P, "screen display resolution refresh scale", [Resolution, Refresh, Work, Compositor]() {
                        HZ_DISPLAY_INFO I;
                        memset(&I, 0, sizeof(I));
                        STATUS Status = NtUserGetDisplayInfo(&I);
                        if (!HZ_SUCCESS(Status)) {
                            Resolution->SetText(hz::String::Format("Unavailable (0x%08X)", (uint32_t)Status).CStr());
                            Refresh->SetText("Unavailable");
                            Work->SetText("Unavailable");
                            Compositor->SetText("Unavailable");
                            return;
                        }
                        if (!I.Width || !I.Height || I.Width > INT32_MAX || I.Height > INT32_MAX ||
                            I.WorkArea.Left < 0 || I.WorkArea.Top < 0 ||
                            I.WorkArea.Right < I.WorkArea.Left || I.WorkArea.Bottom < I.WorkArea.Top ||
                            (uint32_t)I.WorkArea.Right > I.Width || (uint32_t)I.WorkArea.Bottom > I.Height) {
                            Resolution->SetText("Invalid display information");
                            Refresh->SetText("Unavailable");
                            Work->SetText("Unavailable");
                            Compositor->SetText("Unavailable");
                            return;
                        }
                        Resolution->SetText(hz::String::Format("%u x %u pixels, 32-bit color", I.Width, I.Height).CStr());
                        uint32_t Hz = I.FrameIntervalUs ? (1000000 + I.FrameIntervalUs / 2) / I.FrameIntervalUs : 0;
                        Refresh->SetText(hz::String::Format("%u us (about %u frames per second)", I.FrameIntervalUs, Hz).CStr());
                        Work->SetText(hz::String::Format("%d, %d to %d, %d", I.WorkArea.Left, I.WorkArea.Top, I.WorkArea.Right,
                                                         I.WorkArea.Bottom)
                                          .CStr());
                        Compositor->SetText(I.Composited ? "On (damage-driven, per-window surfaces)" : "Off");
                    }});
}

void SettingsApp::AddStorage()
{
    StackPanel *P = NewPage("Storage", "Volumes and space usage. Refresh to query the file systems again.");
    Button *Refresh = P->Add<Button>("Refresh storage", ButtonStyle::Subtle);
    Refresh->HorizontalAlignment = Align::Start;
    StackPanel *List = P->Add<StackPanel>(Orientation::Vertical, 10);
    auto Update = [this, List]() {
                        List->ClearChildren();
                        size_t Found = 0;
                        for (const hz::fs::Volume &V : hz::fs::Volumes()) {
                            if (V.Root.Length() < 3)
                                continue;
                            Found++;
                            InfoCard *Card = List->Add<InfoCard>(
                                hz::String::Format("%s (%c:)", V.Label.Empty() ? "Local disk" : V.Label.CStr(), V.Root[0]).CStr());
                            Card->Row("File system")->SetText(
                                hz::String::Format("%s%s", V.FileSystem.CStr(), V.ReadOnly ? ", read-only" : "").CStr());
                            if (V.FreeBytes > V.TotalBytes) {
                                Card->Row("Space")->SetText("Unavailable (reported free space exceeds capacity)");
                                HzPrint("settings: inconsistent volume space for %s", V.Root.CStr());
                                continue;
                            }
                            uint64_t Used = V.TotalBytes - V.FreeBytes;
                            Card->Row("Used")->SetText(hz::String::Format("%s of %s", hz::fs::FormatSize(Used).CStr(),
                                                                          hz::fs::FormatSize(V.TotalBytes).CStr())
                                                           .CStr());
                            if (V.TotalBytes) {
                                float Percent = (float)((double)Used * 100.0 / (double)V.TotalBytes);
                                ProgressBar *Bar = Card->Add<ProgressBar>(Percent);
                                Bar->Margin = Thickness(0, 6, 0, 2);
                            } else {
                                Card->Row("Usage")->SetText("Unavailable (zero capacity reported)");
                            }
                        }
                        if (!Found)
                            List->Add<TextBlock>("No mounted volumes were returned.", FontRole::Body);
                        Main_->InvalidateLayout();
                    };
    Refresh->OnClick = Update;
    List_.Push(Page{"Storage", Icon::Drive, P, "storage disk drive volume free capacity", Update});
}

void SettingsApp::AddNetwork()
{
    StackPanel *P = NewPage("Network", "Detected network adapters and addresses. This is not Internet connectivity.");
    Button *Refresh = P->Add<Button>("Refresh network status", ButtonStyle::Subtle);
    Refresh->HorizontalAlignment = Align::Start;
    InfoCard *Card = P->Add<InfoCard>("Adapters");
    TextBlock *Details = Card->Add<TextBlock>("", FontRole::Body);
    Details->Wrap = true;
    TextBlock *Availability = Card->Add<TextBlock>("", FontRole::Caption);
    Availability->Foreground = Theme::TextSecondary;
    Availability->Wrap = true;
    Availability->Margin = Thickness(0, 8, 0, 0);
    auto Update = [Details, Availability]() {
                        HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION Interfaces[HZ_MAX_NETWORK_INTERFACES] = {};
                        uint32_t Returned = 0;
                        STATUS Status = NtQuerySystemInformation(HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS,
                                                                 Interfaces, sizeof(Interfaces), &Returned);
                        if (!HZ_SUCCESS(Status)) {
                            Details->SetText("Adapter information unavailable.");
                            Availability->SetText(
                                hz::String::Format("Could not query adapters (0x%08X).", (uint32_t)Status).CStr());
                            return;
                        }
                        if (Returned > sizeof(Interfaces) || Returned % sizeof(Interfaces[0])) {
                            Details->SetText("Adapter information was malformed.");
                            Availability->SetText("The kernel returned an invalid byte count; no records were read.");
                            HzPrint("settings: malformed network information length %u", Returned);
                            return;
                        }
                        uint32_t Count = Returned / sizeof(Interfaces[0]);
                        if (!Count) {
                            Details->SetText("No supported adapter was detected.");
                            Availability->SetText("No supported network adapter was detected.");
                            return;
                        }
                        hz::String Summary;
                        for (uint32_t Index = 0; Index < Count; Index++) {
                            const HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION &I = Interfaces[Index];
                            auto FormatAddress = [](const uint8_t Address[4]) {
                                return hz::String::Format("%u.%u.%u.%u", Address[0], Address[1], Address[2], Address[3]);
                            };
                            Summary.Append(hz::String::Format(
                                "%s\nLink: %s  |  MTU: %u\nMAC: %02X:%02X:%02X:%02X:%02X:%02X", I.Name,
                                I.LinkUp ? "Connected" : "Disconnected", I.Mtu, I.MacAddress[0], I.MacAddress[1],
                                I.MacAddress[2], I.MacAddress[3], I.MacAddress[4], I.MacAddress[5]));
                            if (I.ConfigurationState == 2) {
                                Summary.Append(hz::String::Format("\nIPv4: %s  |  Gateway: %s  |  DNS: %s",
                                                                  FormatAddress(I.Ipv4Address).CStr(),
                                                                  FormatAddress(I.Gateway).CStr(),
                                                                  FormatAddress(I.DnsServer).CStr()));
                            } else if (I.ConfigurationState == 1) {
                                Summary.Append("\nIPv4 configuration: requesting address");
                            } else if (I.ConfigurationState == 3) {
                                Summary.Append("\nIPv4 configuration: DHCP failed");
                            } else {
                                Summary.Append("\nIPv4 configuration: not configured");
                            }
                            if (Index + 1 < Count)
                                Summary.Append("\n\n");
                        }
                        Details->SetText(Summary.CStr());
                        Availability->SetText("DHCP may assign an address, gateway, and DNS server. DNS requests, routing, TCP, HTTP, TLS, and browser Internet access are not implemented.");
                    };
    Refresh->OnClick = Update;
    List_.Push(Page{"Network", Icon::Info, P, "network ethernet adapter wifi internet ip dns dhcp", Update});
}

void SettingsApp::AddTime()
{
    bool Live = hz::fs::IsLiveSystem();
    StackPanel *P = NewPage("Time", "The computer's clock runs in UTC. The clocks on screen show the time zone chosen "
                                    "here (no daylight saving changes); file times stay in UTC.");
    InfoCard *Card = P->Add<InfoCard>("Clock");
    TextBlock *Now = Card->Row("Date and time");
    TextBlock *Zone = Card->Row("Time zone");
    TextBlock *Up = Card->Row("Up since start");
    StackPanel *Steps = Card->Add<StackPanel>(Orientation::Vertical, 8);
    Steps->Margin = Thickness(0, 8, 0, 0);
    Button *Earlier = Steps->Add<Button>("30 minutes earlier");
    Button *Later = Steps->Add<Button>("30 minutes later");
    Button *Utc = Steps->Add<Button>("UTC");
    TimeNote_ = Card->Add<TextBlock>("", FontRole::Caption);
    TimeNote_->Wrap = true;
    TimeNote_->Foreground = Theme::TextSecondary;
    Earlier->OnClick = [this]() { ChangeOffset(hz::fs::UtcOffsetMinutes() - 30); };
    Later->OnClick = [this]() { ChangeOffset(hz::fs::UtcOffsetMinutes() + 30); };
    Utc->OnClick = [this]() { ChangeOffset(0); };
    if (Live) {
        Earlier->SetEnabled(false);
        Later->SetEnabled(false);
        Utc->SetEnabled(false);
        Zone->SetText("UTC (the live system cannot save a time zone)");
    }
    List_.Push(Page{"Time", Icon::Clock, P, "time clock timezone date utc", [Now, Zone, Up, Live]() {
                        uint64_t T = 0, U = 0;
                        STATUS TimeStatus = NtQuerySystemTime(&T);
                        STATUS UptimeStatus = NtQueryUptime(&U);
                        if (!HZ_SUCCESS(TimeStatus) || !HZ_SUCCESS(UptimeStatus)) {
                            Now->SetText(hz::String::Format("Unavailable (time 0x%08X, uptime 0x%08X)",
                                                            (uint32_t)TimeStatus, (uint32_t)UptimeStatus).CStr());
                            Up->SetText("Unavailable");
                            return;
                        }
                        int Offset = Live ? 0 : hz::fs::UtcOffsetMinutes();
                        hz::fs::CivilTime C = hz::fs::ToCivil(T, Offset);
                        Now->SetText(hz::String::Format("%s %u %s %lld, %02u:%02u:%02u", hz::fs::WeekdayNames[C.Weekday], C.Day,
                                                        hz::fs::MonthNames[(C.Month - 1) % 12], C.Year, C.Hour, C.Minute,
                                                        C.Second)
                                         .CStr());
                        if (!Live)
                            Zone->SetText(hz::String::Format("%s (the clock keeps UTC: %s)",
                                                             hz::fs::FormatUtcOffset(Offset).CStr(),
                                                             hz::fs::FormatTime(T).CStr())
                                              .CStr());
                        uint64_t S = U / 1000000;
                        Up->SetText(hz::String::Format("%llu:%02llu:%02llu", (unsigned long long)(S / 3600),
                                                       (unsigned long long)(S / 60 % 60), (unsigned long long)(S % 60))
                                        .CStr());
                    }, true});
}

void SettingsApp::AddAbout()
{
    StackPanel *P = NewPage("About", "This system and the software it runs.");
    InfoCard *Product = P->Add<InfoCard>("Nexora OS");
    Product->Row("Version")->SetText(HZ_VERSION_STRING " (" HZ_MILESTONE ")");
    Product->Row("Kernel")->SetText("hzkrnl, x86-64, UEFI");
    Product->Row("Architecture")->SetText("NT-style executive, win32k-style window manager");
    Product->Row("Started from")->SetText(hz::fs::IsLiveSystem() ? "The live image (read-only: settings are not saved)"
                                                                 : "This computer's disk (C: is FAT32)");
    InfoCard *Machine = P->Add<InfoCard>("Device");
    TextBlock *Cpus = Machine->Row("Processors");
    TextBlock *Memory = Machine->Row("Memory");
    TextBlock *Load = Machine->Row("Processes and threads");
    TextBlock *Switches = Machine->Row("Context switches");
    InfoCard *Credits = P->Add<InfoCard>("Open-source parts");
    Credits->Row("Fonts")->SetText("Inter, JetBrains Mono (OFL)");
    Credits->Row("Boot loader")->SetText("Limine (BSD-2-Clause)");
    Credits->Row("Licenses")->SetText("C:\\Nexora\\Licenses");
    List_.Push(Page{"About", Icon::Info, P, "about system version processor memory uptime", [Cpus, Memory, Load, Switches]() {
                        HZ_SYSTEM_BASIC_INFORMATION I = {};
                        uint32_t Returned = 0;
                        STATUS Status =
                            NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &I, sizeof(I), &Returned);
                        if (!HZ_SUCCESS(Status) || Returned != sizeof(I)) {
                            Cpus->SetText(hz::String::Format("Unavailable (0x%08X)", (uint32_t)Status).CStr());
                            Memory->SetText("Unavailable");
                            Load->SetText("Unavailable");
                            Switches->SetText("Unavailable");
                            return;
                        }
                        if (!I.PageSize || I.AvailablePages > I.TotalPhysicalPages ||
                            I.TotalPhysicalPages > (~0ull / I.PageSize)) {
                            Cpus->SetText(hz::String::Format("%u", I.ProcessorCount).CStr());
                            Memory->SetText("Unavailable (inconsistent memory totals)");
                            Load->SetText(hz::String::Format("%u processes, %u threads", I.ProcessCount, I.ThreadCount).CStr());
                            Switches->SetText(hz::String::Format("%llu", (unsigned long long)I.ContextSwitches).CStr());
                            return;
                        }
                        Cpus->SetText(hz::String::Format("%u", I.ProcessorCount).CStr());
                        uint64_t Total = I.TotalPhysicalPages * I.PageSize, Free = I.AvailablePages * I.PageSize;
                        Memory->SetText(hz::String::Format("%s (%s available)", hz::fs::FormatSize(Total).CStr(),
                                                           hz::fs::FormatSize(Free).CStr())
                                            .CStr());
                        Load->SetText(hz::String::Format("%u processes, %u threads", I.ProcessCount, I.ThreadCount).CStr());
                        Switches->SetText(hz::String::Format("%llu", (unsigned long long)I.ContextSwitches).CStr());
                    }, true});
}

void SettingsApp::AddHome()
{
    StackPanel *P = NewPage("Home", "Your Nexora OS settings, with live system status.");
    InfoCard *Status = P->Add<InfoCard>("At a glance");
    TextBlock *Display = Status->Row("Display");
    TextBlock *Memory = Status->Row("Memory");
    TextBlock *Processes = Status->Row("Processes");
    TextBlock *Network = Status->Row("Network");
    Grid *Shortcuts = P->Add<Grid>();
    Shortcuts->Columns.Push(GridLength::Stars(1));
    Shortcuts->Columns.Push(GridLength::Stars(1));
    Shortcuts->Rows.Push(GridLength::AutoSize());
    Shortcuts->Rows.Push(GridLength::AutoSize());
    Shortcuts->Rows.Push(GridLength::AutoSize());
    Shortcuts->ColumnSpacing = 8;
    Shortcuts->RowSpacing = 8;
    const char *Targets[] = {"Personalization", "Accounts", "Display", "Storage", "Network", "Time"};
    for (size_t Index = 0; Index < sizeof(Targets) / sizeof(Targets[0]); Index++) {
        Button *Shortcut = Shortcuts->Add<Button>(Targets[Index]);
        Shortcut->GridColumn = Index % 2;
        Shortcut->GridRow = Index / 2;
        Shortcut->OnClick = [this, Title = Targets[Index]]() { OpenPage(Title); };
    }
    List_.Push(Page{"Home", Icon::Home, P, "home start overview status", [Display, Memory, Processes, Network]() {
                        HZ_DISPLAY_INFO Screen = {};
                        STATUS DisplayStatus = NtUserGetDisplayInfo(&Screen);
                        if (HZ_SUCCESS(DisplayStatus) && Screen.Width && Screen.Height)
                            Display->SetText(hz::String::Format("%u x %u", Screen.Width, Screen.Height).CStr());
                        else
                            Display->SetText(hz::String::Format("Unavailable (0x%08X)", (uint32_t)DisplayStatus).CStr());

                        HZ_SYSTEM_BASIC_INFORMATION Info = {};
                        uint32_t Returned = 0;
                        STATUS SystemStatus = NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Info,
                                                                       sizeof(Info), &Returned);
                        if (HZ_SUCCESS(SystemStatus) && Returned == sizeof(Info) && Info.PageSize &&
                            Info.AvailablePages <= Info.TotalPhysicalPages &&
                            Info.TotalPhysicalPages <= (~0ull / Info.PageSize)) {
                            uint64_t Total = Info.TotalPhysicalPages * Info.PageSize;
                            uint64_t Free = Info.AvailablePages * Info.PageSize;
                            Memory->SetText(hz::String::Format("%s available of %s", hz::fs::FormatSize(Free).CStr(),
                                                               hz::fs::FormatSize(Total).CStr()).CStr());
                            Processes->SetText(hz::String::Format("%u processes, %u threads", Info.ProcessCount,
                                                                  Info.ThreadCount).CStr());
                        } else {
                            Memory->SetText("Unavailable");
                            Processes->SetText("Unavailable");
                        }

                        HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION Interfaces[HZ_MAX_NETWORK_INTERFACES] = {};
                        uint32_t NetworkBytes = 0;
                        STATUS NetworkStatus = NtQuerySystemInformation(HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS,
                                                                         Interfaces, sizeof(Interfaces), &NetworkBytes);
                        if (!HZ_SUCCESS(NetworkStatus))
                            Network->SetText(hz::String::Format("Unavailable (0x%08X)", (uint32_t)NetworkStatus).CStr());
                        else if (NetworkBytes > sizeof(Interfaces) || NetworkBytes % sizeof(Interfaces[0]))
                            Network->SetText("Invalid adapter information");
                        else {
                            uint32_t Count = NetworkBytes / sizeof(Interfaces[0]);
                            Network->SetText(Count ? hz::String::Format("%u adapter%s detected; Internet unavailable",
                                                                        Count, Count == 1 ? "" : "s").CStr()
                                                   : "No supported adapter detected");
                        }
                    }, true});
}

void SettingsApp::AddUnavailablePages()
{
    auto AddUnavailable = [this](const char *Title, const char *Keywords, const char *Message) {
        StackPanel *P = NewPage(Title, Message);
        InfoCard *Card = P->Add<InfoCard>("Availability");
        TextBlock *Text = Card->Add<TextBlock>(Message, FontRole::Body);
        Text->Wrap = true;
        List_.Push(Page{Title, Icon::Info, P, Keywords, {}});
    };
    AddUnavailable("Sound", "sound audio volume microphone speaker",
                   "Sound device discovery, volume controls, and audio playback are not implemented in this build.");
    AddUnavailable("Devices", "devices usb bluetooth controller keyboard mouse",
                   "USB host-controller and Bluetooth stacks are not available. Bluetooth scanning, BLE/GATT, HID, and Classic Bluetooth are not supported.");
    AddUnavailable("Accessibility", "accessibility contrast text size scale",
                   "Keyboard navigation is available throughout Settings. System-wide contrast, text scaling, and assistive-technology controls are not implemented.");

    StackPanel *P = NewPage("Diagnostics", "Live kernel and device-query checks. These are status reads, not a complete diagnostic suite.");
    InfoCard *Checks = P->Add<InfoCard>("System query");
    TextBlock *Kernel = Checks->Row("System information");
    TextBlock *Display = Checks->Row("Display information");
    TextBlock *Network = Checks->Row("Network interfaces");
    List_.Push(Page{"Diagnostics", Icon::Info, P, "diagnostics troubleshoot logs system errors health", [Kernel, Display, Network]() {
                        HZ_SYSTEM_BASIC_INFORMATION Basic = {};
                        uint32_t BasicBytes = 0;
                        STATUS BasicStatus = NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &Basic,
                                                                      sizeof(Basic), &BasicBytes);
                        Kernel->SetText(HZ_SUCCESS(BasicStatus) && BasicBytes == sizeof(Basic)
                                            ? "Available"
                                            : hz::String::Format("Failed (0x%08X)", (uint32_t)BasicStatus).CStr());
                        HZ_DISPLAY_INFO Screen = {};
                        STATUS DisplayStatus = NtUserGetDisplayInfo(&Screen);
                        Display->SetText(HZ_SUCCESS(DisplayStatus) && Screen.Width && Screen.Height
                                             ? "Available"
                                             : hz::String::Format("Failed (0x%08X)", (uint32_t)DisplayStatus).CStr());
                        HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION Interfaces[HZ_MAX_NETWORK_INTERFACES] = {};
                        uint32_t NetworkBytes = 0;
                        STATUS NetworkStatus = NtQuerySystemInformation(HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS,
                                                                         Interfaces, sizeof(Interfaces), &NetworkBytes);
                        if (!HZ_SUCCESS(NetworkStatus))
                            Network->SetText(hz::String::Format("Failed (0x%08X)", (uint32_t)NetworkStatus).CStr());
                        else if (NetworkBytes > sizeof(Interfaces) || NetworkBytes % sizeof(Interfaces[0]))
                            Network->SetText("Invalid response length");
                        else {
                            uint32_t Count = (uint32_t)(NetworkBytes / sizeof(Interfaces[0]));
                            Network->SetText(hz::String::Format("%u record%s", Count, Count == 1 ? "" : "s").CStr());
                        }
                    }});
}

void SettingsApp::OpenPage(const char *Title)
{
    for (size_t Index = 0; Index < List_.Size(); Index++)
        if (hz::String(List_[Index].Title) == Title) {
            bool Visible = false;
            for (size_t NavIndex = 0; NavIndex < NavigationPages_.Size(); NavIndex++)
                Visible |= NavigationPages_[NavIndex] == Index;
            if (!Visible)
                Search_->SetText("", true);
            Show(Index);
            for (size_t NavIndex = 0; NavIndex < NavigationPages_.Size(); NavIndex++)
                if (NavigationPages_[NavIndex] == Index)
                    Nav_->ScrollIntoView(NavIndex);
            return;
        }
}

void SettingsApp::FilterNavigation(const hz::String &Query)
{
    NavigationPages_.Clear();
    for (size_t Index = 0; Index < List_.Size(); Index++)
        if (Query.Empty() || hz::String(List_[Index].Title).ContainsIgnoreCase(Query.CStr()) ||
            hz::String(List_[Index].Keywords).ContainsIgnoreCase(Query.CStr()))
            NavigationPages_.Push(Index);
    Nav_->SetItemCount(NavigationPages_.Empty() ? 1 : NavigationPages_.Size());
    Nav_->Height = (int32_t)((NavigationPages_.Empty() ? 1 : NavigationPages_.Size()) * Nav_->ItemHeight + 8);
    size_t Selected = hz::NotFound;
    for (size_t Index = 0; Index < NavigationPages_.Size(); Index++)
        if (NavigationPages_[Index] == Current_) {
            Selected = Index;
            break;
        }
    Nav_->SetSelectedIndex(Selected, false);
    Main_->InvalidateLayout();
}

void SettingsApp::Show(size_t Index)
{
    if (Index >= List_.Size() || Index == Current_)
        return;
    if (Current_ < List_.Size())
        List_[Current_].Content->SetVisible(false);
    Current_ = Index;
    List_[Index].Content->SetVisible(true);
    if (List_[Index].Update)
        List_[Index].Update();
    Host_->ScrollTo(0, false);
    Main_->InvalidateLayout();
    for (size_t NavIndex = 0; NavIndex < NavigationPages_.Size(); NavIndex++)
        if (NavigationPages_[NavIndex] == Index) {
            if (Nav_->SelectedIndex() != NavIndex)
                Nav_->SetSelectedIndex(NavIndex, false);
            break;
        }
    HzPrint("settings: %s", List_[Index].Title);
}

bool SettingsApp::Create(const char *Initial)
{
    Wallpaper_ = HZ_MAX_WALLPAPER + 1;
    Main_ = new Window("Settings", 980, 640);
    Main_->SetMinimumSize(640, 400);
    Grid *Root = new Grid();
    Root->Columns.Push(GridLength::Pixels(200));
    Root->Columns.Push(GridLength::Stars(1));

    StackPanel *Left = Root->Add<StackPanel>(Orientation::Vertical, 8);
    Left->Padding = Thickness(8, 8, 8, 12);
    TextBlock *Brand = Left->Add<TextBlock>("Settings", FontRole::Subtitle);
    Brand->Margin = Thickness(8, 0, 0, 0);
    Search_ = Left->Add<TextBox>("Search settings");
    Search_->MaxLength = 64;
    Nav_ = Left->Add<ListView>();
    Nav_->ItemHeight = 38;

    Host_ = Root->Add<ScrollViewer>();
    Host_->GridColumn = 1;
    Pages_ = new Grid();
    Host_->SetContent(Pages_);

    AddHome();
    AddPersonalization();
    AddAccounts();
    AddDisplay();
    AddStorage();
    AddNetwork();
    AddTime();
    AddAbout();
    AddUnavailablePages();

    Nav_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
        if (NavigationPages_.Empty()) {
            P.TextLine(GetFont(FontRole::Caption), Rect{R.X + 8, R.Y, R.Width - 16, R.Height}, "No matches",
                       Theme::TextSecondary);
            return;
        }
        if (I >= NavigationPages_.Size())
            return;
        const Page &Item = List_[NavigationPages_[I]];
        P.DrawIcon(Item.Glyph, Rect{R.X + 8, R.Y + (R.Height - 18) / 2, 18, 18}, Theme::TextSecondary);
        P.TextLine(GetFont(FontRole::Body), Rect{R.X + 34, R.Y, R.Width - 38, R.Height}, Item.Title, Theme::TextPrimary);
    };
    Nav_->OnSelectionChanged = [this](size_t I) {
        if (I < NavigationPages_.Size())
            Show(NavigationPages_[I]);
    };
    Search_->OnTextChanged = [this](const hz::String &Query) { FilterNavigation(Query); };

    Main_->SetContent(Root);
    Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    size_t First = 0;
    for (size_t i = 0; Initial && i < List_.Size(); i++)
        if (hz::String(List_[i].Title).ContainsIgnoreCase(Initial))
            First = i;
    Search_->SetText(Initial ? Initial : "", false);
    FilterNavigation(Search_->Text());
    Show(First);
    if (!Main_->Show())
        return false;
    Main_->SetFocus(Search_);
    Main_->SetTimer(1000, [this]() {
        if (Current_ < List_.Size() && List_[Current_].Periodic && List_[Current_].Update)
            List_[Current_].Update();
    });
    return true;
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    SettingsApp *App = new SettingsApp();
    if (!App->Create(argc > 1 ? argv[1] : nullptr)) {
        HzPrint("settings: cannot create the window");
        return 1;
    }
    HzPrint("settings: started");
    return RunMessageLoop();
}
