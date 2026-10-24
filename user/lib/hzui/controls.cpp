/*
 * Nexora OS - user/lib/hzui/controls.cpp
 * Buttons, toggle switches, check boxes, radio buttons, sliders, progress bars, the combo box and
 * the tab view. Visual states (rest, hover, pressed, disabled, focused after keyboard navigation)
 * follow the theme tokens in hzui.h; hover changes fade with short transitions.
 */
#include "hzuip.h"

namespace hzui {

namespace {

int32_t TextWidth(FontRole Role, const hz::String &Text)
{
    HZ_FONT *F = GetFont(Role);
    return F ? HzMeasureTextN(F, Text.CStr(), Text.Length()) : 0;
}

void DrawLabel(Painter &P, const Rect &Area, const hz::String &Text, Color C)
{
    if (!Text.Empty())
        P.TextLine(GetFont(FontRole::Body), Area, Text.CStr(), C);
}

}  // namespace

/* ---- Button ----------------------------------------------------------------------------------------- */

Button::Button(const char *Text, ButtonStyle S) : Style(S), Text_(Text)
{
    Focusable = true;
    Padding = Thickness(16, 0);
    MinWidth = 32;
}

void Button::SetText(const char *Text)
{
    Text_ = Text;
    InvalidateLayout();
}

void Button::Click()
{
    if (OnClick) {
        hz::Function<void()> Callback = OnClick;
        Callback();
    }
}

Size Button::MeasureOverride(Size)
{
    int32_t W = TextWidth(FontRole::Body, Text_);
    if (Glyph != Icon::None)
        W += Text_.Empty() ? 16 : 24;
    return Size{W, Theme::ControlHeight};
}

void Button::Paint(Painter &P)
{
    bool Enabled = IsEnabled(), Pressed = IsPressed();
    float Hover = Hover_.Value();
    Color Fill, Text;
    switch (Style) {
    case ButtonStyle::Accent:
        Fill = !Enabled ? Theme::AccentDisabled
                        : (Pressed ? Theme::AccentPressed : MixColors(Theme::Accent, Theme::AccentHover, Hover));
        Text = Enabled ? Theme::TextOnAccent : Theme::TextDisabled;
        break;
    case ButtonStyle::Subtle:
        Fill = !Enabled ? 0 : (Pressed ? Theme::SubtleFillPressed : ScaleAlpha(Theme::SubtleFillHover, Hover));
        Text = Enabled ? (Pressed ? Theme::TextSecondary : Theme::TextPrimary) : Theme::TextDisabled;
        break;
    default:
        Fill = !Enabled ? Theme::ControlFillDisabled
                        : (Pressed ? Theme::ControlFillPressed
                                   : MixColors(Theme::ControlFill, Theme::ControlFillHover, Hover));
        Text = Enabled ? (Pressed ? Theme::TextSecondary : Theme::TextPrimary) : Theme::TextDisabled;
        break;
    }
    const Rect &B = Bounds();
    P.FillRoundRect(B, Theme::ControlRadius, Fill);
    if (Style == ButtonStyle::Standard)
        P.StrokeRoundRect(B, Theme::ControlRadius, 1, Theme::ControlStroke);

    int32_t TextW = TextWidth(FontRole::Body, Text_);
    int32_t IconW = Glyph == Icon::None ? 0 : (Text_.Empty() ? 16 : 24);
    int32_t Total = hz::Min(TextW + IconW, B.Width - 8);
    int32_t X = B.X + (B.Width - Total) / 2;
    if (Glyph != Icon::None)
        P.DrawIcon(Glyph, Rect{X, B.Y + (B.Height - 16) / 2, 16, 16}, Text);
    DrawLabel(P, Rect{X + IconW, B.Y, Total - IconW + 1, B.Height}, Text_, Text);
}

void Button::PaintOverlay(Painter &P)
{
    if (ShowsFocusRing())
        P.FocusRing(Bounds(), Theme::ControlRadius);
}

void Button::OnPointerEnter()
{
    Hover_.To(1, Motion::HoverEnterMs);
    StartAnimation();
}

void Button::OnPointerLeave()
{
    Hover_.To(0, Motion::HoverLeaveMs);
    StartAnimation();
}

void Button::OnPointerDown(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_LEFT)
        E.Handled = true;
}

