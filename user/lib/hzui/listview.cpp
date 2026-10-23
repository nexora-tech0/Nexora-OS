/*
 * Nexora OS - user/lib/hzui/listview.cpp
 * The virtualized list: rows are not elements, so painting and hit testing cost depends on the
 * rows that are visible, not on the item count.
 */
#include "hzuip.h"

namespace hzui {

ListView::ListView()
{
    Focusable = true;
    ClipChildren = true;
    Bar_ = Add<ScrollBar>();
    Bar_->OnScroll = [this](int32_t Offset) { SetOffset(Offset, false); };
}

void ListView::SetItemCount(size_t Count)
{
    Count_ = Count;
    if (Selected_ != hz::NotFound && Selected_ >= Count)
        Selected_ = hz::NotFound;
    Hot_ = hz::NotFound;
    Offset_ = hz::Clamp(Offset_, 0, MaxOffset());
    Scroll_.Jump((float)Offset_);
    ArrangeOverride(ContentBounds());
    Invalidate();
}

int32_t ListView::MaxOffset() const
{
    return hz::Max((int32_t)Count_ * ItemHeight - ContentBounds().Height, 0);
}

Size ListView::MeasureOverride(Size Available)
{
    Bar_->Measure(Available);
    return Size{200, hz::Min<int32_t>((int32_t)Count_ * ItemHeight, 360)};
}

void ListView::ArrangeOverride(const Rect &Content)
{
    Bar_->SetRange((int32_t)Count_ * ItemHeight, Content.Height);
    Offset_ = hz::Clamp(Offset_, 0, MaxOffset());
    Bar_->SetOffset(Offset_, false);
    Bar_->Arrange(Rect{Content.Right() - 12, Content.Y, 12, Content.Height});
}

size_t ListView::IndexAt(Point P) const
{
    Rect Content = ContentBounds();
    if (!Content.Contains(P))
        return hz::NotFound;
    size_t Index = (size_t)((P.Y - Content.Y + Offset_) / ItemHeight);
    return Index < Count_ ? Index : hz::NotFound;
}

void ListView::Paint(Painter &P)
{
    Element::Paint(P);
    RowsPainted_ = 0;
    Rect Content = ContentBounds();
    P.PushClip(Content);
    HZ_FONT *F = GetFont(FontRole::Body);
    int32_t RowWidth = Content.Width - 8 - (Bar_->IsNeeded() ? 8 : 0);
    for (size_t i = (size_t)(Offset_ / ItemHeight); i < Count_; i++) {
        int32_t Top = Content.Y + (int32_t)i * ItemHeight - Offset_;
        if (Top >= Content.Bottom())
            break;
        Rect Row{Content.X + 4, Top + 2, RowWidth, ItemHeight - 4};
        if (!P.IsVisible(Row))
            continue;
        RowsPainted_++;
        bool Selected = i == Selected_, Hot = i == Hot_;
        if (Selected) {
            P.FillRoundRect(Row, Theme::ControlRadius, Hot ? Theme::ControlFillHover : Theme::ControlFill);
            P.FillRoundRect((float)Row.X, Row.Y + (Row.Height - 16) / 2.0f, 3, 16, 1.5f, Theme::Accent);
        } else if (Hot) {
            P.FillRoundRect(Row, Theme::ControlRadius, Theme::SubtleFillHover);
        }
        Rect Text = Row.Deflate(Thickness(12, 0, 8, 0));
        if (PaintItem)
            PaintItem(P, Text, i);
        else if (ItemText && F)
            P.TextLine(F, Text, ItemText(i).CStr(), IsEnabled() ? Theme::TextPrimary : Theme::TextDisabled);
    }
    P.PopClip();
}

void ListView::PaintOverlay(Painter &P)
{
    if (!ShowsFocusRing())
        return;
    Rect Content = ContentBounds();
    if (Selected_ != hz::NotFound) {
        int32_t Top = Content.Y + (int32_t)Selected_ * ItemHeight - Offset_;
        Rect Row{Content.X + 4, Top + 2, Content.Width - 8 - (Bar_->IsNeeded() ? 8 : 0), ItemHeight - 4};
        if (!Row.Intersect(Content).Empty()) {
            P.PushClip(Content);
            P.FocusRing(Row.Deflate(Thickness(3)), Theme::ControlRadius);
            P.PopClip();
            return;
        }
    }
    P.FocusRing(Bounds(), Theme::ControlRadius);
}

void ListView::SetSelectedIndex(size_t Index, bool Notify)
{
    if (Index != hz::NotFound && Index >= Count_)
        return;
    if (Index == Selected_)
        return;
    Selected_ = Index;
    Invalidate();
    if (Notify && OnSelectionChanged)
        OnSelectionChanged(Index);
}

void ListView::SetOffset(int32_t Offset, bool Animated)
{
    Offset = hz::Clamp(Offset, 0, MaxOffset());
    if (Animated) {
        Scroll_.To((float)Offset, 150);
        StartAnimation();
        return;
    }
    Scroll_.Jump((float)Offset);
    if (Offset != Offset_) {
        Offset_ = Offset;
        Bar_->SetOffset(Offset_, false);
        Invalidate();
    }
}

bool ListView::Animate()
{
    int32_t Offset = (int32_t)(Scroll_.Value() + 0.5f);
    if (Offset != Offset_) {
        Offset_ = Offset;
        Bar_->SetOffset(Offset_, false);
        Invalidate();
    }
    return Scroll_.Running();
}

void ListView::ScrollIntoView(size_t Index)
{
    if (Index >= Count_)
        return;
    int32_t Target = Scroll_.Running() ? (int32_t)Scroll_.Target() : Offset_;
    int32_t Top = (int32_t)Index * ItemHeight, Height = ContentBounds().Height;
    if (Top < Target)
        SetOffset(Top, true);
    else if (Top + ItemHeight > Target + Height)
        SetOffset(Top + ItemHeight - Height, true);
}

void ListView::OnPointerLeave()
{
    if (Hot_ != hz::NotFound) {
        Hot_ = hz::NotFound;
        Invalidate();
    }
}

void ListView::OnPointerMove(PointerEvent &E)
{
    size_t Hot = IndexAt(E.Position);
    if (Hot != Hot_) {
        Hot_ = Hot;
        Invalidate();
    }
}

void ListView::OnPointerDown(PointerEvent &E)
{
    if (E.Button != HZ_MOUSE_LEFT && E.Button != HZ_MOUSE_RIGHT)
        return;
    E.Handled = E.Button == HZ_MOUSE_LEFT;
    size_t Index = IndexAt(E.Position);
    if (Index == hz::NotFound)
        return;
    SetSelectedIndex(Index);
    if (E.Button == HZ_MOUSE_LEFT && E.ClickCount == 2 && OnItemInvoked)
        OnItemInvoked(Index);
}

void ListView::OnPointerWheel(PointerEvent &E)
{
    if (MaxOffset() == 0)
        return;
    int32_t Target = Scroll_.Running() ? (int32_t)Scroll_.Target() : Offset_;
    SetOffset(Target - E.WheelDelta * 3 * ItemHeight / 2 / 120, true);
    E.Handled = true;
}

void ListView::OnKeyDown(KeyEvent &E)
{
    if (Count_ == 0)
        return;
    size_t Page = (size_t)hz::Max(ContentBounds().Height / ItemHeight - 1, 1);
    size_t Current = Selected_ == hz::NotFound ? 0 : Selected_;
    size_t Next = Current;
    switch (E.Key) {
    case HZ_VK_UP:
        Next = Selected_ == hz::NotFound ? 0 : (Current > 0 ? Current - 1 : 0);
        break;
    case HZ_VK_DOWN:
        Next = Selected_ == hz::NotFound ? 0 : hz::Min(Current + 1, Count_ - 1);
        break;
    case HZ_VK_HOME:
        Next = 0;
        break;
    case HZ_VK_END:
        Next = Count_ - 1;
        break;
    case HZ_VK_PRIOR:
        Next = Current > Page ? Current - Page : 0;
        break;
    case HZ_VK_NEXT:
        Next = hz::Min(Current + Page, Count_ - 1);
        break;
    case HZ_VK_RETURN:
        if (Selected_ != hz::NotFound && OnItemInvoked)
            OnItemInvoked(Selected_);
        E.Handled = true;
        return;
    default:
        return;
    }
    SetSelectedIndex(Next);
    ScrollIntoView(Next);
    E.Handled = true;
}

}  // namespace hzui
