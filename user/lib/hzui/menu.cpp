/*
 * Nexora OS - user/lib/hzui/menu.cpp
 * Menus and tooltips: popup windows owned by a window of the same thread. A menu holds the
 * explicit mouse capture (so a click anywhere else closes it), its owner forwards the keyboard to
 * it, and choosing an item closes it before the item's action runs.
 */
#include "hzuip.h"

namespace hzui {

namespace {

constexpr int32_t kItemHeight = 32;
constexpr int32_t kSeparatorHeight = 9;
constexpr int32_t kMenuPadding = 4;
constexpr int32_t kCheckColumn = 28;

class MenuWindow;

class MenuItemView final : public Element {
public:
    MenuItemView(MenuWindow *Menu, size_t Index) : Menu_(Menu), Index_(Index) {}
    Size MeasureOverride(Size) override;
    void Paint(Painter &P) override;
    void OnPointerEnter() override;
    void OnPointerDown(PointerEvent &E) override
    {
        if (E.Button == HZ_MOUSE_LEFT)
            E.Handled = true;
    }
    void OnPointerUp(PointerEvent &E) override;

private:
    MenuWindow *Menu_;
    size_t Index_;
};

class MenuWindow final : public PopupWindow {
public:
    MenuWindow(hz::Vector<MenuItem> Entries, int32_t Width, int32_t Height, bool CheckColumn)
        : PopupWindow(Width, Height), Items(hz::Move(Entries)), HasCheckColumn(CheckColumn)
    {
        StackPanel *List = new StackPanel(Orientation::Vertical, 0);
        List->Padding = Thickness(kMenuPadding);
        for (size_t i = 0; i < Items.Size(); i++)
            Views.Push(List->Add<MenuItemView>(this, i));
        SetContent(List);
    }

    bool Selectable(size_t Index) const { return !Items[Index].Separator && Items[Index].Enabled; }

    void SetHighlight(int32_t Index)
    {
        if (Index == Highlight)
            return;
        if (Highlight >= 0)
            Views[(size_t)Highlight]->Invalidate();
        Highlight = Index;
        if (Highlight >= 0)
            Views[(size_t)Highlight]->Invalidate();
    }

    void Invoke(size_t Index)
    {
        if (Index >= Items.Size() || !Selectable(Index))
            return;
        hz::Function<void()> Action = Items[Index].Action;
        if (Owner)
            Owner->ClosePopups(); /* this window is freed later, by the message loop */
        if (Action)
            Action();
    }

    bool HandlePopupKey(KeyEvent &E) override
    {
        int32_t Count = (int32_t)Items.Size();
        auto Step = [&](int32_t From, int32_t Direction) {
            for (int32_t n = 0; n < Count; n++) {
                From = (From + Direction + Count) % Count;
                if (Selectable((size_t)From))
                    return From;
            }
            return Highlight;
        };
        switch (E.Key) {
        case HZ_VK_DOWN:
            SetHighlight(Step(Highlight < 0 ? -1 : Highlight, 1));
            break;
        case HZ_VK_UP:
            SetHighlight(Step(Highlight < 0 ? 0 : Highlight, -1));
            break;
        case HZ_VK_HOME:
            SetHighlight(Step(-1, 1));
            break;
        case HZ_VK_END:
            SetHighlight(Step(0, -1));
            break;
        case HZ_VK_RETURN:
        case HZ_VK_SPACE:
            if (Highlight >= 0)
                Invoke((size_t)Highlight);
            break;
        case HZ_VK_ESCAPE:
        case HZ_VK_LEFT:
        case HZ_VK_TAB:
            if (Owner)
                Owner->ClosePopups();
            break;
        default:
            break;
        }
        E.Handled = true;
        return true;
    }

    void OnPopupPointerOutside() override
    {
        if (Owner)
            Owner->ClosePopups();
    }

