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

uint32_t CurrentWallpaper()
{
    HZ_DISPLAY_INFO Info;
    memset(&Info, 0, sizeof(Info));
    NtUserGetDisplayInfo(&Info);
    return Info.Wallpaper;
}

class WallpaperCard final : public Element {
public:
    WallpaperCard(uint32_t Index, uint32_t *Selected) : Index_(Index), Selected_(Selected)
    {
        Width = 200;
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
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }
    void OnPointerUp(PointerEvent &E) override
    {
        if (Bounds().Contains(E.Position))
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
            return;
        }
        NtUserPostMessage(Shell, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_WALLPAPER, Index_);
        *Selected_ = Index_;
        HzPrint("settings: wallpaper %u requested", Index_);
        if (Parent())
            Parent()->Invalidate();
    }

private:
    uint32_t Index_;
    uint32_t *Selected_;
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
        G->Columns.Push(GridLength::Pixels(180));
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
    hz::Function<void()> Update; /* called when shown and every second while shown */
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
    void Show(size_t Index);

    Window *Main_ = nullptr;
    ListView *Nav_ = nullptr;
    ScrollViewer *Host_ = nullptr;
    Grid *Pages_ = nullptr;
    hz::Vector<Page> List_;
    size_t Current_ = hz::NotFound;
    uint32_t Wallpaper_ = 0;
    /* Accounts */
    TextBox *AccountName_ = nullptr, *OldPassword_ = nullptr, *NewPassword_ = nullptr, *Confirm_ = nullptr;
    TextBlock *NameNote_ = nullptr, *PasswordNote_ = nullptr, *PasswordState_ = nullptr;
    Button *ChangePassword_ = nullptr;
};

/* shell.ini, and the shell told to read it again. */
STATUS SaveShellSettings(const hz::fs::Settings &S)
{
    STATUS Status = S.Save(hz::fs::SettingsPath("shell"));
    HWND Shell = 0;
    if (HZ_SUCCESS(Status) && HZ_SUCCESS(NtUserGetShellWindow(&Shell)) && Shell)
        NtUserPostMessage(Shell, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_SETTINGS, 0);
    return Status;
}