void Button::OnPointerMove(PointerEvent &)
{
    Invalidate(); /* the pressed look follows the pointer */
}

void Button::OnPointerUp(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_LEFT && Bounds().Contains(E.Position))
        Click();
}

void Button::OnKeyDown(KeyEvent &E)
{
    if (E.Key == HZ_VK_SPACE || E.Key == HZ_VK_RETURN) {
        E.Handled = true;
        Click();
    }
}

bool Button::Animate()
{
    Invalidate();
    return Hover_.Running();
}

/* ---- ToggleSwitch ------------------------------------------------------------------------------- */

ToggleSwitch::ToggleSwitch(const char *On, const char *Off) : OnText(On), OffText(Off)
{
    Focusable = true;
}

void ToggleSwitch::SetOn(bool On, bool Notify)
{
    if (On == On_)
        return;
    On_ = On;
    if (GetWindow()) {
        Knob_.To(On ? 1.0f : 0.0f, Motion::ControlStateMs);
        StartAnimation();
    } else {
        Knob_.Jump(On ? 1.0f : 0.0f);
    }
    Invalidate();
    if (Notify && OnToggled)
        OnToggled(On);
}

Rect ToggleSwitch::TrackRect() const
{
    return Rect{Bounds().X, Bounds().Y + (Bounds().Height - 20) / 2, 40, 20};
}

Size ToggleSwitch::MeasureOverride(Size)
{
    int32_t Label = hz::Max(TextWidth(FontRole::Body, OnText), TextWidth(FontRole::Body, OffText));
    return Size{40 + (Label ? 12 + Label : 0), Theme::ControlHeight};
}

void ToggleSwitch::Paint(Painter &P)
{
    bool Enabled = IsEnabled();
    float K = Knob_.Value(), Hover = Hover_.Value();
    Rect T = TrackRect();
    if (K < 1) {
        P.FillRoundRect(T, 10, ScaleAlpha(MixColors(Theme::ControlFill, Theme::ControlFillHover, Hover), 1 - K));
        P.StrokeRoundRect(T, 10, 1, ScaleAlpha(Enabled ? Theme::TextSecondary : Theme::TextDisabled, 1 - K));
    }
    if (K > 0) {
        Color On = !Enabled ? Theme::AccentDisabled
                            : (IsPressed() ? Theme::AccentPressed : MixColors(Theme::Accent, Theme::AccentHover, Hover));
        P.FillRoundRect(T, 10, ScaleAlpha(On, K));
    }
    float Radius = 6 + Hover + (IsPressed() ? 1 : 0);
    Color Knob = Enabled ? MixColors(Theme::TextSecondary, Theme::TextOnAccent, K) : Theme::TextDisabled;
    P.FillCircle(T.X + 10 + K * 20, T.Y + 10.0f, Radius, Knob);
    const hz::String &Label = On_ ? OnText : OffText;
    DrawLabel(P, Rect{T.Right() + 12, Bounds().Y, Bounds().Width - 52, Bounds().Height}, Label,
              Enabled ? Theme::TextPrimary : Theme::TextDisabled);
}

void ToggleSwitch::PaintOverlay(Painter &P)
{
    if (ShowsFocusRing())
        P.FocusRing(TrackRect(), 10);
}

void ToggleSwitch::OnPointerEnter()
{
    Hover_.To(1, Motion::HoverEnterMs);
    StartAnimation();
}

void ToggleSwitch::OnPointerLeave()
{
    Hover_.To(0, Motion::HoverLeaveMs);
    StartAnimation();
}

void ToggleSwitch::OnPointerDown(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_LEFT)
        E.Handled = true;
}

void ToggleSwitch::OnPointerUp(PointerEvent &E)
{
    if (Bounds().Contains(E.Position))
        SetOn(!On_);
}

void ToggleSwitch::OnKeyDown(KeyEvent &E)
{
    if (E.Key == HZ_VK_SPACE) {
        E.Handled = true;
        SetOn(!On_);
    }
}

bool ToggleSwitch::Animate()
{
    Invalidate();
    return Knob_.Running() || Hover_.Running();
}

