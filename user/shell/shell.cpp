/*
 * Nexora OS - user/shell/shell.cpp
 * The shell's start-up and state: the installed programs (manifests), the list of app windows
 * (from win32k's notifications) and the programs they belong to, recently opened programs,
 * launching, activation from the taskbar and the switcher, requests from programs, flyouts,
 * program icons and the lock screen. Notable steps are logged ("shell: ...") for the smoke tests.
 */
#include "shell.h"
#include "hzcrypto.h"
#include "hzfile.h"
#include "taskbar.h"

namespace shell {

Shell g_Shell;

/* ---- Programs ------------------------------------------------------------------------------------ */

void Shell::LoadApps()
{
    Apps = hz::apps::Load();
    HzPrint("shell: %zu programs installed", Apps.Size());
}

const hz::apps::App *Shell::FindApp(const hz::String &Id) const
{
    for (const hz::apps::App &A : Apps)
        if (A.Id == Id)
            return &A;
    return nullptr;
}

const hz::apps::App *Shell::AppForWindow(HWND Window) const
{
    const AppWindow *W = Find(Window);
    return W && !W->AppId.Empty() ? FindApp(W->AppId) : nullptr;
}

void Shell::NoteRecent(const hz::String &Id)
{
    if (!Recent.Empty() && Recent[0] == Id)
        return;
    Recent.Remove(Id);
    Recent.Insert(0, Id);
    while (Recent.Size() > kMaxRecent)
        Recent.Pop();
    SaveSettings();
}

/* ---- Settings ------------------------------------------------------------------------------------ */

void Shell::LoadSettings()
{
    Persistent = !hz::fs::IsLiveSystem();
    Config = hz::fs::Settings();
    STATUS Status = Config.Load(hz::fs::SettingsPath("shell"));
    UserName = Config.Get("UserName");
    PasswordSalt = Config.Get("PasswordSalt");
    PasswordHash = Config.Get("PasswordHash");
    UtcOffset = hz::fs::ClampUtcOffset(Config.GetInt("UtcOffsetMinutes", 0));
    FirstBoot = Persistent && Config.GetInt("FirstBoot", 0) != 0;
    int Saved = Config.GetInt("Wallpaper", -1);
    if (Saved >= 0 && Saved <= (int)HZ_MAX_WALLPAPER && (uint32_t)Saved != Wallpaper && HZ_SUCCESS(NtUserSetWallpaper((uint32_t)Saved)))
        Wallpaper = (uint32_t)Saved;
    Recent.Clear();
    hz::String List = Config.Get("Recent");
    for (const char *P = List.CStr(); *P;) {
        const char *End = P;
        while (*End && *End != ',')
            End++;
        hz::String Id(P, (size_t)(End - P));
        if (!Id.Empty() && FindApp(Id) && Recent.Size() < kMaxRecent)
            Recent.Push(Id);
        P = *End ? End + 1 : End;
    }
    TaskbarPins.Clear();
    List = Config.Get("TaskbarPins");
    for (const char *P = List.CStr(); *P;) {
        const char *End = P;
        while (*End && *End != ',')
            End++;
        hz::String Id(P, (size_t)(End - P));
        if (!Id.Empty() && FindApp(Id) && !IsTaskbarPinned(Id))
            TaskbarPins.Push(Id);
        P = *End ? End + 1 : End;
    }
    if (Bar)
        Bar->Rebuild();
    HzPrint("shell: settings %s: user '%s', wallpaper %u%s%s%s", HZ_SUCCESS(Status) ? "loaded" : "not found",
            DisplayName(), Wallpaper, HasPassword() ? ", password" : "", FirstBoot ? ", first boot" : "",
            Persistent ? "" : " (live system: not saved)");
}

void Shell::SaveSettings()
{
    if (!Persistent)
        return;
    Config.SetInt("Wallpaper", (int)Wallpaper);
    hz::String List;
    for (size_t i = 0; i < Recent.Size(); i++) {
        if (i)
            List.Append(',');
        List.Append(Recent[i]);
    }
    Config.Set("Recent", List.CStr());
    List = hz::String();
    for (size_t i = 0; i < TaskbarPins.Size(); i++) {
        if (i)
            List.Append(',');
        List.Append(TaskbarPins[i]);
    }
    Config.Set("TaskbarPins", List.CStr());
    STATUS Status = Config.Save(hz::fs::SettingsPath("shell"));
    if (!HZ_SUCCESS(Status))
        HzPrint("shell: cannot save the settings (0x%08X)", (uint32_t)Status);
}

bool Shell::IsTaskbarPinned(const hz::String &Id) const
{
    for (const hz::String &Pinned : TaskbarPins)
        if (Pinned == Id)
            return true;
    return false;
}

void Shell::ToggleTaskbarPin(const hz::String &Id)
{
    if (!FindApp(Id))
        return;
    if (IsTaskbarPinned(Id))
        TaskbarPins.Remove(Id);
    else
        TaskbarPins.Push(Id);
    SaveSettings();
    if (Bar)
        Bar->Rebuild();
    HzPrint("shell: %s taskbar pin %s", IsTaskbarPinned(Id) ? "added" : "removed", Id.CStr());
}

void Shell::SetWallpaper(uint32_t Index)
{
    if (Index > HZ_MAX_WALLPAPER || !HZ_SUCCESS(NtUserSetWallpaper(Index)))
        return;
    Wallpaper = Index;
    if (Quick && Quick->Content())
        Quick->Content()->Invalidate();
    SaveSettings();
}

void ShowAppContextMenu(Window *Owner, const hz::apps::App &App, Point Screen, HWND Window)
{
    if (!Owner || !Owner->IsOpen())
        return;
    hz::apps::App Program = App;
    hz::Vector<MenuItem> Items;
    Items.Push(MenuItem::Make(Window ? "Open" : "Launch", [Program, Window]() {
        if (Window) {
            g_Shell.ActivateWindow(Window);
        } else {
            if (g_Shell.Start)
                static_cast<Flyout *>(g_Shell.Start)->Dismiss();
            g_Shell.Launch(Program);
        }
    }));
    Items.Push(MenuItem::Make(g_Shell.IsTaskbarPinned(Program.Id) ? "Unpin from taskbar" : "Pin to taskbar",
                              [Id = Program.Id]() { g_Shell.ToggleTaskbarPin(Id); }));
    if (Window) {
        Items.Push(MenuItem::MakeSeparator());
        Items.Push(MenuItem::Make("Close window", [Window]() { NtUserPostMessage(Window, HZ_WM_CLOSE, 0, 0); }));
    }
    ShowMenu(Owner, Screen, hz::Move(Items));
}

namespace {

/* The image name of a process ("notepad.exe"), "" if it is gone. */
hz::String ImageOf(uint32_t ProcessId)
{
    static HZ_SYSTEM_PROCESS_INFORMATION Buffer[128];
    uint32_t Returned = 0;
    if (!HZ_SUCCESS(NtQuerySystemInformation(HZ_SYSTEM_PROCESS_INFORMATION_CLASS, Buffer, sizeof(Buffer), &Returned)))
        return hz::String();
    size_t Count = hz::Min<size_t>(Returned / sizeof(HZ_SYSTEM_PROCESS_INFORMATION), 128);
    for (size_t i = 0; i < Count; i++)
        if (Buffer[i].ProcessId == ProcessId)
            return hz::String(Buffer[i].ImageName);
    return hz::String();
}

}  // namespace

/* ---- Window list ----------------------------------------------------------------------------- */

void Shell::RefreshWindows()
{
    HWND Handles[64];
    uint32_t Count = 0;
    if (!HZ_SUCCESS(NtUserEnumWindows(Handles, 64, &Count)))
        return;
    Count = hz::Min<uint32_t>(Count, 64);
    hz::Vector<AppWindow> Next;
    /* Keep the existing order, then add new windows at the end. */
    for (const AppWindow &Known : Windows)
        for (uint32_t i = 0; i < Count; i++)
            if (Handles[i] == Known.Handle)
                Next.Push(Known);
    for (uint32_t i = 0; i < Count; i++) {
        HWND H = Handles[Count - 1 - i]; /* bottom of the z-order first */
        bool Present = false;
        for (const AppWindow &W : Next)
            Present = Present || W.Handle == H;
        if (!Present) {
            AppWindow W;
            W.Handle = H;
            W.ProcessId = 0xFFFFFFFFu; /* not looked up yet */
            Next.Push(W);
        }
    }
    for (AppWindow &W : Next) {
        HZ_WINDOW_INFO Info;
        if (HZ_SUCCESS(NtUserGetWindowInfo(W.Handle, &Info))) {
            W.Title = Info.Title;
            W.Minimized = (Info.State & HZ_WINDOW_STATE_MINIMIZED) != 0;
            if (Info.State & HZ_WINDOW_STATE_ACTIVE)
                Active = W.Handle;
            if (W.ProcessId == 0xFFFFFFFFu) { /* new: which program is it? */
                W.ProcessId = Info.ProcessId;
                hz::String Image = ImageOf(Info.ProcessId);
                if (const hz::apps::App *A = hz::apps::FindByImage(Apps, Image.CStr())) {
                    W.AppId = A->Id;
                    NoteRecent(A->Id);
                }
            }
        }
    }
    Windows = hz::Move(Next);
    if (Bar)
        Bar->Rebuild();
}

const AppWindow *Shell::Find(HWND Window) const
{
    for (const AppWindow &W : Windows)
        if (W.Handle == Window)
            return &W;
    return nullptr;
}

hz::String Shell::TitleOf(HWND Window) const
{
    HZ_WINDOW_INFO Info;
    if (HZ_SUCCESS(NtUserGetWindowInfo(Window, &Info)))
        return hz::String(Info.Title);
    return hz::String("?");
}

void Shell::OnShellEvent(uint64_t Event, uint64_t Detail)
{
    switch (Event) {
    case HZ_SHELL_WINDOW_CREATED:
    case HZ_SHELL_WINDOW_DESTROYED:
    case HZ_SHELL_WINDOW_CHANGED:
        RefreshWindows();
        break;
    case HZ_SHELL_WINDOW_ACTIVATED:
        Active = (HWND)Detail;
        if (Bar)
            Bar->Refresh();
        break;
    case HZ_SHELL_START_KEY:
        if (!Locked)
            ToggleStart();
        break;
    case HZ_SHELL_SWITCH_NEXT:
        if (!Locked)
            SwitchNext(Detail != 0);
        break;
    case HZ_SHELL_SWITCH_END:
        SwitchEnd(Detail != 0);
        break;
    case HZ_SHELL_PROCESS_CRASHED: {
        uint32_t Pid = (uint32_t)(Detail >> 32);
        STATUS Status = (STATUS)(uint32_t)Detail;
        /* The notice is posted before the process is torn down: its windows are still listed
           (their destruction is reported later); a windowless one may already be gone. */
        const hz::apps::App *App = nullptr;
        for (const AppWindow &W : Windows)
            if (W.ProcessId == Pid && !W.AppId.Empty())
                App = FindApp(W.AppId);
        hz::String Image = ImageOf(Pid);
        if (!App && !Image.Empty())
            App = hz::apps::FindByImage(Apps, Image.CStr());
        if (Image.Empty())
            Image = hz::String::Format("A program (process %u)", Pid);
        const char *Why = Status == STATUS_ACCESS_VIOLATION        ? "it accessed memory it does not own"
                          : Status == STATUS_STACK_OVERFLOW         ? "it ran out of stack"
                          : Status == STATUS_INTEGER_DIVIDE_BY_ZERO ? "it divided by zero"
                          : Status == STATUS_ILLEGAL_INSTRUCTION    ? "it ran an invalid instruction"
                          : Status == STATUS_PRIVILEGED_INSTRUCTION ? "it ran a privileged instruction"
                                                                    : "of an unexpected error";
        hz::String Text = hz::String::Format("%s stopped working because %s (0x%08X). Unsaved work in it is lost.",
                                             App ? App->Name.CStr() : Image.CStr(), Why, (uint32_t)Status);
        HzPrint("shell: %s crashed (0x%08X)", Image.CStr(), (uint32_t)Status);
        MessageBox(nullptr, "Program error", Text.CStr(), nullptr, nullptr);
        break;
    }
    default:
        break;
    }
}

void Shell::OnShellRequest(uint64_t Request, uint64_t Detail)
{
    if (Request == HZ_SHELL_REQUEST_WALLPAPER) {
        if (Detail > HZ_MAX_WALLPAPER)
            return;
        SetWallpaper((uint32_t)Detail);
        HzPrint("shell: wallpaper %u (requested)", Wallpaper);
    } else if (Request == HZ_SHELL_REQUEST_SETTINGS) {
        bool WasFirstBoot = FirstBoot;
        LoadSettings();
        if (Start)
            static_cast<Flyout *>(Start)->Dismiss();
        if (WasFirstBoot && !FirstBoot) {
            HzPrint("shell: first-boot setup finished; hello, %s", DisplayName());
            if (const hz::apps::App *Welcome = FindApp(hz::String("welcome")))
                Launch(*Welcome);
        }
    } else if (Request == HZ_SHELL_REQUEST_LOCK) {
        LockNow();
    } else if (Request == HZ_SHELL_REQUEST_SHUTDOWN) {
        if (Detail == HZ_SHUTDOWN_POWER_OFF || Detail == HZ_SHUTDOWN_REBOOT)
            RequestShutdown((uint32_t)Detail);
    }
}

/* ---- Launching and activation ------------------------------------------------------------------ */

void Shell::Launch(const hz::apps::App &App)
{
    uint32_t Id = 0;
    STATUS Status = hz::apps::Launch(App, nullptr, &Id);
    if (HZ_SUCCESS(Status)) {
        NoteRecent(App.Id);
        HzPrint("shell: launched %s", hz::fs::FileName(App.Path).CStr());
    } else {
        HzPrint("shell: cannot start %s (0x%08X)", App.Path.CStr(), (uint32_t)Status);
    }
}

void Shell::ActivateWindow(HWND Window)
{
    HZ_WINDOW_INFO Info;
    if (!HZ_SUCCESS(NtUserGetWindowInfo(Window, &Info)))
        return;
    if (Info.State & HZ_WINDOW_STATE_MINIMIZED)
        NtUserShowWindow(Window, HZ_SW_RESTORE);
    else
        NtUserSetForegroundWindow(Window);
}

void Shell::TaskbarClick(HWND Window)
{
    CloseFlyouts();
    HZ_WINDOW_INFO Info;
    if (!HZ_SUCCESS(NtUserGetWindowInfo(Window, &Info)))
        return;
    if (Info.State & HZ_WINDOW_STATE_MINIMIZED) {
        NtUserShowWindow(Window, HZ_SW_RESTORE);
        HzPrint("shell: restored %s", Info.Title);
    } else if (Info.State & HZ_WINDOW_STATE_ACTIVE) {
        NtUserShowWindow(Window, HZ_SW_MINIMIZE);
        HzPrint("shell: minimized %s", Info.Title);
    } else {
        NtUserSetForegroundWindow(Window);
        HzPrint("shell: activated %s", Info.Title);
    }
}

/* ---- Flyouts ------------------------------------------------------------------------------------ */

Flyout::Flyout(const char *Title, int32_t Width, int32_t Height, Window **Slot)
    : Window(Title, Width, Height, HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_MICA | HZ_WS_VISIBLE), Slot_(Slot)
{
    *Slot = this;
    OnUnhandledKey = [this](KeyEvent &E) {
        if (E.Key == HZ_VK_ESCAPE) {
            E.Handled = true;
            Dismiss();
        }
    };
}

int64_t Flyout::HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (Message == HZ_WM_ACTIVATE && WParam == 0 && IsOpen()) {
        Window::HandleMessage(Message, WParam, LParam);
        Dismiss(); /* clicked elsewhere */
        return 0;
    }
    return Window::HandleMessage(Message, WParam, LParam);
}

