/*
 * Nexora OS - user/include/hzui.h
 * hzui: the C++ UI toolkit (user/lib/hzui, linked as hzui.lib) on top of hzuser.
 *
 * A Window owns one tree of Elements drawn into its surface (win32k knows top-level windows
 * only, see docs/ARCHITECTURE.md §8). Elements are laid out in two passes (Measure, then Arrange)
 * like WPF/WinUI, in window coordinates (the caption is above the client area). Painting is
 * damage-driven: an element that changes calls Invalidate(), the window repaints only that
 * rectangle (clipped), back to front. Pointer input goes to the deepest element under the
 * pointer, bubbling up until one handles it; the element that handles a press keeps receiving
 * the pointer until the release. Keyboard input goes to the focused element and bubbles up; Tab
 * and Shift+Tab move focus in tree order (the focus ring shows after keyboard navigation only).
 *
 * Popups (menus, tooltips, the combo box list) are separate top-level windows with
 * HZ_WS_POPUP | HZ_WS_TOPMOST | HZ_WS_NOACTIVATE; a menu holds the explicit mouse capture while it
 * is open and its owner window forwards keys to it.
 *
 * Everything runs on the thread that created the windows. Objects are owned by their parent
 * (elements) or by the program (windows); popups are owned by hzui.
 */
#ifndef HZUI_H
#define HZUI_H

#include "hzcore.h"
#include "hzuser.h"

namespace hzui {

using Color = uint32_t; /* 0xAARRGGBB, not premultiplied */

/* ---- Geometry ---------------------------------------------------------------------------------- */

struct Thickness {
    int32_t Left = 0, Top = 0, Right = 0, Bottom = 0;
    constexpr Thickness() = default;
    constexpr Thickness(int32_t All) : Left(All), Top(All), Right(All), Bottom(All) {}
    constexpr Thickness(int32_t Horizontal, int32_t Vertical)
        : Left(Horizontal), Top(Vertical), Right(Horizontal), Bottom(Vertical)
    {
    }
    constexpr Thickness(int32_t L, int32_t T, int32_t R, int32_t B) : Left(L), Top(T), Right(R), Bottom(B) {}
    constexpr int32_t Horizontal() const { return Left + Right; }
    constexpr int32_t Vertical() const { return Top + Bottom; }
};

struct Point {
    int32_t X = 0, Y = 0;
    bool operator==(const Point &O) const { return X == O.X && Y == O.Y; }
};

struct Size {
    int32_t Width = 0, Height = 0;
    bool operator==(const Size &O) const { return Width == O.Width && Height == O.Height; }
};

constexpr int32_t Infinite = 0x3FFFFFFF; /* "unconstrained" in layout */

struct Rect {
    int32_t X = 0, Y = 0, Width = 0, Height = 0;

    int32_t Right() const { return X + Width; }
    int32_t Bottom() const { return Y + Height; }
    bool Empty() const { return Width <= 0 || Height <= 0; }
    bool Contains(Point P) const { return P.X >= X && P.X < X + Width && P.Y >= Y && P.Y < Y + Height; }
    bool operator==(const Rect &O) const { return X == O.X && Y == O.Y && Width == O.Width && Height == O.Height; }
    bool operator!=(const Rect &O) const { return !(*this == O); }
    Rect Intersect(const Rect &O) const;
    Rect Union(const Rect &O) const;
    Rect Deflate(const Thickness &T) const;
    Rect Inflate(int32_t By) const { return Rect{X - By, Y - By, Width + 2 * By, Height + 2 * By}; }
    Rect Offset(int32_t Dx, int32_t Dy) const { return Rect{X + Dx, Y + Dy, Width, Height}; }
    HZ_RECT ToHz() const { return HZ_RECT{X, Y, X + Width, Y + Height}; }
    static Rect FromHz(const HZ_RECT &R) { return Rect{R.Left, R.Top, R.Right - R.Left, R.Bottom - R.Top}; }
};

enum class Align : uint8_t { Stretch, Start, Center, End };
enum class Orientation : uint8_t { Vertical, Horizontal };

namespace Motion {
constexpr uint32_t HoverEnterMs = 110;
constexpr uint32_t HoverLeaveMs = 140;
constexpr uint32_t ControlStateMs = 160;
constexpr uint32_t ScrollMs = 180;
constexpr uint32_t FocusExpandMs = 150;
constexpr uint32_t FocusCollapseMs = 250;
constexpr uint32_t LockScreenExitMs = 260;
}  // namespace Motion

/* ---- Theme (dark) ------------------------------------------------------------------------------- */

namespace Theme {
constexpr Color TextPrimary = 0xFFFFFFFF;
constexpr Color TextSecondary = 0xC8FFFFFF;
constexpr Color TextTertiary = 0x8BFFFFFF;
constexpr Color TextDisabled = 0x5DFFFFFF;
constexpr Color TextOnAccent = 0xFF24150D;
constexpr Color Accent = 0xFFFF9F5A; /* the sun on the horizon */
constexpr Color AccentHover = 0xFFFFB37C;
constexpr Color AccentPressed = 0xFFE58A48;
constexpr Color AccentDisabled = 0x28FFFFFF;
constexpr Color AccentSelection = 0x99FF9F5A;
constexpr Color ControlFill = 0x0FFFFFFF;
constexpr Color ControlFillHover = 0x17FFFFFF;
constexpr Color ControlFillPressed = 0x08FFFFFF;
constexpr Color ControlFillDisabled = 0x0BFFFFFF;
constexpr Color ControlStroke = 0x14FFFFFF;
constexpr Color ControlStrongStroke = 0x8BFFFFFF;
constexpr Color ControlStrongStrokeDisabled = 0x28FFFFFF;
constexpr Color SubtleFillHover = 0x0FFFFFFF;
constexpr Color SubtleFillPressed = 0x0AFFFFFF;
constexpr Color InputActive = 0xC01C1C24;
constexpr Color CardBackground = 0x0DFFFFFF;
constexpr Color CardStroke = 0x12FFFFFF;
constexpr Color Divider = 0x15FFFFFF;
constexpr Color SolidBackground = 0xFF202020;
constexpr Color PopupBackground = 0xFF2B2A30;
constexpr Color FocusStroke = 0xFFFFFFFF;
constexpr Color FocusStrokeInner = 0xB3000000;
constexpr Color KnobFill = 0xFFFFFFFF;
constexpr float ControlRadius = 4.0f;
constexpr float OverlayRadius = 8.0f;
constexpr int32_t ControlHeight = 32;
}  // namespace Theme

enum class FontRole : uint8_t { Body, BodyStrong, Caption, Subtitle, Title, Mono, BodyLarge, Display };
HZ_FONT *GetFont(FontRole Role);

/* Colors */
Color WithAlpha(Color C, uint32_t Alpha);          /* replaces the alpha */
Color ScaleAlpha(Color C, float Factor);           /* multiplies it */
Color MixColors(Color From, Color To, float T);    /* channel-wise, T in 0..1 */

/* ---- Painter ------------------------------------------------------------------------------------ */

enum class Icon : uint8_t {
    None, Check, ChevronDown, ChevronUp, ChevronLeft, ChevronRight, Close, Add, Remove, Search, More,
    Info, Clock, Home, Settings, Star, Power, Volume, Sun, Moon, Leaf, Plane,
    Folder, Document, Drive, Prompt, Calculator, Chart, ArrowUp, Pencil, Trash, Save, Open, Refresh,
    Cut, Copy, Paste, Person, Cube, Nexora
};

/* Draws into a canvas in its coordinates, through a stack of clip rectangles. Shapes are
   anti-aliased with signed-distance coverage; "Fill" operations blend over what is there. */
class Painter {
public:
    explicit Painter(const HZ_CANVAS &Canvas);

