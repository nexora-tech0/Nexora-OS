/*
 * Nexora OS - user/shell/widgets.h
 * WidgetKit-shaped widgets (small / medium / large, 22 px corners) for the desktop and the
 * Windows 11 lock screen. Battery and GPU report "Unavailable" until those sensors exist.
 */
#ifndef SHELL_WIDGETS_H
#define SHELL_WIDGETS_H

#include "hzui.h"

namespace shell {

using namespace hzui;

enum class WidgetSize : uint8_t {
    Small,  /* 164 x 156 */
    Medium, /* 300 x 168 */
    Large   /* 320 x 280 */
};

enum class WidgetSurface : uint8_t { Desktop, Lock };

int32_t WidgetPixelWidth(WidgetSize Size);
int32_t WidgetPixelHeight(WidgetSize Size);
WidgetSize NextWidgetSize(WidgetSize Size);

class WidgetHost;

class Widget : public Element {
public:
    Widget(WidgetSize Size, const char *Id, const char *Title, Icon Glyph);
    WidgetSize GetSize() const { return Size_; }
    void SetSizeKind(WidgetSize Size);
    const char *Id() const { return Id_.CStr(); }
    const char *Title() const { return Title_.CStr(); }
    void SetFrame(const Rect &Frame);
    void SetEditing(bool Editing);
    bool Editing() const { return Editing_; }
    Rect RemoveButtonRect() const;
    Rect SizeHandleRect() const;

    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;

protected:
    Rect Body() const;
    WidgetSize Size_;

private:
    friend class WidgetHost;
    hz::String Id_, Title_;
    Icon Glyph_;
    Rect Frame_;
    bool Editing_ = false;
};

class CPUWidget final : public Widget {
public:
    CPUWidget();
    void Paint(Painter &P) override;
    bool Animate() override;

private:
    uint64_t LastIdle_ = 0, LastUptime_ = 0;
    float Usage_ = 0.0f;
    float History_[24] = {};
    int HistoryCount_ = 0;
};

class RAMWidget final : public Widget {
public:
    RAMWidget();
    void Paint(Painter &P) override;
};

class CalendarWidget final : public Widget {
public:
    CalendarWidget();
    void Paint(Painter &P) override;
};

class BatteryWidget final : public Widget {
public:
    BatteryWidget();
    void Paint(Painter &P) override;
};

class GPUWidget final : public Widget {
public:
    GPUWidget();
    void Paint(Painter &P) override;
};

class ClockWidget final : public Widget {
public:
    ClockWidget();
    void Paint(Painter &P) override;
    bool Animate() override;
};

class WidgetHost final : public Element {
public:
    explicit WidgetHost(WidgetSurface Surface = WidgetSurface::Desktop);
    WidgetSurface Surface() const { return Surface_; }
    void AddWidget(Widget *W, bool PlaceInFreeArea = false);
    void RemoveWidget(Widget *W);
    void RefreshPrefs();

    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void ArrangeOverride(const Rect &Content) override;
    Element *HitTest(Point P) override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerMove(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;

private:
    void StartRefreshTimer();
    void SaveWidgetPrefs();
    void ShowGallery(Point Screen);
    Widget *MakeWidget(const char *Id) const;
    Widget *FindWidget(const char *Id) const;
    Widget *WidgetAt(Point Position) const;
    Rect PlusRect() const;
    const char *KeyPrefix() const { return Surface_ == WidgetSurface::Lock ? "LockWidget" : "Widget"; }
    int DefaultEnabled(const char *Id) const;

    WidgetSurface Surface_;
    hz::Vector<Widget *> Widgets_;
    bool Editing_ = false;
    Widget *ActiveWidget_ = nullptr;
    Point DragOffset_;
    uint64_t RefreshTimer_ = 0;
};

} /* namespace shell */

#endif /* SHELL_WIDGETS_H */
