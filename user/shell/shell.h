/*
 * Nexora OS - user/shell/shell.h
 * The shell (shell.exe): started by the kernel once the desktop is shown. It registers its
 * taskbar window with win32k as the shell window, reserves the bottom strip of the screen (the
 * work area), shows the lock screen, and then runs the taskbar, the Start menu, quick settings and
 * the Alt+Tab switcher. win32k tells it about app windows of other processes (HZ_WM_SHELLNOTIFY)
 * and hands it the logo key and Alt+Tab; programs ask it for changes (HZ_WM_SHELLREQUEST).
 * Installed programs come from their manifests (C:\Nexora\Apps\*.app, see hzapps.h).
 */
#ifndef SHELL_H
#define SHELL_H

#include "hzapps.h"
#include "hzfile.h"
#include "hzui.h"

namespace shell {

using namespace hzui;

constexpr int32_t kTaskbarHeight = 48;
constexpr size_t kMaxRecent = 4;

struct AppWindow {
    HWND Handle = 0;
    hz::String Title;
    bool Minimized = false;
    uint32_t ProcessId = 0;
    hz::String AppId; /* the program's manifest id, "" if it has none */
};

class Taskbar;

struct Shell {
    Rect Screen, Work;
    Taskbar *Bar = nullptr;
    Window *Lock = nullptr;
    Window *Start = nullptr;
    Window *Quick = nullptr;
    Window *Switcher = nullptr;
    hz::Vector<hz::apps::App> Apps;  /* from the manifests, by Order */
    hz::Vector<hz::String> Recent;   /* app ids, most recent first */
    hz::Vector<hz::String> TaskbarPins; /* app ids, independent of Start's manifest pins */
    hz::Vector<AppWindow> Windows;   /* taskbar order: first seen first */
    HWND Active = 0;
    bool Locked = true;
    /* Quick settings state (visual only, except the wallpaper) */
    uint32_t Wallpaper = 0;
    uint32_t LockWallpaper = HZ_LOCK_WALLPAPER_DEFAULT;
    bool LockWallpaperApplied = false;
    bool NightLight = false, Focus = false, EnergySaver = false, Airplane = false;
    float Brightness = 80, Volume = 60;
    /* The switcher */
    hz::Vector<HWND> SwitchList;
    size_t SwitchIndex = 0;
    /* Settings (M10): C:\Nexora\Settings\shell.ini; saved only on an installed system. */
    hz::fs::Settings Config;
    hz::String UserName;  /* "" = Guest */
    hz::String PasswordSalt, PasswordHash; /* M11: no hash = no password (hzcrypto.h) */
    int UtcOffset = 0;                     /* M11: minutes; the clocks show local time */
    bool FirstBoot = false;
    bool Persistent = false;
    bool Started = false; /* unlocked once: Welcome or first-boot setup ran */
    /* Shutting down (M11): windows were asked to close; the timer checks until they are gone. */
    bool ShuttingDown = false, ShutdownAsked = false;
    uint32_t ShutdownAction = 0;
    uint64_t ShutdownStartUs = 0, ShutdownTimer = 0;

    void LoadApps();
    void LoadSettings();
    void SaveSettings();
    void SetWallpaper(uint32_t Index); /* applies, remembers and saves */
    void SetLockWallpaper(uint32_t Index);
    bool ShowLockWallpaper();
    void RestoreDesktopWallpaper();
    const char *DisplayName() const { return UserName.Empty() ? "Guest" : UserName.CStr(); }
    const hz::apps::App *FindApp(const hz::String &Id) const;
    const hz::apps::App *AppForWindow(HWND Window) const;
    void NoteRecent(const hz::String &Id);
    bool IsTaskbarPinned(const hz::String &Id) const;
    void ToggleTaskbarPin(const hz::String &Id);
    void OnShellEvent(uint64_t Event, uint64_t Detail);
    void OnShellRequest(uint64_t Request, uint64_t Detail);
    void RefreshWindows();
    const AppWindow *Find(HWND Window) const;
    hz::String TitleOf(HWND Window) const;
    void Launch(const hz::apps::App &App);
    void ActivateWindow(HWND Window);   /* restores if minimized */
    void TaskbarClick(HWND Window);     /* activate, or minimize the active one */
    void ToggleStart();
    void ToggleQuick();
    void CloseFlyouts();
    void SwitchNext(bool Back);
    void SwitchEnd(bool Commit);
    void Unlock();
    void LockNow();
    /* Start > Power: asks every program window to close (programs may ask to save), waits for
       them, offers "Shut down anyway" after kShutdownWaitMs, then flushes and powers off or restarts. */
    void RequestShutdown(uint32_t Action);
    void ShutdownTick();
    void FinishShutdown();
    bool HasPassword() const { return !PasswordHash.Empty(); }
};

extern Shell g_Shell;

/* A window that closes itself when it loses activation (Start, quick settings). */
class Flyout : public Window {
public:
    Flyout(const char *Title, int32_t Width, int32_t Height, Window **Slot);
    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override;
    void Dismiss();

private:
    Window **Slot_;
};

/* The Nexora OS logo (Start) and program icons: the manifest's glyph on its coloured tile, or a
   letter tile for programs without a manifest. */
void PaintLogo(Painter &P, const Rect &Box);
void PaintAppIcon(Painter &P, const Rect &Box, const hz::apps::App *App, const char *Title);
Icon IconByName(const hz::String &Name);
void ShowAppContextMenu(Window *Owner, const hz::apps::App &App, Point Screen, HWND Window = 0);

Window *CreateLockScreen();
Window *CreateStartMenu();
Window *CreateQuickSettings();
Window *CreateSwitcher();

}  // namespace shell

#endif /* SHELL_H */