    HZ_CANVAS &Canvas() { return Canvas_; }
    void PushClip(const Rect &R);
    void PopClip();
    Rect Clip() const { return Rect::FromHz(Canvas_.Clip); }
    bool IsVisible(const Rect &R) const { return !Clip().Intersect(R).Empty(); }

    void Clear(const Rect &R, Color C);       /* replaces pixels (backgrounds) */
    void FillRect(const Rect &R, Color C);
    void FillRoundRect(float X, float Y, float W, float H, float Radius, Color C);
    void FillRoundRect(const Rect &R, float Radius, Color C) { FillRoundRect((float)R.X, (float)R.Y, (float)R.Width, (float)R.Height, Radius, C); }
    /* A stroke of Thickness inside the edge. */
    void StrokeRoundRect(float X, float Y, float W, float H, float Radius, float Thickness, Color C);
    void StrokeRoundRect(const Rect &R, float Radius, float Thickness, Color C) { StrokeRoundRect((float)R.X, (float)R.Y, (float)R.Width, (float)R.Height, Radius, Thickness, C); }
    void FillVerticalGradient(const Rect &R, float Radius, Color Top, Color Bottom);
    void FillCircle(float Cx, float Cy, float Radius, Color C);
    void StrokeCircle(float Cx, float Cy, float Radius, float Thickness, Color C);
    void Line(float X0, float Y0, float X1, float Y1, float Width, Color C);
    void Polyline(const float *Points, int Count, float Width, Color C); /* x0, y0, x1, y1, ... */
    int32_t Text(HZ_FONT *Font, int32_t X, int32_t Baseline, const char *Text, size_t Length, Color C);
    int32_t Text(HZ_FONT *Font, int32_t X, int32_t Baseline, const char *Text, Color C) { return this->Text(Font, X, Baseline, Text, strlen(Text), C); }
    /* One line vertically centred in R, aligned horizontally, with an ellipsis if it does not fit. */
    void TextLine(HZ_FONT *Font, const Rect &R, const char *Text, Color C, Align Horizontal = Align::Start);
    void DrawIcon(Icon Kind, const Rect &Box, Color C, float StrokeWidth = 1.25f);
    /* The keyboard focus ring around R (outside it). */
    void FocusRing(const Rect &R, float Radius);

private:
    void RoundRectCoverage(float X, float Y, float W, float H, float Radius, float Thickness, Color Top, Color Bottom);

