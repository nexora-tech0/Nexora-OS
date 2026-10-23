/*
 * Nexora OS - user/apps/calc/calc.cpp
 * Calculator (standard mode): the expression line and a large display over a 4x6 keypad. The
 * keyboard works too: digits, + - * / x, '.', Enter or '=', Backspace, Escape (C), Delete (CE),
 * '%', and Ctrl+C / Ctrl+V for the display. Results are logged ("calc: = 42") for the smoke tests.
 */
#include "engine.h"
#include "hzui.h"

using namespace hzui;

namespace {

/* The two text lines at the top; the number shrinks to fit. */
class CalcDisplay final : public Element {
public:
    explicit CalcDisplay(calc::Engine *E) : Engine_(E)
    {
        Large_ = HzLoadFont("C:\\Nexora\\Fonts\\Inter-SemiBold.ttf", 46);
        Medium_ = HzLoadFont("C:\\Nexora\\Fonts\\Inter-SemiBold.ttf", 30);
        Height = 112;
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        Rect Line{B.X + 12, B.Y + 6, B.Width - 24, 24};
        P.TextLine(GetFont(FontRole::Body), Line, Engine_->Expression().CStr(), Theme::TextTertiary, Align::End);
        const hz::String &Text = Engine_->Display();
        HZ_FONT *F = Large_;
        if (F && HzMeasureTextN(F, Text.CStr(), Text.Length()) > B.Width - 24)
            F = Medium_;
        if (!F) {
            P.TextLine(GetFont(FontRole::Title), Rect{B.X + 12, B.Y + 36, B.Width - 24, 60}, Text.CStr(), Theme::TextPrimary,
                       Align::End);
            return;
        }
        int32_t W = HzMeasureTextN(F, Text.CStr(), Text.Length());
        int32_t X = B.Right() - 12 - W;
        if (X < B.X + 12) { /* still too wide: show the end */
            P.TextLine(F, Rect{B.X + 12, B.Y + 36, B.Width - 24, 64}, Text.CStr(), Theme::TextPrimary, Align::End);
            return;
        }
        P.Text(F, X, B.Y + 40 + HzFontAscent(F) - (F == Medium_ ? -8 : 0), Text.CStr(), Text.Length(),
               Engine_->HasError() ? Theme::TextSecondary : Theme::TextPrimary);
    }

private:
    calc::Engine *Engine_;
    HZ_FONT *Large_, *Medium_;
};

class CalcRoot final : public Grid {
public:
    hz::Function<bool(char)> OnInput;

    CalcRoot() { Focusable = true; }

    void OnKeyDown(KeyEvent &E) override
    {
        char C = 0;
        switch (E.Key) {
        case HZ_VK_RETURN: C = '='; break;
        case HZ_VK_BACK: C = '\b'; break;
        case HZ_VK_ESCAPE: C = 0x1B; break;
        case HZ_VK_DELETE: C = 'E'; break; /* CE */
        default:
            if (E.Ctrl() && (E.Key == 'C' || E.Key == 'V'))
                C = E.Key == 'C' ? 'c' : 'v';
            break;
        }
        if (C && OnInput && OnInput(C))
            E.Handled = true;
    }

    void OnChar(KeyEvent &E) override
    {
        if (E.Character < 0x20 || E.Character > 0x7E || E.Ctrl())
            return;
        char C = (char)E.Character;
        if (C == 'r' || C == 'q' || C == 'n' || C == 'c' || C == 'v' || C == 'E')
            return; /* only through the buttons and shortcuts */
        if (OnInput && OnInput(C))
            E.Handled = true;
    }
};

class Calculator {
public:
    bool Create()
    {
        Main_ = new Window("Calculator", 340, 540, Window::DefaultStyle & ~HZ_WS_MAXIMIZEBOX);
        Main_->SetMinimumSize(300, 460);
        Root_ = new CalcRoot();
        Root_->Padding = Thickness(6, 4, 6, 6);
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::Stars(1));
        Display_ = Root_->Add<CalcDisplay>(&Engine_);
        Root_->OnInput = [this](char C) { return Input(C); };

