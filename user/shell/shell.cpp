/*
 * Nexora OS - user/shell/shell.cpp
 * The shell's start-up and state: the installed programs (manifests), the list of app windows
 * (from win32k's notifications) and the programs they belong to, recently opened programs,
 * launching, activation from the taskbar and the switcher, requests from programs, flyouts,
 * program icons and the lock screen. Notable steps are logged ("shell: ...") for the smoke tests.
 */
#include "desktop.h"
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
    ProfilePicture = Config.Get("ProfilePicture");
    PasswordSalt = Config.Get("PasswordSalt");
    PasswordHash = Config.Get("PasswordHash");
    UtcOffset = hz::fs::ClampUtcOffset(Config.GetInt("UtcOffsetMinutes", 0));
    FirstBoot = Persistent && Config.GetInt("FirstBoot", 0) != 0;
    int Saved = Config.GetInt("Wallpaper", 0);
    if (Saved >= 0 && Saved <= (int)HZ_MAX_WALLPAPER && (uint32_t)Saved != Wallpaper && HZ_SUCCESS(NtUserSetWallpaper((uint32_t)Saved)))
        Wallpaper = (uint32_t)Saved;
    int SavedLock = Config.GetInt("LockWallpaper", HZ_LOCK_WALLPAPER_DEFAULT);
    LockWallpaper = SavedLock >= 0 && SavedLock <= (int)HZ_MAX_WALLPAPER
                        ? (uint32_t)SavedLock
                        : HZ_LOCK_WALLPAPER_DEFAULT;
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
    /* The shell's own appearance (M10/M11): the top dock and the desktop icon layer. Settings
       writes both and asks for HZ_SHELL_REQUEST_SETTINGS, so a change takes effect at once. */
    ShowTopDock = Config.GetInt("TopDock", 1) != 0;
    ShowDesktopIcons = Config.GetInt("DesktopIcons", 1) != 0;
    if (Dock && !Locked)
        NtUserShowWindow(Dock->Handle(), ShowTopDock ? HZ_SW_SHOWNOACTIVATE : HZ_SW_HIDE);
    if (Desk && !Locked)
        NtUserShowWindow(Desk->Handle(), ShowDesktopIcons ? HZ_SW_SHOWNOACTIVATE : HZ_SW_HIDE);
    /* The colour scheme (shell.ini "Theme": 0 dark, 1 light). It is switched before the desktop,
       the lock screen and the flyouts are built, so they are painted in the chosen scheme; the
       taskbar and the dock, which already exist, are re-filled by ReloadChrome(). */
    LightMode = Config.GetInt("Theme", 0) == 1;
    Theme::ApplyScheme(LightMode ? Theme::Scheme::Light : Theme::Scheme::Dark);
    ReloadChrome();
    HzPrint("shell: settings %s: user '%s', wallpaper %u%s%s%s, lock wallpaper %u, dock %s, desktop icons %s, %s mode",
            HZ_SUCCESS(Status) ? "loaded" : "not found", DisplayName(), Wallpaper,
            HasPassword() ? ", password" : "", FirstBoot ? ", first boot" : "",
            Persistent ? "" : " (live system: not saved)", LockWallpaper, ShowTopDock ? "on" : "off",
            ShowDesktopIcons ? "on" : "off", LightMode ? "light" : "dark");
}