/* ---- CheckBox and RadioButton ------------------------------------------------------------------- */

CheckBox::CheckBox(const char *Label, bool Checked) : Text(Label), Checked_(Checked)
{
    Focusable = true;
}

void CheckBox::SetChecked(bool Checked, bool Notify)
{
    if (Checked == Checked_)
        return;
    Checked_ = Checked;
    Invalidate();
    if (Notify && OnChanged)
        OnChanged(Checked);
}

Rect CheckBox::BoxRect() const
{
    return Rect{Bounds().X, Bounds().Y + (Bounds().Height - 20) / 2, 20, 20};
}

Size CheckBox::MeasureOverride(Size)
{
    int32_t Label = TextWidth(FontRole::Body, Text);
    return Size{20 + (Label ? 8 + Label : 0), Theme::ControlHeight};
}

void CheckBox::Paint(Painter &P)
{
    bool Enabled = IsEnabled(), Pressed = IsPressed(), Hover = IsHovered();
    Rect Box = BoxRect();
    if (Checked_) {
        Color Fill = !Enabled ? Theme::AccentDisabled
                              : (Pressed ? Theme::AccentPressed : (Hover ? Theme::AccentHover : Theme::Accent));
        P.FillRoundRect(Box, Theme::ControlRadius, Fill);
        P.DrawIcon(Icon::Check, Box.Deflate(Thickness(2)), Enabled ? Theme::TextOnAccent : Theme::TextDisabled, 1.6f);
    } else {
        Color Fill = Pressed ? Theme::ControlFillPressed : (Hover ? Theme::ControlFillHover : Theme::ControlFill);
        P.FillRoundRect(Box, Theme::ControlRadius, Enabled ? Fill : Theme::ControlFillDisabled);
        P.StrokeRoundRect(Box, Theme::ControlRadius, 1,
                          Enabled ? Theme::ControlStrongStroke : Theme::ControlStrongStrokeDisabled);
    }
    DrawLabel(P, Rect{Box.Right() + 8, Bounds().Y, Bounds().Width - 28, Bounds().Height}, Text,
              Enabled ? Theme::TextPrimary : Theme::TextDisabled);
}

void CheckBox::PaintOverlay(Painter &P)
{
    if (ShowsFocusRing())
        P.FocusRing(BoxRect(), Theme::ControlRadius);
}

void CheckBox::OnPointerEnter()
{
    Invalidate();
}

void CheckBox::OnPointerLeave()
{
    Invalidate();
}

void CheckBox::OnPointerDown(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_LEFT)
        E.Handled = true;
}

void CheckBox::OnPointerUp(PointerEvent &E)
{
    if (Bounds().Contains(E.Position))
        Toggle();
}

void CheckBox::OnKeyDown(KeyEvent &E)
{
    if (E.Key == HZ_VK_SPACE) {
        E.Handled = true;
        Toggle();
    }
}

RadioButton::RadioButton(const char *Label, const char *GroupName, bool Checked) : CheckBox(Label, Checked), Group(GroupName)
{
}

void RadioButton::Select(bool Notify)
{
    if (Element *Siblings = Parent()) {
        for (size_t i = 0; i < Siblings->ChildCount(); i++) {
            Element *Other = Siblings->ChildAt(i);
            const char *OtherGroup = Other->RadioGroup();
            if (Other != this && OtherGroup && Group == OtherGroup)
                static_cast<RadioButton *>(Other)->SetChecked(false, Notify);
        }
    }
    SetChecked(true, Notify);
}

void RadioButton::Paint(Painter &P)
{
    bool Enabled = IsEnabled(), Pressed = IsPressed(), Hover = IsHovered();
    Rect Box = BoxRect();
    float Cx = Box.X + 10.0f, Cy = Box.Y + 10.0f;
    if (Checked_) {
        Color Fill = !Enabled ? Theme::AccentDisabled
                              : (Pressed ? Theme::AccentPressed : (Hover ? Theme::AccentHover : Theme::Accent));
        P.FillCircle(Cx, Cy, 10, Fill);
        P.FillCircle(Cx, Cy, Pressed ? 3.5f : (Hover ? 5.0f : 4.0f), Enabled ? Theme::TextOnAccent : Theme::TextDisabled);
    } else {
        Color Fill = Pressed ? Theme::ControlFillPressed : (Hover ? Theme::ControlFillHover : Theme::ControlFill);
        P.FillCircle(Cx, Cy, 10, Enabled ? Fill : Theme::ControlFillDisabled);
        P.StrokeCircle(Cx, Cy, 9.5f, 1, Enabled ? Theme::ControlStrongStroke : Theme::ControlStrongStrokeDisabled);
    }
    DrawLabel(P, Rect{Box.Right() + 8, Bounds().Y, Bounds().Width - 28, Bounds().Height}, Text,
              Enabled ? Theme::TextPrimary : Theme::TextDisabled);
}

