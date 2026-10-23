/*
 * Nexora OS - user/lib/hzui/textbox.cpp
 * The single-line text box and the clipboard (win32k's, shared by every process since M8).
 * Offsets are UTF-8 byte offsets that always sit on code point boundaries; x positions come from
 * the font's fractional advances.
 */
#include "hzuip.h"

namespace hzui {

namespace {

hz::String g_Clipboard;

constexpr uint32_t kBlinkMs = 530;

bool IsWordSpace(char C)
{
    return C == ' ' || C == '\t';
}

}  // namespace

void Clipboard::SetText(const char *Text, size_t Length)
{
    if (Length > HZ_MAX_CLIPBOARD) /* cut on a code point boundary */
        for (Length = HZ_MAX_CLIPBOARD; Length > 0 && ((uint8_t)Text[Length] & 0xC0) == 0x80; Length--)
            ;
    NtUserSetClipboardText(Text, (uint32_t)Length);
}

const hz::String &Clipboard::GetText()
{
    uint32_t Length = 0;
    NtUserGetClipboardText(nullptr, 0, &Length);
    char *Buffer = (char *)malloc((size_t)Length + 1);
    if (Buffer && HZ_SUCCESS(NtUserGetClipboardText(Buffer, Length + 1, &Length)))
        g_Clipboard = hz::String(Buffer, Length);
    else
        g_Clipboard.Clear();
    free(Buffer);
    return g_Clipboard;
}

TextBox::TextBox(const char *Hint) : Placeholder(Hint)
{
    Focusable = true;
    MinWidth = 120;
}

Size TextBox::MeasureOverride(Size)
{
    return Size{200, Theme::ControlHeight};
}

Rect TextBox::TextArea() const
{
    return Bounds().Deflate(Thickness(11, 0));
}

const hz::String &TextBox::Shown() const
{
    if (!Password)
        return Text_;
    Masked_ = hz::String();
    for (size_t i = 0; i < Text_.Length(); i = Text_.NextChar(i))
        Masked_.Append("\xE2\x80\xA2"); /* U+2022 BULLET */
    return Masked_;
}

size_t TextBox::ShownOffset(size_t Offset) const
{
    Offset = hz::Min(Offset, Text_.Length());
    if (!Password)
        return Offset;
    size_t Characters = 0;
    for (size_t i = 0; i < Offset; i = Text_.NextChar(i))
        Characters++;
    return Characters * 3;
}

int32_t TextBox::OffsetToX(size_t Offset) const
{
    HZ_FONT *F = GetFont(FontRole::Body);
    int32_t Fixed = F ? HzMeasureTextFixed(F, Shown().CStr(), ShownOffset(Offset)) : 0;
    return TextArea().X - ScrollX_ + (Fixed + 32) / 64;
}

size_t TextBox::XToOffset(int32_t X) const
{
    size_t Best = 0;
    int32_t BestDistance = Infinite;
    for (size_t i = 0;; i = Text_.NextChar(i)) {
        int32_t D = OffsetToX(i) - X;
        if (D < 0)
            D = -D;
        if (D < BestDistance) {
            BestDistance = D;
            Best = i;
        }
        if (i >= Text_.Length())
            break;
    }
    return Best;
}

void TextBox::EnsureCaretVisible()
{
    HZ_FONT *F = GetFont(FontRole::Body);
    if (!F)
        return;
    int32_t Width = TextArea().Width;
    if (Width <= 0) { /* not laid out yet (text set before the window is shown): show the start */
        ScrollX_ = 0;
        return;
    }
    const hz::String &Text = Shown();
    int32_t Caret = (HzMeasureTextFixed(F, Text.CStr(), ShownOffset(Caret_)) + 32) / 64;
    int32_t Total = HzMeasureTextN(F, Text.CStr(), Text.Length());
    if (Caret - ScrollX_ > Width - 2)
        ScrollX_ = Caret - Width + 2;
    if (Caret - ScrollX_ < 0)
        ScrollX_ = Caret;
    if (Total - ScrollX_ < Width - 2) /* do not leave empty space on the right */
        ScrollX_ = hz::Max(Total - Width + 2, 0);
    ScrollX_ = hz::Max(ScrollX_, 0);
}

void TextBox::ResetBlink()
{
    BlinkStartUs_ = NowUs();
}

void TextBox::Changed()
{
    EnsureCaretVisible();
    ResetBlink();
    Invalidate();
    if (OnTextChanged) {
        hz::Function<void(const hz::String &)> Callback = OnTextChanged;
        Callback(Text_);
    }
}

void TextBox::SetText(const char *Text, bool Notify)
{
    hz::String New(Text);
    if (New.Length() > MaxLength) {
        size_t Cut = 0;
        while (Cut < New.Length() && New.NextChar(Cut) <= MaxLength)
            Cut = New.NextChar(Cut);
        New = New.Substring(0, Cut);
    }
    Text_ = hz::Move(New);
    Caret_ = Anchor_ = Text_.Length();
    ScrollX_ = 0;
    EnsureCaretVisible();
    Invalidate();
    if (Notify && OnTextChanged)
        OnTextChanged(Text_);
}

void TextBox::Select(size_t Start, size_t End)
{
    Anchor_ = hz::Min(Start, Text_.Length());
    Caret_ = hz::Min(End, Text_.Length());
    EnsureCaretVisible();
    ResetBlink();
    Invalidate();
}

void TextBox::MoveCaret(size_t To, bool Extend)
{
    Caret_ = hz::Min(To, Text_.Length());
    if (!Extend)
        Anchor_ = Caret_;
    EnsureCaretVisible();
    ResetBlink();
    Invalidate();
}

void TextBox::ReplaceSelection(const char *Text, size_t Length)
{
    size_t Start = SelectionStart(), Count = SelectionEnd() - Start;
    size_t Room = MaxLength - (Text_.Length() - Count);
    if (Length > Room) { /* cut on a code point boundary */
        size_t Cut = 0;
        hz::String Insert(Text, Length);
        while (Cut < Insert.Length() && Insert.NextChar(Cut) <= Room)
            Cut = Insert.NextChar(Cut);
        Length = Cut;
    }
    if (Count == 0 && Length == 0)
        return;
    Text_.Erase(Start, Count);
    Text_.Insert(Start, Text, Length);
    Caret_ = Anchor_ = Start + Length;
    Changed();
}

size_t TextBox::WordLeft(size_t From) const
{
    if (Password)
        return 0;
    size_t i = From;
    while (i > 0 && IsWordSpace(Text_[Text_.PreviousChar(i)]))
        i = Text_.PreviousChar(i);
    while (i > 0 && !IsWordSpace(Text_[Text_.PreviousChar(i)]))
        i = Text_.PreviousChar(i);
    return i;
}

size_t TextBox::WordRight(size_t From) const
{
    size_t i = From, Length = Text_.Length();
    if (Password)
        return Length;
    while (i < Length && !IsWordSpace(Text_[i]))
        i = Text_.NextChar(i);
    while (i < Length && IsWordSpace(Text_[i]))
        i = Text_.NextChar(i);
    return i;
}

void TextBox::Paint(Painter &P)
{
    bool Enabled = IsEnabled(), Focused = IsFocused() && GetWindow() && GetWindow()->IsActive();
    const Rect &B = Bounds();
    Color Fill = !Enabled ? Theme::ControlFillDisabled
                          : (IsFocused() ? Theme::InputActive : (IsHovered() ? Theme::ControlFillHover : Theme::ControlFill));
    P.FillRoundRect(B, Theme::ControlRadius, Fill);
    P.StrokeRoundRect(B, Theme::ControlRadius, 1, Theme::ControlStroke);
    if (IsFocused() && Enabled) {
        P.PushClip(Rect{B.X, B.Bottom() - 2, B.Width, 2});
        P.FillRoundRect(B, Theme::ControlRadius, Theme::Accent);
        P.PopClip();
    } else {
        P.PushClip(Rect{B.X, B.Bottom() - 1, B.Width, 1});
        P.StrokeRoundRect(B, Theme::ControlRadius, 1, Enabled ? Theme::ControlStrongStroke : Theme::ControlStroke);
        P.PopClip();
    }

    HZ_FONT *F = GetFont(FontRole::Body);
    if (!F)
        return;
    Rect Area = TextArea();
    P.PushClip(Area);
    int32_t Baseline = B.Y + (B.Height - HzFontLineHeight(F)) / 2 + HzFontAscent(F);
    if (IsFocused() && HasSelection()) {
        int32_t X0 = OffsetToX(SelectionStart()), X1 = OffsetToX(SelectionEnd());
        P.FillRect(Rect{X0, B.Y + 6, X1 - X0, B.Height - 12}, Theme::AccentSelection);
    }
    if (Text_.Empty()) {
        P.Text(F, Area.X, Baseline, Placeholder.CStr(), Placeholder.Length(),
               Enabled ? Theme::TextSecondary : Theme::TextDisabled);
    } else {
        const hz::String &Text = Shown();
        P.Text(F, Area.X - ScrollX_, Baseline, Text.CStr(), Text.Length(),
               Enabled ? Theme::TextPrimary : Theme::TextDisabled);
    }
    if (Focused && Enabled && ((NowUs() - BlinkStartUs_) / (kBlinkMs * 1000)) % 2 == 0) {
        int32_t X = OffsetToX(Caret_);
        P.FillRect(Rect{X, B.Y + 7, 1, B.Height - 14}, Theme::TextPrimary);
    }
    P.PopClip();
}

void TextBox::PaintOverlay(Painter &P)
{
}

void TextBox::OnPointerEnter()
{
    Invalidate();
}

void TextBox::OnPointerLeave()
{
    Invalidate();
}

void TextBox::OnPointerDown(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_RIGHT) {
        E.Handled = true;
        return;
    }
    if (E.Button != HZ_MOUSE_LEFT)
        return;
    E.Handled = true;
    size_t At = XToOffset(E.Position.X);
    if (E.ClickCount == 2) { /* select the word */
        if (Password) {
            SelectAll();
            return;
        }
        size_t Start = At, End = At;
        while (Start > 0 && !IsWordSpace(Text_[Text_.PreviousChar(Start)]))
            Start = Text_.PreviousChar(Start);
        while (End < Text_.Length() && !IsWordSpace(Text_[End]))
            End = Text_.NextChar(End);
        Select(Start, End);
        return;
    }
    Selecting_ = true;
    MoveCaret(At, (E.Modifiers & HZ_MOD_SHIFT) != 0);
}

