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
#include "hzapps.h"
#include "hzui.h"
#include "hzversion.h"
#include "hzwallpaper.h"

using namespace hzui;

namespace {

/* Fallback tints used only if a packaged wallpaper preview cannot be read. */
struct Swatch {
    Color Top, Middle, Bottom, Glow;
};
const Swatch kFallbackSwatches[hz::wallpaper::Count] = {
    {0xFF153D2C, 0xFF285840, 0xFF102A36, 0xFF85D7A6},
    {0xFF24455D, 0xFF356D89, 0xFF162B42, 0xFF8DD2E8},
    {0xFF272638, 0xFF56516A, 0xFF242C42, 0xFFE4A68A},
    {0xFF344332, 0xFF65744A, 0xFF273841, 0xFFD7BA79},
    {0xFF25334A, 0xFF56677B, 0xFF28394B, 0xFFE9C4B0},
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

uint32_t CurrentLockWallpaper()
{
    hz::fs::Settings S;
    STATUS Status = S.Load(hz::fs::SettingsPath("shell"));
    if (!HZ_SUCCESS(Status) && Status != STATUS_NOT_FOUND)
        HzPrint("settings: could not read lock-screen wallpaper setting (0x%08X)", (uint32_t)Status);
    int Saved = S.GetInt("LockWallpaper", HZ_LOCK_WALLPAPER_DEFAULT);
    return Saved >= 0 && Saved <= (int)HZ_MAX_WALLPAPER ? (uint32_t)Saved : HZ_LOCK_WALLPAPER_DEFAULT;
}

class WallpaperCard final : public Element {
public:
    WallpaperCard(uint32_t Index, uint32_t *Desktop, uint32_t *Lock, bool *LockTarget, TextBlock *Status)
        : Index_(Index), Desktop_(Desktop), Lock_(Lock), LockTarget_(LockTarget), Status_(Status)
    {
        Height = 142;
        Focusable = true;
        const hz::wallpaper::Info *Info = hz::wallpaper::GetInfo(Index);
        if (!Info)
            return;
        ToolTip = Info->Description;
        STATUS LoadStatus = hz::wallpaper::LoadPreview(Index, &Preview_, &PreviewWidth_, &PreviewHeight_);
        if (!HZ_SUCCESS(LoadStatus))
            HzPrint("settings: cannot load wallpaper preview %u (0x%08X)", Index, (uint32_t)LoadStatus);
    }

    ~WallpaperCard() override { free(Preview_); }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        if (Index_ >= hz::wallpaper::Count)
            return;
        const hz::wallpaper::Info *Info = hz::wallpaper::GetInfo(Index_);
        if (!Info)
            return;
        const Swatch &S = kFallbackSwatches[Index_];
        Rect Image{B.X + 4, B.Y + 4, B.Width - 8, 104};
        if (Preview_) {
            P.DrawBitmap(Image, Preview_, PreviewWidth_, PreviewHeight_);
        } else {
            P.PushClip(Image);
            P.FillVerticalGradient(Rect{Image.X, Image.Y, Image.Width, Image.Height * 6 / 10}, 0, S.Top, S.Middle);
            P.FillVerticalGradient(Rect{Image.X, Image.Y + Image.Height * 6 / 10, Image.Width,
                                        Image.Height - Image.Height * 6 / 10},
                                   0, S.Middle, S.Bottom);
            P.FillCircle(Image.X + Image.Width / 2.0f, (float)Image.Bottom() + 16, 44,
                         ScaleAlpha(S.Glow, 0.55f));
            P.PopClip();
        }
        uint32_t Selected = *LockTarget_ ? *Lock_ : *Desktop_;
        bool Chosen = Selected == Index_;
        if (Chosen)
            P.StrokeRoundRect(Rect{B.X, B.Y, B.Width, 108}, 8, 3, Theme::Accent);
        else if (IsHovered())
            P.StrokeRoundRect(Rect{B.X, B.Y, B.Width, 108}, 8, 1, Theme::ControlStrongStroke);
        P.TextLine(GetFont(Chosen ? FontRole::BodyStrong : FontRole::Body),
                   Rect{B.X + 4, B.Y + 112, B.Width - 8, 24}, Info->Name,
                   Theme::TextPrimary);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Rect{Bounds().X, Bounds().Y, Bounds().Width, 108}, 8);
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
        uint32_t Request = *LockTarget_ ? HZ_SHELL_REQUEST_LOCK_WALLPAPER : HZ_SHELL_REQUEST_WALLPAPER;
        STATUS Status = NtUserPostMessage(Shell, HZ_WM_SHELLREQUEST, Request, Index_);
        if (!HZ_SUCCESS(Status)) {
            HzPrint("settings: wallpaper request failed (0x%08X)", (uint32_t)Status);
            Status_->Foreground = Theme::Accent;
            Status_->SetText(hz::String::Format("The wallpaper request failed (0x%08X).", (uint32_t)Status).CStr());
            return;
        }
        if (*LockTarget_)
            *Lock_ = Index_;
        else
            *Desktop_ = Index_;
        if (*LockTarget_)
            HzPrint("settings: lock-screen wallpaper %u requested", Index_);
        else
            HzPrint("settings: wallpaper %u requested", Index_);
        Status_->Foreground = Theme::TextSecondary;
        Status_->SetText(*LockTarget_ ? "Lock-screen wallpaper saved." : "Desktop wallpaper change requested.");
        if (Parent())
            Parent()->Invalidate();
    }

private:
    uint32_t Index_;
    uint32_t *Desktop_, *Lock_;
    bool *LockTarget_;
    TextBlock *Status_;
    uint32_t *Preview_ = nullptr;
    int32_t PreviewWidth_ = 0, PreviewHeight_ = 0;
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