void Flyout::Dismiss()
{
    if (*Slot_ == this)
        *Slot_ = nullptr;
    if (IsOpen())
        Close();
    DeferDelete(this);
}

void Shell::ToggleStart()
{
    if (Start) {
        static_cast<Flyout *>(Start)->Dismiss();
        HzPrint("shell: start closed");
        return;
    }
    if (Quick)
        static_cast<Flyout *>(Quick)->Dismiss();
    if (CreateStartMenu())
        HzPrint("shell: start opened");
}

void Shell::ToggleQuick()
{
    if (Quick) {
        static_cast<Flyout *>(Quick)->Dismiss();
        return;
    }
    if (Start)
        static_cast<Flyout *>(Start)->Dismiss();
    if (CreateQuickSettings())
        HzPrint("shell: quick settings opened");
}

void Shell::CloseFlyouts()
{
    if (Start)
        static_cast<Flyout *>(Start)->Dismiss();
    if (Quick)
        static_cast<Flyout *>(Quick)->Dismiss();
}

/* ---- Alt+Tab ------------------------------------------------------------------------------------- */

void Shell::SwitchNext(bool Back)
{
    if (!Switcher) {
        HWND Handles[32];
        uint32_t Count = 0;
        NtUserEnumWindows(Handles, 32, &Count); /* top first: most recently active first */
        SwitchList.Clear();
        for (uint32_t i = 0; i < hz::Min<uint32_t>(Count, 32); i++)
            SwitchList.Push(Handles[i]);
        if (SwitchList.Empty())
            return;
        CloseFlyouts();
        SwitchIndex = SwitchList.Size() > 1 ? (Back ? SwitchList.Size() - 1 : 1) : 0;
        CreateSwitcher();
        return;
    }
    size_t N = SwitchList.Size();
    SwitchIndex = Back ? (SwitchIndex + N - 1) % N : (SwitchIndex + 1) % N;
    if (Switcher->Content())
        Switcher->Content()->Invalidate();
}