void TextBox::OnPointerMove(PointerEvent &E)
{
    if (Selecting_)
        MoveCaret(XToOffset(E.Position.X), true);
}

void TextBox::OnPointerUp(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_RIGHT) {
        E.Handled = true;
        ShowContextMenu(E.ScreenPosition);
        return;
    }
    Selecting_ = false;
}

void TextBox::ShowContextMenu(Point Screen)
{
    Window *W = GetWindow();
    if (!W)
        return;
    hz::Vector<MenuItem> Items;
    MenuItem Cut = MenuItem::Make("Cut", [this]() {
        Clipboard::SetText(Text_.CStr() + SelectionStart(), SelectionEnd() - SelectionStart());
        ReplaceSelection("", 0);
    }, "Ctrl+X");
    Cut.Enabled = HasSelection() && !Password;
    MenuItem Copy = MenuItem::Make("Copy", [this]() {
        Clipboard::SetText(Text_.CStr() + SelectionStart(), SelectionEnd() - SelectionStart());
    }, "Ctrl+C");
    Copy.Enabled = HasSelection() && !Password;
    MenuItem Paste = MenuItem::Make("Paste", [this]() {
        const hz::String &Clip = Clipboard::GetText();
        ReplaceSelection(Clip.CStr(), Clip.Length());
    }, "Ctrl+V");
    Paste.Enabled = !Clipboard::GetText().Empty();
    Items.Push(hz::Move(Cut));
    Items.Push(hz::Move(Copy));
    Items.Push(hz::Move(Paste));
    Items.Push(MenuItem::MakeSeparator());
    Items.Push(MenuItem::Make("Select all", [this]() { SelectAll(); }, "Ctrl+A"));
    ShowMenu(W, Screen, hz::Move(Items), 160);
}