hz::fs::Settings LoadShellSettings()
{
    hz::fs::Settings S;
    S.Load(hz::fs::SettingsPath("shell"));
    return S;
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
    StackPanel *Cards = P->Add<StackPanel>(Orientation::Horizontal, 16);
    for (uint32_t i = 0; i <= HZ_MAX_WALLPAPER; i++)
        Cards->Add<WallpaperCard>(i, &Wallpaper_);
    InfoCard *Theme = P->Add<InfoCard>("Colors");
    Theme->Row("Mode")->SetText("Dark");
    Theme->Row("Accent color")->SetText("Nexora orange");
    Theme->Row("Transparency")->SetText("Mica (blurred wallpaper behind windows)");
    List_.Push(Page{"Personalization", Icon::Sun, P, [this, Cards]() {
                        uint32_t Now = CurrentWallpaper();
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
    StackPanel *NameRow = Who->Add<StackPanel>(Orientation::Horizontal, 8);
    AccountName_ = NameRow->Add<TextBox>("Your name");
    AccountName_->Width = 280;
    AccountName_->MaxLength = 32;
    AccountName_->SetText(LoadShellSettings().Get("UserName").CStr(), false);
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
    List_.Push(Page{"Accounts", Icon::Person, P, [this]() {
                        bool Has = !LoadShellSettings().Get("PasswordHash").Empty();
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
    hz::fs::Settings S = LoadShellSettings();
    hz::String Old = S.Get("UserName");
    if (Old == Name) {
        NameNote_->SetText("That is already your name.");
        return;
    }
    /* The documents folder follows the name. */
    hz::String OldHome = hz::String::Format("C:\\Users\\%s", Old.CStr());
    hz::String NewHome = hz::String::Format("C:\\Users\\%s", Name.CStr());
    if (!Old.Empty() && hz::fs::Exists(OldHome) && !hz::fs::Exists(NewHome))
        hz::fs::Rename(OldHome, Name.CStr());
    hz::fs::CreateDirectory(hz::String("C:\\Users"));
    hz::fs::CreateDirectory(NewHome);
    hz::fs::CreateDirectory(hz::fs::Join(NewHome, "Documents"));
    S.Set("UserName", Name.CStr());
    STATUS Status = SaveShellSettings(S);
    HzPrint("settings: user name '%s' (0x%08X)", Name.CStr(), (uint32_t)Status);
    NameNote_->Foreground = Theme::TextSecondary;
    NameNote_->SetText(HZ_SUCCESS(Status) ? "Saved. Start and the lock screen show the new name."
                                          : hz::String::Format("The name cannot be saved: %s.", hz::fs::StatusText(Status)).CStr());
}

void SettingsApp::ApplyPassword()
{
    hz::fs::Settings S = LoadShellSettings();
    hz::String Hash = S.Get("PasswordHash");
    PasswordNote_->Foreground = Theme::Accent;
    if (!Hash.Empty() && !hz::crypto::CheckPassword(OldPassword_->Text(), S.Get("PasswordSalt"), Hash)) {
        HzPrint("settings: the current password is wrong");
        PasswordNote_->SetText("The current password is not right.");
        OldPassword_->SetText("");
        Main_->SetFocus(OldPassword_);
        return;
    }
    if (NewPassword_->Text() != Confirm_->Text()) {
        PasswordNote_->SetText("The two new passwords are not the same.");
        Main_->SetFocus(Confirm_);
        return;
    }
    bool Remove = NewPassword_->Text().Empty();
    if (Remove && Hash.Empty()) {
        PasswordNote_->SetText("Type a new password twice to set one.");
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
    STATUS Status = SaveShellSettings(S);
    HzPrint("settings: password %s (0x%08X)", Remove ? "removed" : "set", (uint32_t)Status);
    OldPassword_->SetText("");
    NewPassword_->SetText("");
    Confirm_->SetText("");
    PasswordNote_->Foreground = Theme::TextSecondary;
    PasswordNote_->SetText(!HZ_SUCCESS(Status) ? "The password cannot be saved."
                           : Remove            ? "The password is removed: the lock screen opens with any key."
                                               : "The password is saved. The lock screen asks for it.");
    if (Current_ < List_.Size() && List_[Current_].Update)
        List_[Current_].Update();
}

void SettingsApp::ChangeOffset(int Minutes)
{
    hz::fs::Settings S = LoadShellSettings();
    Minutes = hz::fs::ClampUtcOffset(Minutes);
    S.SetInt("UtcOffsetMinutes", Minutes);
    STATUS Status = SaveShellSettings(S);
    HzPrint("settings: time zone %s (0x%08X)", hz::fs::FormatUtcOffset(Minutes).CStr(), (uint32_t)Status);
    if (Current_ < List_.Size() && List_[Current_].Update)
        List_[Current_].Update();
}

void SettingsApp::AddDisplay()
{
    StackPanel *P = NewPage("Display", "The screen as the window manager sees it.");
    InfoCard *Card = P->Add<InfoCard>("Screen");
    TextBlock *Resolution = Card->Row("Resolution");
    TextBlock *Refresh = Card->Row("Frame interval");
    TextBlock *Work = Card->Row("Work area");
    TextBlock *Compositor = Card->Row("Compositor");
    InfoCard *Fonts = P->Add<InfoCard>("Text");
    Fonts->Row("Interface font")->SetText("Inter (SIL Open Font License)");
    Fonts->Row("Monospaced font")->SetText("JetBrains Mono (SIL Open Font License)");
    Fonts->Row("Scale")->SetText("100%");
    List_.Push(Page{"Display", Icon::Home, P, [Resolution, Refresh, Work, Compositor]() {
                        HZ_DISPLAY_INFO I;
                        memset(&I, 0, sizeof(I));
                        NtUserGetDisplayInfo(&I);
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
    StackPanel *P = NewPage("Storage", "Volumes and how full they are. C: is the read-only system image in memory.");
    StackPanel *List = P->Add<StackPanel>(Orientation::Vertical, 10);
    List_.Push(Page{"Storage", Icon::Drive, P, [List]() {
                        if (List->ChildCount())
                            return; /* built once per visit */
                        for (const hz::fs::Volume &V : hz::fs::Volumes()) {
                            InfoCard *Card = List->Add<InfoCard>(
                                hz::String::Format("%s (%c:)", V.Label.Empty() ? "Local disk" : V.Label.CStr(), V.Root[0]).CStr());
                            Card->Row("File system")->SetText(
                                hz::String::Format("%s%s", V.FileSystem.CStr(), V.ReadOnly ? ", read-only" : "").CStr());
                            uint64_t Used = V.TotalBytes - V.FreeBytes;
                            Card->Row("Used")->SetText(hz::String::Format("%s of %s", hz::fs::FormatSize(Used).CStr(),
                                                                          hz::fs::FormatSize(V.TotalBytes).CStr())
                                                           .CStr());
                            ProgressBar *Bar = Card->Add<ProgressBar>(
                                V.TotalBytes ? (float)(Used * 1000 / V.TotalBytes) / 10.0f : 0.0f);
                            Bar->Margin = Thickness(0, 6, 0, 2);
                        }
                    }});
}

void SettingsApp::AddNetwork()
{
    StackPanel *P = NewPage("Network", "Wired network adapter status for this system.");
    InfoCard *Card = P->Add<InfoCard>("Ethernet");
    TextBlock *Device = Card->Row("Adapter");
    TextBlock *Link = Card->Row("Link");
    TextBlock *Mac = Card->Row("Hardware address");
    TextBlock *Mtu = Card->Row("MTU");
    TextBlock *Ipv4 = Card->Row("IPv4 address");
    TextBlock *Gateway = Card->Row("Gateway");
    TextBlock *Dns = Card->Row("DNS server");
    TextBlock *Configuration = Card->Row("Configuration");
    TextBlock *Availability = Card->Add<TextBlock>("", FontRole::Caption);
    Availability->Foreground = Theme::TextSecondary;
    Availability->Wrap = true;
    Availability->Margin = Thickness(0, 8, 0, 0);
    List_.Push(Page{"Network", Icon::Info, P, [Device, Link, Mac, Mtu, Ipv4, Gateway, Dns, Configuration, Availability]() {
                        HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION Interfaces[HZ_MAX_NETWORK_INTERFACES] = {};
                        uint32_t Returned = 0;
                        STATUS Status = NtQuerySystemInformation(HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS,
                                                                 Interfaces, sizeof(Interfaces), &Returned);
                        if (!HZ_SUCCESS(Status)) {
                            Device->SetText("Status unavailable");
                            Link->SetText("Unknown");
                            Mac->SetText("N/A");
                            Mtu->SetText("N/A");
                            Ipv4->SetText("N/A");
                            Gateway->SetText("N/A");
                            Dns->SetText("N/A");
                            Configuration->SetText("Unavailable");
                            Availability->SetText(
                                hz::String::Format("Could not query adapters (0x%08X).", (uint32_t)Status).CStr());
                            return;
                        }
                        uint32_t Count = Returned / sizeof(Interfaces[0]);
                        if (!Count) {
                            Device->SetText("No supported adapter");
                            Link->SetText("Disconnected");
                            Mac->SetText("N/A");
                            Mtu->SetText("N/A");
                            Ipv4->SetText("N/A");
                            Gateway->SetText("N/A");
                            Dns->SetText("N/A");
                            Configuration->SetText("No adapter");
                            Availability->SetText("No supported network adapter was detected.");
                            return;
                        }
                        const HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION &I = Interfaces[0];
                        Device->SetText(I.Name);
                        Link->SetText(I.LinkUp ? "Connected" : "Cable unplugged");
                        Mac->SetText(hz::String::Format("%02X:%02X:%02X:%02X:%02X:%02X", I.MacAddress[0],
                                                        I.MacAddress[1], I.MacAddress[2], I.MacAddress[3],
                                                        I.MacAddress[4], I.MacAddress[5])
                                         .CStr());
                        Mtu->SetText(hz::String::Format("%u bytes", I.Mtu).CStr());
                        if (I.ConfigurationState == 2) {
                            auto FormatAddress = [](const uint8_t Address[4]) {
                                return hz::String::Format("%u.%u.%u.%u", Address[0], Address[1], Address[2], Address[3]);
                            };
                            Ipv4->SetText(FormatAddress(I.Ipv4Address).CStr());
                            Gateway->SetText(FormatAddress(I.Gateway).CStr());
                            Dns->SetText(FormatAddress(I.DnsServer).CStr());
                        } else {
                            Ipv4->SetText("Not assigned");
                            Gateway->SetText("Not assigned");
                            Dns->SetText("Not assigned");
                        }
                        switch (I.ConfigurationState) {
                        case 1:
                            Configuration->SetText("Requesting address");
                            Availability->SetText("DHCP is requesting network configuration.");
                            break;
                        case 2:
                            Configuration->SetText("Configured by DHCP");
                            Availability->SetText(
                                "An IPv4 address, gateway, and DNS server were assigned. DNS queries, Internet protocols, and browser access are not available yet.");
                            break;
                        case 3:
                            Configuration->SetText("DHCP failed");
                            Availability->SetText(
                                "The adapter is connected, but DHCP did not assign an address. Check the network or DHCP server.");
                            break;
                        default:
                            Configuration->SetText("Not configured");
                            Availability->SetText("The adapter is detected. DHCP configuration has not started.");
                            break;
                        }
                    }});
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
    StackPanel *Steps = Card->Add<StackPanel>(Orientation::Horizontal, 8);
    Steps->Margin = Thickness(0, 8, 0, 0);
    Button *Earlier = Steps->Add<Button>("30 minutes earlier");
    Button *Later = Steps->Add<Button>("30 minutes later");
    Button *Utc = Steps->Add<Button>("UTC");
    Earlier->OnClick = [this]() { ChangeOffset(hz::fs::UtcOffsetMinutes() - 30); };
    Later->OnClick = [this]() { ChangeOffset(hz::fs::UtcOffsetMinutes() + 30); };
    Utc->OnClick = [this]() { ChangeOffset(0); };
    if (Live) {
        Earlier->SetEnabled(false);
        Later->SetEnabled(false);
        Utc->SetEnabled(false);
        Zone->SetText("UTC (the live system cannot save a time zone)");
    }
    InfoCard *Input = P->Add<InfoCard>("Keyboard");
    Input->Row("Layout")->SetText("English (US)");
    List_.Push(Page{"Time", Icon::Clock, P, [Now, Zone, Up, Live]() {
                        uint64_t T = 0, U = 0;
                        NtQuerySystemTime(&T);
                        NtQueryUptime(&U);
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
                    }});
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
    List_.Push(Page{"About", Icon::Info, P, [Cpus, Memory, Load, Switches]() {
                        HZ_SYSTEM_BASIC_INFORMATION I = {};
                        if (!HZ_SUCCESS(NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &I, sizeof(I), nullptr)))
                            return;
                        Cpus->SetText(hz::String::Format("%u", I.ProcessorCount).CStr());
                        uint64_t Total = I.TotalPhysicalPages * I.PageSize, Free = I.AvailablePages * I.PageSize;
                        Memory->SetText(hz::String::Format("%s (%s available)", hz::fs::FormatSize(Total).CStr(),
                                                           hz::fs::FormatSize(Free).CStr())
                                            .CStr());
                        Load->SetText(hz::String::Format("%u processes, %u threads", I.ProcessCount, I.ThreadCount).CStr());
                        Switches->SetText(hz::String::Format("%llu", (unsigned long long)I.ContextSwitches).CStr());
                    }});
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
    if (Nav_->SelectedIndex() != Index)
        Nav_->SetSelectedIndex(Index, false);
    HzPrint("settings: %s", List_[Index].Title);
}

bool SettingsApp::Create(const char *Initial)
{
    Wallpaper_ = CurrentWallpaper();
    Main_ = new Window("Settings", 980, 640);
    Main_->SetMinimumSize(640, 400);
    Grid *Root = new Grid();
    Root->Columns.Push(GridLength::Pixels(240));
    Root->Columns.Push(GridLength::Stars(1));

    StackPanel *Left = Root->Add<StackPanel>(Orientation::Vertical, 8);
    Left->Padding = Thickness(12, 8, 8, 12);
    TextBlock *Brand = Left->Add<TextBlock>("Settings", FontRole::Subtitle);
    Brand->Margin = Thickness(8, 0, 0, 8);
    Nav_ = Left->Add<ListView>();
    Nav_->ItemHeight = 40;

    Host_ = Root->Add<ScrollViewer>();
    Host_->GridColumn = 1;
    Pages_ = new Grid();
    Host_->SetContent(Pages_);

    AddPersonalization();
    AddAccounts();
    AddDisplay();
    AddStorage();
    AddNetwork();
    AddTime();
    AddAbout();

    Nav_->SetItemCount(List_.Size());
    Nav_->Height = (int32_t)(List_.Size() * 40 + 8);
    Nav_->PaintItem = [this](Painter &P, const Rect &R, size_t I) {
        P.DrawIcon(List_[I].Glyph, Rect{R.X + 8, R.Y + (R.Height - 18) / 2, 18, 18}, Theme::TextSecondary);
        P.TextLine(GetFont(FontRole::Body), Rect{R.X + 38, R.Y, R.Width - 38, R.Height}, List_[I].Title, Theme::TextPrimary);
    };
    Nav_->OnSelectionChanged = [this](size_t I) { Show(I); };

    Main_->SetContent(Root);
    Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
    size_t First = 0;
    for (size_t i = 0; Initial && i < List_.Size(); i++)
        if (hz::String(List_[i].Title).ContainsIgnoreCase(Initial))
            First = i;
    Show(First);
    if (!Main_->Show())
        return false;
    Main_->SetFocus(Nav_);
    Main_->SetTimer(1000, [this]() {
        if (Current_ < List_.Size() && List_[Current_].Update)
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