void Shell::SwitchEnd(bool Commit)
{
    if (!Switcher)
        return;
    Window *S = Switcher;
    Switcher = nullptr;
    S->Close();
    DeferDelete(S);
    if (Commit && SwitchIndex < SwitchList.Size()) {
        HWND Target = SwitchList[SwitchIndex];
        ActivateWindow(Target);
        HzPrint("shell: switched to %s", TitleOf(Target).CStr());
    }
}

/* ---- Icons ---------------------------------------------------------------------------------------- */

void PaintLogo(Painter &P, const Rect &Box)
{
    P.DrawIcon(Icon::Nexora, Box, 0xFFC5CDD4, 2.5f);
}

Icon IconByName(const hz::String &Name)
{
    static const struct {
        const char *Name;
        Icon Glyph;
    } Table[] = {
        {"Home", Icon::Home},         {"Clock", Icon::Clock},     {"Settings", Icon::Settings}, {"Star", Icon::Star},
        {"Sun", Icon::Sun},           {"Moon", Icon::Moon},       {"Info", Icon::Info},         {"Search", Icon::Search},
        {"Folder", Icon::Folder},     {"Document", Icon::Document}, {"Drive", Icon::Drive},     {"Prompt", Icon::Prompt},
        {"Calculator", Icon::Calculator}, {"Chart", Icon::Chart}, {"Pencil", Icon::Pencil},     {"Trash", Icon::Trash},
        {"Save", Icon::Save},         {"Cube", Icon::Cube},         {"Open", Icon::Open},       {"Volume", Icon::Volume},     {"Power", Icon::Power},
    };
    for (const auto &E : Table)
        if (hz::fs::EqualNames(E.Name, Name.CStr()))
            return E.Glyph;
    return Icon::None;
}

