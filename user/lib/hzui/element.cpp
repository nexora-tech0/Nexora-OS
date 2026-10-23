/*
 * Nexora OS - user/lib/hzui/element.cpp
 * The element tree: ownership, state queries, the two-pass layout (Measure/Arrange with margins,
 * padding, fixed and minimum/maximum sizes, alignment), hit testing and painting; the panels
 * (StackPanel, Grid), TextBlock, and scrolling (ScrollBar, ScrollViewer).
 */
#include "hzui.h"

namespace hzui {

namespace {

inline int32_t Shrink(int32_t Value, int32_t By)
{
    return Value >= Infinite ? Infinite : hz::Max(Value - By, 0);
}

void PaintElement(Element *E, Painter &P)
{
    if (!E->IsVisible() || !P.IsVisible(E->Bounds().Inflate(4)))
        return;
    E->Paint(P);
    if (E->ClipChildren)
        P.PushClip(E->Bounds());
    for (size_t i = 0; i < E->ChildCount(); i++)
        PaintElement(E->ChildAt(i), P);
    if (E->ClipChildren)
        P.PopClip();
    E->PaintOverlay(P);
}

}  // namespace

/* ---- Tree ------------------------------------------------------------------------------------------ */

Element::Element()
{
}

Element::~Element()
{
    for (Element *Child : Children_) {
        Child->Parent_ = nullptr;
        delete Child;
    }
    Children_.Clear();
    if (Window_)
        Window_->ForgetElement(this);
}

Element *Element::AddChild(Element *Child)
{
    Child->Parent_ = this;
    Children_.Push(Child);
    if (Window_)
        Child->Attach(Window_);
    InvalidateLayout();
    return Child;
}

void Element::RemoveChild(Element *Child)
{
    if (Children_.Remove(Child)) {
        Child->Parent_ = nullptr;
        delete Child;
        InvalidateLayout();
    }
}

void Element::ClearChildren()
{
    while (!Children_.Empty())
        RemoveChild(Children_.Back());
}

void Element::Attach(Window *W)
{
    Window_ = W;
    for (Element *Child : Children_)
        Child->Attach(W);
}

void Element::DetachFromWindow()
{
    if (Window_)
        Window_->ForgetElement(this);
    Window_ = nullptr;
    for (Element *Child : Children_)
        Child->DetachFromWindow();
}

Element *Element::FindByName(const char *Wanted)
{
    if (Name == Wanted)
        return this;
    for (Element *Child : Children_)
        if (Element *Found = Child->FindByName(Wanted))
            return Found;
    return nullptr;
}

bool Element::IsAncestorOf(const Element *Other) const
{
    for (const Element *E = Other ? Other->Parent_ : nullptr; E; E = E->Parent_)
        if (E == this)
            return true;
    return false;
}

/* ---- State --------------------------------------------------------------------------------------- */

void Element::SetVisible(bool Visible)
{
    if (Visible_ == Visible)
        return;
    Invalidate();
    Visible_ = Visible;
    if (!Visible && Window_)
        Window_->ForgetElement(this);
    InvalidateLayout();
}

void Element::SetEnabled(bool Enabled)
{
    if (Enabled_ == Enabled)
        return;
    Enabled_ = Enabled;
    if (!Enabled && Window_ && (IsFocused() || HasFocusWithin()))
        Window_->SetFocus(nullptr);
    Invalidate();
}

bool Element::IsEnabled() const
{
    for (const Element *E = this; E; E = E->Parent_)
        if (!E->Enabled_)
            return false;
    return true;
}

bool Element::IsHovered() const
{
    Element *Hover = Window_ ? Window_->Hover_ : nullptr;
    return Hover && (Hover == this || IsAncestorOf(Hover));
}

bool Element::IsPressed() const
{
    return Window_ && Window_->Pressed_ == this && IsHovered();
}

bool Element::IsFocused() const
{
    return Window_ && Window_->Focus_ == this;
}

bool Element::HasFocusWithin() const
{
    Element *Focus = Window_ ? Window_->Focus_ : nullptr;
    return Focus && (Focus == this || IsAncestorOf(Focus));
}

bool Element::ShowsFocusRing() const
{
    return IsFocused() && Window_->FocusVisible_ && Window_->IsActive();
}

void Element::Focus()
{
    if (Window_)
        Window_->SetFocus(this);
}

void Element::Invalidate()
{
    if (Window_ && Visible_)
        Window_->InvalidateRect(Bounds_.Inflate(4)); /* focus rings lie outside */
}

void Element::InvalidateLayout()
{
    if (Window_)
        Window_->InvalidateLayout();
}

void Element::StartAnimation()
{
    if (Window_)
        Window_->RequestAnimation(this);
}

void Element::BringIntoView(const Rect &R)
{
    if (Parent_)
        Parent_->BringIntoView(R);
}

/* ---- Layout ------------------------------------------------------------------------------------- */

void Element::Measure(Size Available)
{
    if (!Visible_) {
        Desired_ = Size{};
        return;
    }
    int32_t AvailableWidth = Shrink(Available.Width, Margin.Horizontal());
    int32_t AvailableHeight = Shrink(Available.Height, Margin.Vertical());
    if (Width >= 0)
        AvailableWidth = Width;
    if (Height >= 0)
        AvailableHeight = Height;
    AvailableWidth = hz::Min(AvailableWidth, MaxWidth);
    AvailableHeight = hz::Min(AvailableHeight, MaxHeight);
    Size Content = MeasureOverride(
        Size{Shrink(AvailableWidth, Padding.Horizontal()), Shrink(AvailableHeight, Padding.Vertical())});
    int32_t W = Width >= 0 ? Width : Content.Width + Padding.Horizontal();
    int32_t H = Height >= 0 ? Height : Content.Height + Padding.Vertical();
    W = hz::Clamp(W, MinWidth, hz::Max(MaxWidth, MinWidth));
    H = hz::Clamp(H, MinHeight, hz::Max(MaxHeight, MinHeight));
    Desired_ = Size{W + Margin.Horizontal(), H + Margin.Vertical()};
}

void Element::Arrange(const Rect &Slot)
{
    if (!Visible_) {
        Bounds_ = Rect{Slot.X, Slot.Y, 0, 0};
        return;
    }
    Rect R = Slot.Deflate(Margin);
    int32_t DesiredWidth = Desired_.Width - Margin.Horizontal(), DesiredHeight = Desired_.Height - Margin.Vertical();
    int32_t W = Width >= 0 ? Width
                           : (HorizontalAlignment == Align::Stretch ? R.Width : hz::Min(DesiredWidth, R.Width));
    int32_t H = Height >= 0 ? Height
                            : (VerticalAlignment == Align::Stretch ? R.Height : hz::Min(DesiredHeight, R.Height));
    W = hz::Clamp(W, MinWidth, hz::Max(MaxWidth, MinWidth));
    H = hz::Clamp(H, MinHeight, hz::Max(MaxHeight, MinHeight));
    int32_t X = R.X, Y = R.Y;
    if (HorizontalAlignment == Align::Center)
        X += (R.Width - W) / 2;
    else if (HorizontalAlignment == Align::End)
        X += R.Width - W;
    if (VerticalAlignment == Align::Center)
        Y += (R.Height - H) / 2;
    else if (VerticalAlignment == Align::End)
        Y += R.Height - H;
    Bounds_ = Rect{X, Y, W, H};
    ArrangeOverride(ContentBounds());
}

Size Element::MeasureOverride(Size Available)
{
    Size Largest;
    for (Element *Child : Children_) {
        Child->Measure(Available);
        Largest.Width = hz::Max(Largest.Width, Child->DesiredSize().Width);
        Largest.Height = hz::Max(Largest.Height, Child->DesiredSize().Height);
    }
    return Largest;
}

void Element::ArrangeOverride(const Rect &Content)
{
    for (Element *Child : Children_)
        Child->Arrange(Content);
}

void Element::Paint(Painter &P)
{
    if (Background >> 24)
        P.FillRoundRect(Bounds_, CornerRadius, Background);
    if (BorderColor >> 24)
        P.StrokeRoundRect(Bounds_, CornerRadius, 1, BorderColor);
}

Element *Element::HitTest(Point P)
{
    if (!Visible_ || !Bounds_.Contains(P))
        return nullptr;
    for (size_t i = Children_.Size(); i-- > 0;)
        if (Element *Hit = Children_[i]->HitTest(P))
            return Hit;
    return HitTestVisible ? this : nullptr;
}

void LayoutTree(Element *Root, const Rect &Bounds)
{
    Root->Measure(Size{Bounds.Width, Bounds.Height});
    Root->Arrange(Bounds);
}

void PaintTree(Element *Root, Painter &P)
{
    PaintElement(Root, P);
}

/* ---- StackPanel ------------------------------------------------------------------------------------ */

Size StackPanel::MeasureOverride(Size Available)
{
    Size Total;
    int Visible = 0;
    for (size_t i = 0; i < ChildCount(); i++) {
        Element *Child = ChildAt(i);
        if (!Child->IsVisible()) {
            Child->Measure(Available);
            continue;
        }
        Visible++;
        if (Direction == Orientation::Vertical) {
            Child->Measure(Size{Available.Width, Infinite});
            Total.Height += Child->DesiredSize().Height;
            Total.Width = hz::Max(Total.Width, Child->DesiredSize().Width);
        } else {
            Child->Measure(Size{Infinite, Available.Height});
            Total.Width += Child->DesiredSize().Width;
            Total.Height = hz::Max(Total.Height, Child->DesiredSize().Height);
        }
    }
    int32_t Gaps = Visible > 1 ? Spacing * (Visible - 1) : 0;
    if (Direction == Orientation::Vertical)
        Total.Height += Gaps;
    else
        Total.Width += Gaps;
    return Total;
}

void StackPanel::ArrangeOverride(const Rect &Content)
{
    int32_t Position = Direction == Orientation::Vertical ? Content.Y : Content.X;
    for (size_t i = 0; i < ChildCount(); i++) {
        Element *Child = ChildAt(i);
        if (!Child->IsVisible()) {
            Child->Arrange(Rect{Content.X, Content.Y, 0, 0});
            continue;
        }
        if (Direction == Orientation::Vertical) {
            Child->Arrange(Rect{Content.X, Position, Content.Width, Child->DesiredSize().Height});
            Position += Child->DesiredSize().Height + Spacing;
        } else {
            Child->Arrange(Rect{Position, Content.Y, Child->DesiredSize().Width, Content.Height});
            Position += Child->DesiredSize().Width + Spacing;
        }
    }
}

/* ---- Grid ------------------------------------------------------------------------------------------- */

void Grid::ComputeTracks(Size Space, bool Arranging)
{
    size_t ColumnCount = hz::Max<size_t>(Columns.Size(), 1), RowCount = hz::Max<size_t>(Rows.Size(), 1);
    auto Column = [&](size_t i) { return Columns.Empty() ? GridLength::Stars(1) : Columns[i]; };
    auto Row = [&](size_t i) { return Rows.Empty() ? GridLength::Stars(1) : Rows[i]; };
    auto CellColumn = [&](Element *C) { return hz::Min<size_t>(C->GridColumn, ColumnCount - 1); };
    auto CellRow = [&](Element *C) { return hz::Min<size_t>(C->GridRow, RowCount - 1); };

    ColumnSizes_.Resize(ColumnCount);
    RowSizes_.Resize(RowCount);
    for (int32_t &Size : ColumnSizes_)
        Size = 0;
    for (int32_t &Size : RowSizes_)
        Size = 0;

    /* Columns: pixels, then content sizes (auto, and star when the width is unconstrained). */
    if (!Arranging) {
        for (size_t i = 0; i < ChildCount(); i++)
            ChildAt(i)->Measure(hzui::Size{Infinite, Infinite});
    }
    bool WidthUnbounded = Space.Width >= Infinite;
    float Stars = 0;
    for (size_t c = 0; c < ColumnCount; c++) {
        GridLength L = Column(c);
        if (L.Type == GridLength::Pixel)
            ColumnSizes_[c] = (int32_t)L.Value;
        else if (L.Type == GridLength::Star)
            Stars += L.Value;
    }
    for (size_t i = 0; i < ChildCount(); i++) {
        Element *C = ChildAt(i);
        size_t c = CellColumn(C);
        GridLength L = Column(c);
        if (C->IsVisible() && C->GridColumnSpan == 1 &&
            (L.Type == GridLength::Auto || (L.Type == GridLength::Star && WidthUnbounded)))
            ColumnSizes_[c] = hz::Max(ColumnSizes_[c], C->DesiredSize().Width);
    }
    int32_t Gaps = ColumnSpacing * (int32_t)(ColumnCount - 1);
    if (!WidthUnbounded && Stars > 0) {
        int32_t Used = Gaps;
        for (size_t c = 0; c < ColumnCount; c++)
            if (Column(c).Type != GridLength::Star)
                Used += ColumnSizes_[c];
        int32_t Remaining = hz::Max(Space.Width - Used, 0), Given = 0;
        size_t Last = 0;
        for (size_t c = 0; c < ColumnCount; c++) {
            if (Column(c).Type == GridLength::Star) {
                ColumnSizes_[c] = (int32_t)(Remaining * Column(c).Value / Stars);
                Given += ColumnSizes_[c];
                Last = c;
            }
        }
        ColumnSizes_[Last] += Remaining - Given; /* rounding */
    }

    auto SpanWidth = [&](Element *C) {
        size_t First = CellColumn(C), End = hz::Min<size_t>(First + C->GridColumnSpan, ColumnCount);
        int32_t W = ColumnSpacing * (int32_t)(End - First - 1);
        for (size_t c = First; c < End; c++)
            W += ColumnSizes_[c];
        return W;
    };

    /* Rows, with children measured at their final widths (wrapping text gets its height). */
    bool HeightUnbounded = Space.Height >= Infinite;
    Stars = 0;
    for (size_t r = 0; r < RowCount; r++) {
        GridLength L = Row(r);
        if (L.Type == GridLength::Pixel)
            RowSizes_[r] = (int32_t)L.Value;
        else if (L.Type == GridLength::Star)
            Stars += L.Value;
    }
    for (size_t i = 0; i < ChildCount(); i++) {
        Element *C = ChildAt(i);
        size_t r = CellRow(C);
        GridLength L = Row(r);
        if (!Arranging)
            C->Measure(hzui::Size{SpanWidth(C), L.Type == GridLength::Pixel ? (int32_t)L.Value : Infinite});
        if (C->IsVisible() && C->GridRowSpan == 1 &&
            (L.Type == GridLength::Auto || (L.Type == GridLength::Star && HeightUnbounded)))
            RowSizes_[r] = hz::Max(RowSizes_[r], C->DesiredSize().Height);
    }
    Gaps = RowSpacing * (int32_t)(RowCount - 1);
    if (!HeightUnbounded && Stars > 0) {
        int32_t Used = Gaps;
        for (size_t r = 0; r < RowCount; r++)
            if (Row(r).Type != GridLength::Star)
                Used += RowSizes_[r];
        int32_t Remaining = hz::Max(Space.Height - Used, 0), Given = 0;
        size_t Last = 0;
        for (size_t r = 0; r < RowCount; r++) {
            if (Row(r).Type == GridLength::Star) {
                RowSizes_[r] = (int32_t)(Remaining * Row(r).Value / Stars);
                Given += RowSizes_[r];
                Last = r;
            }
        }
        RowSizes_[Last] += Remaining - Given;
    }
}

Size Grid::MeasureOverride(Size Available)
{
    ComputeTracks(Available, false);
    Size Total{ColumnSpacing * (int32_t)(ColumnSizes_.Size() - 1), RowSpacing * (int32_t)(RowSizes_.Size() - 1)};
    for (int32_t W : ColumnSizes_)
        Total.Width += W;
    for (int32_t H : RowSizes_)
        Total.Height += H;
    return Total;
}

void Grid::ArrangeOverride(const Rect &Content)
{
    ComputeTracks(Size{Content.Width, Content.Height}, true);
    size_t ColumnCount = ColumnSizes_.Size(), RowCount = RowSizes_.Size();
    for (size_t i = 0; i < ChildCount(); i++) {
        Element *C = ChildAt(i);
        size_t Column = hz::Min<size_t>(C->GridColumn, ColumnCount - 1), Row = hz::Min<size_t>(C->GridRow, RowCount - 1);
        int32_t X = Content.X, Y = Content.Y;
        for (size_t c = 0; c < Column; c++)
            X += ColumnSizes_[c] + ColumnSpacing;
        for (size_t r = 0; r < Row; r++)
            Y += RowSizes_[r] + RowSpacing;
        size_t ColumnEnd = hz::Min<size_t>(Column + C->GridColumnSpan, ColumnCount);
        size_t RowEnd = hz::Min<size_t>(Row + C->GridRowSpan, RowCount);
        int32_t W = ColumnSpacing * (int32_t)(ColumnEnd - Column - 1), H = RowSpacing * (int32_t)(RowEnd - Row - 1);
        for (size_t c = Column; c < ColumnEnd; c++)
            W += ColumnSizes_[c];
        for (size_t r = Row; r < RowEnd; r++)
            H += RowSizes_[r];
        C->Arrange(Rect{X, Y, W, H});
    }
}

/* ---- TextBlock ------------------------------------------------------------------------------------- */

TextBlock::TextBlock(const char *Text, FontRole Role) : Font(Role), Text_(Text)
{
    HitTestVisible = false;
}

void TextBlock::SetText(const char *Text)
{
    if (Text_ == Text)
        return;
    Text_ = Text;
    InvalidateLayout();
    Invalidate();
}

Size TextBlock::MeasureOverride(Size Available)
{
    HZ_FONT *F = GetFont(Font);
    if (!F)
        return Size{};
    if (Wrap && Available.Width < Infinite) {
        Layout_.Layout(F, Text_.CStr(), Text_.Length(), Available.Width);
        return Size{Layout_.Width, Layout_.Height};
    }
    return Size{HzMeasureTextN(F, Text_.CStr(), Text_.Length()), HzFontLineHeight(F)};
}

void TextBlock::ArrangeOverride(const Rect &Content)
{
    if (Wrap && GetFont(Font))
        Layout_.Layout(GetFont(Font), Text_.CStr(), Text_.Length(), Content.Width);
}

void TextBlock::Paint(Painter &P)
{
    Element::Paint(P);
    HZ_FONT *F = GetFont(Font);
    if (!F)
        return;
    Color C = IsEnabled() ? Foreground : Theme::TextDisabled;
    Rect Content = ContentBounds();
    if (!Wrap) {
        if (Ellipsis) {
            P.TextLine(F, Content, Text_.CStr(), C, TextAlignment);
        } else {
            P.PushClip(Bounds());
            int32_t Baseline = Content.Y + (Content.Height - HzFontLineHeight(F)) / 2 + HzFontAscent(F);
            P.Text(F, Content.X, Baseline, Text_.CStr(), Text_.Length(), C);
            P.PopClip();
        }
        return;
    }
    P.PushClip(Bounds());
    int32_t Y = Content.Y;
    for (const TextLine &Line : Layout_.Lines) {
        int32_t X = Content.X;
        if (TextAlignment == Align::Center)
            X += (Content.Width - Line.Width) / 2;
        else if (TextAlignment == Align::End)
            X += Content.Width - Line.Width;
        P.Text(F, X, Y + HzFontAscent(F), Text_.CStr() + Line.Start, Line.Length, C);
        Y += Layout_.LineHeight;
    }
    P.PopClip();
}

/* ---- ScrollBar ------------------------------------------------------------------------------------ */

ScrollBar::ScrollBar()
{
    HorizontalAlignment = Align::End;
}

void ScrollBar::SetRange(int32_t Extent, int32_t Viewport)
{
    if (Extent == Extent_ && Viewport == Viewport_)
        return;
    Extent_ = Extent;
    Viewport_ = Viewport;
    Offset_ = hz::Clamp(Offset_, 0, MaxOffset());
    Invalidate();
}

void ScrollBar::SetOffset(int32_t Offset, bool Notify)
{
    Offset = hz::Clamp(Offset, 0, MaxOffset());
    if (Offset == Offset_)
        return;
    Offset_ = Offset;
    Invalidate();
    if (Notify && OnScroll)
        OnScroll(Offset_);
}

Rect ScrollBar::ThumbRect() const
{
    Rect Track = Bounds().Deflate(Thickness(0, 4));
    if (!IsNeeded() || Track.Height <= 0)
        return Rect{};
    int32_t Length = hz::Max(Track.Height * Viewport_ / hz::Max(Extent_, 1), 24);
    Length = hz::Min(Length, Track.Height);
    int32_t Travel = Track.Height - Length;
    int32_t Y = Track.Y + (MaxOffset() ? (int32_t)((int64_t)Travel * Offset_ / MaxOffset()) : 0);
    return Rect{Track.X, Y, Track.Width, Length};
}

Element *ScrollBar::HitTest(Point P)
{
    return IsNeeded() ? Element::HitTest(P) : nullptr;
}

void ScrollBar::Paint(Painter &P)
{
    if (!IsNeeded())
        return;
    float Expand = Expand_.Value();
    if (Expand > 0.01f)
        P.FillRoundRect(Bounds().Deflate(Thickness(2, 2)), 4, ScaleAlpha(0x26000000, Expand));
    Rect Thumb = ThumbRect();
    float ThumbWidth = 2 + 4 * Expand;
    float X = Bounds().Right() - 3 - ThumbWidth;
    P.FillRoundRect(X, (float)Thumb.Y, ThumbWidth, (float)Thumb.Height, ThumbWidth / 2,
                    MixColors(Theme::TextTertiary, Theme::TextSecondary, Expand));
}

void ScrollBar::OnPointerEnter()
{
    Expand_.To(1, 150);
    StartAnimation();
}

void ScrollBar::OnPointerLeave()
{
    if (DragStartY_ < 0) {
        Expand_.To(0, 250);
        StartAnimation();
    }
}

void ScrollBar::OnPointerDown(PointerEvent &E)
{
    if (E.Button != HZ_MOUSE_LEFT || !IsNeeded())
        return;
    E.Handled = true;
    Rect Thumb = ThumbRect();
    if (E.Position.Y >= Thumb.Y && E.Position.Y < Thumb.Bottom()) {
        DragStartY_ = E.Position.Y;
        DragStartOffset_ = Offset_;
    } else {
        SetOffset(Offset_ + (E.Position.Y < Thumb.Y ? -Viewport_ : Viewport_));
    }
}

void ScrollBar::OnPointerMove(PointerEvent &E)
{
    if (DragStartY_ < 0)
        return;
    Rect Track = Bounds().Deflate(Thickness(0, 4));
    int32_t Travel = Track.Height - ThumbRect().Height;
    if (Travel > 0)
        SetOffset(DragStartOffset_ + (int32_t)((int64_t)(E.Position.Y - DragStartY_) * MaxOffset() / Travel));
}

void ScrollBar::OnPointerUp(PointerEvent &E)
{
    DragStartY_ = -1;
    if (!IsHovered()) {
        Expand_.To(0, 250);
        StartAnimation();
    }
}

bool ScrollBar::Animate()
{
    Invalidate();
    return Expand_.Running();
}

/* ---- ScrollViewer -------------------------------------------------------------------------------- */

ScrollViewer::ScrollViewer()
{
    ClipChildren = true;
    Bar_ = Add<ScrollBar>();
    Bar_->OnScroll = [this](int32_t Offset) { ScrollTo(Offset, false); };
}

void ScrollViewer::SetContent(Element *Content)
{
    if (Content_)
        RemoveChild(Content_);
    Content_ = Content;
    if (Content) {
        AddChild(Content);
        hz::Vector<Element *> &Children = ChildList(); /* keep the bar last: painted and hit on top */
        Children.Remove(Bar_);
        Children.Push(Bar_);
    }
}

Size ScrollViewer::MeasureOverride(Size Available)
{
    Bar_->Measure(Available);
    if (!Content_)
        return Size{};
    Content_->Measure(Size{Available.Width, Infinite});
    Extent_ = Content_->DesiredSize().Height;
    return Size{Content_->DesiredSize().Width, hz::Min(Extent_, Available.Height)};
}

void ScrollViewer::ApplyOffset()
{
    Rect Content = ContentBounds();
    if (Content_)
        Content_->Arrange(Rect{Content.X, Content.Y - Offset_, Content.Width, hz::Max(Extent_, Content.Height)});
    Bar_->SetRange(Extent_, Content.Height);
    Bar_->SetOffset(Offset_, false);
    Bar_->Arrange(Rect{Content.Right() - 12, Content.Y, 12, Content.Height});
}

void ScrollViewer::ArrangeOverride(const Rect &Content)
{
    int32_t Max = hz::Max(Extent_ - Content.Height, 0);
    if (Offset_ > Max) {
        Offset_ = Max;
        Scroll_.Jump((float)Offset_);
    }
    ApplyOffset();
}

void ScrollViewer::ScrollTo(int32_t Offset, bool Animated)
{
    Offset = hz::Clamp(Offset, 0, hz::Max(Extent_ - ContentBounds().Height, 0));
    if (Animated) {
        Scroll_.To((float)Offset, 180);
        StartAnimation();
        return;
    }
    Scroll_.Jump((float)Offset);
    if (Offset != Offset_) {
        Offset_ = Offset;
        ApplyOffset();
        Invalidate();
    }
}

bool ScrollViewer::Animate()
{
    int32_t Offset = (int32_t)(Scroll_.Value() + 0.5f);
    if (Offset != Offset_) {
        Offset_ = Offset;
        ApplyOffset();
        Invalidate();
    }
    return Scroll_.Running();
}

void ScrollViewer::OnPointerWheel(PointerEvent &E)
{
    if (Extent_ <= ContentBounds().Height)
        return;
    int32_t Target = (int32_t)Scroll_.Target();
    if (!Scroll_.Running())
        Target = Offset_;
    ScrollTo(Target - E.WheelDelta * 64 / 120, true);
    E.Handled = true;
}

void ScrollViewer::ScrollIntoView(const Rect &R)
{
    Rect View = ContentBounds();
    if (R.Y < View.Y)
        ScrollTo(Offset_ - (View.Y - R.Y) - 8, true);
    else if (R.Bottom() > View.Bottom())
        ScrollTo(Offset_ + (R.Bottom() - View.Bottom()) + 8, true);
}

void ScrollViewer::BringIntoView(const Rect &R)
{
    ScrollIntoView(R);
    Element::BringIntoView(R);
}

}  // namespace hzui
