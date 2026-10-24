/*
 * Nexora OS - user/shell/taskbar.h
 * The top dock and bottom taskbar are frameless, topmost, never-activated Mica surfaces. The
 * taskbar is the window win32k sends shell notifications to.
 */
#ifndef SHELL_TASKBAR_H
#define SHELL_TASKBAR_H

#include "shell.h"

namespace shell {

class Taskbar final : public Window {
public:
    explicit Taskbar(int32_t ScreenWidth);
    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override;
    void Rebuild(); /* window buttons from g_Shell.Windows */
    void Refresh(); /* active indicators */

protected:
    void OnPaint(Painter &P) override;
    void OnContextMenu(Point Screen) override;

private:
    StackPanel *Apps_;
    Element *Clock_;
    uint64_t ClockTimer_ = 0;
};

class TopDock final : public Window {
public:
    explicit TopDock(int32_t ScreenWidth);

protected:
    void OnPaint(Painter &P) override;

private:
    Element *Clock_;
    uint64_t ClockTimer_ = 0;
};

}  // namespace shell

#endif /* SHELL_TASKBAR_H */
