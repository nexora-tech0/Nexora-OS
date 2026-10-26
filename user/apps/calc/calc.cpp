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

/* The two text lines at the top over a calm screen card; the number shrinks to fit and eases in
   whenever the engine produces a new value. */
class CalcDisplay final : public Element {
public:
    explicit CalcDisplay(calc::Engine *E) : Engine_(E)
    {
        Large_ = HzLoadFont("C:\\Nexora\\Fonts\\Inter-SemiBold.ttf", 46);
        Medium_ = HzLoadFont("C:\\Nexora\\Fonts\\Inter-SemiBold.ttf", 30);
        Height = 112;
        Settle_.Jump(1.0f);
        Shown_ = Engine_->Display();
        ShownExpression_ = Engine_->Expression();
    }

    /* Called after a key the engine accepted: a new number or expression eases into place. */
    void ValueChanged()
    {
        if (Engine_->Display() == Shown_ && Engine_->Expression() == ShownExpression_) {
            Invalidate();
            return;
        }
        Shown_ = Engine_->Display();
        ShownExpression_ = Engine_->Expression();
        Settle_.Jump(0.0f);
        if (GetWindow()) {
            Settle_.To(1.0f, Motion::ControlStateMs);
            StartAnimation();
        } else {
            Settle_.Jump(1.0f);
        }
        Invalidate();
    }

    void OnPointerEnter() override
    {
        Hover_.To(1, Motion::HoverEnterMs);
        StartAnimation();
    }

    void OnPointerLeave() override
    {
        Hover_.To(0, Motion::HoverLeaveMs);
        StartAnimation();
    }

    bool Animate() override
    {
        Invalidate();
        return Hover_.Running() || Settle_.Running();
    }

    void Paint(Painter &P) override
    {
        const Rect &B = Bounds();
        float Hover = Hover_.Value(), Settle = Settle_.Value();
        int32_t Rise = (int32_t)((1.0f - Settle) * 5.0f); /* a new number settles up into place */

        /* The screen: a card that lifts a little under the pointer. */
        P.FillRoundRect(B, 8, MixColors(Theme::CardBackground, Theme::ControlFillHover, Hover));
        P.StrokeRoundRect(B, 8, 1, Theme::CardStroke);

        P.TextLine(GetFont(FontRole::Caption), Rect{B.X + 16, B.Y + 8, B.Width - 32, 20},
                   Engine_->Expression().CStr(), Theme::TextTertiary, Align::End);
        P.FillRect(Rect{B.X + 16, B.Y + 34, B.Width - 32, 1}, Theme::Divider);

        const hz::String &Text = Engine_->Display();
        Color Number = ScaleAlpha(Engine_->HasError() ? Theme::TextSecondary : Theme::TextPrimary,
                                  0.5f + 0.5f * Settle);
        HZ_FONT *F = Large_;
        if (F && HzMeasureTextN(F, Text.CStr(), Text.Length()) > B.Width - 32)
            F = Medium_;
        if (!F) {
            P.TextLine(GetFont(FontRole::Title), Rect{B.X + 16, B.Y + 36 + Rise, B.Width - 32, 60}, Text.CStr(), Number,
                       Align::End);
            return;
        }
        int32_t W = HzMeasureTextN(F, Text.CStr(), Text.Length());
        int32_t X = B.Right() - 16 - W;
        if (X < B.X + 16) { /* still too wide: show the end */
            P.TextLine(F, Rect{B.X + 16, B.Y + 36 + Rise, B.Width - 32, 64}, Text.CStr(), Number, Align::End);
            return;
        }
        P.Text(F, X, B.Y + 40 + Rise + HzFontAscent(F) - (F == Medium_ ? -8 : 0), Text.CStr(), Text.Length(), Number);
    }

private:
    calc::Engine *Engine_;
    HZ_FONT *Large_, *Medium_;
    hz::String Shown_, ShownExpression_;
    Transition Hover_, Settle_;
};

class CalcRoot final : public Grid {
public:
    hz::Function<bool(char)> OnInput;
    Element *FocusTarget = nullptr; /* the screen: ringed while the keyboard focus feeds it */

    CalcRoot() { Focusable = true; }

    /* Keys go to the root, so show the focus ring on the screen the keys write to. */
    void PaintOverlay(Painter &P) override
    {
        if (ShowsFocusRing() && FocusTarget)
            P.FocusRing(FocusTarget->Bounds(), 8);
    }

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
        Root_->Padding = Thickness(10, 8, 10, 10);
        Root_->Rows.Push(GridLength::AutoSize());
        Root_->Rows.Push(GridLength::Stars(1));
        Display_ = Root_->Add<CalcDisplay>(&Engine_);
        Display_->Margin = Thickness(0, 0, 0, 4); /* room for the focus ring under the screen */
        Root_->OnInput = [this](char C) { return Input(C); };
        Root_->FocusTarget = Display_;

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
            const char *Tip = nullptr;
            switch (K.Code) {
            case '%': Tip = "Percent"; break;
            case 'E': Tip = "Clear entry"; break;
            case 0x1B: Tip = "Clear"; break;
            case 'r': Tip = "Reciprocal"; break;
            case 'q': Tip = "Square"; break;
            case '@': Tip = "Square root"; break;
            case 'n': Tip = "Negate"; break;
            case '/': Tip = "Divide"; break;
            case '*': Tip = "Multiply"; break;
            case '-': Tip = "Subtract"; break;
            case '+': Tip = "Add"; break;
            default: break;
            }
            if (Tip)
                B->ToolTip = Tip;
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
        Display_->ValueChanged();
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