void PaintAppIcon(Painter &P, const Rect &Box, const hz::apps::App *App, const char *Title)
{
    float Radius = Box.Width / 4.0f;
    if (App) {
        Icon Glyph = IconByName(App->Icon);
        P.FillVerticalGradient(Box, Radius, MixColors(App->Color, 0xFFFFFFFF, 0.12f), App->Color);
        if (Glyph != Icon::None) {
            P.DrawIcon(Glyph, Box.Deflate(Thickness(Box.Width / 5)), 0xFFFFFFFF, 1.5f);
            return;
        }
        Title = App->Name.CStr();
    }
    static const Color Tiles[] = {0xFF3A6FD8, 0xFF2D9E6E, 0xFFD0693A, 0xFF8E52C8, 0xFFC94471, 0xFF2697A6};
    uint32_t Hash = 5381;
    for (const char *C = Title; *C; C++)
        Hash = Hash * 33 + (uint8_t)*C;
    if (!App)
        P.FillRoundRect(Box, Radius, Tiles[Hash % 6]);
    char Letter[2] = {Title[0] ? Title[0] : '?', 0};
    if (Letter[0] >= 'a' && Letter[0] <= 'z')
        Letter[0] = (char)(Letter[0] - 32);
    P.TextLine(GetFont(Box.Width >= 32 ? FontRole::Subtitle : FontRole::BodyStrong), Box, Letter, 0xFFFFFFFF, Align::Center);
}