void RadioButton::PaintOverlay(Painter &P)
{
    if (ShowsFocusRing())
        P.FocusRing(BoxRect(), 10);
}

/* ---- Slider ---------------------------------------------------------------------------------------- */

Slider::Slider(float Min, float Max, float V) : Minimum(Min), Maximum(Max), Value_(V)
{
    Focusable = true;
    MinWidth = 120;
}

void Slider::SetValue(float V, bool Notify)
{
    V = hz::Clamp(V, Minimum, Maximum);
    if (Step > 0) {
        float Steps = (V - Minimum) / Step;
        V = Minimum + (float)(int64_t)(Steps + 0.5f) * Step;
        V = hz::Clamp(V, Minimum, Maximum);
    }
    if (V == Value_)
        return;
    Value_ = V;
    Invalidate();
    if (Notify && OnValueChanged)
        OnValueChanged(V);
}

Rect Slider::TrackRect() const
{
    return Rect{Bounds().X + 10, Bounds().Y + Bounds().Height / 2 - 2, hz::Max(Bounds().Width - 20, 1), 4};
}

float Slider::ThumbX() const
{
    Rect T = TrackRect();
    float Range = Maximum > Minimum ? Maximum - Minimum : 1;
    return T.X + (Value_ - Minimum) / Range * T.Width;
}

Size Slider::MeasureOverride(Size)
{
    return Size{200, Theme::ControlHeight};
}

void Slider::Paint(Painter &P)
{
    bool Enabled = IsEnabled();
    Rect T = TrackRect();
    float X = ThumbX();
    P.FillRoundRect(T, 2, Enabled ? Theme::ControlStrongStroke : Theme::ControlStrongStrokeDisabled);
    P.FillRoundRect((float)T.X, (float)T.Y, X - T.X, (float)T.Height, 2, Enabled ? Theme::Accent : Theme::AccentDisabled);
    float Cy = T.Y + 2.0f, Hover = Hover_.Value();
    P.FillCircle(X, Cy, 10, 0xFF45454C);
    P.StrokeCircle(X, Cy, 9.5f, 1, Theme::ControlStroke);
    float Inner = IsPressed() ? 5.0f : 6.0f + Hover;
    P.FillCircle(X, Cy, Inner, Enabled ? (IsPressed() ? Theme::AccentPressed : Theme::Accent) : Theme::AccentDisabled);
}

void Slider::PaintOverlay(Painter &P)
{
    if (ShowsFocusRing()) {
        int32_t X = (int32_t)ThumbX();
        P.FocusRing(Rect{X - 10, TrackRect().Y + 2 - 10, 20, 20}, 10);
    }
}

void Slider::OnPointerEnter()
{
    Hover_.To(1, Motion::HoverEnterMs);
    StartAnimation();
}

void Slider::OnPointerLeave()
{
    Hover_.To(0, Motion::HoverLeaveMs);
    StartAnimation();
}

void Slider::SetFromPointer(int32_t X)
{
    Rect T = TrackRect();
    float Fraction = hz::Clamp((float)(X - T.X) / (float)T.Width, 0.0f, 1.0f);
    SetValue(Minimum + Fraction * (Maximum - Minimum));
}

void Slider::OnPointerDown(PointerEvent &E)
{
    if (E.Button != HZ_MOUSE_LEFT)
        return;
    E.Handled = true;
    SetFromPointer(E.Position.X);
}

void Slider::OnPointerMove(PointerEvent &E)
{
    if (GetWindow() && GetWindow()->PressedElement() == this)
        SetFromPointer(E.Position.X);
}

