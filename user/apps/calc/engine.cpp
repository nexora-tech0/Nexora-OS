/*
 * Nexora OS - user/apps/calc/engine.cpp
 * The calculator's engine; see engine.h.
 */
#include "engine.h"

namespace calc {

namespace {

constexpr int kDigits = 15;           /* significant digits shown */
constexpr size_t kMaxEntry = 16;      /* digits typed */

/* Exact powers of ten (all of 1e0..1e22 are exact in a double). */
double Pow10(int N)
{
    static const double Table[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (N >= 0) {
        double R = 1;
        while (N > 22) {
            R *= 1e22;
            N -= 22;
        }
        return R * Table[N];
    }
    return 1 / Pow10(-N);
}

bool IsFinite(double V)
{
    return V == V && V - V == 0; /* NaN fails the first test, infinities the second */
}

}  // namespace

double SquareRoot(double Value)
{
    double Result;
    __asm__("sqrtsd %1, %0" : "=x"(Result) : "x"(Value));
    return Result;
}

hz::String FormatNumber(double Value)
{
    if (!IsFinite(Value))
        return hz::String("Overflow");
    if (Value == 0)
        return hz::String("0");
    hz::String Out;
    if (Value < 0) {
        Out.Append('-');
        Value = -Value;
    }
    /* Decimal exponent: Value = M * 10^E with 1 <= M < 10. */
    int E = 0;
    while (Value >= Pow10(E + 1) && E < 400)
        E++;
    while (Value < Pow10(E) && E > -400)
        E--;
    /* kDigits significant digits as an integer. */
    int Shift = kDigits - 1 - E;
    double Scaled = Shift >= 0 ? Value * Pow10(Shift) : Value / Pow10(-Shift);
    int64_t Digits = (int64_t)(Scaled + 0.5);
    if (Digits >= (int64_t)Pow10(kDigits)) { /* rounding carried into a new digit */
        Digits /= 10;
        E++;
    }
    char Buffer[24];
    for (int i = kDigits - 1; i >= 0; i--) {
        Buffer[i] = (char)('0' + Digits % 10);
        Digits /= 10;
    }
    int Used = kDigits;
    while (Used > 1 && Buffer[Used - 1] == '0')
        Used--;
    if (E >= kDigits || E < -9) { /* scientific */
        Out.Append(Buffer[0]);
        if (Used > 1) {
            Out.Append('.');
            Out.Append(Buffer + 1, (size_t)(Used - 1));
        }
        Out.Append(hz::String::Format("e%c%d", E < 0 ? '-' : '+', E < 0 ? -E : E));
        return Out;
    }
    if (E < 0) {
        Out.Append("0.");
        for (int i = 0; i < -E - 1; i++)
            Out.Append('0');
        Out.Append(Buffer, (size_t)Used);
        return Out;
    }
    /* E + 1 integer digits */
    for (int i = 0; i <= E; i++)
        Out.Append(i < Used ? Buffer[i] : '0');
    if (Used > E + 1) {
        Out.Append('.');
        Out.Append(Buffer + E + 1, (size_t)(Used - E - 1));
    }
    return Out;
}

bool ParseNumber(const char *Text, double *Value)
{
    const char *P = Text;
    bool Negative = false;
    if (*P == '-' || *P == '+')
        Negative = *P++ == '-';
    double Result = 0;
    bool Any = false;
    while (*P >= '0' && *P <= '9') {
        Result = Result * 10 + (*P++ - '0');
        Any = true;
    }
    if (*P == '.') {
        P++;
        double Scale = 0.1;
        while (*P >= '0' && *P <= '9') {
            Result += (*P++ - '0') * Scale;
            Scale /= 10;
            Any = true;
        }
    }
    if (!Any)
        return false;
    if (*P == 'e' || *P == 'E') {
        P++;
        bool NegativeExponent = false;
        if (*P == '-' || *P == '+')
            NegativeExponent = *P++ == '-';
        if (*P < '0' || *P > '9')
            return false;
        int Exponent = 0;
        while (*P >= '0' && *P <= '9' && Exponent < 1000)
            Exponent = Exponent * 10 + (*P++ - '0');
        Result = NegativeExponent ? Result / Pow10(Exponent) : Result * Pow10(Exponent);
    }
    if (*P)
        return false;
    *Value = Negative ? -Result : Result;
    return true;
}

/* ---- Engine ---------------------------------------------------------------------------------- */

double Engine::Value() const
{
    double V = 0;
    ParseNumber(Display_.CStr(), &V);
    return V;
}

void Engine::SetValue(double V)
{
    if (!IsFinite(V) || V > 1e300 || V < -1e300) {
        Fail("Overflow");
        return;
    }
    Display_ = FormatNumber(V);
}

void Engine::Fail(const char *Message)
{
    Display_ = Message;
    Error_ = true;
    Pending_ = 0;
    LastOp_ = 0;
    Fresh_ = true;
}

const char *Engine::OpText(char Op)
{
    switch (Op) {
    case '+': return "+";
    case '-': return "-";
    case '*': return "x";
    case '/': return "/";
    default: return "?";
    }
}

bool Engine::Apply(char Op, double Left, double Right, double *Result)
{
    switch (Op) {
    case '+': *Result = Left + Right; return true;
    case '-': *Result = Left - Right; return true;
    case '*': *Result = Left * Right; return true;
    case '/':
        if (Right == 0) {
            Fail(Left == 0 ? "Result is undefined" : "Cannot divide by zero");
            return false;
        }
        *Result = Left / Right;
        return true;
    default:
        *Result = Right;
        return true;
    }
}

void Engine::Clear()
{
    Display_ = "0";
    Expression_ = hz::String();
    Accumulator_ = 0;
    Pending_ = 0;
    LastOp_ = 0;
    LastOperand_ = 0;
    Fresh_ = true;
    Error_ = false;
}

void Engine::ClearEntry()
{
    if (Error_) {
        Clear();
        return;
    }
    Display_ = "0";
    Fresh_ = true;
}

void Engine::Digit(int Value)
{
    if (Error_)
        Clear();
    if (Fresh_) {
        Display_ = "0";
        Fresh_ = false;
        if (!Pending_)
            Expression_ = hz::String();
    }
    size_t Count = 0;
    for (size_t i = 0; i < Display_.Length(); i++)
        Count += Display_[i] >= '0' && Display_[i] <= '9';
    if (Count >= kMaxEntry)
        return;
    if (Display_ == "0")
        Display_ = hz::String();
    else if (Display_ == "-0")
        Display_ = "-";
    Display_.Append((char)('0' + Value));
}

void Engine::Point()
{
    if (Error_)
        Clear();
    if (Fresh_) {
        Display_ = "0";
        Fresh_ = false;
        if (!Pending_)
            Expression_ = hz::String();
    }
    if (Display_.Find(".") == hz::NotFound)
        Display_.Append('.');
}

void Engine::Backspace()
{
    if (Error_) {
        Clear();
        return;
    }
    if (Fresh_)
        return;
    Display_.Erase(Display_.Length() - 1, 1);
    if (Display_.Empty() || Display_ == "-")
        Display_ = "0";
}

void Engine::Negate()
{
    if (Error_)
        return;
    if (Display_ == "0")
        return;
    if (Display_[0] == '-')
        Display_.Erase(0, 1);
    else
        Display_.Insert(0, "-", 1);
}

void Engine::Operator(char Op)
{
    if (Error_)
        return;
    double Entry = Value();
    if (Pending_ && !Fresh_) {
        double Result;
        if (!Apply(Pending_, Accumulator_, Entry, &Result))
            return;
        Accumulator_ = Result;
        SetValue(Result);
        if (Error_)
            return;
    } else if (!Pending_ || !Fresh_) {
        Accumulator_ = Entry;
    }
    /* Pending with a fresh entry: the operator is replaced. */
    Pending_ = Op;
    LastOp_ = 0;
    Fresh_ = true;
    Expression_ = hz::String::Format("%s %s", FormatNumber(Accumulator_).CStr(), OpText(Op));
}

void Engine::Equals()
{
    if (Error_) {
        Clear();
        return;
    }
    double Entry = Value();
    double Result;
    if (Pending_) {
        double Right = Entry; /* "5 + =" uses the shown 5: 10 */
        Expression_ = hz::String::Format("%s %s %s =", FormatNumber(Accumulator_).CStr(), OpText(Pending_),
                                         FormatNumber(Right).CStr());
        if (!Apply(Pending_, Accumulator_, Right, &Result))
            return;
        LastOp_ = Pending_;
        LastOperand_ = Right;
        Pending_ = 0;
    } else if (LastOp_) { /* repeat: "2 + 3 = =" gives 8 */
        Expression_ = hz::String::Format("%s %s %s =", FormatNumber(Entry).CStr(), OpText(LastOp_),
                                         FormatNumber(LastOperand_).CStr());
        if (!Apply(LastOp_, Entry, LastOperand_, &Result))
            return;
    } else {
        Expression_ = hz::String::Format("%s =", FormatNumber(Entry).CStr());
        Result = Entry;
    }
    Accumulator_ = Result;
    SetValue(Result);
    Fresh_ = true;
}

void Engine::Percent()
{
    if (Error_)
        return;
    double Entry = Value();
    double Result = (Pending_ == '+' || Pending_ == '-') ? Accumulator_ * Entry / 100 : Entry / 100;
    SetValue(Result);
    Fresh_ = true;
}

void Engine::Reciprocal()
{
    if (Error_)
        return;
    double Entry = Value();
    if (Entry == 0) {
        Fail("Cannot divide by zero");
        return;
    }
    if (!Pending_)
        Expression_ = hz::String::Format("1/(%s)", FormatNumber(Entry).CStr());
    SetValue(1 / Entry);
    Fresh_ = true;
}

void Engine::Square()
{
    if (Error_)
        return;
    double Entry = Value();
    if (!Pending_)
        Expression_ = hz::String::Format("sqr(%s)", FormatNumber(Entry).CStr());
    SetValue(Entry * Entry);
    Fresh_ = true;
}

void Engine::Root()
{
    if (Error_)
        return;
    double Entry = Value();
    if (Entry < 0) {
        Fail("Invalid input");
        return;
    }
    if (!Pending_)
        Expression_ = hz::String::Format("sqrt(%s)", FormatNumber(Entry).CStr());
    SetValue(SquareRoot(Entry));
    Fresh_ = true;
}

bool Engine::Key(char C)
{
    if (C >= '0' && C <= '9')
        Digit(C - '0');
    else if (C == '.' || C == ',')
        Point();
    else if (C == '+' || C == '-' || C == '*' || C == '/')
        Operator(C);
    else if (C == 'x' || C == 'X')
        Operator('*');
    else if (C == '=' || C == '\r' || C == '\n')
        Equals();
    else if (C == '\b')
        Backspace();
    else if (C == 0x1B)
        Clear();
    else if (C == '%')
        Percent();
    else if (C == 'r')
        Reciprocal();
    else if (C == 'q')
        Square();
    else if (C == '@')
        Root();
    else if (C == 'n')
        Negate();
    else
        return false;
    return true;
}

}  // namespace calc