/* ---- Lock screen ---------------------------------------------------------------------------------- */

namespace {

void FormatClock(char *Time, size_t TimeSize, char *Date, size_t DateSize)
{
    uint64_t Now = 0;
    NtQuerySystemTime(&Now);
    hz::fs::CivilTime T = hz::fs::ToCivil(Now, g_Shell.UtcOffset);
    HzFormat(Time, TimeSize, "%02u:%02u", T.Hour, T.Minute);
    HzFormat(Date, DateSize, "%s, %u %s", hz::fs::WeekdayNames[T.Weekday], T.Day, hz::fs::MonthNames[(T.Month - 1) % 12]);
}

/* The lock screen: the time and date, a greeting and a hint. Any key or a click unlocks; with a
   password (M11) it first shows the password box (Enter checks it, Escape hides it again). */
class LockView final : public Element {
public:
    LockView()
    {
        Big_ = HzLoadFont("C:\\Nexora\\Fonts\\Inter-SemiBold.ttf", 72);
        Medium_ = HzLoadFont("C:\\Nexora\\Fonts\\Inter-Regular.ttf", 18);
    }

    TextBox *Box = nullptr; /* the password box (a sibling over this view) */
    bool Prompting() const { return Prompting_; }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        float T = Leave_.Value();
        int32_t Shift = (int32_t)(T * B.Height * 0.25f);
        float Fade = 1 - T;
        P.FillVerticalGradient(Rect{B.X, B.Y + B.Height / 3, B.Width, B.Height - B.Height / 3}, 0,
                               ScaleAlpha(0x00000000, Fade), ScaleAlpha(0xA0000000, Fade));
        char Time[16], Date[64];
        FormatClock(Time, sizeof(Time), Date, sizeof(Date));
        int32_t Y = B.Y + B.Height * 12 / 100 - Shift;
        if (Big_) {
            int32_t W = HzMeasureText(Big_, Time);
            P.Text(Big_, B.X + (B.Width - W) / 2, Y + HzFontAscent(Big_), Time, ScaleAlpha(0xFFFFFFFF, Fade));
            Y += HzFontLineHeight(Big_);
        }
        if (Medium_) {
            int32_t W = HzMeasureText(Medium_, Date);
            P.Text(Medium_, B.X + (B.Width - W) / 2, Y + HzFontAscent(Medium_), Date, ScaleAlpha(0xFFFFFFFF, Fade));
        }
        if (!Prompting_)
            return;
        bool Password = g_Shell.HasPassword();
        if (!g_Shell.UserName.Empty() || Password) {
            hz::String Hello = hz::String::Format("Hello, %s", g_Shell.DisplayName());
            P.TextLine(GetFont(FontRole::Subtitle), Rect{B.X, B.Bottom() - (Password ? 190 : 112) - Shift, B.Width, 32},
                       Hello.CStr(), ScaleAlpha(0xFFFFFFFF, Fade), Align::Center);
        }
        const char *Hint = !Password    ? "Press any key or click to unlock"
                           : !Prompting_ ? "Press any key or click to sign in"
                           : Wrong_      ? "The password is incorrect. Try again."
                                         : "Type your password and press Enter";
        P.TextLine(GetFont(FontRole::Body), Rect{B.X, B.Bottom() - 72 - Shift, B.Width, 24}, Hint,
                   ScaleAlpha(Wrong_ && Prompting_ ? 0xFFFF9A8A : Theme::TextSecondary, Fade), Align::Center);
    }