void TextBox::OnKeyDown(KeyEvent &E)
{
    bool Shift = E.Shift(), Ctrl = E.Ctrl();
    size_t Length = Text_.Length();
    switch (E.Key) {
    case HZ_VK_LEFT:
        if (HasSelection() && !Shift)
            MoveCaret(SelectionStart(), false);
        else
            MoveCaret(Ctrl ? WordLeft(Caret_) : Text_.PreviousChar(Caret_), Shift);
        break;
    case HZ_VK_RIGHT:
        if (HasSelection() && !Shift)
            MoveCaret(SelectionEnd(), false);
        else
            MoveCaret(Ctrl ? WordRight(Caret_) : Text_.NextChar(Caret_), Shift);
        break;
    case HZ_VK_HOME:
        MoveCaret(0, Shift);
        break;
    case HZ_VK_END:
        MoveCaret(Length, Shift);
        break;
    case HZ_VK_BACK:
        if (!HasSelection()) {
            if (Caret_ == 0)
                break;
            Anchor_ = Caret_;
            Caret_ = Ctrl ? WordLeft(Caret_) : Text_.PreviousChar(Caret_);
        }
        ReplaceSelection("", 0);
        break;
    case HZ_VK_DELETE:
        if (!HasSelection()) {
            if (Caret_ >= Length)
                break;
            Anchor_ = Caret_;
            Caret_ = Ctrl ? WordRight(Caret_) : Text_.NextChar(Caret_);
        }
        ReplaceSelection("", 0);
        break;
    case HZ_VK_RETURN:
        if (OnSubmit) {
            hz::Function<void(const hz::String &)> Callback = OnSubmit;
            Callback(Text_);
        }
        break;
    case 'A':
        if (!Ctrl)
            return;
        SelectAll();
        break;
    case 'C':
    case 'X':
        if (!Ctrl)
            return;
        if (HasSelection() && !Password) {
            Clipboard::SetText(Text_.CStr() + SelectionStart(), SelectionEnd() - SelectionStart());
            if (E.Key == 'X')
                ReplaceSelection("", 0);
        }
        break;
    case 'V':
        if (!Ctrl)
            return;
        {
            const hz::String &Clip = Clipboard::GetText();
            size_t Line = Clip.Find("\n");
            ReplaceSelection(Clip.CStr(), Line == hz::NotFound ? Clip.Length() : Line);
        }
        break;
    default:
        return;
    }
    E.Handled = true;
}

void TextBox::OnChar(KeyEvent &E)
{
    if (E.Ctrl() || E.Alt() || E.Character < 0x20 || E.Character == 0x7F)
        return;
    hz::String Bytes;
    Bytes.AppendCodePoint(E.Character);
    ReplaceSelection(Bytes.CStr(), Bytes.Length());
    E.Handled = true;
}

void TextBox::OnFocusChanged(bool Focused)
{
    Window *W = GetWindow();
    if (Focused) {
        ResetBlink();
        if (W && !BlinkTimer_)
            BlinkTimer_ = W->SetTimer(kBlinkMs, [this]() { Invalidate(); });
    } else {
        Selecting_ = false;
        if (W && BlinkTimer_)
            W->KillTimer(BlinkTimer_);
        BlinkTimer_ = 0;
    }
    Invalidate();
}

void TextBox::OnWindowActivated(bool)
{
    ResetBlink();
    Invalidate();
}

}  // namespace hzui