void Shell::SaveSettings()
{
    if (!Persistent)
        return;
    Config.SetInt("Wallpaper", (int)Wallpaper);
    Config.SetInt("LockWallpaper", (int)LockWallpaper);
    /* The shell's own appearance, so the Control Center's switches survive a restart. */
    Config.SetInt("Theme", LightMode ? 1 : 0);
    Config.SetInt("TopDock", ShowTopDock ? 1 : 0);
    Config.SetInt("DesktopIcons", ShowDesktopIcons ? 1 : 0);
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

/* Re-fills everything that captured the palette when it was built. The Control Center holds the
   fills of the scheme it was created in, so it is closed and built again (which also replays its
   reveal); the desktop draws with the live palette, so a repaint is enough. */
void Shell::ReloadChrome()
{
    if (Bar) {
        Bar->ApplyTheme();
        Bar->Rebuild();
    }
    if (Dock)
        Dock->ApplyTheme();
    if (Desk)
        Desk->InvalidateRect(Desk->ClientRect());
    if (Quick) {
        static_cast<Flyout *>(Quick)->Dismiss();
        if (!CreateQuickSettings())
            HzPrint("shell: cannot reopen the Control Center");
    }
}

void Shell::SetLightMode(bool Light)
{
    if (Light == LightMode)
        return;
    LightMode = Light;
    Theme::ApplyScheme(Light ? Theme::Scheme::Light : Theme::Scheme::Dark);
    ReloadChrome();
    SaveSettings();
    HzPrint("shell: %s colour scheme", Light ? "light" : "dark");
}

void Shell::SetTopDock(bool Show)
{
    if (Show == ShowTopDock)
        return;
    ShowTopDock = Show;
    if (Dock && !Locked)
        NtUserShowWindow(Dock->Handle(), Show ? HZ_SW_SHOWNOACTIVATE : HZ_SW_HIDE);
    SaveSettings();
    HzPrint("shell: top dock %s", Show ? "shown" : "hidden");
}

void Shell::SetDesktopIcons(bool Show)
{
    if (Show == ShowDesktopIcons)
        return;
    ShowDesktopIcons = Show;
    if (Desk && !Locked)
        NtUserShowWindow(Desk->Handle(), Show ? HZ_SW_SHOWNOACTIVATE : HZ_SW_HIDE);
    SaveSettings();
    HzPrint("shell: desktop icons %s", Show ? "shown" : "hidden");
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
    if (Index > HZ_MAX_WALLPAPER)
        return;
    if (!Locked || !LockWallpaperApplied) {
        STATUS Status = NtUserSetWallpaper(Index);
        if (!HZ_SUCCESS(Status)) {
            HzPrint("shell: cannot set desktop wallpaper %u (0x%08X)", Index, (uint32_t)Status);
            return;
        }
    }
    Wallpaper = Index;
    if (Quick && Quick->Content())
        Quick->Content()->Invalidate();
    SaveSettings();
}

void Shell::SetLockWallpaper(uint32_t Index)
{
    if (Index > HZ_MAX_WALLPAPER)
        return;
    if (Locked && LockWallpaperApplied) {
        STATUS Status = NtUserSetWallpaper(Index);
        if (!HZ_SUCCESS(Status)) {
            HzPrint("shell: cannot set lock-screen wallpaper %u (0x%08X)", Index, (uint32_t)Status);
            return;
        }
    }
    LockWallpaper = Index;
    SaveSettings();
    HzPrint("shell: lock-screen wallpaper %u", LockWallpaper);
}

bool Shell::ShowLockWallpaper()
{
    STATUS Status = NtUserSetWallpaper(LockWallpaper);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("shell: cannot apply lock-screen wallpaper %u (0x%08X)", LockWallpaper, (uint32_t)Status);
        LockWallpaperApplied = false;
        return false;
    }
    LockWallpaperApplied = true;
    return true;
}