    void OnPointerDown(PointerEvent &E) override
    {
        E.Handled = true;
        if (Prompting_)
            GetWindow()->SetFocus(Box);
        else
            Begin();
    }

    void Begin()
    {
        if (Leaving_)
            return;
        if (g_Shell.HasPassword() && !Accepted_) {
            if (!Prompting_) {
                Prompting_ = true;
                Box->SetVisible(true);
                GetWindow()->SetFocus(Box);
                GetWindow()->DropNextChar(); /* the key that opened the box is not part of the password */
                HzPrint("shell: password prompt");
                Invalidate();
            }
            return;
        }
        Leaving_ = true;
        Leave_.To(1, 260);
        StartAnimation();
    }

    void Submit(const hz::String &Typed)
    {
        if (hz::crypto::CheckPassword(Typed, g_Shell.PasswordSalt, g_Shell.PasswordHash)) {
            HzPrint("shell: password accepted");
            Accepted_ = true;
            Prompting_ = false;
            Box->SetVisible(false);
            Begin();
            return;
        }
        Failures_++;
        HzPrint("shell: wrong password (%u)", Failures_);
        Wrong_ = true;
        Box->SetText("");
        Invalidate();
    }

    void Cancel()
    {
        Prompting_ = Wrong_ = false;
        Box->SetText("");
        Box->SetVisible(false);
        Invalidate();
    }