        Grid *Pad = Root_->Add<Grid>();
        Pad->GridRow = 1;
        Pad->RowSpacing = 3;
        Pad->ColumnSpacing = 3;
        for (int i = 0; i < 4; i++)
            Pad->Columns.Push(GridLength::Stars(1));
        for (int i = 0; i < 6; i++)
            Pad->Rows.Push(GridLength::Stars(1));
        struct Key {
            const char *Text;
            char Code;
            int Kind; /* 0 function, 1 digit, 2 operator, 3 equals */
        };
        static const Key Keys[24] = {
            {"%", '%', 0},       {"CE", 'E', 0},       {"C", 0x1B, 0},       {"", '\b', 0},
            {"1/x", 'r', 0},     {"x\xC2\xB2", 'q', 0}, {"\xE2\x88\x9Ax", '@', 0}, {"\xC3\xB7", '/', 2},
            {"7", '7', 1},       {"8", '8', 1},        {"9", '9', 1},        {"\xC3\x97", '*', 2},
            {"4", '4', 1},       {"5", '5', 1},        {"6", '6', 1},        {"\xE2\x88\x92", '-', 2},
            {"1", '1', 1},       {"2", '2', 1},        {"3", '3', 1},        {"+", '+', 2},
            {"+/\xE2\x88\x92", 'n', 0}, {"0", '0', 1}, {".", '.', 1},        {"=", '=', 3},
        };
        for (int i = 0; i < 24; i++) {
            const Key &K = Keys[i];
            Button *B = Pad->Add<Button>(K.Text, K.Kind == 3 ? ButtonStyle::Accent : ButtonStyle::Standard);
            B->GridRow = (uint8_t)(i / 4);
            B->GridColumn = (uint8_t)(i % 4);
            B->Focusable = false; /* keys go to the calculator, Enter is always '=' */
            B->Name = hz::String::Format("key%d", i);
            if (K.Code == '\b') {
                B->Glyph = Icon::ChevronLeft;
                B->ToolTip = "Backspace";
            }
            if (K.Kind == 1)
                B->Background = 0x14FFFFFF;
            char Code = K.Code;
            B->OnClick = [this, Code]() {
                Input(Code);
                Main_->SetFocus(Root_);
            };
        }
        Main_->SetContent(Root_);
        Main_->OnClosed = []() { NtUserPostQuitMessage(0); };
        if (!Main_->Show())
            return false;
        Main_->SetFocus(Root_);
        return true;
    }

private:
    bool Input(char C)
    {
        if (C == 'E') {
            Engine_.ClearEntry();
        } else if (C == 'c') {
            Clipboard::SetText(Engine_.Display().CStr(), Engine_.Display().Length());
            return true;
        } else if (C == 'v') {
            double V;
            hz::String Text = Clipboard::GetText();
            while (!Text.Empty() && (Text[Text.Length() - 1] == '\n' || Text[Text.Length() - 1] == ' '))
                Text.Erase(Text.Length() - 1, 1);
            if (!calc::ParseNumber(Text.CStr(), &V))
                return true;
            Engine_.Clear();
            hz::String Digits = calc::FormatNumber(V);
            for (size_t i = 0; i < Digits.Length(); i++)
                if (Digits[i] != '-')
                    Engine_.Key(Digits[i]);
            if (V < 0)
                Engine_.Negate();
        } else if (!Engine_.Key(C)) {
            return false;
        }
        Display_->Invalidate();
        if (C == '=' || C == '\r')
            HzPrint("calc: %s %s", Engine_.Expression().CStr(), Engine_.Display().CStr());
        return true;
    }

    Window *Main_ = nullptr;
    CalcRoot *Root_ = nullptr;
    CalcDisplay *Display_ = nullptr;
    calc::Engine Engine_;
};

}  // namespace

extern "C" int main(int argc, char **argv)
{
    Calculator *App = new Calculator();
    if (!App->Create()) {
        HzPrint("calc: cannot create the window");
        return 1;
    }
    HzPrint("calc: started");
    return RunMessageLoop();
}
