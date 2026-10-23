/*
 * Nexora OS - user/apps/terminal/console.h
 * The terminal's console view: output lines in JetBrains Mono, then the prompt and the line being
 * typed (with a caret, history, and the usual editing keys). Lines longer than the view wrap onto
 * further rows (M11; the font is monospaced, so a row holds a fixed number of characters). The
 * view keeps the last kMaxLines lines and follows the end of the output.
 */
#ifndef TERMINAL_CONSOLE_H
#define TERMINAL_CONSOLE_H

#include "hzui.h"

namespace term {

using namespace hzui;

class ConsoleView final : public Element {
public:
    static constexpr size_t kMaxLines = 3000;
    static constexpr size_t kMaxHistory = 100;

    ConsoleView();
    /* Appends output: '\n' ends a line, '\r' is dropped, tabs advance to the next 8 columns. */
    void Write(const char *Text, size_t Length);
    void Write(const char *Text) { Write(Text, strlen(Text)); }
    void Clear();
    void SetPrompt(const hz::String &Prompt);
    /* While input is off (a program runs) the prompt is hidden and typed keys are ignored, except
       Ctrl+C, which calls OnInterrupt. */
    void SetInputEnabled(bool Enabled);
    bool InputEnabled() const { return InputEnabled_; }
    /* While input is off, keys other than Ctrl+C and Page Up/Down go here (the '| more' pager). */
    hz::Function<void(KeyEvent &)> OnBusyKey;
    /* Removes the output after the last line feed (the pager's "-- More --"). */
    void DropPartial();
    size_t VisibleRows() const;
    size_t RowsOfText(const hz::String &Line) const; /* rows a line of output takes */
    hz::String AllText() const;
    size_t LineCount() const { return Lines_.Size(); }
    const hz::String &LineAt(size_t Index) const { return Lines_[Index]; }
    const hz::Vector<hz::String> &History() const { return History_; }
    void ScrollToEnd();

    hz::Function<void(const hz::String &)> OnCommand; /* Enter: the typed line */
    hz::Function<void()> OnInterrupt;
    hz::Function<hz::String(const hz::String &)> OnComplete; /* Tab: the line completed, or the same */

    Size MeasureOverride(Size Available) override;
    void ArrangeOverride(const Rect &Content) override;
    void Paint(Painter &P) override;
    void OnPointerDown(PointerEvent &E) override;
    void OnPointerWheel(PointerEvent &E) override;
    void OnKeyDown(KeyEvent &E) override;
    void OnChar(KeyEvent &E) override;
    void OnFocusChanged(bool Focused) override;
    void OnWindowActivated(bool Active) override;
    uint32_t CursorShape() const override { return HZ_CURSOR_IBEAM; }

private:
    void EndLine();
    void Changed(bool FollowEnd);
    void SetInput(const hz::String &Text);
    void InsertText(const char *Text, size_t Length);
    size_t RowCount() const; /* wrapped rows of the lines + of the partial/prompt line */
    int32_t RowHeight() const;
    size_t Columns() const;  /* characters per row */
    size_t RowsOf(size_t Characters) const;
    hz::String LastLine() const; /* the partial output, the prompt and the input */
    Rect TextArea() const;
    void UpdateScrollBar();
    Rect CaretRect() const;
    void ResetBlink();

    hz::Vector<hz::String> Lines_;
    hz::String Partial_;     /* output after the last line feed */
    hz::String Prompt_;
    hz::String Input_;
    size_t Caret_ = 0;       /* byte offset in Input_ */
    bool InputEnabled_ = true;
    bool SwallowChar_ = false;
    hz::Vector<hz::String> History_;
    size_t HistoryIndex_ = 0; /* == History_.Size(): the line being typed */
    hz::String Draft_;       /* the typed line while browsing history */
    int32_t ScrollY_ = 0;
    ScrollBar *Bar_;
    uint64_t BlinkTimer_ = 0, BlinkStartUs_ = 0;
};

}  // namespace term

#endif /* TERMINAL_CONSOLE_H */