void Shell::RestoreDesktopWallpaper()
{
    if (!LockWallpaperApplied)
        return;
    STATUS Status = NtUserSetWallpaper(Wallpaper);
    if (!HZ_SUCCESS(Status)) {
        HzPrint("shell: cannot restore desktop wallpaper %u (0x%08X)", Wallpaper, (uint32_t)Status);
        return;
    }
    LockWallpaperApplied = false;
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
    } else if (Request == HZ_SHELL_REQUEST_LOCK_WALLPAPER) {
        if (Detail > HZ_MAX_WALLPAPER)
            return;
        SetLockWallpaper((uint32_t)Detail);
    } else if (Request == HZ_SHELL_REQUEST_SETTINGS) {
        bool WasFirstBoot = FirstBoot;
        LoadSettings();
        if (Desk)
            Desk->RefreshPrefs(); /* Settings may have changed the icon size or the sort order */
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
    if (Dismissed_)
        return;
    Dismissed_ = true;
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

/* A manifest icon name (or a semantic alias) to a glyph. Every unknown or missing name falls back
   to the default application icon, so an app without an "Icon=" line still gets a real icon tile
   instead of a bare letter. */
Icon IconByName(const hz::String &Name)
{
    static const struct {
        const char *Name;
        Icon Glyph;
    } Table[] = {
        {"Home", Icon::Home},         {"Clock", Icon::Clock},     {"Settings", Icon::Settings}, {"Star", Icon::Star},
        {"Sun", Icon::Sun},           {"Moon", Icon::Moon},       {"Info", Icon::Info},         {"Search", Icon::Search},
        {"Folder", Icon::Folder},     {"Document", Icon::Document}, {"Calendar", Icon::Calendar},
        {"Drive", Icon::Drive},       {"Prompt", Icon::Prompt},     {"Calculator", Icon::Calculator},
        {"Chart", Icon::Chart},       {"Pencil", Icon::Pencil},     {"Palette", Icon::Palette},
        {"Stopwatch", Icon::Stopwatch}, {"Globe", Icon::Globe},     {"Trash", Icon::Trash},
        {"Save", Icon::Save},         {"Cube", Icon::Cube},         {"Open", Icon::Open},       {"Volume", Icon::Volume},
        {"Power", Icon::Power},       {"Nexora", Icon::Nexora},     {"Wifi", Icon::Wifi},       {"Network", Icon::Wifi},
        {"Accessibility", Icon::Accessibility}, {"AppWindow", Icon::AppWindow}, {"Check", Icon::Check},
        {"ChevronDown", Icon::ChevronDown}, {"ChevronUp", Icon::ChevronUp}, {"ChevronLeft", Icon::ChevronLeft},
        {"ChevronRight", Icon::ChevronRight}, {"Close", Icon::Close}, {"Add", Icon::Add}, {"Remove", Icon::Remove},
        {"More", Icon::More}, {"Leaf", Icon::Leaf}, {"Plane", Icon::Plane}, {"Refresh", Icon::Refresh},
        {"Cut", Icon::Cut}, {"Copy", Icon::Copy}, {"Paste", Icon::Paste}, {"Person", Icon::Person},
        {"ArrowUp", Icon::ArrowUp},
        /* Aliases: the same glyph under the words a manifest is likely to use. */
        {"Files", Icon::Folder},      {"FolderExplorer", Icon::Folder}, {"Text", Icon::Document},
        {"Editor", Icon::Document},   {"Terminal", Icon::Prompt},       {"Console", Icon::Prompt},
        {"Blockyard", Icon::Cube},    {"Block", Icon::Cube},            {"Sound", Icon::Volume},
        {"Audio", Icon::Volume},      {"Speaker", Icon::Volume},        {"Battery", Icon::Power},
        {"Image", Icon::Palette},     {"Photo", Icon::Palette},         {"Timer", Icon::Stopwatch},
        {"Web", Icon::Globe},         {"Browser", Icon::Globe},         {"Wireless", Icon::Wifi},
        {"Apps", Icon::AppWindow},    {"Window", Icon::AppWindow},      {"Gear", Icon::Settings},
        {"Wallpaper", Icon::Sun},     {"Theme", Icon::Palette},
    };
    for (const auto &E : Table)
        if (hz::fs::EqualNames(E.Name, Name.CStr()))
            return E.Glyph;
    return Icon::AppWindow; /* the default app icon */
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

/* A Nexora sign-in surface with a focused password field, account label and lock-screen controls. */
class LockView final : public Element {
public:
    LockView()
    {
        Big_ = HzGetFont(HZ_FONT_DISPLAY);
        Medium_ = HzGetFont(HZ_FONT_BODY_LARGE);
        Prompting_ = g_Shell.HasPassword();
    }

    TextBox *Box = nullptr;
    bool Prompting() const { return Prompting_; }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        float T = Leave_.Value();
        int32_t Shift = (int32_t)(T * B.Height * 0.07f);
        float Fade = 1 - T;
        uint32_t TopShade = HighContrast_ ? 0x74000000 : 0x35000000;
        uint32_t BottomShade = HighContrast_ ? 0xB8000000 : 0x71000000;
        P.FillVerticalGradient(Rect{B.X, B.Y, B.Width, B.Height / 2}, 0, ScaleAlpha(TopShade, Fade),
                               ScaleAlpha(0x08000000, Fade));
        P.FillVerticalGradient(Rect{B.X, B.Y + B.Height / 3, B.Width, B.Height - B.Height / 3}, 0,
                               ScaleAlpha(0x08000000, Fade), ScaleAlpha(BottomShade, Fade));
        char Time[16], Date[64];
        FormatClock(Time, sizeof(Time), Date, sizeof(Date));
        if (!g_Shell.HasPassword()) {
            int32_t Y = B.Y + B.Height * 12 / 100 - Shift;
            if (Big_) {
                int32_t W = HzMeasureText(Big_, Time);
                P.Text(Big_, B.X + (B.Width - W) / 2, Y + HzFontAscent(Big_), Time,
                       ScaleAlpha(0xFFFFFFFF, Fade));
                Y += HzFontLineHeight(Big_);
            }
            if (Medium_) {
                int32_t W = HzMeasureText(Medium_, Date);
                P.Text(Medium_, B.X + (B.Width - W) / 2, Y + HzFontAscent(Medium_), Date,
                       ScaleAlpha(0xFFFFFFFF, Fade));
            }
            if (Prompting_) {
                hz::String Hello = hz::String::Format("Hello, %s", g_Shell.DisplayName());
                P.TextLine(GetFont(FontRole::Subtitle),
                           Rect{B.X, B.Bottom() - 112 - Shift, B.Width, 32}, Hello.CStr(),
                           ScaleAlpha(0xFFFFFFFF, Fade), Align::Center);
                P.TextLine(GetFont(FontRole::Body),
                           Rect{B.X, B.Bottom() - 72 - Shift, B.Width, 24},
                           "Press any key or click to unlock", ScaleAlpha(Theme::TextSecondary, Fade),
                           Align::Center);
            }
            return;
        }
        int32_t Y = B.Y + 48 - Shift;
        if (Big_) {
            P.Text(Big_, B.X + 40, Y + HzFontAscent(Big_), Time, ScaleAlpha(0xFFFFFFFF, Fade));
            Y += HzFontLineHeight(Big_) + 3;
        }
        if (Medium_) {
            P.Text(Medium_, B.X + 42, Y + HzFontAscent(Medium_), Date, ScaleAlpha(0xF0FFFFFF, Fade));
        }
        int32_t CenterX = B.X + B.Width / 2;
        int32_t CenterY = B.Y + B.Height * 46 / 100 - Shift;
        P.FillRoundRect(Rect{CenterX - 190, CenterY - 162, 380, 320}, 14, 0x62424E60);
        P.StrokeRoundRect(Rect{CenterX - 190, CenterY - 162, 380, 320}, 14, 1, 0x50FFFFFF);
        P.FillCircle((float)CenterX, (float)CenterY - 105, 40, ScaleAlpha(0xB82A3544, Fade));
        P.DrawIcon(Icon::Person, Rect{CenterX - 25, CenterY - 130, 50, 50}, ScaleAlpha(0xFFFFFFFF, Fade), 1.8f);
        P.TextLine(GetFont(FontRole::Subtitle), Rect{B.X + 16, CenterY - 58, B.Width - 32, 34},
                   g_Shell.DisplayName(), ScaleAlpha(0xFFFFFFFF, Fade), Align::Center);
        const char *Hint = Wrong_ ? "That password did not match. Try again."
                           : Prompting_ ? "Enter your password to sign in"
                                        : "Press any key or click to continue";
        P.TextLine(GetFont(FontRole::Body), Rect{B.X + 16, CenterY - 22, B.Width - 32, 24}, Hint,
                   ScaleAlpha(Wrong_ ? 0xFFFFA18F : 0xE8FFFFFF, Fade), Align::Center);
        if (!Prompting_) {
            P.FillRoundRect(Rect{CenterX - 58, CenterY + 18, 116, 34}, 5, ScaleAlpha(0xCCFFFFFF, Fade));
            P.TextLine(GetFont(FontRole::BodyStrong), Rect{CenterX - 58, CenterY + 18, 116, 34},
                       "Continue", ScaleAlpha(0xFF18202A, Fade), Align::Center);
        }
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
        Leave_.To(1, Motion::LockScreenExitMs);
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

    void ToggleHighContrast()
    {
        HighContrast_ = !HighContrast_;
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
    bool HighContrast_ = false;
    uint32_t Failures_ = 0;
};

enum class LockControl : uint8_t { Network, Accessibility, Power };

class LockControlButton final : public Element {
public:
    LockControlButton(Icon Glyph, const char *Label, LockControl Action, LockView *View)
        : Glyph_(Glyph), Action_(Action), View_(View)
    {
        Width = Height = 48;
        Focusable = true;
        ToolTip = Label;
    }

    void Paint(Painter &P) override
    {
        Color Fill = IsPressed() ? 0x806A7A8E : IsHovered() ? 0x70586779 : 0x50374455;
        P.FillRoundRect(Bounds(), 7, Fill);
        P.StrokeRoundRect(Bounds(), 7, 1, 0x70FFFFFF);
        P.DrawIcon(Glyph_, Rect{Bounds().X + 15, Bounds().Y + 15, 18, 18}, 0xFFFFFFFF, 1.45f);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds(), 7);
    }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }

    void OnPointerUp(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT && Bounds().Contains(E.Position))
            Activate();
    }

    void OnKeyDown(KeyEvent &E) override
    {
        if (E.Key == HZ_VK_RETURN || E.Key == HZ_VK_SPACE) {
            E.Handled = true;
            Activate();
        }
    }

private:
    void Activate()
    {
        if (Action_ == LockControl::Accessibility) {
            View_->ToggleHighContrast();
            return;
        }
        if (Action_ == LockControl::Power) {
            MessageBox(GetWindow(), "Power", "Choose a power option.", "Shut down|Restart|Cancel", [](int Choice) {
                if (Choice == 0)
                    g_Shell.RequestShutdown(HZ_SHUTDOWN_POWER_OFF);
                else if (Choice == 1)
                    g_Shell.RequestShutdown(HZ_SHUTDOWN_REBOOT);
            });
            return;
        }

        HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION Interfaces[HZ_MAX_NETWORK_INTERFACES] = {};
        uint32_t Returned = 0;
        STATUS Status = NtQuerySystemInformation(HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION_CLASS,
                                                 Interfaces, sizeof(Interfaces), &Returned);
        hz::String Text;
        if (!HZ_SUCCESS(Status)) {
            Text = hz::String::Format("Network status is unavailable (0x%08X).", (uint32_t)Status);
        } else if (Returned > sizeof(Interfaces) || Returned % sizeof(Interfaces[0])) {
            Text = "The network status response was malformed.";
        } else {
            size_t Count = Returned / sizeof(Interfaces[0]);
            if (!Count)
                Text = "No network adapters were detected.";
            for (size_t i = 0; i < Count; i++) {
                const HZ_SYSTEM_NETWORK_INTERFACE_INFORMATION &Interface = Interfaces[i];
                if (!Text.Empty())
                    Text.Append("\n");
                Text.Append(hz::String::Format("%s: %s%s", Interface.Name,
                                               Interface.LinkUp ? "connected" : "disconnected",
                                               Interface.ConfigurationState == 2 ? " (configured)" : "")
                               .CStr());
            }
        }
        MessageBox(GetWindow(), "Network", Text.CStr(), "Close", nullptr);
    }

    Icon Glyph_;
    LockControl Action_;
    LockView *View_;
};