    /* A plain paragraph that wraps: the notes under a card's rows. */
    TextBlock *Note(const char *Text)
    {
        TextBlock *T = Add<TextBlock>(Text, FontRole::Caption);
        T->Foreground = Theme::TextSecondary;
        T->Wrap = true;
        return T;
    }
};

/* The circular profile picture on the Accounts page: the chosen 24-bit .bmp, or the initial of
   the account's name on a disc. The matte behind the picture matches the fill, so the disc is
   seamless whatever the card's background is. */
class ProfilePreview final : public Element {
public:
    ProfilePreview()
    {
        Width = kSize;
        Height = kSize;
        HorizontalAlignment = Align::Start;
        HitTestVisible = false;
    }
    ~ProfilePreview() override { delete[] Pixels_; }

    /* Points the preview at a picture path; "" shows the initial instead. Repaints when it changes. */
    void SetPicture(const hz::String &Path, char Initial)
    {
        Initial_ = Initial ? Initial : 'G';
        Path_ = Path;
        delete[] Pixels_;
        Pixels_ = nullptr;
        if (!Path_.Empty()) {
            uint32_t *Buffer = new uint32_t[(size_t)kSize * kSize];
            if (HZ_SUCCESS(hz::wallpaper::MakeRoundPicture(Path_.CStr(), kSize, Matte(), Buffer)))
                Pixels_ = Buffer;
            else
                delete[] Buffer;
        }
        Invalidate();
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillCircle(B.X + kSize / 2.0f, B.Y + kSize / 2.0f, kSize / 2.0f, Matte());
        if (Pixels_) {
            P.DrawBitmap(B, Pixels_, kSize, kSize);
            return;
        }
        char Text[2] = {Initial_, 0};
        if (Text[0] >= 'a' && Text[0] <= 'z')
            Text[0] = (char)(Text[0] - 32);
        P.TextLine(GetFont(FontRole::Title), B, Text, 0xFFFFFFFF, Align::Center);
    }

private:
    static constexpr int32_t kSize = 88;
    static uint32_t Matte()
    {
        return Theme::Current() == Theme::Scheme::Light ? 0xFFE4E4EC : 0xFF3B3B46;
    }
    hz::String Path_;
    char Initial_ = 'G';
    uint32_t *Pixels_ = nullptr;
};

/* A card row: a label on the left, a toggle switch on the right. */
ToggleSwitch *AddToggle(InfoCard *Card, const char *Label, bool On, hz::Function<void(bool)> Changed)
{
    Grid *G = Card->Add<Grid>();
    G->Columns.Push(GridLength::Stars(1));
    G->Columns.Push(GridLength::AutoSize());
    TextBlock *Name = G->Add<TextBlock>(Label, FontRole::Body);
    Name->VerticalAlignment = Align::Center;
    ToggleSwitch *Switch = G->Add<ToggleSwitch>("On", "Off");
    Switch->GridColumn = 1;
    Switch->SetOn(On, false);
    Switch->OnToggled = hz::Move(Changed);
    return Switch;
}

/* A card row: a label on the left, a drop-down on the right. */
ComboBox *AddChoice(InfoCard *Card, const char *Label, const hz::Vector<hz::String> &Items, size_t Selected,
                    hz::Function<void(size_t)> Changed)
{
    Grid *G = Card->Add<Grid>();
    G->Columns.Push(GridLength::Stars(1));
    G->Columns.Push(GridLength::Pixels(168));
    TextBlock *Name = G->Add<TextBlock>(Label, FontRole::Body);
    Name->VerticalAlignment = Align::Center;
    ComboBox *Box = G->Add<ComboBox>();
    Box->GridColumn = 1;
    Box->Items = Items;
    Box->SetSelectedIndex(Selected, false);
    Box->OnSelectionChanged = hz::Move(Changed);
    return Box;
}

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
    void ChoosePicture();
    void ClearPicture();
    void ChangeOffset(int Minutes);
    void AddSystem();
    hz::Function<void()> AddStorage(StackPanel *Parent);
    void AddNetwork();
    void AddTime();
    void AddAbout();
    void AddApps();
    void AddPrivacySecurity();
    void AddHome();
    void AddDevices();
    void AddAccessibility();
    void AddRecovery();
    void AddDesktop();
    void AddDefaultApps();
    void AddKeyboard();
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
    uint32_t LockWallpaper_ = HZ_LOCK_WALLPAPER_DEFAULT;
    bool WallpaperTargetIsLock_ = false;
    /* Accounts */
    TextBox *AccountName_ = nullptr, *OldPassword_ = nullptr, *NewPassword_ = nullptr, *Confirm_ = nullptr;
    TextBlock *NameNote_ = nullptr, *PasswordNote_ = nullptr, *PasswordState_ = nullptr;
    ProfilePreview *Profile_ = nullptr;
    TextBlock *PictureNote_ = nullptr;
    Button *RemovePicture_ = nullptr;
    TextBlock *TimeNote_ = nullptr;
    Button *ChangePassword_ = nullptr;
};

/* Tells the shell that a settings file changed, so it reads it again at once (the desktop layer,
   the dock and the wallpaper all live in the shell process). */
bool NotifyShell()
{
    HWND Shell = 0;
    STATUS Status = NtUserGetShellWindow(&Shell);
    if (HZ_SUCCESS(Status) && Shell)
        Status = NtUserPostMessage(Shell, HZ_WM_SHELLREQUEST, HZ_SHELL_REQUEST_SETTINGS, 0);
    else if (HZ_SUCCESS(Status))
        Status = STATUS_NOT_FOUND;
    if (!HZ_SUCCESS(Status)) {
        HzPrint("settings: saved the settings but the shell notification failed (0x%08X)", (uint32_t)Status);
        return false;
    }
    return true;
}

