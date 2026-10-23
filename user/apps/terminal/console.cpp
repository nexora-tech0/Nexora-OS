/*
 * Nexora OS - user/apps/terminal/console.cpp
 * The terminal's console view; see console.h.
 */
#include "console.h"

namespace term {

namespace {

constexpr uint32_t kBlinkMs = 530;
constexpr int32_t kRowGap = 3;

size_t CodePoints(const char *Text, size_t Length)
{
    size_t Count = 0;
    for (size_t i = 0; i < Length; i++)
        Count += ((uint8_t)Text[i] & 0xC0) != 0x80;
    return Count;
}

/* Byte offset of the code point at index Skip (Length if past the end). */
size_t OffsetOfCodePoint(const char *Text, size_t Length, size_t Skip)
{
    size_t i = 0;
    while (i < Length && Skip) {
        i++;
        while (i < Length && ((uint8_t)Text[i] & 0xC0) == 0x80)
            i++;
        Skip--;
    }
    return i;
}

}  // namespace

ConsoleView::ConsoleView()
{
    Focusable = true;
    ClipChildren = true;
    Background = 0xF20C0C0E;
    Padding = Thickness(12, 8, 4, 8);
    Bar_ = Add<ScrollBar>();
    Bar_->OnScroll = [this](int32_t Offset) {
        ScrollY_ = Offset;
        Invalidate();
    };
}

/* ---- Content ------------------------------------------------------------------------------------- */

void ConsoleView::EndLine()
{
    Lines_.Push(hz::Move(Partial_));
    Partial_ = hz::String();
    if (Lines_.Size() > kMaxLines) {
        size_t Drop = Lines_.Size() - kMaxLines;
        hz::Vector<hz::String> Kept;
        Kept.Reserve(kMaxLines);
        for (size_t i = Drop; i < Lines_.Size(); i++)
            Kept.Push(hz::Move(Lines_[i]));
        Lines_ = hz::Move(Kept);
    }
}

void ConsoleView::Write(const char *Text, size_t Length)
{
    for (size_t i = 0; i < Length; i++) {
        char C = Text[i];
        if (C == '\n') {
            EndLine();
        } else if (C == '\r') {
            continue;
        } else if (C == '\t') {
            size_t Columns = 0;
            for (size_t j = 0; j < Partial_.Length(); j++)
                Columns += ((uint8_t)Partial_[j] & 0xC0) != 0x80;
            do {
                Partial_.Append(' ');
                Columns++;
            } while (Columns % 8);
        } else if ((uint8_t)C >= 0x20 || C == 0x1B) {
            if (C != 0x1B)
                Partial_.Append(C);
        }
    }
    Changed(true);
}

void ConsoleView::Clear()
{
    Lines_.Clear();
    Partial_ = hz::String();
    ScrollY_ = 0;
    Changed(true);
}

void ConsoleView::SetPrompt(const hz::String &Prompt)
{
    Prompt_ = Prompt;
    Changed(true);
}

void ConsoleView::SetInputEnabled(bool Enabled)
{
    InputEnabled_ = Enabled;
    ResetBlink();
    Changed(true);
}

hz::String ConsoleView::AllText() const
{
    hz::String Out;
    for (const hz::String &L : Lines_) {
        Out.Append(L);
        Out.Append('\n');
    }
    Out.Append(Partial_);
    if (InputEnabled_) {
        Out.Append(Prompt_);
        Out.Append(Input_);
    }
    return Out;
}

void ConsoleView::DropPartial()
{
    Partial_ = hz::String();
    Changed(true);
}

size_t ConsoleView::VisibleRows() const
{
    int32_t Rows = TextArea().Height / RowHeight();
    return Rows > 2 ? (size_t)Rows : 2;
}

size_t ConsoleView::RowsOfText(const hz::String &Line) const
{
    return RowsOf(CodePoints(Line.CStr(), Line.Length()));
}

size_t ConsoleView::Columns() const
{
    HZ_FONT *F = GetFont(FontRole::Mono);
    int32_t Width = F ? HzMeasureTextFixed(F, "M", 1) : 8 * 64; /* 26.6: rounding would drift */
    int32_t Columns = Width > 0 ? (int32_t)((int64_t)TextArea().Width * 64 / Width) : 80;
    return Columns > 1 ? (size_t)Columns : 1;
}

size_t ConsoleView::RowsOf(size_t Characters) const
{
    size_t Columns = this->Columns();
    return Characters == 0 ? 1 : (Characters + Columns - 1) / Columns;
}

hz::String ConsoleView::LastLine() const
{
    hz::String Line = Partial_;
    if (InputEnabled_) {
        Line.Append(Prompt_);
        Line.Append(Input_);
    }
    return Line;
}

size_t ConsoleView::RowCount() const
{
    size_t Rows = 0;
    for (const hz::String &L : Lines_)
        Rows += RowsOf(CodePoints(L.CStr(), L.Length()));
    if (InputEnabled_ || !Partial_.Empty()) {
        hz::String Last = LastLine();
        size_t Characters = CodePoints(Last.CStr(), Last.Length());
        Rows += RowsOf(InputEnabled_ ? Characters + 1 : Characters); /* room for the caret */
    }
    return Rows;
}

int32_t ConsoleView::RowHeight() const
{
    HZ_FONT *F = GetFont(FontRole::Mono);
    return (F ? HzFontLineHeight(F) : 18) + kRowGap;
}

Rect ConsoleView::TextArea() const
{
    Rect C = ContentBounds();
    return Rect{C.X, C.Y, hz::Max(C.Width - 14, 0), C.Height};
}

void ConsoleView::UpdateScrollBar()
{
    Rect Area = TextArea();
    int32_t Extent = (int32_t)RowCount() * RowHeight();
    ScrollY_ = hz::Clamp(ScrollY_, 0, hz::Max(Extent - Area.Height, 0));
    Bar_->SetRange(Extent, Area.Height);
    Bar_->SetOffset(ScrollY_, false);
}

void ConsoleView::ScrollToEnd()
{
    Rect Area = TextArea();
    int32_t Extent = (int32_t)RowCount() * RowHeight();
    ScrollY_ = hz::Max(Extent - Area.Height, 0);
    UpdateScrollBar();
    Invalidate();
}

void ConsoleView::Changed(bool FollowEnd)
{
    if (FollowEnd)
        ScrollToEnd();
    else
        UpdateScrollBar();
    Invalidate();
}

/* ---- Layout and painting ------------------------------------------------------------------------- */

Size ConsoleView::MeasureOverride(Size Available)
{
    Bar_->Measure(Available);
    return Size{300, 200};
}

void ConsoleView::ArrangeOverride(const Rect &Content)
{
    const Rect &B = Bounds();
    Bar_->Arrange(Rect{B.Right() - 14, B.Y + 2, 12, B.Height - 4});
    ScrollToEnd();
}

Rect ConsoleView::CaretRect() const
{
    HZ_FONT *F = GetFont(FontRole::Mono);
    Rect Area = TextArea();
    size_t FirstRow = 0;
    for (const hz::String &L : Lines_)
        FirstRow += RowsOf(CodePoints(L.CStr(), L.Length()));
    size_t Before = CodePoints(Partial_.CStr(), Partial_.Length()) + CodePoints(Prompt_.CStr(), Prompt_.Length()) +
                    CodePoints(Input_.CStr(), Caret_);
    size_t Columns = this->Columns();
    int64_t CharWidth = F ? HzMeasureTextFixed(F, "M", 1) : 8 * 64;
    int32_t Row = (int32_t)(FirstRow + Before / Columns);
    int32_t X = Area.X + (int32_t)(((int64_t)(Before % Columns) * CharWidth + 32) / 64);
    return Rect{X, Area.Y - ScrollY_ + Row * RowHeight(), 2, RowHeight() - 2};
}

void ConsoleView::Paint(Painter &P)
{
    const Rect &B = Bounds();
    P.FillRect(B, Background);
    HZ_FONT *F = GetFont(FontRole::Mono);
    if (!F)
        return;
    Rect Area = TextArea();
    P.PushClip(Area);
    int32_t Height = RowHeight(), Ascent = HzFontAscent(F);
    size_t Columns = this->Columns();
    int32_t FirstVisible = ScrollY_ / Height, LastVisible = (ScrollY_ + Area.Height) / Height;
    /* Draws the rows of one logical line (in color runs) starting at row Row. */
    auto DrawLine = [&](size_t Row, const char *const *Texts, const size_t *Lengths, const Color *Colors, int Runs) {
        size_t Character = 0; /* across the runs */
        for (int r = 0; r < Runs; r++) {
            const char *T = Texts[r];
            size_t Length = Lengths[r];
            size_t Offset = 0;
            while (Offset < Length) {
                size_t RowIndex = Row + Character / Columns, Column = Character % Columns;
                size_t Fit = Columns - Column;
                size_t End = Offset + OffsetOfCodePoint(T + Offset, Length - Offset, Fit);
                size_t Taken = CodePoints(T + Offset, End - Offset);
                if ((int32_t)RowIndex >= FirstVisible && (int32_t)RowIndex <= LastVisible) {
                    int32_t Y = Area.Y - ScrollY_ + (int32_t)RowIndex * Height;
                    int32_t X = Area.X + (int32_t)(((int64_t)Column * HzMeasureTextFixed(F, "M", 1) + 32) / 64);
                    P.Text(F, X, Y + kRowGap / 2 + Ascent, T + Offset, End - Offset, Colors[r]);
                }
                Character += Taken;
                Offset = End;
            }
        }
    };
    size_t Row = 0;
    for (const hz::String &L : Lines_) {
        size_t Rows = RowsOf(CodePoints(L.CStr(), L.Length()));
        if ((int32_t)(Row + Rows) > FirstVisible && (int32_t)Row <= LastVisible && !L.Empty()) {
            const char *Texts[1] = {L.CStr()};
            size_t Lengths[1] = {L.Length()};
            Color Colors[1] = {0xFFD8DCE3};
            DrawLine(Row, Texts, Lengths, Colors, 1);
        }
        Row += Rows;
    }
    if (InputEnabled_ || !Partial_.Empty()) {
        const char *Texts[3] = {Partial_.CStr(), Prompt_.CStr(), Input_.CStr()};
        size_t Lengths[3] = {Partial_.Length(), InputEnabled_ ? Prompt_.Length() : 0, InputEnabled_ ? Input_.Length() : 0};
        Color Colors[3] = {0xFFD8DCE3, Theme::Accent, Theme::TextPrimary};
        DrawLine(Row, Texts, Lengths, Colors, 3);
    }
    bool Focused = IsFocused() && GetWindow() && GetWindow()->IsActive();
    if (Focused && InputEnabled_ && ((NowUs() - BlinkStartUs_) / (kBlinkMs * 1000)) % 2 == 0)
        P.FillRect(CaretRect(), Theme::TextPrimary);
    P.PopClip();
}

/* ---- Input ---------------------------------------------------------------------------------------- */

void ConsoleView::ResetBlink()
{
    BlinkStartUs_ = NowUs();
}

void ConsoleView::SetInput(const hz::String &Text)
{
    Input_ = Text;
    Caret_ = Input_.Length();
    ResetBlink();
    Changed(true);
}

void ConsoleView::InsertText(const char *Text, size_t Length)
{
    hz::String Clean;
    for (size_t i = 0; i < Length; i++) {
        char C = Text[i];
        if (C == '\n' || C == '\r' || C == '\t')
            C = ' ';
        if ((uint8_t)C >= 0x20)
            Clean.Append(C);
    }
    if (Input_.Length() + Clean.Length() > 1000)
        return;
    Input_.Insert(Caret_, Clean.CStr(), Clean.Length());
    Caret_ += Clean.Length();
    ResetBlink();
    Changed(true);
}

void ConsoleView::OnPointerDown(PointerEvent &E)
{
    Focus();
    if (E.Button == HZ_MOUSE_RIGHT) {
        E.Handled = true;
        Window *W = GetWindow();
        if (!W)
            return;
        hz::Vector<MenuItem> Items;
        Items.Push(MenuItem::Make("Copy all text", [this]() {
            hz::String All = AllText();
            Clipboard::SetText(All.CStr(), All.Length());
        }));
        MenuItem PasteItem = MenuItem::Make("Paste", [this]() {
            const hz::String &Text = Clipboard::GetText();
            InsertText(Text.CStr(), Text.Length());
        }, "Ctrl+V");
        PasteItem.Enabled = InputEnabled_;
        Items.Push(hz::Move(PasteItem));
        Items.Push(MenuItem::MakeSeparator());
        Items.Push(MenuItem::Make("Clear", [this]() { Clear(); }, "Ctrl+L"));
        ShowMenu(W, E.ScreenPosition, hz::Move(Items), 180);
        return;
    }
    E.Handled = true;
}

void ConsoleView::OnPointerWheel(PointerEvent &E)
{
    ScrollY_ -= E.WheelDelta / 120 * RowHeight() * 3;
    UpdateScrollBar();
    Invalidate();
    E.Handled = true;
}

void ConsoleView::OnKeyDown(KeyEvent &E)
{
    SwallowChar_ = false;
    if (E.Ctrl() && E.Key == 'C') {
        E.Handled = true;
        if (!InputEnabled_) {
            if (OnInterrupt)
                OnInterrupt();
            return;
        }
        /* Abandon the line. */
        Write(Prompt_.CStr(), Prompt_.Length());
        Write(Input_.CStr(), Input_.Length());
        Write("^C\n");
        SetInput(hz::String());
        return;
    }
    if (E.Ctrl() && E.Key == 'L') {
        E.Handled = true;
        Clear();
        return;
    }
    if (E.Key == HZ_VK_PRIOR || E.Key == HZ_VK_NEXT) {
        E.Handled = true;
        int32_t Page = hz::Max(TextArea().Height - RowHeight(), RowHeight());
        ScrollY_ += E.Key == HZ_VK_PRIOR ? -Page : Page;
        UpdateScrollBar();
        Invalidate();
        return;
    }
    if (!InputEnabled_) {
        if (OnBusyKey)
            OnBusyKey(E);
        /* A key the pager used must not also type its character if it turned input back on. */
        SwallowChar_ = E.Handled;
        return;
    }
    if (E.Ctrl() && E.Key == 'V') {
        E.Handled = true;
        const hz::String &Text = Clipboard::GetText();
        InsertText(Text.CStr(), Text.Length());
        return;
    }
    switch (E.Key) {
    case HZ_VK_RETURN: {
        hz::String Line = Input_;
        Write(Prompt_.CStr(), Prompt_.Length());
        Write(Line.CStr(), Line.Length());
        Write("\n");
        bool Blank = true;
        for (size_t i = 0; i < Line.Length(); i++)
            Blank = Blank && Line[i] == ' ';
        if (!Blank && (History_.Empty() || History_.Back() != Line)) {
            History_.Push(Line);
            if (History_.Size() > kMaxHistory)
                History_.Erase(0);
        }
        HistoryIndex_ = History_.Size();
        Draft_ = hz::String();
        Input_ = hz::String();
        Caret_ = 0;
        Changed(true);
        if (OnCommand)
            OnCommand(Line);
        break;
    }
    case HZ_VK_BACK:
        if (Caret_ > 0) {
            size_t Previous = Input_.PreviousChar(Caret_);
            Input_.Erase(Previous, Caret_ - Previous);
            Caret_ = Previous;
        }
        break;
    case HZ_VK_DELETE:
        if (Caret_ < Input_.Length())
            Input_.Erase(Caret_, Input_.NextChar(Caret_) - Caret_);
        break;
    case HZ_VK_LEFT:
        if (Caret_ > 0)
            Caret_ = Input_.PreviousChar(Caret_);
        break;
    case HZ_VK_RIGHT:
        if (Caret_ < Input_.Length())
            Caret_ = Input_.NextChar(Caret_);
        break;
    case HZ_VK_HOME:
        Caret_ = 0;
        break;
    case HZ_VK_END:
        Caret_ = Input_.Length();
        break;
    case HZ_VK_ESCAPE:
        Input_ = hz::String();
        Caret_ = 0;
        break;
    case HZ_VK_UP:
        if (HistoryIndex_ > 0) {
            if (HistoryIndex_ == History_.Size())
                Draft_ = Input_;
            HistoryIndex_--;
            SetInput(History_[HistoryIndex_]);
        }
        break;
    case HZ_VK_DOWN:
        if (HistoryIndex_ < History_.Size()) {
            HistoryIndex_++;
            SetInput(HistoryIndex_ == History_.Size() ? Draft_ : History_[HistoryIndex_]);
        }
        break;
    case HZ_VK_TAB:
        if (OnComplete) {
            hz::String Completed = OnComplete(Input_);
            if (Completed != Input_)
                SetInput(Completed);
        }
        break;
    default:
        return;
    }
    E.Handled = true;
    ResetBlink();
    Changed(true);
}

void ConsoleView::OnChar(KeyEvent &E)
{
    if (SwallowChar_) {
        SwallowChar_ = false;
        E.Handled = true;
        return;
    }
    if (!InputEnabled_ || E.Ctrl() || E.Alt() || E.Character < 0x20 || E.Character == 0x7F)
        return;
    hz::String Bytes;
    Bytes.AppendCodePoint(E.Character);
    InsertText(Bytes.CStr(), Bytes.Length());
    E.Handled = true;
}

void ConsoleView::OnFocusChanged(bool Focused)
{
    Window *W = GetWindow();
    if (Focused) {
        ResetBlink();
        if (W && !BlinkTimer_)
            BlinkTimer_ = W->SetTimer(kBlinkMs, [this]() {
                if (Window *Owner = GetWindow())
                    Owner->InvalidateRect(CaretRect().Inflate(1).Intersect(TextArea()));
            });
    } else {
        if (W && BlinkTimer_)
            W->KillTimer(BlinkTimer_);
        BlinkTimer_ = 0;
    }
    Invalidate();
}

void ConsoleView::OnWindowActivated(bool)
{
    ResetBlink();
    Invalidate();
}

}  // namespace term