void Slider::OnPointerUp(PointerEvent &)
{
    Invalidate();
}

void Slider::OnPointerWheel(PointerEvent &E)
{
    SetValue(Value_ + (E.WheelDelta > 0 ? Step : -Step));
    E.Handled = true;
}

void Slider::OnKeyDown(KeyEvent &E)
{
    float Big = (Maximum - Minimum) / 10;
    switch (E.Key) {
    case HZ_VK_LEFT:
    case HZ_VK_DOWN:
        SetValue(Value_ - Step);
        break;
    case HZ_VK_RIGHT:
    case HZ_VK_UP:
        SetValue(Value_ + Step);
        break;
    case HZ_VK_PRIOR:
        SetValue(Value_ + Big);
        break;
    case HZ_VK_NEXT:
        SetValue(Value_ - Big);
        break;
    case HZ_VK_HOME:
        SetValue(Minimum);
        break;
    case HZ_VK_END:
        SetValue(Maximum);
        break;
    default:
        return;
    }
    E.Handled = true;
}

bool Slider::Animate()
{
    Invalidate();
    return Hover_.Running();
}

/* ---- ProgressBar ---------------------------------------------------------------------------------- */

void ProgressBar::SetValue(float Percent)
{
    Value_ = hz::Clamp(Percent, 0.0f, 100.0f);
    Invalidate();
}

void ProgressBar::SetIndeterminate(bool On)
{
    Indeterminate_ = On;
    StartUs_ = NowUs();
    if (On)
        StartAnimation();
    Invalidate();
}

Size ProgressBar::MeasureOverride(Size)
{
    return Size{100, 4};
}

void ProgressBar::Paint(Painter &P)
{
    const Rect &B = Bounds();
    float Cy = B.Y + B.Height / 2.0f;
    P.FillRoundRect((float)B.X, Cy - 0.5f, (float)B.Width, 1, 0.5f, Theme::ControlStrongStroke);
    Color Bar = IsEnabled() ? Theme::Accent : Theme::AccentDisabled;
    if (!Indeterminate_) {
        float W = B.Width * Value_ / 100;
        if (W > 0)
            P.FillRoundRect((float)B.X, Cy - 1.5f, hz::Max(W, 3.0f), 3, 1.5f, Bar);
        return;
    }
    /* A segment 40% wide sweeping across every 1.6 s. */
    float T = (float)((NowUs() - StartUs_) % 1600000) / 1600000.0f;
    float Segment = B.Width * 0.4f;
    float Left = B.X - Segment + T * (B.Width + Segment);
    float L = hz::Max(Left, (float)B.X), R = hz::Min(Left + Segment, (float)B.Right());
    if (R > L)
        P.FillRoundRect(L, Cy - 1.5f, R - L, 3, 1.5f, Bar);
}

bool ProgressBar::Animate()
{
    Invalidate();
    return Indeterminate_;
}

/* ---- ComboBox ---------------------------------------------------------------------------------------- */

ComboBox::ComboBox()
{
    Focusable = true;
    MinWidth = 120;
}

void ComboBox::SetSelectedIndex(size_t Index, bool Notify)
{
    if (Index != hz::NotFound && Index >= Items.Size())
        return;
    if (Index == Selected_)
        return;
    Selected_ = Index;
    Invalidate();
    if (Notify && OnSelectionChanged)
        OnSelectionChanged(Index);
}

Size ComboBox::MeasureOverride(Size)
{
    int32_t W = TextWidth(FontRole::Body, Placeholder);
    for (const hz::String &Item : Items)
        W = hz::Max(W, TextWidth(FontRole::Body, Item));
    return Size{W + 11 + 38, Theme::ControlHeight};
}