class LockAccountChip final : public Element {
public:
    LockAccountChip()
    {
        Width = 220;
        Height = 52;
        HitTestVisible = false;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        P.FillRoundRect(B, 7, 0x52374455);
        P.FillCircle(B.X + 26.0f, B.Y + 26.0f, 17, 0xB82A3544);
        P.DrawIcon(Icon::Person, Rect{B.X + 15, B.Y + 15, 22, 22}, 0xFFFFFFFF, 1.4f);
        P.TextLine(GetFont(FontRole::BodyStrong), Rect{B.X + 54, B.Y + 7, B.Width - 62, 20},
                   g_Shell.DisplayName(), Theme::TextPrimary);
        P.TextLine(GetFont(FontRole::Caption), Rect{B.X + 54, B.Y + 27, B.Width - 62, 17},
                   "Active account", Theme::TextSecondary);
    }
};

}  // namespace

Window *CreateLockScreen()
{
    const Rect &S = g_Shell.Screen;
    Window *Lock = new Window("Lock", S.Width, S.Height,
                              HZ_WS_POPUP | HZ_WS_TOPMOST |
                                  (g_Shell.HasPassword() ? HZ_WS_MICA : HZ_WS_TRANSPARENT) |
                                  HZ_WS_NOFRAME | HZ_WS_VISIBLE);
    Lock->Background = 0x00000000;
    Grid *Root = new Grid();
    LockView *View = Root->Add<LockView>();
    TextBox *Box = Root->Add<TextBox>("Password");
    Box->Password = true;
    Box->MaxLength = 64;
    Box->Width = 320;
    Box->Height = 42;
    Box->HorizontalAlignment = Align::Center;
    Box->VerticalAlignment = Align::Center;
    Box->Margin = Thickness(0, 54, 0, 0);
    Box->SetVisible(View->Prompting());
    Box->OnSubmit = [View](const hz::String &Typed) { View->Submit(Typed); };
    View->Box = Box;
    LockAccountChip *Account = Root->Add<LockAccountChip>();
    Account->SetVisible(g_Shell.HasPassword());
    Account->HorizontalAlignment = Align::Start;
    Account->VerticalAlignment = Align::End;
    Account->Margin = Thickness(24, 0, 0, 22);
    StackPanel *Controls = Root->Add<StackPanel>(Orientation::Horizontal, 8);
    Controls->Width = 160;
    Controls->Height = 48;
    Controls->HorizontalAlignment = Align::End;
    Controls->VerticalAlignment = Align::End;
    Controls->Margin = Thickness(0, 0, 24, 24);
    Controls->Add<LockControlButton>(Icon::Wifi, "Network status", LockControl::Network, View);
    Controls->Add<LockControlButton>(Icon::Accessibility, "High contrast", LockControl::Accessibility, View);
    Controls->Add<LockControlButton>(Icon::Power, "Power options", LockControl::Power, View);
    Controls->SetVisible(g_Shell.HasPassword());
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
    if (View->Prompting())
        Lock->SetFocus(Box);
    Lock->SetTimer(1000, [View]() { View->Invalidate(); });
    return Lock;
}