    HZ_CANVAS Canvas_;
    hz::Vector<HZ_RECT> Clips_;
};

/* ---- Text layout ------------------------------------------------------------------------------ */

struct TextLine {
    size_t Start = 0, Length = 0;
    int32_t Width = 0;
};

/* Breaks UTF-8 text into lines: at '\n', and (when MaxWidth > 0) at spaces before the line gets
   wider than MaxWidth, or inside a word that alone is wider. */
class TextLayout {
public:
    void Layout(HZ_FONT *Font, const char *Text, size_t Length, int32_t MaxWidth);
    hz::Vector<TextLine> Lines;
    int32_t Width = 0, Height = 0, LineHeight = 0;
};

/* Text that fits in MaxWidth, ending in an ellipsis when it had to be cut. */
hz::String EllipsizeText(HZ_FONT *Font, const char *Text, size_t Length, int32_t MaxWidth);

/* ---- Animation ---------------------------------------------------------------------------------- */

/* A value that eases from where it is towards a target over a duration (cubic ease-out). */
class Transition {
public:
    void To(float Target, uint32_t DurationMs);
    void Jump(float Value);
    float Value() const;
    float Target() const { return To_; }
    bool Running() const;

private:
    float From_ = 0, To_ = 0;
    uint64_t StartUs_ = 0;
    uint32_t DurationUs_ = 0;
};

uint64_t NowUs();

/* ---- Elements ----------------------------------------------------------------------------------- */

class Window;

struct PointerEvent {
    Point Position;         /* window coordinates */
    Point ScreenPosition;
    uint32_t Button = 0;    /* HZ_MOUSE_* that changed (press and release) */
    uint32_t Buttons = 0;   /* held */
    uint16_t Modifiers = 0; /* HZ_MOD_* */
    int32_t ClickCount = 1;
    int32_t WheelDelta = 0; /* 120 per notch, positive away from the user */
    bool Handled = false;
};

struct KeyEvent {
    uint32_t Key = 0;       /* HZ_VK_* */
    uint32_t Character = 0; /* OnChar */
    uint16_t Modifiers = 0;
    bool Handled = false;
    bool Shift() const { return (Modifiers & HZ_MOD_SHIFT) != 0; }
    bool Ctrl() const { return (Modifiers & HZ_MOD_CTRL) != 0; }
    bool Alt() const { return (Modifiers & HZ_MOD_ALT) != 0; }
};

class Element {
public:
    Element();
    virtual ~Element();
    Element(const Element &) = delete;
    Element &operator=(const Element &) = delete;

    /* Tree (children are owned and deleted with their parent). */
    template <class T, class... A>
    T *Add(A &&...Args)
    {
        T *Child = new T(hz::Forward<A>(Args)...);
        AddChild(Child);
        return Child;
    }
    Element *AddChild(Element *Child);
    void RemoveChild(Element *Child);
    void ClearChildren();
    size_t ChildCount() const { return Children_.Size(); }
    Element *ChildAt(size_t Index) const { return Children_[Index]; }
    Element *Parent() const { return Parent_; }
    Window *GetWindow() const { return Window_; }
    Element *FindByName(const char *Name);
    bool IsAncestorOf(const Element *Other) const;

    /* Layout and appearance */
    hz::String Name;
    Thickness Margin, Padding;
    Align HorizontalAlignment = Align::Stretch, VerticalAlignment = Align::Stretch;
    int32_t Width = -1, Height = -1; /* fixed size; -1 = from the content */
    int32_t MinWidth = 0, MinHeight = 0, MaxWidth = Infinite, MaxHeight = Infinite;
    uint8_t GridRow = 0, GridColumn = 0, GridRowSpan = 1, GridColumnSpan = 1;
    Color Background = 0, BorderColor = 0;
    float CornerRadius = 0;
    hz::String ToolTip;
    bool Focusable = false;
    bool HitTestVisible = true;
    bool ClipChildren = false;

    void SetVisible(bool Visible);
    bool IsVisible() const { return Visible_; }
    void SetEnabled(bool Enabled);
    bool IsEnabled() const; /* false if it or an ancestor is disabled */
    bool IsHovered() const; /* the pointer is over it or a descendant */
    bool IsPressed() const; /* it holds the pointer and the pointer is over it */
    bool IsFocused() const;
    bool HasFocusWithin() const;
    bool ShowsFocusRing() const; /* focused after keyboard navigation */
    void Focus();

    const Rect &Bounds() const { return Bounds_; }
    Rect ContentBounds() const { return Bounds_.Deflate(Padding); }
    Size DesiredSize() const { return Desired_; }
    void Measure(Size Available);
    void Arrange(const Rect &Slot);
    void Invalidate();
    void InvalidateLayout();

    /* Overridables */
    virtual Size MeasureOverride(Size Available); /* content size; default: the largest child */
    virtual void ArrangeOverride(const Rect &Content); /* default: every child fills Content */
    virtual void Paint(Painter &P);                   /* default: background and border */
    virtual void PaintOverlay(Painter &P) {}          /* after the children */
    virtual Element *HitTest(Point P);
    virtual void OnPointerEnter() {}
    virtual void OnPointerLeave() {}
    virtual void OnPointerDown(PointerEvent &E) {}
    virtual void OnPointerMove(PointerEvent &E) {}
    virtual void OnPointerUp(PointerEvent &E) {}
    virtual void OnPointerWheel(PointerEvent &E) {}
    virtual void OnKeyDown(KeyEvent &E) {}
    virtual void OnChar(KeyEvent &E) {}
    virtual void OnFocusChanged(bool Focused) {}
    virtual void OnWindowActivated(bool Active) {}
    virtual uint32_t CursorShape() const { return HZ_CURSOR_ARROW; }
    /* Called each frame after StartAnimation() while it returns true. */
    virtual bool Animate() { return false; }
    /* Scrolls containers so R (window coordinates) is visible; default: ask the parent. */
    virtual void BringIntoView(const Rect &R);
    /* Non-null only for radio buttons (type test without RTTI). */
    virtual const char *RadioGroup() const { return nullptr; }

protected:
    void StartAnimation();
    hz::Vector<Element *> &ChildList() { return Children_; }

private:
    friend class Window;
    void Attach(Window *W);
    void DetachFromWindow();