void ComboBox::Paint(Painter &P)
{
    bool Enabled = IsEnabled();
    Color Fill = !Enabled ? Theme::ControlFillDisabled
                          : (IsPressed() ? Theme::ControlFillPressed
                                         : (IsHovered() ? Theme::ControlFillHover : Theme::ControlFill));
    const Rect &B = Bounds();
    P.FillRoundRect(B, Theme::ControlRadius, Fill);
    P.StrokeRoundRect(B, Theme::ControlRadius, 1, Theme::ControlStroke);
    bool HasValue = Selected_ != hz::NotFound;
    const hz::String &Label = HasValue ? Items[Selected_] : Placeholder;
    Color Text = !Enabled ? Theme::TextDisabled : (HasValue ? Theme::TextPrimary : Theme::TextSecondary);
    P.TextLine(GetFont(FontRole::Body), Rect{B.X + 11, B.Y, B.Width - 11 - 38, B.Height}, Label.CStr(), Text);
    P.DrawIcon(Icon::ChevronDown, Rect{B.Right() - 30, B.Y + (B.Height - 12) / 2, 12, 12},
               Enabled ? Theme::TextSecondary : Theme::TextDisabled, 1.4f);
}

void ComboBox::PaintOverlay(Painter &P)
{
    if (ShowsFocusRing())
        P.FocusRing(Bounds(), Theme::ControlRadius);
}

void ComboBox::OnPointerEnter()
{
    Invalidate();
}

void ComboBox::OnPointerLeave()
{
    Invalidate();
}

void ComboBox::OnPointerDown(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_LEFT)
        E.Handled = true;
}

void ComboBox::OnPointerUp(PointerEvent &E)
{
    if (Bounds().Contains(E.Position))
        Open();
}

void ComboBox::Open()
{
    Window *W = GetWindow();
    if (!W || Items.Empty())
        return;
    hz::Vector<MenuItem> Entries;
    for (size_t i = 0; i < Items.Size(); i++) {
        MenuItem Item;
        Item.Text = Items[i];
        Item.Checked = i == Selected_;
        Item.Action = [this, i]() { SetSelectedIndex(i); };
        Entries.Push(hz::Move(Item));
    }
    Point Origin = W->ScreenOrigin();
    ShowMenu(W, Point{Origin.X + Bounds().X, Origin.Y + Bounds().Bottom() + 2}, hz::Move(Entries), Bounds().Width,
             Selected_ == hz::NotFound ? 0 : (int32_t)Selected_);
}

void ComboBox::OnKeyDown(KeyEvent &E)
{
    switch (E.Key) {
    case HZ_VK_SPACE:
    case HZ_VK_RETURN:
        Open();
        break;
    case HZ_VK_DOWN:
        if (E.Alt())
            Open();
        else if (!Items.Empty())
            SetSelectedIndex(Selected_ == hz::NotFound ? 0 : hz::Min(Selected_ + 1, Items.Size() - 1));
        break;
    case HZ_VK_UP:
        if (!Items.Empty())
            SetSelectedIndex(Selected_ == hz::NotFound || Selected_ == 0 ? 0 : Selected_ - 1);
        break;
    default:
        return;
    }
    E.Handled = true;
}

/* ---- TabView ----------------------------------------------------------------------------------- */

class TabHeader final : public Element {
public:
    /* The strip is one tab stop: only the selected header takes the focus; arrows switch tabs. */
    TabHeader(TabView *Owner, size_t Index, const char *Title) : Owner_(Owner), Index_(Index), Title_(Title)
    {
        Focusable = Index == Owner->SelectedTab();
        Padding = Thickness(12, 0);
    }

    /* Room for the semibold title of the selected tab, so selecting does not move the others. */
    Size MeasureOverride(Size) override
    {
        return Size{hz::Max(TextWidth(FontRole::Body, Title_), TextWidth(FontRole::BodyStrong, Title_)), 40};
    }