    bool Animate() override
    {
        Invalidate();
        if (Leave_.Running())
            return true;
        g_Shell.Unlock();
        return false;
    }

private:
    HZ_FONT *Big_, *Medium_;
    Transition Leave_;
    bool Leaving_ = false;
    bool Prompting_ = false, Accepted_ = false, Wrong_ = false;
    uint32_t Failures_ = 0;
};

}  // namespace

Window *CreateLockScreen()
{
    const Rect &S = g_Shell.Screen;
    Window *Lock = new Window("Lock", S.Width, S.Height,
                              HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_TRANSPARENT | HZ_WS_NOFRAME | HZ_WS_VISIBLE);
    Lock->Background = 0x00000000;
    Grid *Root = new Grid();
    LockView *View = Root->Add<LockView>();
    TextBox *Box = Root->Add<TextBox>("Password");
    Box->Password = true;
    Box->MaxLength = 64;
    Box->Width = 300;
    Box->HorizontalAlignment = Align::Center;
    Box->VerticalAlignment = Align::End;
    Box->Margin = Thickness(0, 0, 0, 120);
    Box->SetVisible(false);
    Box->OnSubmit = [View](const hz::String &Typed) { View->Submit(Typed); };
    View->Box = Box;
    Lock->SetContent(Root);
    Lock->OnUnhandledKey = [View](KeyEvent &E) {
        E.Handled = true;
        if (View->Prompting()) {
            if (E.Key == HZ_VK_ESCAPE)
                View->Cancel();
            return;
        }
        View->Begin();
    };
    if (!Lock->ShowAt(S.X, S.Y)) {
        delete Lock;
        return nullptr;
    }
    Lock->SetTimer(1000, [View]() { View->Invalidate(); });
    return Lock;
}

void Shell::Unlock()
{
    if (!Locked)
        return;
    Locked = false;
    if (Lock) {
        Window *L = Lock;
        Lock = nullptr;
        L->Close();
        DeferDelete(L);
    }
    if (Bar)
        NtUserShowWindow(Bar->Handle(), HZ_SW_SHOWNOACTIVATE); /* hidden while locked */
    HzPrint("shell: unlocked");
    if (Started)
        return; /* unlocked again after Lock */
    Started = true;
    if (FirstBoot) { /* an installed system's first start: setup before anything else */
        HANDLE Process = 0;
        STATUS Status = NtCreateProcess("C:\\Nexora\\System\\setup.exe", "setup.exe firstboot", &Process, nullptr);
        if (HZ_SUCCESS(Status)) {
            NtClose(Process);
            HzPrint("shell: first boot: started first-boot setup");
            return;
        }
        HzPrint("shell: cannot start first-boot setup (0x%08X)", (uint32_t)Status);
    }
    if (const hz::apps::App *Welcome = FindApp(hz::String("welcome")))
        Launch(*Welcome);
}

/* ---- Shutdown (M11) ------------------------------------------------------------------------------- */

namespace {
constexpr uint64_t kShutdownWaitMs = 15000;
}

void Shell::RequestShutdown(uint32_t Action)
{
    if (ShuttingDown)
        return;
    CloseFlyouts();
    ShuttingDown = true;
    ShutdownAsked = false;
    ShutdownAction = Action;
    ShutdownStartUs = NowUs();
    SaveSettings();
    HzPrint("shell: %s: closing %zu window(s)", Action == HZ_SHUTDOWN_REBOOT ? "restart" : "shut down", Windows.Size());
    for (const AppWindow &W : Windows)
        NtUserPostMessage(W.Handle, HZ_WM_CLOSE, 0, 0);
    if (Windows.Empty()) {
        FinishShutdown();
        return;
    }
    ShutdownTimer = Bar->SetTimer(250, [this]() { ShutdownTick(); });
}

