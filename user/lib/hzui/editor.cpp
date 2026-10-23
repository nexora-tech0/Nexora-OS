/*
 * Nexora OS - user/lib/hzui/editor.cpp
 * The multi-line text editor (TextEditor). The document is a vector of lines; every change goes
 * through Replace(), and edits are recorded as (start, removed text, inserted text) so undo and
 * redo replay them in reverse. Consecutive typed characters are grouped into one edit until a
 * space starts a new word. Tabs become four spaces (there are no tab stops yet).
 */
#include "hzuip.h"

namespace hzui {

namespace {

constexpr uint32_t kBlinkMs = 530;
constexpr int32_t kLineGap = 2;

bool IsWordByte(char C)
{
    uint8_t B = (uint8_t)C;
    return (B >= '0' && B <= '9') || (B >= 'A' && B <= 'Z') || (B >= 'a' && B <= 'z') || B == '_' || B >= 0x80;
}

char Lower(char C)
{
    return (C >= 'A' && C <= 'Z') ? (char)(C + 32) : C;
}

/* Text as the editor stores it: no carriage returns, tabs as four spaces. */
hz::String Normalize(const char *Text, size_t Length)
{
    hz::String Out;
    for (size_t i = 0; i < Length; i++) {
        if (Text[i] == '\r')
            continue;
        if (Text[i] == '\t')
            Out.Append("    ");
        else
            Out.Append(Text[i]);
    }
    return Out;
}

/* Where text inserted at Start ends. */
TextPosition After(TextPosition Start, const hz::String &Text)
{
    size_t LastBreak = hz::NotFound, Breaks = 0;
    for (size_t i = 0; i < Text.Length(); i++) {
        if (Text[i] == '\n') {
            Breaks++;
            LastBreak = i;
        }
    }
    if (!Breaks)
        return TextPosition{Start.Line, Start.Column + Text.Length()};
    return TextPosition{Start.Line + Breaks, Text.Length() - LastBreak - 1};
}

}  // namespace

TextEditor::TextEditor()
{
    Focusable = true;
    ClipChildren = true;
    MinWidth = 100;
    MinHeight = 60;
    Lines_.Push(hz::String());
    Bar_ = Add<ScrollBar>();
    Bar_->OnScroll = [this](int32_t Offset) {
        ScrollY_ = Offset;
        Invalidate();
    };
}

/* ---- Document ------------------------------------------------------------------------------------- */

void TextEditor::SetText(const char *Text, size_t Length)
{
    hz::String Clean = Normalize(Text, Length);
    Lines_.Clear();
    size_t Start = 0;
    for (size_t i = 0; i <= Clean.Length(); i++) {
        if (i == Clean.Length() || Clean[i] == '\n') {
            Lines_.Push(Clean.Substring(Start, i - Start));
            Start = i + 1;
        }
    }
    Caret_ = Anchor_ = TextPosition{};
    ScrollX_ = ScrollY_ = 0;
    PreferredX_ = -1;
    RowsDirty_ = true;
    Undo_.Clear();
    Redo_.Clear();
    Modified = false;
    UpdateScrollBar();
    Invalidate();
    if (OnCaretMoved)
        OnCaretMoved();
}

hz::String TextEditor::Text() const
{
    hz::String Out;
    for (size_t i = 0; i < Lines_.Size(); i++) {
        if (i)
            Out.Append('\n');
        Out.Append(Lines_[i]);
    }
    return Out;
}

size_t TextEditor::CharacterCount() const
{
    size_t Count = Lines_.Size() - 1; /* line feeds */
    for (const hz::String &L : Lines_)
        for (size_t i = 0; i < L.Length(); i++)
            Count += ((uint8_t)L[i] & 0xC0) != 0x80;
    return Count;
}

TextPosition TextEditor::Clamp(TextPosition P) const
{
    P.Line = hz::Min(P.Line, Lines_.Size() - 1);
    P.Column = hz::Min(P.Column, Lines_[P.Line].Length());
    return P;
}

TextPosition TextEditor::Next(TextPosition P) const
{
    const hz::String &L = Lines_[P.Line];
    if (P.Column < L.Length())
        return TextPosition{P.Line, L.NextChar(P.Column)};
    if (P.Line + 1 < Lines_.Size())
        return TextPosition{P.Line + 1, 0};
    return P;
}

TextPosition TextEditor::Previous(TextPosition P) const
{
    if (P.Column > 0)
        return TextPosition{P.Line, Lines_[P.Line].PreviousChar(P.Column)};
    if (P.Line > 0)
        return TextPosition{P.Line - 1, Lines_[P.Line - 1].Length()};
    return P;
}

TextPosition TextEditor::WordLeft(TextPosition P) const
{
    if (P.Column == 0)
        return Previous(P);
    const hz::String &L = Lines_[P.Line];
    size_t i = P.Column;
    while (i > 0 && !IsWordByte(L[L.PreviousChar(i)]))
        i = L.PreviousChar(i);
    while (i > 0 && IsWordByte(L[L.PreviousChar(i)]))
        i = L.PreviousChar(i);
    return TextPosition{P.Line, i};
}

TextPosition TextEditor::WordRight(TextPosition P) const
{
    const hz::String &L = Lines_[P.Line];
    if (P.Column >= L.Length())
        return Next(P);
    size_t i = P.Column;
    while (i < L.Length() && IsWordByte(L[i]))
        i = L.NextChar(i);
    while (i < L.Length() && !IsWordByte(L[i]))
        i = L.NextChar(i);
    return TextPosition{P.Line, i};
}

hz::String TextEditor::Range(TextPosition Start, TextPosition End) const
{
    if (Start.Line == End.Line)
        return Lines_[Start.Line].Substring(Start.Column, End.Column - Start.Column);
    hz::String Out = Lines_[Start.Line].Substring(Start.Column);
    for (size_t Line = Start.Line + 1; Line < End.Line; Line++) {
        Out.Append('\n');
        Out.Append(Lines_[Line]);
    }
    Out.Append('\n');
    Out.Append(Lines_[End.Line].Substring(0, End.Column));
    return Out;
}

TextPosition TextEditor::Replace(TextPosition Start, TextPosition End, const char *Text, size_t Length)
{
    RowsDirty_ = true;
    hz::String Head = Lines_[Start.Line].Substring(0, Start.Column);
    hz::String Tail = Lines_[End.Line].Substring(End.Column);
    for (size_t Line = End.Line; Line > Start.Line; Line--)
        Lines_.Erase(Line);
    /* Split the new text into lines. */
    hz::Vector<hz::String> Pieces;
    size_t From = 0;
    for (size_t i = 0; i <= Length; i++) {
        if (i == Length || Text[i] == '\n') {
            Pieces.Push(hz::String(Text + From, i - From));
            From = i + 1;
        }
    }
    if (Pieces.Size() == 1) {
        Lines_[Start.Line] = Head;
        Lines_[Start.Line].Append(Pieces[0]);
        size_t Column = Lines_[Start.Line].Length();
        Lines_[Start.Line].Append(Tail);
        return TextPosition{Start.Line, Column};
    }
    Lines_[Start.Line] = Head;
    Lines_[Start.Line].Append(Pieces[0]);
    for (size_t i = 1; i < Pieces.Size(); i++)
        Lines_.Insert(Start.Line + i, Pieces[i]);
    size_t Last = Start.Line + Pieces.Size() - 1;
    size_t Column = Lines_[Last].Length();
    Lines_[Last].Append(Tail);
    return TextPosition{Last, Column};
}

hz::String TextEditor::SelectedText() const
{
    return Range(SelectionStart(), SelectionEnd());
}

void TextEditor::Select(TextPosition Anchor, TextPosition Caret)
{
    Anchor_ = Clamp(Anchor);
    Caret_ = Clamp(Caret);
    PreferredX_ = -1;
    EnsureCaretVisible();
    Invalidate();
    if (OnCaretMoved)
        OnCaretMoved();
}

void TextEditor::SelectAll()
{
    Select(TextPosition{}, TextPosition{Lines_.Size() - 1, Lines_.Back().Length()});
}

void TextEditor::Apply(const char *Text, size_t Length, bool Typing)
{
    if (ReadOnly)
        return;
    hz::String Clean = Normalize(Text, Length);
    TextPosition Start = SelectionStart(), End = SelectionEnd();
    if (Start == End && Clean.Empty())
        return;
    hz::String Removed = Range(Start, End);
    TextPosition CaretBefore = Caret_, AnchorBefore = Anchor_;
    TextPosition NewEnd = Replace(Start, End, Clean.CStr(), Clean.Length());
    /* Group typing: same place, no selection replaced, until a space starts a new word. */
    Edit *Last = Undo_.Empty() ? nullptr : &Undo_.Back();
    bool Group = Typing && Last && Last->Typing && Removed.Empty() && After(Last->Start, Last->Inserted) == Start &&
                 Clean.Find("\n") == hz::NotFound &&
                 !(Clean[0] == ' ' && Last->Inserted.Length() && Last->Inserted[Last->Inserted.Length() - 1] != ' ');
    if (Group) {
        Last->Inserted.Append(Clean);
    } else {
        Edit E;
        E.Start = Start;
        E.Removed = hz::Move(Removed);
        E.Inserted = hz::Move(Clean);
        E.CaretBefore = CaretBefore;
        E.AnchorBefore = AnchorBefore;
        E.Typing = Typing;
        Undo_.Push(hz::Move(E));
    }
    Redo_.Clear();
    Caret_ = Anchor_ = NewEnd;
    PreferredX_ = -1;
    Modified = true;
    Changed();
}

void TextEditor::Insert(const char *Text, size_t Length)
{
    Apply(Text, Length, false);
}

bool TextEditor::Undo()
{
    if (Undo_.Empty() || ReadOnly)
        return false;
    Edit E = hz::Move(Undo_.Back());
    Undo_.Pop();
    Replace(E.Start, After(E.Start, E.Inserted), E.Removed.CStr(), E.Removed.Length());
    Caret_ = Clamp(E.CaretBefore);
    Anchor_ = Clamp(E.AnchorBefore);
    E.Typing = false;
    Redo_.Push(hz::Move(E));
    Modified = true;
    Changed();
    return true;
}

bool TextEditor::Redo()
{
    if (Redo_.Empty() || ReadOnly)
        return false;
    Edit E = hz::Move(Redo_.Back());
    Redo_.Pop();
    TextPosition End = Replace(E.Start, After(E.Start, E.Removed), E.Inserted.CStr(), E.Inserted.Length());
    Caret_ = Anchor_ = End;
    Undo_.Push(hz::Move(E));
    Modified = true;
    Changed();
    return true;
}

void TextEditor::Copy()
{
    if (HasSelection()) {
        hz::String Text = SelectedText();
        Clipboard::SetText(Text.CStr(), Text.Length());
    }
}

void TextEditor::Cut()
{
    if (HasSelection() && !ReadOnly) {
        Copy();
        DeleteSelection();
    }
}

void TextEditor::Paste()
{
    const hz::String &Text = Clipboard::GetText();
    if (!Text.Empty())
        Insert(Text.CStr(), Text.Length());
}

bool TextEditor::Find(const char *Needle, bool Forward)
{
    size_t N = strlen(Needle);
    if (!N)
        return false;
    auto MatchAt = [&](size_t Line, size_t Column) {
        const hz::String &L = Lines_[Line];
        if (Column + N > L.Length())
            return false;
        for (size_t i = 0; i < N; i++)
            if (Lower(L[Column + i]) != Lower(Needle[i]))
                return false;
        return true;
    };
    size_t Count = Lines_.Size();
    TextPosition From = Forward ? SelectionEnd() : SelectionStart();
    for (size_t Step = 0; Step <= Count; Step++) {
        size_t Line = Forward ? (From.Line + Step) % Count : (From.Line + Count * 2 - Step) % Count;
        const hz::String &L = Lines_[Line];
        if (Forward) {
            size_t Begin = Step == 0 ? From.Column : 0;
            for (size_t c = Begin; c + N <= L.Length(); c++) {
                if (MatchAt(Line, c)) {
                    Select(TextPosition{Line, c}, TextPosition{Line, c + N});
                    return true;
                }
            }
        } else {
            size_t Limit = Step == 0 ? From.Column : L.Length();
            for (size_t c = Limit >= N ? Limit - N + 1 : 0; c-- > 0;) {
                if (MatchAt(Line, c)) {
                    Select(TextPosition{Line, c + N}, TextPosition{Line, c});
                    return true;
                }
            }
        }
    }
    return false;
}

void TextEditor::Changed()
{
    UpdateScrollBar();
    EnsureCaretVisible();
    BlinkStartUs_ = NowUs();
    Invalidate();
    if (OnChanged) {
        hz::Function<void()> Callback = OnChanged;
        Callback();
    }
    if (OnCaretMoved)
        OnCaretMoved();
}

/* ---- View ----------------------------------------------------------------------------------------- */

int32_t TextEditor::LineHeight() const
{
    HZ_FONT *F = GetFont(Font);
    return (F ? HzFontLineHeight(F) : 16) + kLineGap;
}

Rect TextEditor::TextArea() const
{
    return ContentBounds().Deflate(Thickness(10, 6, 16, 6));
}

/* ---- Rows (word wrap, M11) ---------------------------------------------------------------------- */

size_t TextEditor::RowCount() const
{
    EnsureRows();
    return Rows_.Size();
}

void TextEditor::EnsureRows() const
{
    int32_t Width = TextArea().Width;
    if (!RowsDirty_ && RowsWidth_ == Width && RowsFont_ == Font && RowsWrap_ == WordWrap)
        return;
    RowsDirty_ = false;
    RowsWidth_ = Width;
    RowsFont_ = Font;
    RowsWrap_ = WordWrap;
    Rows_.Clear();
    HZ_FONT *F = GetFont(Font);
    for (size_t Line = 0; Line < Lines_.Size(); Line++) {
        const hz::String &L = Lines_[Line];
        if (!WordWrap || !F || Width <= 20 || L.Empty()) {
            Rows_.Push(Row{Line, 0, L.Length()});
            continue;
        }
        size_t Start = 0;
        while (Start < L.Length()) {
            /* The longest piece that fits; then back up to just after the last space in it. */
            /* Widths add up in 26.6 fixed point (rounding each glyph would drift past the edge). */
            size_t End = Start, LastSpace = hz::NotFound;
            int64_t X = 0, Limit = (int64_t)(Width - 2) * 64;
            while (End < L.Length()) {
                size_t Next = L.NextChar(End);
                int64_t W = HzMeasureTextFixed(F, L.CStr() + End, Next - End);
                if (X + W > Limit && End > Start)
                    break;
                X += W;
                if (L[End] == ' ')
                    LastSpace = Next;
                End = Next;
            }
            if (End < L.Length() && LastSpace != hz::NotFound && LastSpace > Start)
                End = LastSpace;
            Rows_.Push(Row{Line, Start, End});
            Start = End;
        }
    }
}

size_t TextEditor::RowOf(TextPosition P) const
{
    EnsureRows();
    /* Binary search for the last row starting at or before P (a position at a wrap point
       belongs to the next row). */
    size_t Low = 0, High = Rows_.Size();
    while (High - Low > 1) {
        size_t Mid = (Low + High) / 2;
        const Row &R = Rows_[Mid];
        if (R.Line < P.Line || (R.Line == P.Line && R.Start <= P.Column))
            Low = Mid;
        else
            High = Mid;
    }
    return Low;
}

int32_t TextEditor::RowX(size_t RowIndex, size_t Column) const
{
    HZ_FONT *F = GetFont(Font);
    const Row &R = Rows_[RowIndex];
    size_t End = Column < R.Start ? R.Start : (Column > R.End ? R.End : Column);
    return F ? (HzMeasureTextFixed(F, Lines_[R.Line].CStr() + R.Start, End - R.Start) + 32) / 64 : 0;
}

size_t TextEditor::RowColumnAt(size_t RowIndex, int32_t X) const
{
    const Row &R = Rows_[RowIndex];
    const hz::String &L = Lines_[R.Line];
    bool LastOfLine = RowIndex + 1 >= Rows_.Size() || Rows_[RowIndex + 1].Line != R.Line;
    /* A wrapped row's end is the next row's start: stop before it. */
    size_t Limit = LastOfLine ? R.End : (R.End > R.Start ? L.PreviousChar(R.End) : R.End);
    size_t Best = R.Start;
    int32_t BestDistance = Infinite;
    for (size_t i = R.Start;; i = L.NextChar(i)) {
        int32_t D = RowX(RowIndex, i) - X;
        D = D < 0 ? -D : D;
        if (D < BestDistance) {
            BestDistance = D;
            Best = i;
        } else if (D > BestDistance) {
            break; /* positions only grow from here */
        }
        if (i >= Limit)
            break;
    }
    return Best;
}

TextPosition TextEditor::PositionAt(Point P) const
{
    EnsureRows();
    Rect Area = TextArea();
    int32_t Y = P.Y - Area.Y + ScrollY_;
    size_t RowIndex = Y < 0 ? 0 : hz::Min<size_t>((size_t)(Y / LineHeight()), Rows_.Size() - 1);
    return TextPosition{Rows_[RowIndex].Line, RowColumnAt(RowIndex, P.X - Area.X + ScrollX_)};
}

Point TextEditor::PointOf(TextPosition P) const
{
    Rect Area = TextArea();
    size_t RowIndex = RowOf(P);
    return Point{Area.X - ScrollX_ + RowX(RowIndex, P.Column), Area.Y - ScrollY_ + (int32_t)RowIndex * LineHeight()};
}

void TextEditor::UpdateScrollBar()
{
    Rect Area = TextArea();
    int32_t Extent = (int32_t)RowCount() * LineHeight();
    ScrollY_ = hz::Clamp(ScrollY_, 0, hz::Max(Extent - Area.Height, 0));
    Bar_->SetRange(Extent, Area.Height);
    Bar_->SetOffset(ScrollY_, false);
}

void TextEditor::EnsureCaretVisible()
{
    Rect Area = TextArea();
    if (Area.Empty())
        return;
    size_t RowIndex = RowOf(Caret_);
    int32_t Y = (int32_t)RowIndex * LineHeight();
    if (Y < ScrollY_)
        ScrollY_ = Y;
    if (Y + LineHeight() > ScrollY_ + Area.Height)
        ScrollY_ = Y + LineHeight() - Area.Height;
    int32_t X = RowX(RowIndex, Caret_.Column);
    if (WordWrap)
        ScrollX_ = 0;
    else if (X < ScrollX_)
        ScrollX_ = hz::Max(X - 40, 0);
    if (X > ScrollX_ + Area.Width - 2)
        ScrollX_ = X - Area.Width + 40;
    UpdateScrollBar();
}

void TextEditor::MoveCaret(TextPosition To, bool Extend, bool KeepColumn)
{
    Caret_ = Clamp(To);
    if (!Extend)
        Anchor_ = Caret_;
    if (!KeepColumn)
        PreferredX_ = -1;
    EnsureCaretVisible();
    BlinkStartUs_ = NowUs();
    Invalidate();
    if (OnCaretMoved)
        OnCaretMoved();
}

Size TextEditor::MeasureOverride(Size Available)
{
    Bar_->Measure(Available);
    return Size{200, 100};
}

void TextEditor::ArrangeOverride(const Rect &Content)
{
    Bar_->Arrange(Rect{Content.Right() - 12, Content.Y, 12, Content.Height});
    if (WordWrap)
        ScrollX_ = 0;
    UpdateScrollBar();
}

void TextEditor::Paint(Painter &P)
{
    const Rect &B = Bounds();
    bool Focused = IsFocused() && GetWindow() && GetWindow()->IsActive();
    if (Background >> 24) {
        P.FillRoundRect(B, CornerRadius, Background);
    } else {
        P.FillRoundRect(B, Theme::ControlRadius, IsFocused() ? Theme::InputActive : Theme::ControlFill);
        P.StrokeRoundRect(B, Theme::ControlRadius, 1, Theme::ControlStroke);
    }
    HZ_FONT *F = GetFont(Font);
    if (!F)
        return;
    Rect Area = TextArea();
    P.PushClip(Rect{Area.X - 4, Area.Y, Area.Width + 8, Area.Height});
    int32_t Height = LineHeight(), Ascent = HzFontAscent(F);
    TextPosition Start = SelectionStart(), End = SelectionEnd();
    EnsureRows();
    size_t First = (size_t)(ScrollY_ / Height);
    for (size_t RowIndex = First; RowIndex < Rows_.Size(); RowIndex++) {
        int32_t Y = Area.Y - ScrollY_ + (int32_t)RowIndex * Height;
        if (Y >= Area.Bottom())
            break;
        const Row &R = Rows_[RowIndex];
        size_t Line = R.Line;
        bool LastOfLine = RowIndex + 1 >= Rows_.Size() || Rows_[RowIndex + 1].Line != Line;
        if (HasSelection() && Line >= Start.Line && Line <= End.Line) {
            size_t From = Line == Start.Line ? hz::Max(Start.Column, R.Start) : R.Start;
            size_t To = Line == End.Line ? hz::Min(End.Column, R.End) : R.End;
            bool PastEnd = LastOfLine && Line != End.Line; /* the line feed is selected too */
            if (From < To || (PastEnd && From <= To && (Line != Start.Line || Start.Column <= R.End))) {
                int32_t X0 = RowX(RowIndex, From), X1 = RowX(RowIndex, To) + (PastEnd ? 7 : 0);
                P.FillRect(Rect{Area.X - ScrollX_ + X0, Y, X1 - X0, Height},
                           IsFocused() ? Theme::AccentSelection : 0x40FFFFFF);
            }
        }
        const hz::String &L = Lines_[Line];
        if (R.End > R.Start)
            P.Text(F, Area.X - ScrollX_, Y + kLineGap / 2 + Ascent, L.CStr() + R.Start, R.End - R.Start,
                   IsEnabled() ? Theme::TextPrimary : Theme::TextDisabled);
    }
    if (Focused && !ReadOnly && ((NowUs() - BlinkStartUs_) / (kBlinkMs * 1000)) % 2 == 0) {
        Point C = PointOf(Caret_);
        P.FillRect(Rect{C.X, C.Y + 1, 2, Height - 2}, Theme::TextPrimary);
    }
    P.PopClip();
}

/* ---- Input ---------------------------------------------------------------------------------------- */

void TextEditor::OnPointerDown(PointerEvent &E)
{
    if (E.Button == HZ_MOUSE_RIGHT) {
        E.Handled = true;
        Window *W = GetWindow();
        if (!W)
            return;
        hz::Vector<MenuItem> Items;
        MenuItem UndoItem = MenuItem::Make("Undo", [this]() { Undo(); }, "Ctrl+Z");
        UndoItem.Enabled = CanUndo() && !ReadOnly;
        MenuItem CutItem = MenuItem::Make("Cut", [this]() { Cut(); }, "Ctrl+X");
        CutItem.Enabled = HasSelection() && !ReadOnly;
        MenuItem CopyItem = MenuItem::Make("Copy", [this]() { Copy(); }, "Ctrl+C");
        CopyItem.Enabled = HasSelection();
        MenuItem PasteItem = MenuItem::Make("Paste", [this]() { Paste(); }, "Ctrl+V");
        PasteItem.Enabled = !ReadOnly;
        Items.Push(hz::Move(UndoItem));
        Items.Push(MenuItem::MakeSeparator());
        Items.Push(hz::Move(CutItem));
        Items.Push(hz::Move(CopyItem));
        Items.Push(hz::Move(PasteItem));
        Items.Push(MenuItem::MakeSeparator());
        Items.Push(MenuItem::Make("Select all", [this]() { SelectAll(); }, "Ctrl+A"));
        ShowMenu(W, E.ScreenPosition, hz::Move(Items), 180);
        return;
    }
    if (E.Button != HZ_MOUSE_LEFT)
        return;
    E.Handled = true;
    TextPosition At = PositionAt(E.Position);
    if (E.ClickCount == 2) {
        const hz::String &L = Lines_[At.Line];
        size_t S = At.Column, End = At.Column;
        while (S > 0 && IsWordByte(L[L.PreviousChar(S)]))
            S = L.PreviousChar(S);
        while (End < L.Length() && IsWordByte(L[End]))
            End = L.NextChar(End);
        Select(TextPosition{At.Line, S}, TextPosition{At.Line, End});
        return;
    }
    Selecting_ = true;
    MoveCaret(At, (E.Modifiers & HZ_MOD_SHIFT) != 0);
}

void TextEditor::OnPointerMove(PointerEvent &E)
{
    if (Selecting_)
        MoveCaret(PositionAt(E.Position), true);
}

void TextEditor::OnPointerUp(PointerEvent &E)
{
    Selecting_ = false;
}

void TextEditor::OnPointerWheel(PointerEvent &E)
{
    int32_t Extent = (int32_t)RowCount() * LineHeight();
    int32_t Limit = hz::Max(Extent - TextArea().Height, 0);
    if (Limit == 0)
        return;
    ScrollY_ = hz::Clamp(ScrollY_ - E.WheelDelta * 3 * LineHeight() / 120, 0, Limit);
    Bar_->SetOffset(ScrollY_, false);
    Invalidate();
    E.Handled = true;
}

void TextEditor::OnKeyDown(KeyEvent &E)
{
    bool Shift = E.Shift(), Ctrl = E.Ctrl();
    size_t Page = (size_t)hz::Max(TextArea().Height / LineHeight() - 1, 1);
    auto Vertical = [&](long Rows) {
        size_t Current = RowOf(Caret_);
        if (PreferredX_ < 0)
            PreferredX_ = RowX(Current, Caret_.Column);
        long Target = (long)Current + Rows;
        Target = Target < 0 ? 0 : (Target >= (long)Rows_.Size() ? (long)Rows_.Size() - 1 : Target);
        MoveCaret(TextPosition{Rows_[(size_t)Target].Line, RowColumnAt((size_t)Target, PreferredX_)}, Shift, true);
    };
    switch (E.Key) {
    case HZ_VK_LEFT:
        if (HasSelection() && !Shift)
            MoveCaret(SelectionStart(), false);
        else
            MoveCaret(Ctrl ? WordLeft(Caret_) : Previous(Caret_), Shift);
        break;
    case HZ_VK_RIGHT:
        if (HasSelection() && !Shift)
            MoveCaret(SelectionEnd(), false);
        else
            MoveCaret(Ctrl ? WordRight(Caret_) : Next(Caret_), Shift);
        break;
    case HZ_VK_UP:
        Vertical(-1);
        break;
    case HZ_VK_DOWN:
        Vertical(1);
        break;
    case HZ_VK_PRIOR:
        Vertical(-(long)Page);
        break;
    case HZ_VK_NEXT:
        Vertical((long)Page);
        break;
    case HZ_VK_HOME:
        if (Ctrl) {
            MoveCaret(TextPosition{}, Shift);
        } else { /* to the first non-space, then to the line start */
            const hz::String &L = Lines_[Caret_.Line];
            size_t Indent = 0;
            while (Indent < L.Length() && L[Indent] == ' ')
                Indent++;
            MoveCaret(TextPosition{Caret_.Line, Caret_.Column == Indent ? 0 : Indent}, Shift);
        }
        break;
    case HZ_VK_END:
        if (Ctrl)
            MoveCaret(TextPosition{Lines_.Size() - 1, Lines_.Back().Length()}, Shift);
        else
            MoveCaret(TextPosition{Caret_.Line, Lines_[Caret_.Line].Length()}, Shift);
        break;
    case HZ_VK_BACK:
        if (ReadOnly)
            break;
        if (!HasSelection())
            Anchor_ = Ctrl ? WordLeft(Caret_) : Previous(Caret_);
        DeleteSelection();
        break;
    case HZ_VK_DELETE:
        if (ReadOnly)
            break;
        if (!HasSelection())
            Anchor_ = Ctrl ? WordRight(Caret_) : Next(Caret_);
        DeleteSelection();
        break;
    case HZ_VK_RETURN: {
        if (ReadOnly)
            break;
        const hz::String &L = Lines_[Caret_.Line];
        size_t Indent = 0;
        while (Indent < L.Length() && Indent < SelectionStart().Column && L[Indent] == ' ')
            Indent++;
        hz::String Break("\n");
        for (size_t i = 0; i < Indent; i++)
            Break.Append(' ');
        Insert(Break.CStr(), Break.Length());
        break;
    }
    case HZ_VK_TAB:
        if (Ctrl || Shift || ReadOnly)
            return; /* focus navigation */
        Apply("    ", 4, true);
        break;
    case 'A':
        if (!Ctrl)
            return;
        SelectAll();
        break;
    case 'C':
        if (!Ctrl)
            return;
        Copy();
        break;
    case 'X':
        if (!Ctrl)
            return;
        Cut();
        break;
    case 'V':
        if (!Ctrl)
            return;
        Paste();
        break;
    case 'Z':
        if (!Ctrl)
            return;
        if (Shift)
            Redo();
        else
            Undo();
        break;
    case 'Y':
        if (!Ctrl)
            return;
        Redo();
        break;
    default:
        return;
    }
    E.Handled = true;
}

void TextEditor::OnChar(KeyEvent &E)
{
    if (ReadOnly || E.Ctrl() || E.Alt() || E.Character < 0x20 || E.Character == 0x7F)
        return;
    hz::String Bytes;
    Bytes.AppendCodePoint(E.Character);
    Apply(Bytes.CStr(), Bytes.Length(), true);
    E.Handled = true;
}

void TextEditor::OnFocusChanged(bool Focused)
{
    Window *W = GetWindow();
    if (Focused) {
        BlinkStartUs_ = NowUs();
        if (W && !BlinkTimer_)
            BlinkTimer_ = W->SetTimer(kBlinkMs, [this]() {
                Point C = PointOf(Caret_); /* only the caret changes */
                if (Window *Owner = GetWindow())
                    Owner->InvalidateRect(Rect{C.X - 1, C.Y, 4, LineHeight()}.Intersect(TextArea()));
            });
    } else {
        Selecting_ = false;
        if (W && BlinkTimer_)
            W->KillTimer(BlinkTimer_);
        BlinkTimer_ = 0;
    }
    Invalidate();
}

void TextEditor::OnWindowActivated(bool)
{
    BlinkStartUs_ = NowUs();
    Invalidate();
}

}  // namespace hzui