    void Paint(Painter &P) override
    {
        bool Selected = Owner_->SelectedTab() == Index_;
        const Rect &B = Bounds();
        if (IsHovered() && !Selected)
            P.FillRoundRect(B.Deflate(Thickness(0, 4)), Theme::ControlRadius,
                            IsPressed() ? Theme::SubtleFillPressed : Theme::SubtleFillHover);
        Color Text = Selected || IsHovered() ? Theme::TextPrimary : Theme::TextSecondary;
        HZ_FONT *F = GetFont(Selected ? FontRole::BodyStrong : FontRole::Body);
        P.TextLine(F, ContentBounds(), Title_.CStr(), Text, Align::Center);
        if (Selected)
            P.FillRoundRect(B.X + (B.Width - 16) / 2.0f, (float)B.Bottom() - 3, 16, 3, 1.5f, Theme::Accent);
    }

    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing())
            P.FocusRing(Bounds().Deflate(Thickness(0, 4)), Theme::ControlRadius);
    }

    void OnPointerEnter() override { Invalidate(); }
    void OnPointerLeave() override { Invalidate(); }

    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT) {
            E.Handled = true;
            Owner_->SelectTab(Index_);
        }
    }

    void OnKeyDown(KeyEvent &E) override
    {
        size_t Count = Owner_->TabCount();
        if (E.Key == HZ_VK_LEFT || E.Key == HZ_VK_RIGHT) {
            size_t Next = E.Key == HZ_VK_RIGHT ? (Index_ + 1) % Count : (Index_ + Count - 1) % Count;
            Owner_->SelectTab(Next);
            if (Window *W = GetWindow())
                W->SetFocus(Owner_->Headers_[Next], true);
            E.Handled = true;
        } else if (E.Key == HZ_VK_SPACE || E.Key == HZ_VK_RETURN) {
            Owner_->SelectTab(Index_);
            E.Handled = true;
        }
    }

private:
    TabView *Owner_;
    size_t Index_;
    hz::String Title_;
};

TabView::TabView()
{
    Strip_ = Add<StackPanel>(Orientation::Horizontal, 4);
}

Element *TabView::AddTab(const char *Title, Element *Content)
{
    size_t Index = Headers_.Size();
    Headers_.Push(Strip_->Add<TabHeader>(this, Index, Title));
    Pages_.Push(Content);
    Content->SetVisible(Index == Selected_);
    AddChild(Content);
    return Content;
}

void TabView::SelectTab(size_t Index, bool Notify)
{
    if (Index >= Pages_.Size() || Index == Selected_)
        return;
    bool HadFocus = Pages_[Selected_]->HasFocusWithin() || Strip_->HasFocusWithin();
    Pages_[Selected_]->SetVisible(false);
    Headers_[Selected_]->Focusable = false;
    Selected_ = Index;
    Headers_[Index]->Focusable = true;
    Pages_[Index]->SetVisible(true);
    if (HadFocus && GetWindow()) /* the focus was in the strip or hidden with the page: follow the selection */
        GetWindow()->SetFocus(Headers_[Index], GetWindow()->FocusVisible());
    for (Element *Header : Headers_)
        Header->Invalidate();
    InvalidateLayout();
    if (Notify && OnTabChanged)
        OnTabChanged(Index);
}

Size TabView::MeasureOverride(Size Available)
{
    Strip_->Measure(Available);
    int32_t StripHeight = Strip_->DesiredSize().Height;
    Size Total{Strip_->DesiredSize().Width, StripHeight};
    for (Element *Page : Pages_) {
        Page->Measure(Size{Available.Width, Available.Height >= Infinite ? Infinite : hz::Max(Available.Height - StripHeight - 8, 0)});
        Total.Width = hz::Max(Total.Width, Page->DesiredSize().Width);
        if (Page->IsVisible())
            Total.Height = StripHeight + 8 + Page->DesiredSize().Height;
    }
    return Total;
}

void TabView::ArrangeOverride(const Rect &Content)
{
    int32_t StripHeight = Strip_->DesiredSize().Height;
    Strip_->Arrange(Rect{Content.X, Content.Y, Content.Width, StripHeight});
    Rect PageArea{Content.X, Content.Y + StripHeight + 8, Content.Width, hz::Max(Content.Height - StripHeight - 8, 0)};
    for (Element *Page : Pages_)
        Page->Arrange(PageArea);
}

void TabView::OnKeyDown(KeyEvent &E)
{
    if (E.Key == HZ_VK_TAB && E.Ctrl() && !Pages_.Empty()) {
        size_t Count = Pages_.Size();
        SelectTab(E.Shift() ? (Selected_ + Count - 1) % Count : (Selected_ + 1) % Count);
        E.Handled = true;
    }
}

Rect TabView::HeaderRect(size_t Index) const
{
    return Index < Headers_.Size() ? Headers_[Index]->Bounds() : Rect{};
}

}  // namespace hzui