void Shell::ShutdownTick()
{
    if (!ShuttingDown)
        return;
    if (Windows.Empty()) {
        FinishShutdown();
        return;
    }
    if (ShutdownAsked || NowUs() - ShutdownStartUs < kShutdownWaitMs * 1000)
        return;
    ShutdownAsked = true;
    hz::String Names;
    for (size_t i = 0; i < Windows.Size() && i < 4; i++) {
        if (i)
            Names.Append(", ");
        const hz::apps::App *App = FindApp(Windows[i].AppId);
        Names.Append(App ? App->Name : Windows[i].Title);
    }
    HzPrint("shell: still open after %llu s: %s", (unsigned long long)(kShutdownWaitMs / 1000), Names.CStr());
    hz::String Text = hz::String::Format("These programs are still open: %s. Unsaved work in them is lost if you go on.",
                                         Names.CStr());
    const char *Title = ShutdownAction == HZ_SHUTDOWN_REBOOT ? "Restart" : "Shut down";
    MessageBox(nullptr, Title, Text.CStr(),
               ShutdownAction == HZ_SHUTDOWN_REBOOT ? "Restart anyway|Cancel" : "Shut down anyway|Cancel", [this](int Choice) {
                   if (Choice == 0) {
                       FinishShutdown();
                       return;
                   }
                   HzPrint("shell: shutdown canceled");
                   ShuttingDown = false;
                   if (ShutdownTimer)
                       Bar->KillTimer(ShutdownTimer);
                   ShutdownTimer = 0;
               });
}

void Shell::FinishShutdown()
{
    HzPrint(ShutdownAction == HZ_SHUTDOWN_REBOOT ? "shell: restart" : "shell: shut down");
    NtShutdownSystem(ShutdownAction);
}

/* Start > Power > Lock, or a program's HZ_SHELL_REQUEST_LOCK (the terminal's "lock"). */
void Shell::LockNow()
{
    if (Locked)
        return;
    CloseFlyouts();
    Locked = true;
    Lock = CreateLockScreen();
    if (!Lock) {
        Locked = false;
        return;
    }
    if (Bar)
        NtUserShowWindow(Bar->Handle(), HZ_SW_HIDE);
    HzPrint("shell: locked");
}

}  // namespace shell

using namespace shell;

extern "C" int main(int argc, char **argv)
{
    HZ_DISPLAY_INFO Display;
    memset(&Display, 0, sizeof(Display));
    if (!HZ_SUCCESS(NtUserGetDisplayInfo(&Display)) || Display.Width < 640 || Display.Height < 480) {
        HzPrint("shell: no usable display");
        return 1;
    }
    g_Shell.Screen = Rect{0, 0, (int32_t)Display.Width, (int32_t)Display.Height};
    g_Shell.Wallpaper = Display.Wallpaper;
    g_Shell.LoadApps();
    g_Shell.Work = Rect{0, 0, (int32_t)Display.Width, (int32_t)Display.Height - kTaskbarHeight};

    Taskbar *Bar = new Taskbar(g_Shell.Screen.Width);
    if (!Bar->ShowAt(0, g_Shell.Screen.Height - kTaskbarHeight)) {
        HzPrint("shell: cannot create the taskbar");
        return 1;
    }
    STATUS Status = NtUserRegisterShell(Bar->Handle());
    if (!HZ_SUCCESS(Status)) {
        HzPrint("shell: another shell is running (0x%08X)", (uint32_t)Status);
        return 1;
    }
    HZ_RECT Work = g_Shell.Work.ToHz();
    NtUserSetWorkArea(&Work);
    g_Shell.Bar = Bar;
    g_Shell.RefreshWindows();

    g_Shell.LoadSettings(); /* after registering: the wallpaper may change */
    /* Restarted by the session after it exited (M11): no lock screen, no Welcome. */
    bool Restarted = argc > 1 && strcmp(argv[1], "restart") == 0;
    if (Restarted) {
        HzPrint("shell: restarted");
        g_Shell.Started = true;
        g_Shell.Locked = false;
        if (g_Shell.HasPassword()) { /* ending the shell must not get past the password */
            g_Shell.Locked = true;
            g_Shell.Lock = CreateLockScreen();
            HzPrint("shell: lock screen shown");
        }
        if (!g_Shell.Locked || !g_Shell.Lock) {
            g_Shell.Locked = false;
            NtUserShowWindow(Bar->Handle(), HZ_SW_SHOWNOACTIVATE);
        }
        HzPrint("shell: ready");
        return RunMessageLoop();
    }
    g_Shell.Lock = g_Shell.FirstBoot ? nullptr : CreateLockScreen();
    if (g_Shell.Lock)
        HzPrint("shell: lock screen shown");
    else
        g_Shell.Unlock();
    HzPrint("shell: ready");
    return RunMessageLoop();
}
