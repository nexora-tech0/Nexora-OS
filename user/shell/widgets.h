/*
 * Nexora OS - user/shell/widgets.h
 * Apple-inspired widgets for the desktop and lock screen.
 */
#ifndef SHELL_WIDGETS_H
#define SHELL_WIDGETS_H

#include "hzui.h"

namespace shell {

using namespace hzui;

enum class WidgetSize {
    Small,  /* 2x2 grid units */
    Medium, /* 4x2 grid units */
    Large   /* 4x4 grid units */
};

class Widget : public Element {
public:
    Widget(WidgetSize Size) : Size_(Size) {
        Padding = Thickness(12);
    }

    WidgetSize GetSize() const { return Size_; }
    
    void Paint(Painter& P) override {
        Rect R = Bounds();
        /* Apple-style glassy background */
        P.FillRoundRect(R, 20.0f, 0x25FFFFFF);
        P.StrokeRoundRect(R, 20.0f, 0x15FFFFFF, 1.0f);
    }

protected:
    WidgetSize Size_;
};

/* ---- Specific Widgets ----------------------------------------------------------------------- */

class CPUWidget final : public Widget {
public:
    CPUWidget();
    void Paint(Painter& P) override;
    bool Animate() override;
private:
    uint64_t LastIdle_ = 0, LastUptime_ = 0;
    float Usage_ = 0.0f;
};

class RAMWidget final : public Widget {
public:
    RAMWidget();
    void Paint(Painter& P) override;
    bool Animate() override;
};

class CalendarWidget final : public Widget {
public:
    CalendarWidget();
    void Paint(Painter& P) override;
};

class BatteryWidget final : public Widget {
public:
    BatteryWidget();
    void Paint(Painter& P) override;
};

class ClockWidget final : public Widget {
public:
    ClockWidget();
    void Paint(Painter& P) override;
    bool Animate() override;
};

/* ---- Widget Host --------------------------------------------------------------------------- */

class WidgetHost final : public Element {
public:
    WidgetHost();
    void AddWidget(Widget* W);
    void RemoveWidget(Widget* W);
    void LayoutWidgets();

    void Paint(Painter& P) override;
    void OnPointerDown(PointerEvent& E) override;

private:
    hz::Vector<Widget*> Widgets_;
    bool Editing_ = false;
};

} // namespace shell

#endif /* SHELL_WIDGETS_H */