/* Writes one of the shell's INI files, then asks the shell to re-read it. */
STATUS SaveShellSettings(const hz::fs::Settings &S, bool *ShellNotified = nullptr)
{
    if (hz::fs::IsLiveSystem())
        return STATUS_MEDIA_WRITE_PROTECTED;
    STATUS Status = S.Save(hz::fs::SettingsPath("shell"));
    bool Notified = false;
    if (HZ_SUCCESS(Status))
        Notified = NotifyShell();
    if (ShellNotified)
        *ShellNotified = Notified;
    return Status;
}

/* desktop.ini: the icon size and the sort order of the desktop, read by the shell's desktop. */
STATUS SaveDesktopSettings(const hz::fs::Settings &S, bool *ShellNotified = nullptr)
{
    if (hz::fs::IsLiveSystem())
        return STATUS_MEDIA_WRITE_PROTECTED;
    STATUS Status = S.Save(hz::fs::SettingsPath("desktop"));
    bool Notified = false;
    if (HZ_SUCCESS(Status))
        Notified = NotifyShell();
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
    StackPanel *P = NewPage("Personalization",
                            "Choose the colour mode, and separate desktop and lock-screen images. Accent-colour "
                            "controls are not implemented yet.");
    TextBlock *Status = P->Add<TextBlock>("", FontRole::Caption);
    Status->Wrap = true;
    /* The colour scheme (shell.ini "Theme"). The shell switches at once; other programs take it up
       when they are next opened, because each one paints the palette it was built with. */
    InfoCard *Appearance = P->Add<InfoCard>("Colour mode");
    hz::fs::Settings ShellSettings;
    LoadShellSettings(ShellSettings);
    hz::Vector<hz::String> Modes;
    Modes.Push("Dark");
    Modes.Push("Light");
    AddChoice(Appearance, "Colour mode", Modes, ShellSettings.GetInt("Theme", 0) != 0 ? 1 : 0, [Status](size_t Index) {
        hz::fs::Settings S;
        LoadShellSettings(S);
        S.SetInt("Theme", Index == 1 ? 1 : 0);
        bool Notified = false;
        STATUS Result = SaveShellSettings(S, &Notified);
        if (!HZ_SUCCESS(Result)) {
            Status->Foreground = Theme::Accent;
            Status->SetText(Result == STATUS_MEDIA_WRITE_PROTECTED
                                ? "The live system is read-only. Install Nexora OS to keep this choice."
                                : hz::String::Format("Could not save the colour mode (0x%08X).", (uint32_t)Result).CStr());
            return;
        }
        Status->Foreground = Notified ? Theme::TextSecondary : Theme::Accent;
        Status->SetText(Notified ? "Colour mode saved. The desktop switches now; programs take it up when they "
                                   "are opened again."
                                 : "Saved, but the shell did not pick the change up.");
        HzPrint("settings: colour mode set to %s", Index == 1 ? "light" : "dark");
    });
    Appearance->Note("Light and dark. First-boot setup asks for this too, and the desktop, the taskbar and the "
                     "flyouts follow the choice. Programs opened before the switch keep the mode they started in.");
    StackPanel *Targets = P->Add<StackPanel>(Orientation::Horizontal, 8);
    Button *DesktopTarget = Targets->Add<Button>("Desktop", ButtonStyle::Accent);
    Button *LockTarget = Targets->Add<Button>("Lock screen", ButtonStyle::Standard);
    DesktopTarget->Width = 136;
    LockTarget->Width = 136;
    Grid *Cards = P->Add<Grid>();
    Cards->Columns.Push(GridLength::Stars(1));
    Cards->Columns.Push(GridLength::Stars(1));
    Cards->Rows.Push(GridLength::AutoSize());
    Cards->Rows.Push(GridLength::AutoSize());
    Cards->Rows.Push(GridLength::AutoSize());
    Cards->ColumnSpacing = 8;
    Cards->RowSpacing = 8;
    for (uint32_t i = 0; i <= HZ_MAX_WALLPAPER; i++) {
        WallpaperCard *Card = Cards->Add<WallpaperCard>(i, &Wallpaper_, &LockWallpaper_, &WallpaperTargetIsLock_, Status);
        Card->GridColumn = i % 2;
        Card->GridRow = i / 2;
    }
    DesktopTarget->OnClick = [this, DesktopTarget, LockTarget, Cards, Status]() {
        WallpaperTargetIsLock_ = false;
        DesktopTarget->Style = ButtonStyle::Accent;
        LockTarget->Style = ButtonStyle::Standard;
        Status->SetText("Choose the desktop wallpaper.");
        Cards->Invalidate();
    };
    LockTarget->OnClick = [this, DesktopTarget, LockTarget, Cards, Status]() {
        WallpaperTargetIsLock_ = true;
        DesktopTarget->Style = ButtonStyle::Standard;
        LockTarget->Style = ButtonStyle::Accent;
        Status->SetText("Choose the lock-screen wallpaper.");
        Cards->Invalidate();
    };
    List_.Push(Page{"Personalization", Icon::Sun, P,
                    "wallpaper desktop lock screen background appearance", [this, Cards, Status]() {
                        uint32_t Now = 0;
                        if (CurrentWallpaper(Now))
                            Wallpaper_ = Now;
                        LockWallpaper_ = CurrentLockWallpaper();
                        if (Wallpaper_ > HZ_MAX_WALLPAPER) {
                            Status->Foreground = Theme::Accent;
                            Status->SetText("The active desktop wallpaper could not be queried.");
                        } else {
                            Status->Foreground = Theme::TextSecondary;
                            Status->SetText("");
                        }
                        Cards->Invalidate();
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
    STATUS LoadStatus = Live ? STATUS_SUCCESS : LoadShellSettings(InitialSettings);
    AccountName_->SetText(HZ_SUCCESS(LoadStatus) ? InitialSettings.Get("UserName").CStr() : "", false);
    AccountName_->OnSubmit = [this](const hz::String &) { ApplyName(); };
    Button *SaveName = NameRow->Add<Button>("Save name");
    SaveName->OnClick = [this]() { ApplyName(); };
    NameNote_ = Who->Add<TextBlock>("Letters, digits, spaces, '-', '_' and '.' (up to 32 characters). Your documents "
                                    "folder is renamed with it.",
                                    FontRole::Caption);
    NameNote_->Foreground = Theme::TextTertiary;
    NameNote_->Wrap = true;

    InfoCard *Pic = P->Add<InfoCard>("Profile picture");
    Profile_ = Pic->Add<ProfilePreview>();
    StackPanel *PicRow = Pic->Add<StackPanel>(Orientation::Horizontal, 8);
    Button *ChoosePic = PicRow->Add<Button>("Choose picture...", ButtonStyle::Accent);
    ChoosePic->OnClick = [this]() { ChoosePicture(); };
    RemovePicture_ = PicRow->Add<Button>("Remove picture");
    RemovePicture_->OnClick = [this]() { ClearPicture(); };
    PictureNote_ = Pic->Note("A 24-bit .bmp up to 512x512 (PNG and JPEG are not decoded yet). It appears on the "
                             "Start menu's account row.");
    Profile_->SetPicture(HZ_SUCCESS(LoadStatus) ? InitialSettings.Get("ProfilePicture") : hz::String(),
                         AccountName_->Text().Empty() ? 'G' : AccountName_->Text()[0]);

    InfoCard *Pw = P->Add<InfoCard>("Password");
    PasswordState_ = Pw->Row("Status");
    if (Live)
        PasswordState_->SetText("Account changes are unavailable on the live system.");
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
        Element *Disabled[8] = {AccountName_, SaveName,      OldPassword_, NewPassword_,
                                Confirm_,    ChangePassword_, ChoosePic,    RemovePicture_};
        for (Element *E : Disabled)
            E->SetEnabled(false);
    }
    if (Live) {
        NameNote_->SetText("Account changes are disabled in live mode. Install Nexora OS to save a name or password.");
        NameNote_->Foreground = Theme::TextSecondary;
    } else if (!HZ_SUCCESS(LoadStatus)) {
        NameNote_->Foreground = Theme::Accent;
        NameNote_->SetText(hz::String::Format("Could not load account settings: %s.",
                                              hz::fs::StatusText(LoadStatus)).CStr());
    }
    List_.Push(Page{"Accounts", Icon::Person, P, "account user username password lock screen", [this, Live]() {
                        if (Live)
                            return;
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
    if (HZ_SUCCESS(Status))
        Profile_->SetPicture(S.Get("ProfilePicture"), Name[0]);
}

void SettingsApp::ChoosePicture()
{
    hz::String Folder = hz::fs::DocumentsFolder();
    ShowFileDialog(Main_, FileDialogMode::Open, Folder.CStr(), nullptr, [this](const hz::String &Path) {
        if (Path.Empty())
            return;
        /* Check the file before remembering it: a picture that cannot be decoded must not be saved. */
        uint32_t *Probe = new uint32_t[88 * 88];
        STATUS Check = hz::wallpaper::MakeRoundPicture(Path.CStr(), 88, 0xFF3B3B46, Probe);
        delete[] Probe;
        if (!HZ_SUCCESS(Check)) {
            PictureNote_->Foreground = Theme::Accent;
            PictureNote_->SetText("That file could not be used: choose an uncompressed 24-bit .bmp up to 512x512.");
            return;
        }
        hz::fs::Settings S;
        STATUS LoadStatus = LoadShellSettings(S);
        if (!HZ_SUCCESS(LoadStatus)) {
            PictureNote_->Foreground = Theme::Accent;
            PictureNote_->SetText(hz::String::Format("Account settings could not be loaded: %s.",
                                                     hz::fs::StatusText(LoadStatus)).CStr());
            return;
        }
        S.Set("ProfilePicture", Path.CStr());
        bool Notified = false;
        STATUS Status = SaveShellSettings(S, &Notified);
        PictureNote_->Foreground = HZ_SUCCESS(Status) ? Theme::TextSecondary : Theme::Accent;
        if (!HZ_SUCCESS(Status)) {
            PictureNote_->SetText(hz::String::Format("The picture could not be saved: %s.",
                                                     hz::fs::StatusText(Status)).CStr());
            return;
        }
        char Initial = AccountName_->Text().Empty() ? 'G' : AccountName_->Text()[0];
        Profile_->SetPicture(Path, Initial);
        PictureNote_->SetText(Notified ? "Saved. The Start menu's account row shows the new picture."
                                       : "Saved to disk; the shell did not refresh this session.");
        HzPrint("settings: profile picture '%s'", Path.CStr());
    });
}

void SettingsApp::ClearPicture()
{
    hz::fs::Settings S;
    STATUS LoadStatus = LoadShellSettings(S);
    if (!HZ_SUCCESS(LoadStatus)) {
        PictureNote_->Foreground = Theme::Accent;
        PictureNote_->SetText(hz::String::Format("Account settings could not be loaded: %s.",
                                                 hz::fs::StatusText(LoadStatus)).CStr());
        return;
    }
    if (S.Get("ProfilePicture").Empty()) {
        PictureNote_->SetText("There is no picture to remove; the account shows its initial.");
        return;
    }
    S.Set("ProfilePicture", "");
    bool Notified = false;
    STATUS Status = SaveShellSettings(S, &Notified);
    PictureNote_->Foreground = HZ_SUCCESS(Status) ? Theme::TextSecondary : Theme::Accent;
    if (!HZ_SUCCESS(Status)) {
        PictureNote_->SetText(hz::String::Format("The picture could not be removed: %s.",
                                                 hz::fs::StatusText(Status)).CStr());
        return;
    }
    char Initial = AccountName_->Text().Empty() ? 'G' : AccountName_->Text()[0];
    Profile_->SetPicture("", Initial);
    PictureNote_->SetText(Notified ? "Removed. The account shows its initial again."
                                   : "Removed on disk; the shell did not refresh this session.");
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

void SettingsApp::AddSystem()
{
    StackPanel *P =
        NewPage("System", "Display, storage, power, and system information for this computer.");
    InfoCard *Card = P->Add<InfoCard>("Screen");
    TextBlock *Resolution = Card->Row("Resolution");
    TextBlock *Refresh = Card->Row("Frame interval");
    TextBlock *Work = Card->Row("Work area");
    TextBlock *Compositor = Card->Row("Compositor");
    hz::Function<void()> RefreshStorage = AddStorage(P);
    auto UpdateScreen = [Resolution, Refresh, Work, Compositor]() {
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
                    };
    InfoCard *Capabilities = P->Add<InfoCard>("Power and sound");
    Capabilities->Row("Power")->SetText("Restart and shut down are available from Start > Power. Sleep is not supported.");
    Capabilities->Row("Sound")->SetText("Audio devices and volume controls are not implemented in this build.");
    InfoCard *SystemInfo = P->Add<InfoCard>("System information");
    TextBlock *SystemInfoText = SystemInfo->Add<TextBlock>("Processor and memory details are available in About Nexora OS.",
                                                           FontRole::Body);
    SystemInfoText->Wrap = true;
    Button *AboutButton = SystemInfo->Add<Button>("About Nexora OS", ButtonStyle::Subtle);
    AboutButton->HorizontalAlignment = Align::Start;
    AboutButton->OnClick = [this]() { OpenPage("About Nexora OS"); };
    List_.Push(Page{"System", Icon::Home, P,
                    "system display resolution refresh scale storage disk drive volume power sound processor memory",
                    [UpdateScreen, RefreshStorage]() {
                        UpdateScreen();
                        RefreshStorage();
                    }});
}

hz::Function<void()> SettingsApp::AddStorage(StackPanel *Parent)
{
    InfoCard *Storage = Parent->Add<InfoCard>("Storage");
    TextBlock *Help = Storage->Add<TextBlock>("Mounted volumes and available space.", FontRole::Caption);
    Help->Foreground = Theme::TextSecondary;
    Button *Refresh = Storage->Add<Button>("Refresh storage", ButtonStyle::Subtle);
    Refresh->HorizontalAlignment = Align::Start;
    StackPanel *List = Parent->Add<StackPanel>(Orientation::Vertical, 10);
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
    return Update;
}

void SettingsApp::AddNetwork()
{
    StackPanel *P = NewPage("Network & Internet",
                            "Detected network adapters and addresses. This is not Internet connectivity.");
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
    List_.Push(Page{"Network & Internet", Icon::Wifi, P, "network ethernet adapter wifi internet ip dns dhcp", Update});
}

void SettingsApp::AddTime()
{
    bool Live = hz::fs::IsLiveSystem();
    StackPanel *P = NewPage("Date & Language", "The computer's clock runs in UTC. The clocks on screen show the time zone chosen "
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
    InfoCard *Language = P->Add<InfoCard>("Language and input");
    Language->Row("Language")->SetText("Locale selection is not implemented.");
    Language->Row("Keyboard layout")->SetText("The built-in layout is fixed in this build.");
    List_.Push(Page{"Date & Language", Icon::Clock, P, "time clock timezone date utc language locale keyboard input",
                    [Now, Zone, Up, Live]() {
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
    StackPanel *P = NewPage("About Nexora OS", "This system and the software it runs.");
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
    List_.Push(Page{"About Nexora OS", Icon::Info, P, "about system version processor memory uptime license diagnostics",
                    [Cpus, Memory, Load, Switches]() {
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
    StackPanel *P = NewPage("Home", "Settings for this device, with information queried from the running system.");
    InfoCard *Device = P->Add<InfoCard>("This device");
    Device->Row("Product")->SetText("Nexora OS");
    Device->Row("Version")->SetText(HZ_VERSION_STRING " (" HZ_MILESTONE ")");

    InfoCard *Status = P->Add<InfoCard>("At a glance");
    TextBlock *Display = Status->Row("Display");
    TextBlock *Memory = Status->Row("Memory");
    TextBlock *Processes = Status->Row("Processes");
    TextBlock *Network = Status->Row("Network");
    P->Add<TextBlock>("Recommended settings", FontRole::Subtitle);
    Grid *Shortcuts = P->Add<Grid>();
    Shortcuts->Columns.Push(GridLength::Stars(1));
    Shortcuts->Columns.Push(GridLength::Stars(1));
    Shortcuts->Rows.Push(GridLength::AutoSize());
    Shortcuts->Rows.Push(GridLength::AutoSize());
    Shortcuts->Rows.Push(GridLength::AutoSize());
    Shortcuts->Rows.Push(GridLength::AutoSize());
    Shortcuts->ColumnSpacing = 8;
    Shortcuts->RowSpacing = 8;
    struct ShortcutInfo {
        const char *Title;
        const char *Description;
    };
    const ShortcutInfo Targets[] = {
        {"System", "Display, mounted volumes, and basic system status."},
        {"Personalization", "Choose independent desktop and lock-screen wallpapers."},
        {"Desktop & Dock", "Show or hide the desktop icons and the top dock, and arrange the icons."},
        {"Network & Internet", "Inspect adapter link and assigned IPv4 details."},
        {"Apps", "Browse and open installed Nexora applications."},
        {"Default apps", "See which program opens which kind of file."},
        {"Accounts", "Manage the current account and sign-in password."},
        {"Keyboard", "The key combinations the shell and its file windows understand."},
    };
    for (size_t Index = 0; Index < sizeof(Targets) / sizeof(Targets[0]); Index++) {
        InfoCard *Shortcut = Shortcuts->Add<InfoCard>(Targets[Index].Title);
        Shortcut->GridColumn = Index % 2;
        Shortcut->GridRow = Index / 2;
        TextBlock *Description = Shortcut->Add<TextBlock>(Targets[Index].Description, FontRole::Caption);
        Description->Foreground = Theme::TextSecondary;
        Description->Wrap = true;
        Button *Open = Shortcut->Add<Button>("Open", ButtonStyle::Subtle);
        Open->HorizontalAlignment = Align::Start;
        Open->OnClick = [this, Title = Targets[Index].Title]() { OpenPage(Title); };
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

void SettingsApp::AddApps()
{
    StackPanel *P = NewPage("Apps", "Installed Nexora applications and their launch status.");
    InfoCard *Availability = P->Add<InfoCard>("App management");
    TextBlock *Note = Availability->Add<TextBlock>(
        "Installed apps can be opened here; Default apps lists the file types each one opens. "
        "Startup-app management is not implemented.",
        FontRole::Body);
    Note->Wrap = true;

    hz::Vector<hz::apps::App> Apps = hz::apps::Load();
    if (Apps.Empty()) {
        P->Add<TextBlock>("No valid installed app manifests were found.", FontRole::Body);
    }
    for (const hz::apps::App &Program : Apps) {
        InfoCard *Card = P->Add<InfoCard>(Program.Name.CStr());
        TextBlock *Description = Card->Add<TextBlock>(
            Program.Description.Empty() ? "No description provided." : Program.Description.CStr(), FontRole::Body);
        Description->Wrap = true;
        TextBlock *Result = Card->Add<TextBlock>("", FontRole::Caption);
        Result->Foreground = Theme::TextSecondary;
        Button *Launch = Card->Add<Button>("Open", ButtonStyle::Subtle);
        Launch->HorizontalAlignment = Align::Start;
        Launch->OnClick = [Program, Result]() {
            uint32_t ProcessId = 0;
            STATUS Status = hz::apps::Launch(Program, nullptr, &ProcessId);
            if (!HZ_SUCCESS(Status)) {
                HzPrint("settings: could not launch %s (0x%08X)", Program.Name.CStr(), (uint32_t)Status);
                Result->Foreground = Theme::Accent;
                Result->SetText(hz::String::Format("Could not open this app (0x%08X).", (uint32_t)Status).CStr());
                return;
            }
            Result->Foreground = Theme::TextSecondary;
            Result->SetText(hz::String::Format("Started (process %u).", ProcessId).CStr());
        };
    }
    List_.Push(Page{"Apps", Icon::Document, P, "apps installed applications default startup launch", {}});
}

void SettingsApp::AddPrivacySecurity()
{
    StackPanel *P = NewPage("Privacy & Security", "Security features currently available in this Nexora OS build.");
    InfoCard *AccountSecurity = P->Add<InfoCard>("Sign-in");
    AccountSecurity->Row("Account password")->SetText(
        "Password setup is available from Accounts. Password enforcement depends on the active sign-in flow.");
    InfoCard *Availability = P->Add<InfoCard>("Security management");
    TextBlock *Note = Availability->Add<TextBlock>(
        "App permission controls, firewall rules, local security policy, audit-log tools, and developer settings "
        "are not implemented.",
        FontRole::Body);
    Note->Wrap = true;
    List_.Push(Page{"Privacy & Security", Icon::Info, P, "privacy security permissions firewall policy audit developer",
                    {}});
}

void SettingsApp::AddDevices()
{
    const char *Message = "Keyboard and mouse input are supported, but this build has no peripheral-management page, "
                          "USB host controller stack, or Bluetooth support.";
    StackPanel *P = NewPage("Devices", Message);
    InfoCard *Card = P->Add<InfoCard>("Availability");
    TextBlock *Text = Card->Add<TextBlock>(Message, FontRole::Body);
    Text->Wrap = true;
    List_.Push(Page{"Devices", Icon::Info, P, "devices usb bluetooth controller keyboard mouse", {}});
}

void SettingsApp::AddAccessibility()
{
    const char *Message =
        "Keyboard navigation is available throughout Settings. System-wide contrast, text scaling, and "
        "assistive-technology controls are not implemented.";
    StackPanel *P = NewPage("Accessibility", Message);
    InfoCard *Card = P->Add<InfoCard>("Availability");
    TextBlock *Text = Card->Add<TextBlock>(Message, FontRole::Body);
    Text->Wrap = true;
    List_.Push(Page{"Accessibility", Icon::Info, P, "accessibility contrast text size scale", {}});
}

void SettingsApp::AddRecovery()
{
    StackPanel *Recovery = NewPage("Updates & Recovery", "Update and recovery functions supported by this build.");
    InfoCard *Update = Recovery->Add<InfoCard>("Updates");
    Update->Row("Update service")->SetText("No OS update service is configured.");
    InfoCard *Restore = Recovery->Add<InfoCard>("Recovery");
    Restore->Row("Recovery image")->SetText("No recovery partition or restore image is configured.");
    Restore->Row("Restart and shut down")->SetText("Use Start > Power.");
    List_.Push(Page{"Updates & Recovery", Icon::Info, Recovery, "updates recovery restore reset maintenance", {}});
}

/* ---- Desktop & Dock -------------------------------------------------------------------------- */

void SettingsApp::AddDesktop()
{
    StackPanel *P = NewPage("Desktop & Dock",
                            "The icon layer over the wallpaper, how it arranges its icons, and the top dock.");
    TextBlock *Status = P->Add<TextBlock>("", FontRole::Caption);
    Status->Wrap = true;
    Status->Foreground = Theme::TextSecondary;

    hz::fs::Settings Shell, Prefs;
    STATUS ShellStatus = LoadShellSettings(Shell);
    STATUS PrefsStatus = Prefs.Load(hz::fs::SettingsPath("desktop"));
    if (!HZ_SUCCESS(ShellStatus) && ShellStatus != STATUS_NOT_FOUND)
        HzPrint("settings: desktop page cannot read shell.ini (0x%08X)", (uint32_t)ShellStatus);
    if (!HZ_SUCCESS(PrefsStatus) && PrefsStatus != STATUS_NOT_FOUND)
        HzPrint("settings: desktop page cannot read desktop.ini (0x%08X)", (uint32_t)PrefsStatus);

    /* Both files live in the shell's world: save, then let the shell re-read them. */
    auto Save = [Status](hz::fs::Settings S, bool Desktop) {
        bool Notified = false;
        STATUS Result = Desktop ? SaveDesktopSettings(S, &Notified) : SaveShellSettings(S, &Notified);
        if (!HZ_SUCCESS(Result)) {
            Status->Foreground = Theme::Accent;
            Status->SetText(Result == STATUS_MEDIA_WRITE_PROTECTED
                                ? "The live system is read-only. Install Nexora OS to keep this choice."
                                : hz::String::Format("Could not save the setting (0x%08X).", (uint32_t)Result).CStr());
            return;
        }
        Status->Foreground = Notified ? Theme::TextSecondary : Theme::Accent;
        Status->SetText(Notified ? "Saved." : "Saved, but the shell did not pick the change up.");
        HzPrint("settings: desktop setting saved (%s)", Desktop ? "desktop.ini" : "shell.ini");
    };

    InfoCard *IconsCard = P->Add<InfoCard>("Desktop icons");
    AddToggle(IconsCard, "Show desktop icons", Shell.GetInt("DesktopIcons", 1) != 0, [Save](bool On) {
        hz::fs::Settings S;
        LoadShellSettings(S);
        S.SetInt("DesktopIcons", On ? 1 : 0);
        Save(hz::Move(S), false);
    });
    IconsCard->Note("The shortcut icons on the wallpaper: every pinned app, then Trash. They stay under "
                    "every window and take the keyboard when you click them.");

    InfoCard *Layout = P->Add<InfoCard>("Icon layout");
    hz::Vector<hz::String> Sizes;
    Sizes.Push("Small");
    Sizes.Push("Medium");
    Sizes.Push("Large");
    int Size = Prefs.GetInt("IconSize", 48);
    AddChoice(Layout, "Icon size", Sizes, Size <= 36 ? 0 : Size >= 56 ? 2 : 1, [Save](size_t Index) {
        static const int kSizes[] = {36, 48, 56};
        hz::fs::Settings S;
        S.Load(hz::fs::SettingsPath("desktop"));
        S.SetInt("IconSize", kSizes[Index < 3 ? Index : 1]);
        Save(hz::Move(S), true);
    });
    hz::Vector<hz::String> Keys;
    Keys.Push("Name");
    Keys.Push("Type");
    Keys.Push("Size");
    Keys.Push("Date modified");
    int Sort = Prefs.GetInt("Sort", 0);
    if (Sort < 0 || Sort > 3)
        Sort = 0;
    AddChoice(Layout, "Sort by", Keys, (size_t)Sort, [Save](size_t Index) {
        hz::fs::Settings S;
        S.Load(hz::fs::SettingsPath("desktop"));
        S.SetInt("Sort", (int)Index);
        Save(hz::Move(S), true);
    });
    AddToggle(Layout, "Descending order", Prefs.GetInt("Descending", 0) != 0, [Save](bool On) {
        hz::fs::Settings S;
        S.Load(hz::fs::SettingsPath("desktop"));
        S.SetInt("Descending", On ? 1 : 0);
        Save(hz::Move(S), true);
    });
    Layout->Note("The shortcuts always come first; below them the items of your Desktop folder are "
                 "listed with the folders before the files.");

    InfoCard *Dock = P->Add<InfoCard>("Top dock");
    AddToggle(Dock, "Show the top dock", Shell.GetInt("TopDock", 0) != 0, [Save](bool On) {
        hz::fs::Settings S;
        LoadShellSettings(S);
        S.SetInt("TopDock", On ? 1 : 0);
        Save(hz::Move(S), false);
    });
    Dock->Note("A strip across the top of the screen with the pinned applications, next to the "
               "taskbar at the bottom. Hidden by default.");

    List_.Push(Page{"Desktop & Dock", Icon::AppWindow, P,
                    "desktop icons dock top show hide icon size sort order arrange descending shortcuts",
                    [Status]() { Status->SetText(""); }});
}

/* ---- Default apps ---------------------------------------------------------------------------- */

void SettingsApp::AddDefaultApps()
{
    StackPanel *P = NewPage("Default apps",
                            "Which installed application opens which kind of file. The assignments are read "
                            "from the app manifests in C:\\Nexora\\Apps.");
    hz::Vector<hz::apps::App> Apps = hz::apps::Load();
    InfoCard *Card = P->Add<InfoCard>("Opens by default");
    hz::Vector<hz::String> Extensions;
    for (const hz::apps::App &Program : Apps)
        for (const hz::String &Extension : Program.Opens)
            if (!Extension.Empty() && !Extensions.Contains(Extension))
                Extensions.Push(Extension);
    for (size_t i = 1; i < Extensions.Size(); i++) { /* insertion sort: a readable table of file kinds */
        hz::String Value = hz::Move(Extensions[i]);
        size_t j = i;
        while (j > 0 && strcmp(Extensions[j - 1].CStr(), Value.CStr()) > 0) {
            Extensions[j] = hz::Move(Extensions[j - 1]);
            j--;
        }
        Extensions[j] = hz::Move(Value);
    }
    for (const hz::String &Extension : Extensions) {
        const hz::apps::App *Owner = hz::apps::FindForExtension(Apps, Extension);
        Card->Row(hz::String::Format(".%s", Extension.CStr()).CStr())
            ->SetText(Owner ? Owner->Name.CStr() : "No application");
    }
    if (Extensions.Empty())
        Card->Note("No installed application declares the file types it opens.");
    Card->Note("A file with no assignment opens in Notepad when its text can be read, and a program "
               "listed in its own manifest takes the file. Changing an assignment is not implemented yet.");

    InfoCard *Apps_ = P->Add<InfoCard>("Installed applications");
    for (const hz::apps::App &Program : Apps) {
        TextBlock *Assigned = Apps_->Row(Program.Name.CStr());
        if (Program.Opens.Empty()) {
            Assigned->SetText("Runs its own files");
            continue;
        }
        hz::String List;
        for (size_t i = 0; i < Program.Opens.Size(); i++) {
            if (i)
                List.Append(", ");
            List.Append('.');
            List.Append(Program.Opens[i]);
        }
        Assigned->SetText(List.CStr());
    }
    List_.Push(Page{"Default apps", Icon::AppWindow, P, "default apps file types extensions associations opens", {}});
}

/* ---- Keyboard -------------------------------------------------------------------------------- */

void SettingsApp::AddKeyboard()
{
    StackPanel *P = NewPage("Keyboard", "The keys this build's shell and its file windows understand.");
    InfoCard *System = P->Add<InfoCard>("Everywhere");
    System->Row("Start menu")->SetText("The logo (Windows) key opens and closes Start.");
    System->Row("Switch windows")->SetText("Alt+Tab, Alt+Shift+Tab for the other direction, Escape to cancel, "
                                           "Alt+F4 to close the active window.");
    System->Row("Move focus")->SetText("Tab and Shift+Tab move through the controls; the focus ring appears "
                                       "after keyboard navigation.");
    System->Row("Menus")->SetText("Shift+F10 opens the context menu of the focused item; the arrow keys and "
                                  "Escape work inside it.");

    InfoCard *Files = P->Add<InfoCard>("Files");
    Files->Row("Open")->SetText("Enter, or a double click.");
    Files->Row("Up / back / forward")->SetText("Backspace or Alt+Up; Alt+Left and Alt+Right.");
    Files->Row("Rename")->SetText("F2.");
    Files->Row("Delete")->SetText("Delete asks for confirmation and removes the item for good.");
    Files->Row("Copy / Cut / Paste")->SetText("Ctrl+C, Ctrl+X, Ctrl+V.");
    Files->Row("New folder")->SetText("Ctrl+Shift+N.");
    Files->Row("Address bar / search")->SetText("Ctrl+L; Ctrl+F or Ctrl+E.");
    Files->Row("Refresh")->SetText("F5.");

    InfoCard *Desktop = P->Add<InfoCard>("Desktop icons");
    Desktop->Row("Open")->SetText("Enter, or a double click.");
    Desktop->Row("Rename")->SetText("F2.");
    Desktop->Row("Delete to Trash")->SetText("Delete moves the item to the Trash folder instead of "
                                             "removing it for good.");
    Desktop->Row("Copy / Cut / Paste")->SetText("Ctrl+C, Ctrl+X, Ctrl+V.");
    Desktop->Row("Context menu")->SetText("Shift+F10 on the selected icon, or the right mouse button.");
    Desktop->Row("Move the selection")->SetText("The arrow keys; Escape clears it.");
    Desktop->Row("Refresh")->SetText("F5.");

    InfoCard *Layout = P->Add<InfoCard>("Layout");
    Layout->Row("Keyboard layout")->SetText("The built-in US layout is fixed in this build; no layout "
                                            "switching or IME.");
    List_.Push(Page{"Keyboard", Icon::Prompt, P, "keyboard keys shortcuts hotkeys layout input tab focus", {}});
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
    AddSystem();
    AddPersonalization();
    AddDesktop();
    AddNetwork();
    AddDevices();
    AddApps();
    AddDefaultApps();
    AddAccounts();
    AddPrivacySecurity();
    AddAccessibility();
    AddTime();
    AddKeyboard();
    AddRecovery();
    AddAbout();

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
