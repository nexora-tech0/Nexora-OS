/*
 * Nexora OS - user/include/hzmath.h
 * Small inline math library for user programs (there is no libm). Accurate enough for graphics
 * and font rasterization: floor/ceil/trunc are exact, sqrt uses the SSE2 instruction, the
 * transcendental functions use range reduction plus short polynomials (about 1e-7 relative).
 */
#ifndef HZMATH_H
#define HZMATH_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

#define HZ_PI 3.14159265358979323846

static inline double HzFabs(double x)
{
    return x < 0 ? -x : x;
}

static inline double HzTrunc(double x)
{
    if (HzFabs(x) >= 4503599627370496.0) /* 2^52: already integral */
        return x;
    return (double)(int64_t)x;
}

static inline double HzFloor(double x)
{
    double t = HzTrunc(x);
    return t > x ? t - 1.0 : t;
}

static inline double HzCeil(double x)
{
    double t = HzTrunc(x);
    return t < x ? t + 1.0 : t;
}

static inline double HzSqrt(double x)
{
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static inline double HzFmod(double x, double y)
{
    if (y == 0)
        return 0;
    return x - HzTrunc(x / y) * y;
}

/* exp(x) = 2^k * exp(r), |r| <= ln2/2 */
static inline double HzExp(double x)
{
    if (x > 709)
        return 1.7976931348623157e308;
    if (x < -745)
        return 0;
    const double Ln2 = 0.69314718055994530942;
    double k = HzFloor(x / Ln2 + 0.5);
    double r = x - k * Ln2;
    double Term = 1, Sum = 1;
    for (int i = 1; i < 14; i++) {
        Term *= r / i;
        Sum += Term;
    }
    /* scale by 2^k through the exponent bits */
    int64_t e = (int64_t)k + 1023;
    if (e <= 0)
        return 0;
    uint64_t Bits = (uint64_t)e << 52;
    double Scale;
    __builtin_memcpy(&Scale, &Bits, sizeof(Scale));
    return Sum * Scale;
}

/* log(x) = e*ln2 + log(m), m in [sqrt(1/2), sqrt(2)) via atanh series */
static inline double HzLog(double x)
{
    if (x <= 0)
        return -1.7976931348623157e308;
    uint64_t Bits;
    __builtin_memcpy(&Bits, &x, sizeof(Bits));
    int64_t e = (int64_t)((Bits >> 52) & 0x7FF) - 1023;
    Bits = (Bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;
    double m;
    __builtin_memcpy(&m, &Bits, sizeof(m));
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        e++;
    }
    double s = (m - 1) / (m + 1), s2 = s * s, Term = s, Sum = 0;
    for (int i = 1; i < 40; i += 2) {
        Sum += Term / i;
        Term *= s2;
    }
    return 2 * Sum + (double)e * 0.69314718055994530942;
}

static inline double HzPow(double x, double y)
{
    if (y == 0)
        return 1;
    if (x == 0)
        return 0;
    if (x < 0) {
        double t = HzTrunc(y);
        if (t != y)
            return 0; /* not real */
        double r = HzExp(y * HzLog(-x));
        return ((int64_t)t & 1) ? -r : r;
    }
    return HzExp(y * HzLog(x));
}

static inline double HzSin(double x)
{
    /* reduce to [-pi, pi], then Taylor to x^15 */
    x = HzFmod(x, 2 * HZ_PI);
    if (x > HZ_PI)
        x -= 2 * HZ_PI;
    if (x < -HZ_PI)
        x += 2 * HZ_PI;
    double x2 = x * x, Term = x, Sum = x;
    for (int i = 1; i < 9; i++) {
        Term *= -x2 / ((2 * i) * (2 * i + 1));
        Sum += Term;
    }
    return Sum;
}

static inline double HzCos(double x)
{
    return HzSin(x + HZ_PI / 2);
}

static inline double HzAtan(double x)
{
    /* atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) halves the argument; then the series */
    bool Negative = x < 0;
    if (Negative)
        x = -x;
    bool Invert = x > 1;
    if (Invert)
        x = 1 / x;
    x = x / (1 + HzSqrt(1 + x * x));
    double x2 = x * x, Term = x, Sum = 0;
    for (int i = 1; i < 30; i += 2) {
        Sum += Term / i * ((i / 2) % 2 ? -1 : 1);
        Term *= x2;
    }
    Sum *= 2;
    if (Invert)
        Sum = HZ_PI / 2 - Sum;
    return Negative ? -Sum : Sum;
}

static inline double HzAcos(double x)
{
    if (x >= 1)
        return 0;
    if (x <= -1)
        return HZ_PI;
    return HZ_PI / 2 - HzAtan(x / HzSqrt(1 - x * x));
}

HZ_EXTERN_C_END

#endif /* HZMATH_H */