void Shell::Unlock()
{
    if (!Locked)
        return;
    RestoreDesktopWallpaper();
    Locked = false;
    if (Lock) {
        Window *L = Lock;
        Lock = nullptr;
        L->Close();
        DeferDelete(L);
    }
    if (Bar)
        NtUserShowWindow(Bar->Handle(), HZ_SW_SHOWNOACTIVATE); /* hidden while locked */
    if (Dock && ShowTopDock)
        NtUserShowWindow(Dock->Handle(), HZ_SW_SHOWNOACTIVATE);
    if (Desk && ShowDesktopIcons) { /* hidden while locked; re-read so the icons replay their reveal */
        NtUserShowWindow(Desk->Handle(), HZ_SW_SHOWNOACTIVATE);
        Desk->Reload();
    }
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
    ShowLockWallpaper();
    Lock = CreateLockScreen();
    if (!Lock) {
        RestoreDesktopWallpaper();
        Locked = false;
        return;
    }
    if (Bar)
        NtUserShowWindow(Bar->Handle(), HZ_SW_HIDE);
    if (Dock)
        NtUserShowWindow(Dock->Handle(), HZ_SW_HIDE);
    if (Desk)
        NtUserShowWindow(Desk->Handle(), HZ_SW_HIDE);
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
    g_Shell.Work = Rect{0, kWorkAreaTop, (int32_t)Display.Width,
                        (int32_t)Display.Height - kWorkAreaTop - kTaskbarHeight};

    Taskbar *Bar = new Taskbar(g_Shell.Screen.Width);
    if (!Bar->ShowAt(0, g_Shell.Screen.Height - kTaskbarHeight)) {
        HzPrint("shell: cannot create the taskbar");
        return 1;
    }
    TopDock *Dock = new TopDock(g_Shell.Screen.Width);
    if (!Dock->ShowAt(20, kTopDockTop)) {
        HzPrint("shell: cannot create the top dock");
        return 1;
    }
    NtUserShowWindow(Dock->Handle(), HZ_SW_HIDE);
    STATUS Status = NtUserRegisterShell(Bar->Handle());
    if (!HZ_SUCCESS(Status)) {
        HzPrint("shell: another shell is running (0x%08X)", (uint32_t)Status);
        return 1;
    }
    HZ_RECT Work = g_Shell.Work.ToHz();
    NtUserSetWorkArea(&Work);
    g_Shell.Bar = Bar;
    g_Shell.Dock = Dock;
    g_Shell.RefreshWindows();

    g_Shell.LoadSettings(); /* after registering: the wallpaper may change */
    /* The desktop icon layer: at the very back of the z-order (HZ_WS_DESKTOP), so the wallpaper
       and the icons sit under every window. It is hidden while the lock screen is up, whose scrim
       is translucent and would otherwise let the icons show through. */
    g_Shell.Desk = CreateDesktop();
    if (g_Shell.Desk)
        NtUserShowWindow(g_Shell.Desk->Handle(), HZ_SW_HIDE);
    /* Restarted by the session after it exited (M11): no lock screen, no Welcome. */
    bool Restarted = argc > 1 && strcmp(argv[1], "restart") == 0;
    if (Restarted) {
        HzPrint("shell: restarted");
        g_Shell.Started = true;
        g_Shell.Locked = false;
        if (g_Shell.HasPassword()) { /* ending the shell must not get past the password */
            g_Shell.Locked = true;
            g_Shell.ShowLockWallpaper();
            g_Shell.Lock = CreateLockScreen();
            HzPrint("shell: lock screen shown");
        }
        if (!g_Shell.Locked || !g_Shell.Lock) {
            g_Shell.Locked = false;
            NtUserShowWindow(Bar->Handle(), HZ_SW_SHOWNOACTIVATE);
            if (g_Shell.ShowTopDock)
                NtUserShowWindow(Dock->Handle(), HZ_SW_SHOWNOACTIVATE);
            if (g_Shell.Desk && g_Shell.ShowDesktopIcons) {
                NtUserShowWindow(g_Shell.Desk->Handle(), HZ_SW_SHOWNOACTIVATE);
                g_Shell.Desk->Reload();
            }
        }
        HzPrint("shell: ready");
        return RunMessageLoop();
    }
    if (!g_Shell.FirstBoot)
        g_Shell.ShowLockWallpaper();
    g_Shell.Lock = g_Shell.FirstBoot ? nullptr : CreateLockScreen();
    if (g_Shell.Lock)
        HzPrint("shell: lock screen shown");
    else
        g_Shell.Unlock();
    HzPrint("shell: ready");
    return RunMessageLoop();
}