    Element *Parent_ = nullptr;
    Window *Window_ = nullptr;
    hz::Vector<Element *> Children_;
    Rect Bounds_;
    Size Desired_;
    bool Visible_ = true;
    bool Enabled_ = true;
};

/* Lays out and paints a tree that is not in a window (tests, off-screen rendering). */
void LayoutTree(Element *Root, const Rect &Bounds);
void PaintTree(Element *Root, Painter &P);

/* ---- Panels ------------------------------------------------------------------------------------- */

class StackPanel : public Element {
public:
    explicit StackPanel(Orientation O = Orientation::Vertical, int32_t Spacing = 0) : Direction(O), Spacing(Spacing) {}
    Orientation Direction;
    int32_t Spacing;
    Size MeasureOverride(Size Available) override;
    void ArrangeOverride(const Rect &Content) override;
};

struct GridLength {
    enum Kind : uint8_t { Auto, Pixel, Star };
    Kind Type = Star;
    float Value = 1;
    static GridLength AutoSize() { return GridLength{Auto, 0}; }
    static GridLength Pixels(float V) { return GridLength{Pixel, V}; }
    static GridLength Stars(float V = 1) { return GridLength{Star, V}; }
};

class Grid : public Element {
public:
    hz::Vector<GridLength> Columns, Rows; /* empty = one star track */
    int32_t ColumnSpacing = 0, RowSpacing = 0;
    Size MeasureOverride(Size Available) override;
    void ArrangeOverride(const Rect &Content) override;
    const hz::Vector<int32_t> &ColumnWidths() const { return ColumnSizes_; }
    const hz::Vector<int32_t> &RowHeights() const { return RowSizes_; }

private:
    void ComputeTracks(Size Available, bool Arranging);
    hz::Vector<int32_t> ColumnSizes_, RowSizes_;
};

/* ---- Text and simple controls ----------------------------------------------------------------- */

class TextBlock : public Element {
public:
    explicit TextBlock(const char *Text = "", FontRole Role = FontRole::Body);
    void SetText(const char *Text);
    const hz::String &Text() const { return Text_; }
    FontRole Font;
    Color Foreground = Theme::TextPrimary;
    bool Wrap = false;
    bool Ellipsis = true;
    Align TextAlignment = Align::Start;
    Size MeasureOverride(Size Available) override;
    void ArrangeOverride(const Rect &Content) override;
    void Paint(Painter &P) override;

private:
    hz::String Text_;
    TextLayout Layout_;
};

enum class ButtonStyle : uint8_t { Standard, Accent, Subtle };

class Button : public Element {
public:
    explicit Button(const char *Text = "", ButtonStyle Style = ButtonStyle::Standard);
    void SetText(const char *Text);
    const hz::String &Text() const { return Text_; }
    ButtonStyle Style;
    Icon Glyph = Icon::None;
    hz::Function<void()> OnClick;
    void Click(); /* as if clicked */
    Size MeasureOverride(Size Available) override;
    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void OnPointerEnter() override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerMove(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;
    bool Animate() override;

protected:
    hz::String Text_;
    Transition Hover_;
};

class ToggleSwitch : public Element {
public:
    explicit ToggleSwitch(const char *OnText = "On", const char *OffText = "Off");
    void SetOn(bool On, bool Notify = true);
    bool IsOn() const { return On_; }
    hz::String OnText, OffText;
    hz::Function<void(bool)> OnToggled;
    Size MeasureOverride(Size Available) override;
    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void OnPointerEnter() override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;
    bool Animate() override;
    Rect TrackRect() const;

private:
    bool On_ = false;
    Transition Knob_, Hover_;
};

class CheckBox : public Element {
public:
    explicit CheckBox(const char *Text = "", bool Checked = false);
    void SetChecked(bool Checked, bool Notify = true);
    bool IsChecked() const { return Checked_; }
    hz::String Text;
    hz::Function<void(bool)> OnChanged;
    Size MeasureOverride(Size Available) override;
    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void OnPointerEnter() override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;
    Rect BoxRect() const;

protected:
    virtual void Toggle() { SetChecked(!Checked_); }
    bool Checked_ = false;
};

/* Checking one unchecks the others with the same Group under the same parent. */
class RadioButton : public CheckBox {
public:
    RadioButton(const char *Text, const char *Group, bool Checked = false);
    hz::String Group;
    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void Select(bool Notify = true);
    const char *RadioGroup() const override { return Group.CStr(); }

protected:
    void Toggle() override { Select(); }
};

class Slider : public Element {
public:
    Slider(float Minimum = 0, float Maximum = 100, float Value = 0);
    void SetValue(float Value, bool Notify = true);
    float Value() const { return Value_; }
    float Minimum, Maximum, Step = 1;
    hz::Function<void(float)> OnValueChanged;
    Size MeasureOverride(Size Available) override;
    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void OnPointerEnter() override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerMove(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;
    void OnPointerWheel(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;
    bool Animate() override;
    Rect TrackRect() const;
    float ThumbX() const;

private:
    void SetFromPointer(int32_t X);
    float Value_;
    Transition Hover_;
};

class ProgressBar : public Element {
public:
    explicit ProgressBar(float Value = 0) : Value_(Value) { Height = 4; }
    void SetValue(float Percent);
    float Value() const { return Value_; }
    void SetIndeterminate(bool On);
    bool IsIndeterminate() const { return Indeterminate_; }
    Size MeasureOverride(Size Available) override;
    void Paint(Painter &P) override;
    bool Animate() override;

private:
    float Value_;
    bool Indeterminate_ = false;
    uint64_t StartUs_ = 0;
};

/* ---- Text input ------------------------------------------------------------------------------- */

/* Single-line editor: caret, selection (pointer and Shift+keys), word moves (Ctrl), select all,
   cut/copy/paste through the system clipboard (Clipboard below), horizontal scrolling. */
class TextBox : public Element {
public:
    explicit TextBox(const char *Placeholder = "");
    void SetText(const char *Text, bool Notify = true);
    const hz::String &Text() const { return Text_; }
    hz::String Placeholder;
    size_t MaxLength = 256;
    /* M11: shows a dot per character; Copy and Cut are off and words are not recognized. */
    bool Password = false;
    hz::Function<void(const hz::String &)> OnTextChanged;
    hz::Function<void(const hz::String &)> OnSubmit; /* Enter */

    size_t CaretPosition() const { return Caret_; }
    size_t SelectionStart() const { return hz::Min(Caret_, Anchor_); }
    size_t SelectionEnd() const { return hz::Max(Caret_, Anchor_); }
    bool HasSelection() const { return Caret_ != Anchor_; }
    void Select(size_t Start, size_t End); /* byte offsets; the caret goes to End */
    void SelectAll() { Select(0, Text_.Length()); }
    void ReplaceSelection(const char *Text, size_t Length);
    int32_t OffsetToX(size_t Offset) const; /* window x of a byte offset */
    size_t XToOffset(int32_t X) const;

    Size MeasureOverride(Size Available) override;
    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void OnPointerEnter() override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerMove(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;
    void OnChar(KeyEvent &E) override;
    void OnFocusChanged(bool Focused) override;
    void OnWindowActivated(bool Active) override;
    uint32_t CursorShape() const override { return HZ_CURSOR_IBEAM; }

private:
    Rect TextArea() const;
    void MoveCaret(size_t To, bool Extend);
    void EnsureCaretVisible();
    const hz::String &Shown() const;       /* Text_, or the dots of a password */
    size_t ShownOffset(size_t Offset) const; /* a byte offset of Text_ in Shown() */
    mutable hz::String Masked_;
    void Changed();
    void ResetBlink();
    void ShowContextMenu(Point Screen);
    size_t WordLeft(size_t From) const;
    size_t WordRight(size_t From) const;
    hz::String Text_;
    size_t Caret_ = 0, Anchor_ = 0;
    int32_t ScrollX_ = 0;
    uint64_t BlinkTimer_ = 0;
    uint64_t BlinkStartUs_ = 0;
    bool Selecting_ = false;
};

/* The system clipboard (text; win32k keeps it for every process). */
namespace Clipboard {
void SetText(const char *Text, size_t Length);
const hz::String &GetText();
}  // namespace Clipboard

/* ---- Scrolling -------------------------------------------------------------------------------- */

/* A vertical scroll bar: a thin indicator that widens when the pointer is near. */
class ScrollBar : public Element {
public:
    ScrollBar();
    void SetRange(int32_t Extent, int32_t Viewport); /* content and visible sizes */
    void SetOffset(int32_t Offset, bool Notify = true);
    int32_t Offset() const { return Offset_; }
    int32_t MaxOffset() const { return hz::Max(Extent_ - Viewport_, 0); }
    bool IsNeeded() const { return Extent_ > Viewport_; }
    hz::Function<void(int32_t)> OnScroll;
    Size MeasureOverride(Size Available) override { return Size{12, 0}; }
    void Paint(Painter &P) override;
    void OnPointerEnter() override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerMove(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;
    bool Animate() override;
    Rect ThumbRect() const;
    Element *HitTest(Point P) override;

private:
    int32_t Extent_ = 0, Viewport_ = 0, Offset_ = 0;
    int32_t DragStartY_ = -1, DragStartOffset_ = 0;
    Transition Expand_;
};

/* Scrolls one child vertically (wheel, scroll bar, ScrollIntoView). */
class ScrollViewer : public Element {
public:
    ScrollViewer();
    void SetContent(Element *Content);
    Element *Content() const { return Content_; }
    void ScrollTo(int32_t Offset, bool Animated = true);
    void ScrollIntoView(const Rect &R); /* window coordinates */
    int32_t Offset() const { return Offset_; }
    int32_t Extent() const { return Extent_; }
    Size MeasureOverride(Size Available) override;
    void ArrangeOverride(const Rect &Content) override;
    void OnPointerWheel(PointerEvent &E) override;
    bool Animate() override;
    void BringIntoView(const Rect &R) override;

private:
    void ApplyOffset();
    Element *Content_ = nullptr;
    ScrollBar *Bar_;
    int32_t Offset_ = 0, Extent_ = 0;
    Transition Scroll_;
};

/* A virtualized list: only the visible rows are painted, so it scales to large item counts. */
class ListView : public Element {
public:
    ListView();
    void SetItemCount(size_t Count);
    size_t ItemCount() const { return Count_; }
    int32_t ItemHeight = 36;
    hz::Function<hz::String(size_t)> ItemText;
    /* Optional: paints the row's content (the list paints the selection and hover backgrounds). */
    hz::Function<void(Painter &, const Rect &, size_t)> PaintItem;
    hz::Function<void(size_t)> OnSelectionChanged;
    hz::Function<void(size_t)> OnItemInvoked; /* double-click or Enter */
    void SetSelectedIndex(size_t Index, bool Notify = true); /* hz::NotFound clears */
    size_t SelectedIndex() const { return Selected_; }
    void ScrollIntoView(size_t Index);
    int32_t ScrollOffset() const { return Offset_; }
    size_t IndexAt(Point P) const; /* window coordinates; hz::NotFound if none */
    uint32_t RowsPaintedLastFrame() const { return RowsPainted_; }

    Size MeasureOverride(Size Available) override;
    void ArrangeOverride(const Rect &Content) override;
    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerMove(PointerEvent &E) override;
    void OnPointerWheel(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;
    bool Animate() override;

private:
    void SetOffset(int32_t Offset, bool Animated);
    int32_t MaxOffset() const;
    size_t Count_ = 0;
    size_t Selected_ = hz::NotFound;
    size_t Hot_ = hz::NotFound;
    int32_t Offset_ = 0;
    uint32_t RowsPainted_ = 0;
    ScrollBar *Bar_;
    Transition Scroll_;
};

/* ---- Menus, combo box, tabs ----------------------------------------------------------------- */

struct MenuItem {
    hz::String Text;
    hz::String Shortcut; /* shown on the right */
    Icon Glyph = Icon::None;
    bool Checked = false;
    bool Separator = false;
    bool Enabled = true;
    hz::Function<void()> Action;
    static MenuItem Make(const char *Text, hz::Function<void()> Action, const char *Shortcut = "")
    {
        MenuItem Item;
        Item.Text = Text;
        Item.Shortcut = Shortcut;
        Item.Action = hz::Move(Action);
        return Item;
    }
    static MenuItem MakeSeparator()
    {
        MenuItem Item;
        Item.Separator = true;
        return Item;
    }
};

/* Opens a menu at a screen position (kept on screen) for Owner. MinWidth widens it. Returns false
   if it could not open. The menu closes on a choice, Escape, a click outside it or deactivation. */
bool ShowMenu(Window *Owner, Point Screen, hz::Vector<MenuItem> Items, int32_t MinWidth = 0, int32_t Highlight = -1);

class ComboBox : public Element {
public:
    ComboBox();
    hz::Vector<hz::String> Items;
    void SetSelectedIndex(size_t Index, bool Notify = true);
    size_t SelectedIndex() const { return Selected_; }
    hz::String Placeholder;
    hz::Function<void(size_t)> OnSelectionChanged;
    void Open();
    Size MeasureOverride(Size Available) override;
    void Paint(Painter &P) override;
    void PaintOverlay(Painter &P) override;
    void OnPointerEnter() override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;

private:
    size_t Selected_ = hz::NotFound;
};

/* A row of tab headers above the selected tab's content. Ctrl+Tab moves between tabs. */
class TabView : public Element {
public:
    TabView();
    Element *AddTab(const char *Title, Element *Content); /* takes ownership; returns Content */
    void SelectTab(size_t Index, bool Notify = true);
    size_t SelectedTab() const { return Selected_; }
    size_t TabCount() const { return Headers_.Size(); }
    hz::Function<void(size_t)> OnTabChanged;
    Size MeasureOverride(Size Available) override;
    void ArrangeOverride(const Rect &Content) override;
    void OnKeyDown(KeyEvent &E) override;
    Rect HeaderRect(size_t Index) const;

private:
    friend class TabHeader;
    StackPanel *Strip_;
    hz::Vector<Element *> Headers_;
    hz::Vector<Element *> Pages_;
    size_t Selected_ = 0;
};

/* ---- Multi-line text (M9) -------------------------------------------------------------------- */

struct TextPosition {
    size_t Line = 0, Column = 0; /* Column: byte offset in the line, on a code point boundary */
    bool operator==(const TextPosition &O) const { return Line == O.Line && Column == O.Column; }
    bool operator!=(const TextPosition &O) const { return !(*this == O); }
    bool operator<(const TextPosition &O) const { return Line < O.Line || (Line == O.Line && Column < O.Column); }
};

/* A plain-text editor: lines of UTF-8 text, a caret and a selection, vertical and horizontal
   scrolling (vertical scroll bar, the view follows the caret), undo and redo (typing is grouped
   into words), the system clipboard, find, and the usual keys (arrows with Ctrl and Shift,
   Home/End, PageUp/PageDown, Ctrl+Home/End, Ctrl+A/C/X/V/Z/Y, Tab inserts spaces, Enter keeps the
   indentation). Line ends are '\n' (a "\r\n" pair read from a file becomes '\n'). */
class TextEditor : public Element {
public:
    TextEditor();
    void SetText(const char *Text, size_t Length); /* resets the selection, the view and undo */
    hz::String Text() const;
    size_t LineCount() const { return Lines_.Size(); }
    const hz::String &Line(size_t Index) const { return Lines_[Index]; }
    size_t CharacterCount() const;

    TextPosition Caret() const { return Caret_; }
    TextPosition Anchor() const { return Anchor_; }
    bool HasSelection() const { return Caret_ != Anchor_; }
    TextPosition SelectionStart() const { return Caret_ < Anchor_ ? Caret_ : Anchor_; }
    TextPosition SelectionEnd() const { return Caret_ < Anchor_ ? Anchor_ : Caret_; }
    hz::String SelectedText() const;
    void Select(TextPosition Anchor, TextPosition Caret);
    void SelectAll();
    /* Replaces the selection with Text (which may contain line feeds); undoable. */
    void Insert(const char *Text, size_t Length);
    void Insert(const char *Text) { Insert(Text, strlen(Text)); }
    void DeleteSelection() { Insert("", 0); }
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !Undo_.Empty(); }
    bool CanRedo() const { return !Redo_.Empty(); }
    void Copy();
    void Cut();
    void Paste();
    /* Selects the next (or previous) match after the caret, ignoring ASCII case; wraps around. */
    bool Find(const char *Needle, bool Forward = true);

    bool ReadOnly = false;
    FontRole Font = FontRole::Mono;
    /* Long lines continue on the next rows at word boundaries (M11); no horizontal scrolling. */
    bool WordWrap = false;
    size_t RowCount() const; /* visible rows (lines, or wrapped pieces of lines) */
    bool Modified = false; /* set by edits; the program clears it when it saves */
    hz::Function<void()> OnChanged;
    hz::Function<void()> OnCaretMoved;

    /* View helpers (window coordinates). */
    TextPosition PositionAt(Point P) const;
    Point PointOf(TextPosition P) const; /* top-left of the caret */
    int32_t ScrollY() const { return ScrollY_; }
    int32_t ScrollX() const { return ScrollX_; }

    Size MeasureOverride(Size Available) override;
    void ArrangeOverride(const Rect &Content) override;
    void Paint(Painter &P) override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerMove(PointerEvent &E) override;
    void OnPointerUp(PointerEvent &E) override;
    void OnPointerWheel(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;
    void OnChar(KeyEvent &E) override;
    void OnFocusChanged(bool Focused) override;
    void OnWindowActivated(bool Active) override;
    uint32_t CursorShape() const override { return HZ_CURSOR_IBEAM; }

private:
    struct Edit {
        TextPosition Start;
        hz::String Removed, Inserted;
        TextPosition CaretBefore, AnchorBefore;
        bool Typing = false;
    };
    TextPosition Replace(TextPosition Start, TextPosition End, const char *Text, size_t Length); /* end of inserted */
    hz::String Range(TextPosition Start, TextPosition End) const;
    TextPosition Clamp(TextPosition P) const;
    TextPosition Next(TextPosition P) const;
    TextPosition Previous(TextPosition P) const;
    TextPosition WordLeft(TextPosition P) const;
    TextPosition WordRight(TextPosition P) const;
    void MoveCaret(TextPosition To, bool Extend, bool KeepColumn = false);
    void Apply(const char *Text, size_t Length, bool Typing);
    void Changed();
    void EnsureCaretVisible();
    void UpdateScrollBar();
    Rect TextArea() const;
    int32_t LineHeight() const;

    /* Rows: a line, or with WordWrap a piece of one ([Start, End) bytes). Rebuilt lazily. */
    struct Row {
        size_t Line, Start, End;
    };
    void EnsureRows() const;
    size_t RowOf(TextPosition P) const;
    int32_t RowX(size_t RowIndex, size_t Column) const; /* pixels from the row start */
    size_t RowColumnAt(size_t RowIndex, int32_t X) const;
    mutable hz::Vector<Row> Rows_;
    mutable bool RowsDirty_ = true;
    mutable int32_t RowsWidth_ = -1;
    mutable FontRole RowsFont_ = FontRole::Mono;
    mutable bool RowsWrap_ = false;

    hz::Vector<hz::String> Lines_;
    TextPosition Caret_, Anchor_;
    int32_t ScrollX_ = 0, ScrollY_ = 0;
    int32_t PreferredX_ = -1;
    hz::Vector<Edit> Undo_, Redo_;
    ScrollBar *Bar_;
    uint64_t BlinkTimer_ = 0, BlinkStartUs_ = 0;
    bool Selecting_ = false;
};

/* ---- Menu bar (M9) -------------------------------------------------------------------------- */

/* A row of menu titles; each opens the menu its function builds (built when it opens, so items
   can reflect the current state). */
class MenuBar : public Element {
public:
    MenuBar();
    void AddMenu(const char *Title, hz::Function<hz::Vector<MenuItem>()> Build);
    Size MeasureOverride(Size Available) override;
    void Paint(Painter &P) override;
    void OnPointerMove(PointerEvent &E) override;
    void OnPointerLeave() override;
    void OnPointerDown(PointerEvent &E) override;
    void Open(size_t Index);

private:
    struct Entry {
        hz::String Title;
        hz::Function<hz::Vector<MenuItem>()> Build;
        Rect Bounds;
    };
    size_t IndexAt(Point P) const;
    hz::Vector<Entry> Entries_;
    size_t Hot_ = hz::NotFound;
};

/* ---- Windows ----------------------------------------------------------------------------------- */

class Window {
public:
    static constexpr uint32_t DefaultStyle =
        HZ_WS_CAPTION | HZ_WS_RESIZABLE | HZ_WS_MINIMIZEBOX | HZ_WS_MAXIMIZEBOX | HZ_WS_MICA | HZ_WS_VISIBLE;

    Window(const char *Title, int32_t Width, int32_t Height, uint32_t Style = DefaultStyle);
    virtual ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    void SetContent(Element *Root); /* takes ownership */
    Element *Content() const { return Root_; }
    bool Show();                          /* centred in the work area */
    bool ShowAt(int32_t X, int32_t Y);    /* screen position of the window */
    void Close();                         /* destroys the window (OnClosed runs) */
    bool IsOpen() const { return Handle_ != 0; }
    HWND Handle() const { return Handle_; }
    uint32_t Style() const { return Style_; }
    void SetTitle(const char *Title);
    void SetMinimumSize(int32_t Width, int32_t Height);
    Rect ClientRect() const;              /* window coordinates (below the caption) */
    Point ScreenOrigin() const;           /* screen position of window (0, 0) */
    Size WindowSize() const { return Size{Width_, Height_}; }
    bool IsActive() const;

    /* Focus */
    Element *FocusedElement() const { return Focus_; }
    void SetFocus(Element *E, bool FromKeyboard = false);
    bool MoveFocus(bool Forward);
    bool FocusVisible() const { return FocusVisible_; }
    Element *HoveredElement() const { return Hover_; }
    Element *PressedElement() const { return Pressed_; }

    /* Repainting and layout */
    void InvalidateRect(const Rect &R);
    void InvalidateLayout();
    void UpdateLayout(); /* lays out now if needed */
    void RequestAnimation(Element *E);
    uint32_t PaintCount() const { return PaintCount_; }

    /* Timers (callbacks run on the window's thread) */
    uint64_t SetTimer(uint32_t PeriodMs, hz::Function<void()> Callback);
    void KillTimer(uint64_t Id);

    /* Popups and the cursor */
    void ClosePopups();
    Window *ActiveMenu() const { return Menu_; }
    void UpdateCursor();

    Color Background; /* client background; Mica windows default to transparent */
    hz::Function<void()> OnClosed;
    hz::Function<bool()> OnCloseRequested; /* return false to keep the window open */
    hz::Function<void(int32_t, int32_t)> OnMoved;
    hz::Function<void(KeyEvent &)> OnUnhandledKey;
    /* The character of the key being handled is not delivered (M11: a key that opens a text box). */
    void DropNextChar() { DropChar_ = true; }

    /* The window procedure shared by every hzui window. */
    static int64_t WindowProc(HWND Handle, uint32_t Message, uint64_t WParam, uint64_t LParam);
    virtual int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam);

    /* Internal: elements report their removal; popups register with their owner. */
    void ForgetElement(Element *E);
    void SetMenu(Window *Menu);
    Window *Owner = nullptr;

protected:
    virtual void OnPaint(Painter &P);
    virtual void OnContextMenu(Point Screen);
    virtual void OnPopupPointerOutside() {}   /* a captured click outside the window, or the capture lost */
    virtual bool HandlePopupKey(KeyEvent &E) { return false; }

private:
    friend class Element;
    void PaintWindow();
    void DispatchPointer(uint32_t Message, uint64_t WParam, uint64_t LParam, const HZ_MSG *Info);
    void UpdateHover(Element *Target);
    void HandleKey(uint32_t Message, uint64_t WParam, uint64_t LParam);
    void AnimationTick();
    void UpdateToolTip(Point Screen);
    void HideToolTip();
    void Destroyed();

    struct TimerSlot {
        uint64_t Id;
        hz::Function<void()> Callback;
    };

    hz::String Title_;
    int32_t Width_, Height_;
    uint32_t Style_;
    HWND Handle_ = 0;
    Element *Root_ = nullptr;
    Element *Focus_ = nullptr, *Hover_ = nullptr, *Pressed_ = nullptr;
    bool DropChar_ = false;
    bool FocusVisible_ = false;
    bool LayoutDirty_ = true;
    bool Active_ = false;
    uint32_t PaintCount_ = 0;
    hz::Vector<Element *> Animating_;
    bool AnimationTimer_ = false;
    hz::Vector<TimerSlot> Timers_;
    uint64_t NextTimerId_ = 100;
    Window *Menu_ = nullptr;
    Window *ToolTip_ = nullptr;
    Element *ToolTipOwner_ = nullptr;
    Element *ToolTipCandidate_ = nullptr;
    Element *ToolTipSuppressed_ = nullptr; /* clicked: no tooltip until the pointer leaves it */
    Point LastScreenPointer_;
    Point Origin_;                         /* screen position of the window */
    uint32_t CursorShape_ = HZ_CURSOR_ARROW;
    int32_t MinWidth_ = 0, MinHeight_ = 0;
};

/* Popups: no caption, topmost, never activated, opaque. */
class PopupWindow : public Window {
public:
    PopupWindow(int32_t Width, int32_t Height);
};

/* Message loop helpers: run until WM_QUIT (returns its code), or dispatch what is queued. Both
   also free popups that closed. */
int RunMessageLoop();
void PumpMessages();
void CollectClosedWindows();
void DeferDelete(Window *W);

/* ---- Dialogs (M9) ---------------------------------------------------------------------------- */

/* A window that belongs to another one: centred over it, and the owner's content is disabled
   while it is open (a modal dialog without a nested message loop). Enter presses the default
   action, Escape the cancel action (OnUnhandledKey). Closing re-enables and reactivates the owner. */
class Dialog : public Window {
public:
    Dialog(Window *Owner, const char *Title, int32_t Width, int32_t Height);
    bool Open();
    void Finish(); /* closes and frees the dialog later */
    Window *OwnerWindow() const { return Owner_; }

private:
    Window *Owner_;
    hz::String Caption_;
};

/* Asks a question. Buttons: labels separated by '|', left to right (the first is the default and
   accent one; NULL = "OK"). Result gets the index of the button, or -1 if the dialog was closed
   otherwise; Escape picks the last button. */
void MessageBox(Window *Owner, const char *Title, const char *Text, const char *Buttons,
                hz::Function<void(int)> Result);

/* Asks for one line of text (a name). Result gets the text, or is not called when cancelled.
   The initial text is selected up to its last '.' (the name without the extension). */
void InputBox(Window *Owner, const char *Title, const char *Prompt, const char *Initial, const char *Action,
              hz::Function<void(const hz::String &)> Result);

enum class FileDialogMode : uint8_t { Open, Save };
/* Lets the user pick a file: volumes, folders, a list of the folder's content and a name box.
   Saving over an existing file asks first. Done gets the full path, or "" when cancelled. */
void ShowFileDialog(Window *Owner, FileDialogMode Mode, const char *Folder, const char *Name,
                    hz::Function<void(const hz::String &)> Done);

}  // namespace hzui

#endif /* HZUI_H */