    hz::Vector<MenuItem> Items;
    hz::Vector<MenuItemView *> Views;
    int32_t Highlight = -1;
    bool HasCheckColumn;
};

Size MenuItemView::MeasureOverride(Size)
{
    return Size{0, Menu_->Items[Index_].Separator ? kSeparatorHeight : kItemHeight};
}

void MenuItemView::Paint(Painter &P)
{
    const MenuItem &Item = Menu_->Items[Index_];
    const Rect &B = Bounds();
    if (Item.Separator) {
        P.FillRect(Rect{B.X - kMenuPadding, B.Y + B.Height / 2, B.Width + 2 * kMenuPadding, 1}, Theme::Divider);
        return;
    }
    bool Highlighted = Menu_->Highlight == (int32_t)Index_ && Item.Enabled;
    if (Highlighted)
        P.FillRoundRect(B, Theme::ControlRadius, IsPressed() ? Theme::SubtleFillPressed : Theme::SubtleFillHover);
    Color Text = Item.Enabled ? Theme::TextPrimary : Theme::TextDisabled;
    int32_t X = B.X + 12;
    if (Menu_->HasCheckColumn) {
        Rect Glyph{B.X + 10, B.Y + (B.Height - 16) / 2, 16, 16};
        if (Item.Checked)
            P.DrawIcon(Icon::Check, Glyph, Text, 1.4f);
        else if (Item.Glyph != Icon::None)
            P.DrawIcon(Item.Glyph, Glyph, Text);
        X = B.X + 12 + kCheckColumn - 4;
    }
    HZ_FONT *F = GetFont(FontRole::Body);
    int32_t ShortcutWidth = Item.Shortcut.Empty() ? 0 : HzMeasureTextN(F, Item.Shortcut.CStr(), Item.Shortcut.Length());
    P.TextLine(F, Rect{X, B.Y, B.Right() - 12 - X - (ShortcutWidth ? ShortcutWidth + 24 : 0), B.Height}, Item.Text.CStr(), Text);
    if (ShortcutWidth)
        P.TextLine(F, Rect{B.Right() - 12 - ShortcutWidth, B.Y, ShortcutWidth + 1, B.Height}, Item.Shortcut.CStr(),
                   Item.Enabled ? Theme::TextTertiary : Theme::TextDisabled);
}

void MenuItemView::OnPointerEnter()
{
    if (Menu_->Selectable(Index_))
        Menu_->SetHighlight((int32_t)Index_);
}

void MenuItemView::OnPointerUp(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_LEFT && Bounds().Contains(E.Position))
        Menu_->Invoke(Index_);
}

}  // namespace

bool ShowMenu(Window *Owner, Point Screen, hz::Vector<MenuItem> Items, int32_t MinWidth, int32_t Highlight)
{
    if (!Owner || !Owner->IsOpen() || Items.Empty())
        return false;
    Owner->ClosePopups();
    HZ_FONT *F = GetFont(FontRole::Body);
    int32_t TextWidth = 0, ShortcutWidth = 0, Height = 2 * kMenuPadding;
    bool CheckColumn = false;
    for (const MenuItem &Item : Items) {
        Height += Item.Separator ? kSeparatorHeight : kItemHeight;
        if (Item.Separator)
            continue;
        TextWidth = hz::Max(TextWidth, HzMeasureTextN(F, Item.Text.CStr(), Item.Text.Length()));
        ShortcutWidth = hz::Max(ShortcutWidth, HzMeasureTextN(F, Item.Shortcut.CStr(), Item.Shortcut.Length()));
        CheckColumn = CheckColumn || Item.Checked || Item.Glyph != Icon::None;
    }
    int32_t Width = 2 * kMenuPadding + 12 + (CheckColumn ? kCheckColumn - 4 : 0) + TextWidth +
                    (ShortcutWidth ? 24 + ShortcutWidth : 0) + 12;
    Width = hz::Max(Width, MinWidth);

    Rect Area = WorkArea();
    int32_t X = hz::Min(Screen.X, Area.Right() - Width), Y = Screen.Y;
    if (Y + Height > Area.Bottom())
        Y = hz::Max(Area.Bottom() - Height, Area.Y);
    X = hz::Max(X, Area.X);

    MenuWindow *Menu = new MenuWindow(hz::Move(Items), Width, Height, CheckColumn);
    Menu->Owner = Owner;
    if (Highlight >= 0 && (size_t)Highlight < Menu->Items.Size() && Menu->Selectable((size_t)Highlight))
        Menu->Highlight = Highlight;
    if (!Menu->ShowAt(X, Y)) {
        delete Menu;
        return false;
    }
    HzSetCapture(Menu->Handle());
    Owner->SetMenu(Menu);
    return true;
}

Window *CreateToolTip(const char *Text, Point Screen)
{
    HZ_FONT *F = GetFont(FontRole::Caption);
    if (!F)
        return nullptr;
    TextLayout Layout;
    Layout.Layout(F, Text, strlen(Text), 320);
    int32_t Width = Layout.Width + 18, Height = Layout.Height + 12;
    PopupWindow *Tip = new PopupWindow(Width, Height);
    TextBlock *Label = new TextBlock(Text, FontRole::Caption);
    Label->Wrap = true;
    Label->Margin = Thickness(9, 6);
    Tip->SetContent(Label);
    Rect Area = WorkArea();
    int32_t X = hz::Clamp(Screen.X - 8, Area.X, Area.Right() - Width);
    int32_t Y = Screen.Y + 24;
    if (Y + Height > Area.Bottom())
        Y = Screen.Y - Height - 8;
    if (!Tip->ShowAt(X, Y)) {
        delete Tip;
        return nullptr;
    }
    return Tip;
}

}  // namespace hzui
