/*
 * Nexora OS - user/apps/calc/engine.h
 * The calculator's engine (standard mode), kept apart from the window so it can be exercised on
 * its own: an entry being typed, an accumulator, a pending operator, repeated '=' (repeats the last
 * operation), percent, sign, reciprocal, square and square root, and number formatting with up
 * to 15 significant digits (scientific notation outside 1e-9 .. 1e15).
 */
#ifndef CALC_ENGINE_H
#define CALC_ENGINE_H

#include "hzcore.h"

namespace calc {

/* Formats a double for the display: "0", "-12.5", "0.3", "1.23456789e+20", "Overflow". */
hz::String FormatNumber(double Value);
/* Parses what FormatNumber writes and plain decimal input ("12", "-0.5", "3e4"). */
bool ParseNumber(const char *Text, double *Value);
double SquareRoot(double Value);

class Engine {
public:
    Engine() { Clear(); }

    void Digit(int Value);       /* 0..9 */
    void Point();
    void Operator(char Op);      /* '+', '-', '*', '/' */
    void Equals();
    void Clear();                /* C: everything */
    void ClearEntry();           /* CE: the entry only */
    void Backspace();
    void Negate();
    void Percent();              /* x% of the accumulator for + and -, x/100 otherwise */
    void Reciprocal();
    void Square();
    void Root();
    /* Feeds one key character ("0".."9", ".", "+-* /", "=", Enter as '\r', Backspace as '\b',
       Escape as 0x1B, '%', 'r' reciprocal, 'q' square, '@' root, 'n' negate). Returns false if
       the character means nothing. */
    bool Key(char C);

    const hz::String &Display() const { return Display_; }
    const hz::String &Expression() const { return Expression_; } /* the line above the display */
    bool HasError() const { return Error_; }
    double Value() const;

private:
    void SetValue(double V);
    void Fail(const char *Message);
    bool Apply(char Op, double Left, double Right, double *Result);
    static const char *OpText(char Op);

    hz::String Display_;
    hz::String Expression_;
    double Accumulator_ = 0;
    char Pending_ = 0;
    char LastOp_ = 0;
    double LastOperand_ = 0;
    bool Fresh_ = true;  /* the next digit starts a new entry */
    bool Error_ = false;
};

}  // namespace calc

#endif /* CALC_ENGINE_H */
