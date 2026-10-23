/*
 * Nexora OS - user/lib/hzuser/draw.cpp
 * Canvas drawing: fills, "over" compositing with non-premultiplied alpha (the destination's alpha
 * counts, so drawing on a transparent Mica area keeps it translucent), anti-aliased rounded
 * rectangles and lines. User mode may use floating point; the compositor in the kernel does not.
 */
#include "hzmath.h"
#include "hzuser.h"

namespace {

inline uint32_t Over(uint32_t Dst, uint32_t Src, uint32_t Coverage)
{
    uint32_t Sa = (Src >> 24) * Coverage / 255;
    if (Sa == 0)
        return Dst;
    uint32_t Da = Dst >> 24;
    uint32_t Keep = Da * (255 - Sa) / 255; /* destination weight */
    uint32_t Oa = Sa + Keep;
    if (Oa == 0)
        return 0;
    uint32_t Out = Oa << 24;
    for (int Shift = 0; Shift <= 16; Shift += 8) {
        uint32_t S = (Src >> Shift) & 0xFF, D = (Dst >> Shift) & 0xFF;
        Out |= ((S * Sa + D * Keep) / Oa) << Shift;
    }
    return Out;
}

inline HZ_RECT ClipRect(const HZ_CANVAS *C, HZ_RECT R)
{
    HZ_RECT Out = {HZ_MAX(R.Left, C->Clip.Left), HZ_MAX(R.Top, C->Clip.Top), HZ_MIN(R.Right, C->Clip.Right),
                   HZ_MIN(R.Bottom, C->Clip.Bottom)};
    return Out;
}

inline double Clamp01(double V)
{
    return V < 0 ? 0 : (V > 1 ? 1 : V);
}

}  // namespace

extern "C" void HzCanvasFromSurface(HZ_CANVAS *Canvas, const HZ_SURFACE *Surface)
{
    Canvas->Pixels = (uint32_t *)Surface->Pixels;
    Canvas->Width = (int32_t)Surface->Width;
    Canvas->Height = (int32_t)Surface->Height;
    Canvas->Stride = (int32_t)(Surface->Stride / 4);
    Canvas->Clip = HZ_RECT{0, 0, Canvas->Width, Canvas->Height};
}

extern "C" void HzBlendPixel(HZ_CANVAS *C, int32_t X, int32_t Y, uint32_t Color, uint32_t Coverage)
{
    if (X < C->Clip.Left || X >= C->Clip.Right || Y < C->Clip.Top || Y >= C->Clip.Bottom || !Coverage)
        return;
    uint32_t *P = C->Pixels + (size_t)Y * C->Stride + X;
    *P = Over(*P, Color, Coverage > 255 ? 255 : Coverage);
}

extern "C" void HzFillRect(HZ_CANVAS *C, HZ_RECT Rect, uint32_t Color)
{
    HZ_RECT R = ClipRect(C, Rect);
    for (int32_t y = R.Top; y < R.Bottom; y++) {
        uint32_t *Row = C->Pixels + (size_t)y * C->Stride;
        for (int32_t x = R.Left; x < R.Right; x++)
            Row[x] = Color;
    }
}

extern "C" void HzBlendRect(HZ_CANVAS *C, HZ_RECT Rect, uint32_t Color)
{
    HZ_RECT R = ClipRect(C, Rect);
    for (int32_t y = R.Top; y < R.Bottom; y++) {
        uint32_t *Row = C->Pixels + (size_t)y * C->Stride;
        for (int32_t x = R.Left; x < R.Right; x++)
            Row[x] = Over(Row[x], Color, 255);
    }
}

extern "C" void HzFillRoundRect(HZ_CANVAS *C, HZ_RECT Rect, int32_t Radius, uint32_t Color)
{
    HZ_RECT R = ClipRect(C, Rect);
    double Rad = Radius;
    for (int32_t y = R.Top; y < R.Bottom; y++) {
        for (int32_t x = R.Left; x < R.Right; x++) {
            /* Distance from the pixel centre to the nearest corner circle (inside the rounding). */
            double Px = x + 0.5, Py = y + 0.5;
            double Cx = Px < Rect.Left + Rad ? Rect.Left + Rad : (Px > Rect.Right - Rad ? Rect.Right - Rad : Px);
            double Cy = Py < Rect.Top + Rad ? Rect.Top + Rad : (Py > Rect.Bottom - Rad ? Rect.Bottom - Rad : Py);
            double Coverage = 1;
            if (Cx != Px && Cy != Py) {
                double D = HzSqrt((Px - Cx) * (Px - Cx) + (Py - Cy) * (Py - Cy));
                Coverage = Clamp01(Rad - D + 0.5);
            }
            HzBlendPixel(C, x, y, Color, (uint32_t)(Coverage * 255 + 0.5));
        }
    }
}

extern "C" void HzDrawLine(HZ_CANVAS *C, double X0, double Y0, double X1, double Y1, double Width, uint32_t Color)
{
    double Half = Width / 2;
    int32_t Left = (int32_t)HzFloor((X0 < X1 ? X0 : X1) - Half - 1), Right = (int32_t)HzCeil((X0 > X1 ? X0 : X1) + Half + 1);
    int32_t Top = (int32_t)HzFloor((Y0 < Y1 ? Y0 : Y1) - Half - 1), Bottom = (int32_t)HzCeil((Y0 > Y1 ? Y0 : Y1) + Half + 1);
    double Dx = X1 - X0, Dy = Y1 - Y0, Length2 = Dx * Dx + Dy * Dy;
    for (int32_t y = Top; y < Bottom; y++) {
        for (int32_t x = Left; x < Right; x++) {
            double Px = x + 0.5, Py = y + 0.5;
            double T = Length2 > 0 ? ((Px - X0) * Dx + (Py - Y0) * Dy) / Length2 : 0;
            T = Clamp01(T);
            double Qx = X0 + T * Dx - Px, Qy = Y0 + T * Dy - Py;
            double Coverage = Clamp01(Half - HzSqrt(Qx * Qx + Qy * Qy) + 0.5);
            if (Coverage > 0)
                HzBlendPixel(C, x, y, Color, (uint32_t)(Coverage * 255 + 0.5));
        }
    }
}
