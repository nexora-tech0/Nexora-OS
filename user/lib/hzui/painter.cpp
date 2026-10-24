/*
 * Nexora OS - user/lib/hzui/painter.cpp
 * Geometry and colour helpers, fonts by role, the Painter (anti-aliased shapes from signed
 * distances, polylines from segment distances so joints are not blended twice, icons, text),
 * text layout and transitions.
 */
#include "hzmath.h"
#include "hzui.h"

namespace hzui {

/* ---- Geometry ----------------------------------------------------------------------------------- */

Rect Rect::Intersect(const Rect &O) const
{
    int32_t L = hz::Max(X, O.X), T = hz::Max(Y, O.Y);
    int32_t R = hz::Min(Right(), O.Right()), B = hz::Min(Bottom(), O.Bottom());
    if (R <= L || B <= T)
        return Rect{L, T, 0, 0};
    return Rect{L, T, R - L, B - T};
}

Rect Rect::Union(const Rect &O) const
{
    if (Empty())
        return O;
    if (O.Empty())
        return *this;
    int32_t L = hz::Min(X, O.X), T = hz::Min(Y, O.Y);
    return Rect{L, T, hz::Max(Right(), O.Right()) - L, hz::Max(Bottom(), O.Bottom()) - T};
}

Rect Rect::Deflate(const Thickness &T) const
{
    return Rect{X + T.Left, Y + T.Top, hz::Max(Width - T.Horizontal(), 0), hz::Max(Height - T.Vertical(), 0)};
}

/* ---- Colours and fonts --------------------------------------------------------------------------- */

Color WithAlpha(Color C, uint32_t Alpha)
{
    return (C & 0xFFFFFF) | (hz::Min<uint32_t>(Alpha, 255) << 24);
}

Color ScaleAlpha(Color C, float Factor)
{
    float A = (float)(C >> 24) * hz::Clamp(Factor, 0.0f, 1.0f);
    return WithAlpha(C, (uint32_t)(A + 0.5f));
}

Color MixColors(Color From, Color To, float T)
{
    T = hz::Clamp(T, 0.0f, 1.0f);
    Color Out = 0;
    for (int Shift = 0; Shift <= 24; Shift += 8) {
        float A = (float)((From >> Shift) & 0xFF), B = (float)((To >> Shift) & 0xFF);
        Out |= (uint32_t)(A + (B - A) * T + 0.5f) << Shift;
    }
    return Out;
}

HZ_FONT *GetFont(FontRole Role)
{
    switch (Role) {
    case FontRole::BodyStrong:
        return HzGetFont(HZ_FONT_UI_SEMIBOLD);
    case FontRole::Caption:
        return HzGetFont(HZ_FONT_SMALL);
    case FontRole::Subtitle:
        return HzGetFont(HZ_FONT_SUBTITLE);
    case FontRole::Title:
        return HzGetFont(HZ_FONT_TITLE);
    case FontRole::Mono:
        return HzGetFont(HZ_FONT_MONO);
    case FontRole::BodyLarge:
        return HzGetFont(HZ_FONT_BODY_LARGE);
    case FontRole::Display:
        return HzGetFont(HZ_FONT_DISPLAY);
    default:
        return HzGetFont(HZ_FONT_UI);
    }
}

uint64_t NowUs()
{
    uint64_t Now = 0;
    NtQueryUptime(&Now);
    return Now;
}

/* ---- Painter ------------------------------------------------------------------------------------ */

namespace {

inline float Abs(float V)
{
    return V < 0 ? -V : V;
}

inline float Saturate(float V)
{
    return V < 0 ? 0 : (V > 1 ? 1 : V);
}

/* Non-premultiplied "over" with the destination's alpha (see hzuser draw.cpp). */
inline void Blend(uint32_t *P, Color Src, uint32_t Coverage)
{
    uint32_t Sa = (Src >> 24) * Coverage / 255;
    if (Sa == 0)
        return;
    uint32_t Dst = *P;
    if (Sa == 255) {
        *P = Src | 0xFF000000;
        return;
    }
    uint32_t Keep = (Dst >> 24) * (255 - Sa) / 255;
    uint32_t Oa = Sa + Keep;
    uint32_t Out = Oa << 24;
    for (int Shift = 0; Shift <= 16; Shift += 8) {
        uint32_t S = (Src >> Shift) & 0xFF, D = (Dst >> Shift) & 0xFF;
        Out |= ((S * Sa + D * Keep) / Oa) << Shift;
    }
    *P = Out;
}

inline float SegmentDistance(float Px, float Py, float X0, float Y0, float X1, float Y1)
{
    float Dx = X1 - X0, Dy = Y1 - Y0, L2 = Dx * Dx + Dy * Dy;
    float T = L2 > 0 ? Saturate(((Px - X0) * Dx + (Py - Y0) * Dy) / L2) : 0;
    float Qx = X0 + T * Dx - Px, Qy = Y0 + T * Dy - Py;
    return (float)HzSqrt(Qx * Qx + Qy * Qy);
}

}  // namespace

Painter::Painter(const HZ_CANVAS &Canvas) : Canvas_(Canvas)
{
}

void Painter::PushClip(const Rect &R)
{
    Clips_.Push(Canvas_.Clip);
    Canvas_.Clip = Clip().Intersect(R).ToHz();
}

void Painter::PopClip()
{
    if (!Clips_.Empty()) {
        Canvas_.Clip = Clips_.Back();
        Clips_.Pop();
    }
}

void Painter::Clear(const Rect &R, Color C)
{
    Rect A = Clip().Intersect(R);
    for (int32_t y = A.Y; y < A.Bottom(); y++) {
        uint32_t *Row = Canvas_.Pixels + (size_t)y * Canvas_.Stride;
        for (int32_t x = A.X; x < A.Right(); x++)
            Row[x] = C;
    }
}

void Painter::FillRect(const Rect &R, Color C)
{
    if (!(C >> 24))
        return;
    Rect A = Clip().Intersect(R);
    for (int32_t y = A.Y; y < A.Bottom(); y++) {
        uint32_t *Row = Canvas_.Pixels + (size_t)y * Canvas_.Stride;
        for (int32_t x = A.X; x < A.Right(); x++)
            Blend(&Row[x], C, 255);
    }
}

void Painter::DrawBitmap(const Rect &Destination, const uint32_t *Pixels, int32_t Width, int32_t Height)
{
    if (Destination.Empty() || !Pixels || Width <= 0 || Height <= 0)
        return;
    Rect Area = Destination.Intersect(Clip());
    for (int32_t Y = Area.Y; Y < Area.Bottom(); Y++) {
        int32_t SourceY = (int64_t)(Y - Destination.Y) * Height / Destination.Height;
        uint32_t *Row = Canvas_.Pixels + (size_t)Y * Canvas_.Stride;
        const uint32_t *Source = Pixels + (size_t)SourceY * Width;
        for (int32_t X = Area.X; X < Area.Right(); X++) {
            int32_t SourceX = (int64_t)(X - Destination.X) * Width / Destination.Width;
            Row[X] = Source[SourceX] | 0xFF000000u;
        }
    }
}

void Painter::RoundRectCoverage(float X, float Y, float W, float H, float Radius, float Thickness, Color Top,
                                Color Bottom)
{
    if (W <= 0 || H <= 0 || (!(Top >> 24) && !(Bottom >> 24)))
        return;
    float Hx = W / 2, Hy = H / 2, Cx = X + Hx, Cy = Y + Hy;
    float R = hz::Min(Radius, hz::Min(Hx, Hy));
    int32_t L = hz::Max((int32_t)HzFloor(X), Canvas_.Clip.Left), T = hz::Max((int32_t)HzFloor(Y), Canvas_.Clip.Top);
    int32_t Ri = hz::Min((int32_t)HzCeil(X + W), Canvas_.Clip.Right);
    int32_t Bo = hz::Min((int32_t)HzCeil(Y + H), Canvas_.Clip.Bottom);
    bool Gradient = Top != Bottom;
    for (int32_t y = T; y < Bo; y++) {
        float Py = y + 0.5f, Qy = Abs(Py - Cy) - (Hy - R);
        Color C = Gradient ? MixColors(Top, Bottom, (Py - Y) / H) : Top;
        uint32_t *Row = Canvas_.Pixels + (size_t)y * Canvas_.Stride;
        for (int32_t x = L; x < Ri; x++) {
            float Px = x + 0.5f, Qx = Abs(Px - Cx) - (Hx - R);
            float D = (Qx > 0 && Qy > 0) ? (float)HzSqrt(Qx * Qx + Qy * Qy) - R : hz::Max(Qx, Qy) - R;
            float Coverage = Saturate(0.5f - D);
            if (Thickness > 0)
                Coverage -= Saturate(0.5f - (D + Thickness));
            if (Coverage > 0.002f)
                Blend(&Row[x], C, (uint32_t)(Coverage * 255 + 0.5f));
        }
    }
}

void Painter::FillRoundRect(float X, float Y, float W, float H, float Radius, Color C)
{
    RoundRectCoverage(X, Y, W, H, Radius, 0, C, C);
}

void Painter::StrokeRoundRect(float X, float Y, float W, float H, float Radius, float Thickness, Color C)
{
    RoundRectCoverage(X, Y, W, H, Radius, Thickness, C, C);
}

void Painter::FillVerticalGradient(const Rect &R, float Radius, Color Top, Color Bottom)
{
    RoundRectCoverage((float)R.X, (float)R.Y, (float)R.Width, (float)R.Height, Radius, 0, Top, Bottom);
}

void Painter::FillCircle(float Cx, float Cy, float Radius, Color C)
{
    RoundRectCoverage(Cx - Radius, Cy - Radius, 2 * Radius, 2 * Radius, Radius, 0, C, C);
}

void Painter::StrokeCircle(float Cx, float Cy, float Radius, float Thickness, Color C)
{
    float Outer = Radius + Thickness / 2;
    RoundRectCoverage(Cx - Outer, Cy - Outer, 2 * Outer, 2 * Outer, Outer, Thickness, C, C);
}

void Painter::Line(float X0, float Y0, float X1, float Y1, float Width, Color C)
{
    float Points[4] = {X0, Y0, X1, Y1};
    Polyline(Points, 2, Width, C);
}

void Painter::Polyline(const float *Points, int Count, float Width, Color C)
{
    if (Count < 2)
        return;
    float MinX = Points[0], MaxX = Points[0], MinY = Points[1], MaxY = Points[1];
    for (int i = 1; i < Count; i++) {
        MinX = hz::Min(MinX, Points[2 * i]);
        MaxX = hz::Max(MaxX, Points[2 * i]);
        MinY = hz::Min(MinY, Points[2 * i + 1]);
        MaxY = hz::Max(MaxY, Points[2 * i + 1]);
    }
    float Half = Width / 2;
    int32_t L = hz::Max((int32_t)HzFloor(MinX - Half - 1), Canvas_.Clip.Left);
    int32_t T = hz::Max((int32_t)HzFloor(MinY - Half - 1), Canvas_.Clip.Top);
    int32_t R = hz::Min((int32_t)HzCeil(MaxX + Half + 1), Canvas_.Clip.Right);
    int32_t B = hz::Min((int32_t)HzCeil(MaxY + Half + 1), Canvas_.Clip.Bottom);
    for (int32_t y = T; y < B; y++) {
        uint32_t *Row = Canvas_.Pixels + (size_t)y * Canvas_.Stride;
        for (int32_t x = L; x < R; x++) {
            float Px = x + 0.5f, Py = y + 0.5f, D = 1e9f;
            for (int i = 0; i + 1 < Count; i++)
                D = hz::Min(D, SegmentDistance(Px, Py, Points[2 * i], Points[2 * i + 1], Points[2 * i + 2],
                                               Points[2 * i + 3]));
            float Coverage = Saturate(Half - D + 0.5f);
            if (Coverage > 0.002f)
                Blend(&Row[x], C, (uint32_t)(Coverage * 255 + 0.5f));
        }
    }
}

int32_t Painter::Text(HZ_FONT *Font, int32_t X, int32_t Baseline, const char *Text, size_t Length, Color C)
{
    return HzDrawTextN(&Canvas_, Font, X, Baseline, Text, Length, C);
}

void Painter::TextLine(HZ_FONT *Font, const Rect &R, const char *Text, Color C, Align Horizontal)
{
    if (!Font)
        return;
    size_t Length = strlen(Text);
    int32_t Width = HzMeasureTextN(Font, Text, Length);
    hz::String Cut;
    if (Width > R.Width) {
        Cut = EllipsizeText(Font, Text, Length, R.Width);
        Text = Cut.CStr();
        Length = Cut.Length();
        Width = HzMeasureTextN(Font, Text, Length);
    }
    int32_t X = R.X;
    if (Horizontal == Align::Center)
        X += (R.Width - Width) / 2;
    else if (Horizontal == Align::End)
        X += R.Width - Width;
    int32_t Baseline = R.Y + (R.Height - HzFontLineHeight(Font)) / 2 + HzFontAscent(Font);
    PushClip(R);
    this->Text(Font, X, Baseline, Text, Length, C);
    PopClip();
}

void Painter::FocusRing(const Rect &R, float Radius)
{
    Rect Outer = R.Inflate(3);
    StrokeRoundRect(Outer, Radius + 3, 2, Theme::FocusStroke);
    Rect Inner = R.Inflate(1);
    StrokeRoundRect(Inner, Radius + 1, 1, Theme::FocusStrokeInner);
}

void Painter::DrawIcon(Icon Kind, const Rect &Box, Color C, float Stroke)
{
    float S = Box.Width / 16.0f, Ox = (float)Box.X, Oy = (float)Box.Y + (Box.Height - Box.Width) / 2.0f;
    auto Poly = [&](auto... Coordinates) {
        float List[] = {(float)Coordinates...};
        constexpr int N = sizeof...(Coordinates);
        static_assert(N % 2 == 0 && N <= 24, "x, y pairs");
        float Points[N];
        for (int i = 0; i < N; i++)
            Points[i] = (i % 2 == 0) ? Ox + List[i] * S : Oy + List[i] * S;
        Polyline(Points, N / 2, Stroke * S, C);
    };
    switch (Kind) {
    case Icon::Check:
        Poly(3.5f, 8.5f, 6.5f, 11.5f, 12.5f, 4.5f);
        break;
    case Icon::ChevronDown:
        Poly(4, 6, 8, 10, 12, 6);
        break;
    case Icon::ChevronUp:
        Poly(4, 10, 8, 6, 12, 10);
        break;
    case Icon::ChevronLeft:
        Poly(10, 4, 6, 8, 10, 12);
        break;
    case Icon::ChevronRight:
        Poly(6, 4, 10, 8, 6, 12);
        break;
    case Icon::Close:
        Poly(4, 4, 12, 12);
        Poly(12, 4, 4, 12);
        break;
    case Icon::Add:
        Poly(8, 3, 8, 13);
        Poly(3, 8, 13, 8);
        break;
    case Icon::Remove:
        Poly(3, 8, 13, 8);
        break;
    case Icon::Search:
        StrokeCircle(Ox + 7 * S, Oy + 7 * S, 4.5f * S, Stroke * S, C);
        Poly(10.3f, 10.3f, 14, 14);
        break;
    case Icon::More:
        FillCircle(Ox + 3.5f * S, Oy + 8 * S, 1.15f * S, C);
        FillCircle(Ox + 8 * S, Oy + 8 * S, 1.15f * S, C);
        FillCircle(Ox + 12.5f * S, Oy + 8 * S, 1.15f * S, C);
        break;
    case Icon::Info:
        StrokeCircle(Ox + 8 * S, Oy + 8 * S, 6.5f * S, Stroke * S, C);
        FillCircle(Ox + 8 * S, Oy + 5 * S, 0.9f * S, C);
        Poly(8, 7.5f, 8, 11.5f);
        break;
    case Icon::Clock:
        StrokeCircle(Ox + 8 * S, Oy + 8 * S, 6.5f * S, Stroke * S, C);
        Poly(8, 4.5f, 8, 8, 10.5f, 9.5f);
        break;
    case Icon::Home:
        Poly(2.5f, 8, 8, 2.8f, 13.5f, 8);
        Poly(4, 6.8f, 4, 13.5f, 12, 13.5f, 12, 6.8f);
        break;
    case Icon::Settings:
        StrokeCircle(Ox + 8 * S, Oy + 8 * S, 2.2f * S, Stroke * S, C);
        StrokeCircle(Ox + 8 * S, Oy + 8 * S, 5.0f * S, Stroke * S, C);
        for (int i = 0; i < 8; i++) {
            double A = i * HZ_PI / 4;
            float Ca = (float)HzCos(A), Sa = (float)HzSin(A);
            Poly(8 + 5.0f * Ca, 8 + 5.0f * Sa, 8 + 7.0f * Ca, 8 + 7.0f * Sa);
        }
        break;
    case Icon::Power: { /* an open ring with a bar through the gap */
        float Points[22];
        for (int i = 0; i <= 10; i++) {
            double A = -HZ_PI / 2 + HZ_PI / 5 + i * (2 * HZ_PI - 2 * HZ_PI / 5) / 10;
            Points[2 * i] = Ox + (8 + 5.5f * (float)HzCos(A)) * S;
            Points[2 * i + 1] = Oy + (8.5f + 5.5f * (float)HzSin(A)) * S;
        }
        Polyline(Points, 11, Stroke * S, C);
        Poly(8, 1.8f, 8, 7.5f);
        break;
    }
    case Icon::Volume:
        Poly(2.5f, 6, 5, 6, 8.5f, 3, 8.5f, 13, 5, 10, 2.5f, 10, 2.5f, 6);
        Poly(10.8f, 6, 11.6f, 8, 10.8f, 10);
        Poly(12.8f, 4, 14.2f, 8, 12.8f, 12);
        break;
    case Icon::Sun:
        StrokeCircle(Ox + 8 * S, Oy + 8 * S, 2.8f * S, Stroke * S, C);
        for (int i = 0; i < 8; i++) {
            double A = i * HZ_PI / 4;
            float Ca = (float)HzCos(A), Sa = (float)HzSin(A);
            Poly(8 + 4.8f * Ca, 8 + 4.8f * Sa, 8 + 6.6f * Ca, 8 + 6.6f * Sa);
        }
        break;
    case Icon::Moon: { /* a crescent: an outer arc and an inner arc */
        float Points[26];
        for (int i = 0; i <= 6; i++) {
            double A = HZ_PI * 0.30 + i * HZ_PI * 1.40 / 6;
            Points[2 * i] = Ox + (8 + 6 * (float)HzCos(A)) * S;
            Points[2 * i + 1] = Oy + (8 + 6 * (float)HzSin(A)) * S;
        }
        for (int i = 0; i <= 5; i++) {
            double A = HZ_PI * 1.55 - i * HZ_PI * 1.10 / 5;
            Points[14 + 2 * i] = Ox + (10.2f + 4.4f * (float)HzCos(A)) * S;
            Points[15 + 2 * i] = Oy + (6.2f + 4.4f * (float)HzSin(A)) * S;
        }
        Polyline(Points, 13, Stroke * S, C);
        break;
    }
    case Icon::Leaf:
        Poly(3, 13, 3.5f, 7, 7, 3.5f, 13.5f, 2.5f, 12.5f, 9, 9, 12.5f, 3, 13);
        Poly(3, 13, 10, 6);
        break;
    case Icon::Plane:
        Poly(8, 1.5f, 9, 3, 9, 6.5f, 14.5f, 9.5f, 14.5f, 11, 9, 9.3f, 9, 12.5f, 10.8f, 14, 10.8f, 15);
        Poly(8, 1.5f, 7, 3, 7, 6.5f, 1.5f, 9.5f, 1.5f, 11, 7, 9.3f, 7, 12.5f, 5.2f, 14, 5.2f, 15);
        break;
    case Icon::Folder:
        FillRoundRect(Ox + 1.5f * S, Oy + 3.5f * S, 5.5f * S, 3 * S, 1 * S, C);
        FillRoundRect(Ox + 1.5f * S, Oy + 5 * S, 13 * S, 8.5f * S, 1.5f * S, C);
        break;
    case Icon::Document:
        Poly(3.5f, 1.5f, 9.5f, 1.5f, 12.5f, 4.5f, 12.5f, 14.5f, 3.5f, 14.5f, 3.5f, 1.5f);
        Poly(9.5f, 1.5f, 9.5f, 4.5f, 12.5f, 4.5f);
        Poly(5.5f, 8, 10.5f, 8);
        Poly(5.5f, 11, 10.5f, 11);
        break;
    case Icon::Drive:
        StrokeRoundRect(Ox + 1.5f * S, Oy + 4.5f * S, 13 * S, 7 * S, 1.5f * S, Stroke * S, C);
        FillCircle(Ox + 11.5f * S, Oy + 8 * S, 0.9f * S, C);
        Poly(4, 8, 8, 8);
        break;
    case Icon::Prompt:
        StrokeRoundRect(Ox + 1.5f * S, Oy + 2.5f * S, 13 * S, 11 * S, 1.5f * S, Stroke * S, C);
        Poly(4.2f, 6, 6.7f, 8, 4.2f, 10);
        Poly(8, 10.5f, 11.5f, 10.5f);
        break;
    case Icon::Calculator:
        StrokeRoundRect(Ox + 3 * S, Oy + 1.5f * S, 10 * S, 13 * S, 1.5f * S, Stroke * S, C);
        Poly(5.5f, 4.5f, 10.5f, 4.5f);
        for (int Row = 0; Row < 2; Row++)
            for (int Column = 0; Column < 3; Column++)
                FillCircle(Ox + (5.7f + Column * 2.3f) * S, Oy + (8.5f + Row * 2.8f) * S, 0.75f * S, C);
        break;
    case Icon::Chart:
        Poly(2, 2, 2, 14, 14, 14);
        Poly(4, 11, 7, 7, 9.5f, 9, 13.5f, 3.5f);
        break;
    case Icon::ArrowUp:
        Poly(8, 13.5f, 8, 3);
        Poly(3.5f, 7.5f, 8, 3, 12.5f, 7.5f);
        break;
    case Icon::Pencil:
        Poly(3, 13, 3.5f, 10.5f, 11, 3, 13, 5, 5.5f, 12.5f, 3, 13);
        Poly(9.5f, 4.5f, 11.5f, 6.5f);
        break;
    case Icon::Trash:
        Poly(3, 4.5f, 13, 4.5f);
        Poly(6, 4.5f, 6.5f, 2.5f, 9.5f, 2.5f, 10, 4.5f);
        Poly(4.5f, 4.5f, 5.2f, 14, 10.8f, 14, 11.5f, 4.5f);
        Poly(7, 7, 7, 11.5f);
        Poly(9, 7, 9, 11.5f);
        break;
    case Icon::Save:
        Poly(2.5f, 2.5f, 11.5f, 2.5f, 13.5f, 4.5f, 13.5f, 13.5f, 2.5f, 13.5f, 2.5f, 2.5f);
        Poly(5, 2.5f, 5, 6, 10.5f, 6, 10.5f, 2.5f);
        Poly(5, 13.5f, 5, 9.5f, 11, 9.5f, 11, 13.5f);
        break;
    case Icon::Open:
        Poly(1.5f, 13, 1.5f, 3.5f, 6, 3.5f, 7.5f, 5, 13, 5, 13, 7);
        Poly(1.5f, 13, 4, 7.5f, 14.5f, 7.5f, 12, 13, 1.5f, 13);
        break;
    case Icon::Cut: /* scissors: two rings (octagons) and crossed blades */
        Poly(6.4f, 12, 5.84f, 13.34f, 4.5f, 13.9f, 3.16f, 13.34f, 2.6f, 12, 3.16f, 10.66f, 4.5f, 10.1f, 5.84f, 10.66f,
             6.4f, 12);
        Poly(13.4f, 12, 12.84f, 13.34f, 11.5f, 13.9f, 10.16f, 13.34f, 9.6f, 12, 10.16f, 10.66f, 11.5f, 10.1f,
             12.84f, 10.66f, 13.4f, 12);
        Poly(5.84f, 10.66f, 11, 2.5f);
        Poly(10.16f, 10.66f, 5, 2.5f);
        break;
    case Icon::Copy: /* two sheets */
        Poly(2.5f, 5.5f, 10.5f, 5.5f, 10.5f, 13.5f, 2.5f, 13.5f, 2.5f, 5.5f);
        Poly(5.5f, 5.5f, 5.5f, 2.5f, 13.5f, 2.5f, 13.5f, 10.5f, 10.5f, 10.5f);
        break;
    case Icon::Cube: /* an isometric block */
        Poly(8, 1.5f, 14, 5, 14, 11.5f, 8, 15, 2, 11.5f, 2, 5, 8, 1.5f);
        Poly(2, 5, 8, 8.5f, 14, 5);
        Poly(8, 8.5f, 8, 15);
        break;
    case Icon::Person: /* head and shoulders */
        StrokeCircle(Ox + 8 * S, Oy + 5.2f * S, 2.7f * S, Stroke * S, C);
        Poly(3, 14, 3.5f, 11.5f, 5.5f, 9.8f, 10.5f, 9.8f, 12.5f, 11.5f, 13, 14);
        break;
    case Icon::Wifi:
        Poly(2.2f, 6.5f, 3.5f, 5.4f, 5, 4.6f, 6.5f, 4.2f, 8, 4.1f, 9.5f, 4.2f,
             11, 4.6f, 12.5f, 5.4f, 13.8f, 6.5f);
        Poly(4.6f, 9, 5.8f, 8.1f, 7, 7.7f, 8, 7.6f, 9, 7.7f, 10.2f, 8.1f, 11.4f, 9);
        Poly(7.1f, 11.4f, 8, 10.8f, 8.9f, 11.4f);
        FillCircle(Ox + 8 * S, Oy + 13.3f * S, 0.75f * S, C);
        break;
    case Icon::Accessibility:
        StrokeCircle(Ox + 8 * S, Oy + 3.3f * S, 1.6f * S, Stroke * S, C);
        Poly(2.8f, 6.7f, 5.3f, 7.8f, 8, 8.1f, 10.7f, 7.8f, 13.2f, 6.7f);
        Poly(8, 8.1f, 8, 11.3f, 5.2f, 14.2f);
        Poly(8, 11.3f, 10.8f, 14.2f);
        break;
    case Icon::AppWindow:
        Poly(2.1f, 3, 13.9f, 3, 13.9f, 13, 2.1f, 13, 2.1f, 3);
        Poly(2.1f, 5.6f, 13.9f, 5.6f);
        FillCircle(Ox + 4.1f * S, Oy + 4.3f * S, 0.55f * S, C);
        FillCircle(Ox + 5.8f * S, Oy + 4.3f * S, 0.55f * S, C);
        Poly(4.3f, 8, 11.7f, 8);
        Poly(4.3f, 10.4f, 9.3f, 10.4f);
        break;
    case Icon::Nexora:
        Poly(4.25f, 4, 4.25f, 12, 11.75f, 4, 11.75f, 12);
        break;
    case Icon::Paste: /* a clipboard */
        Poly(5, 3.5f, 3, 3.5f, 3, 14, 13, 14, 13, 3.5f, 11, 3.5f);
        Poly(5.5f, 2, 10.5f, 2, 10.5f, 5, 5.5f, 5, 5.5f, 2);
        break;
    case Icon::Refresh: /* an arc from -60 to 250 degrees around (8, 8) and an arrowhead */
        Poly(10.75f, 3.24f, 13.17f, 6.12f, 13.17f, 9.88f, 10.75f, 12.76f, 7.04f, 13.42f, 3.79f, 11.54f, 2.5f, 8,
             3.79f, 4.46f, 6.12f, 2.83f);
        Poly(10.6f, 0.6f, 10.75f, 3.24f, 13.4f, 3.4f);
        break;
    case Icon::Star: {
        float Points[22];
        for (int i = 0; i <= 10; i++) {
            double A = -HZ_PI / 2 + i * HZ_PI / 5;
            float Radius = (i % 2 == 0) ? 6.5f : 2.7f;
            Points[2 * i] = Ox + (8 + Radius * (float)HzCos(A)) * S;
            Points[2 * i + 1] = Oy + (8.4f + Radius * (float)HzSin(A)) * S;
        }
        Polyline(Points, 11, Stroke * S, C);
        break;
    }
    default:
        break;
    }
}

/* ---- Text layout ---------------------------------------------------------------------------------- */

namespace {

size_t NextBoundary(const char *Text, size_t Length, size_t At)
{
    At++;
    while (At < Length && ((uint8_t)Text[At] & 0xC0) == 0x80)
        At++;
    return At;
}

}  // namespace

void TextLayout::Layout(HZ_FONT *Font, const char *Text, size_t Length, int32_t MaxWidth)
{
    Lines.Clear();
    Width = 0;
    LineHeight = HzFontLineHeight(Font);
    auto Emit = [&](size_t Start, size_t End) {
        while (End > Start && Text[End - 1] == ' ') /* trailing spaces do not count */
            End--;
        TextLine Line;
        Line.Start = Start;
        Line.Length = End - Start;
        Line.Width = HzMeasureTextN(Font, Text + Start, End - Start);
        Width = hz::Max(Width, Line.Width);
        Lines.Push(Line);
    };
    size_t Start = 0;
    while (Start <= Length) {
        size_t ParagraphEnd = Start;
        while (ParagraphEnd < Length && Text[ParagraphEnd] != '\n')
            ParagraphEnd++;
        size_t LineStart = Start;
        if (MaxWidth > 0) {
            size_t Break = hz::NotFound, i = LineStart;
            while (i < ParagraphEnd) {
                size_t Next = NextBoundary(Text, Length, i);
                if (Next > ParagraphEnd)
                    Next = ParagraphEnd;
                if (Text[i] != ' ' && HzMeasureTextN(Font, Text + LineStart, Next - LineStart) > MaxWidth &&
                    i > LineStart) {
                    size_t End = Break != hz::NotFound ? Break : i;
                    Emit(LineStart, End);
                    LineStart = End;
                    while (LineStart < ParagraphEnd && Text[LineStart] == ' ')
                        LineStart++;
                    Break = hz::NotFound;
                    i = LineStart;
                    continue;
                }
                if (Text[i] == ' ')
                    Break = i;
                i = Next;
            }
        }
        Emit(LineStart, ParagraphEnd);
        Start = ParagraphEnd + 1;
    }
    Height = (int32_t)Lines.Size() * LineHeight;
}

hz::String EllipsizeText(HZ_FONT *Font, const char *Text, size_t Length, int32_t MaxWidth)
{
    if (HzMeasureTextN(Font, Text, Length) <= MaxWidth)
        return hz::String(Text, Length);
    static const char Dots[] = "\xE2\x80\xA6"; /* U+2026 */
    int32_t DotsWidth = HzMeasureText(Font, Dots);
    size_t Keep = 0;
    for (size_t i = 0; i < Length;) {
        size_t Next = NextBoundary(Text, Length, i);
        if (HzMeasureTextN(Font, Text, Next) + DotsWidth > MaxWidth)
            break;
        Keep = Next;
        i = Next;
    }
    while (Keep > 0 && Text[Keep - 1] == ' ')
        Keep--;
    hz::String Out(Text, Keep);
    Out.Append(Dots);
    return Out;
}

/* ---- Transitions ------------------------------------------------------------------------------------ */

void Transition::To(float Target, uint32_t DurationMs)
{
    if (Target == To_ && !Running())
        return;
    From_ = Value();
    To_ = Target;
    StartUs_ = NowUs();
    DurationUs_ = DurationMs * 1000;
}

void Transition::Jump(float Value)
{
    From_ = To_ = Value;
    DurationUs_ = 0;
}

float Transition::Value() const
{
    if (!DurationUs_)
        return To_;
    uint64_t Now = NowUs();
    if (Now >= StartUs_ + DurationUs_)
        return To_;
    float T = (float)(Now - StartUs_) / (float)DurationUs_;
    float Eased = 1 - (1 - T) * (1 - T) * (1 - T);
    return From_ + (To_ - From_) * Eased;
}

bool Transition::Running() const
{
    return DurationUs_ && NowUs() < StartUs_ + DurationUs_;
}

}  // namespace hzui
