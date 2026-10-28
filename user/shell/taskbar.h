/*
 * Nexora OS - user/shell/taskbar.h
 * The top dock and bottom taskbar are topmost, never-activated shell surfaces. The dock floats
 * inside the screen edges; the taskbar is the window win32k sends shell notifications to.
 */
#ifndef SHELL_TASKBAR_H
#define SHELL_TASKBAR_H

#include "shell.h"

namespace shell {

void ShowNotification(const char *Title, const char *Message, Icon Glyph = Icon::Info);

class Taskbar final : public Window {
public:
    explicit Taskbar(int32_t ScreenWidth);
    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override;
    void Rebuild(); /* window buttons from g_Shell.Windows */
    void Refresh(); /* active indicators */
    void ApplyTheme(); /* refreshes the solid dock surface after a colour-scheme switch */
    void Reveal(); /* slide up from the bottom */

protected:
    void OnContextMenu(Point Screen) override;
    void OnPaint(Painter &P) override;

private:
    Grid *Surface_ = nullptr;
    Element *Start_ = nullptr;
    StackPanel *Apps_;
    uint64_t RevealTimer_ = 0;
    int32_t ScreenWidth_;
    Transition Reveal_;
};

class TopDock final : public Window {
public:
    explicit TopDock(int32_t ScreenWidth);
    void ApplyTheme(); /* refreshes the opaque dock surface and tray after a colour-scheme switch */
    void Reveal(); /* slide down from the top */

protected:
    void OnPaint(Painter &P) override;

private:
    Grid *Surface_ = nullptr;
    StackPanel *Brand_ = nullptr;
    StackPanel *Tray_ = nullptr;
    TextBlock *Name_ = nullptr;
    Element *Clock_;
    uint64_t ClockTimer_ = 0;
    uint64_t RevealTimer_ = 0;
    Transition Reveal_;
};

}  // namespace shell

#endif /* SHELL_TASKBAR_H */
