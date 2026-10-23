/*
 * Nexora OS - drivers/bootvid/bootvid.c
 * Boot video driver: framebuffer primitives, antialiased text from pre-rasterized fonts, and
 * the procedurally drawn logo. Integer arithmetic only (the kernel has no FP/SIMD state).
 */
#include "bootvid.h"
#include "rtl.h"

static struct {
    uint8_t *Base;
    int Width;
    int Height;
    int Pitch;
    uint8_t RedShift, GreenShift, BlueShift;
    uint8_t RedLoss, GreenLoss, BlueLoss; /* 8 - mask size */
    int ClipX0, ClipY0, ClipX1, ClipY1;   /* half-open */
    bool Ready;
} Vid;

bool VidInitialize(const FRAMEBUFFER_INFO *Fb)
{
    if (!Fb || !Fb->Address || Fb->Bpp != 32 || Fb->Width == 0 || Fb->Height == 0)
        return false;
    Vid.Base = (uint8_t *)Fb->Address;
    Vid.Width = (int)Fb->Width;
    Vid.Height = (int)Fb->Height;
    Vid.Pitch = (int)Fb->Pitch;
    Vid.RedShift = Fb->RedShift;
    Vid.GreenShift = Fb->GreenShift;
    Vid.BlueShift = Fb->BlueShift;
    Vid.RedLoss = (uint8_t)(Fb->RedSize < 8 ? 8 - Fb->RedSize : 0);
    Vid.GreenLoss = (uint8_t)(Fb->GreenSize < 8 ? 8 - Fb->GreenSize : 0);
    Vid.BlueLoss = (uint8_t)(Fb->BlueSize < 8 ? 8 - Fb->BlueSize : 0);
    VidResetClip();
    Vid.Ready = true;
    return true;
}

bool VidIsReady(void)
{
    return Vid.Ready;
}

int VidWidth(void)
{
    return Vid.Width;
}

int VidHeight(void)
{
    return Vid.Height;
}

void VidSetClip(int X, int Y, int Width, int Height)
{
    Vid.ClipX0 = HZ_MAX(X, 0);
    Vid.ClipY0 = HZ_MAX(Y, 0);
    Vid.ClipX1 = HZ_MIN(X + Width, Vid.Width);
    Vid.ClipY1 = HZ_MIN(Y + Height, Vid.Height);
}

void VidResetClip(void)
{
    Vid.ClipX0 = 0;
    Vid.ClipY0 = 0;
    Vid.ClipX1 = Vid.Width;
    Vid.ClipY1 = Vid.Height;
}

static inline uint32_t VidEncode(uint32_t Color)
{
    uint32_t r = (Color >> 16) & 0xFF, g = (Color >> 8) & 0xFF, b = Color & 0xFF;
    return ((r >> Vid.RedLoss) << Vid.RedShift) | ((g >> Vid.GreenLoss) << Vid.GreenShift) |
           ((b >> Vid.BlueLoss) << Vid.BlueShift);
}

static inline uint32_t *VidRow(int Y)
{
    return (uint32_t *)(Vid.Base + (size_t)Y * (size_t)Vid.Pitch);
}

static inline bool VidInClip(int X, int Y)
{
    return X >= Vid.ClipX0 && X < Vid.ClipX1 && Y >= Vid.ClipY0 && Y < Vid.ClipY1;
}

void VidPutPixel(int X, int Y, uint32_t Color)
{
    if (!Vid.Ready || !VidInClip(X, Y))
        return;
    VidRow(Y)[X] = VidEncode(Color);
}

void VidFillRect(int X, int Y, int Width, int Height, uint32_t Color)
{
    if (!Vid.Ready)
        return;
    int x0 = HZ_MAX(X, Vid.ClipX0), y0 = HZ_MAX(Y, Vid.ClipY0);
    int x1 = HZ_MIN(X + Width, Vid.ClipX1), y1 = HZ_MIN(Y + Height, Vid.ClipY1);
    if (x0 >= x1 || y0 >= y1)
        return;
    uint32_t Pixel = VidEncode(Color);
    for (int y = y0; y < y1; y++) {
        uint32_t *Row = VidRow(y);
        for (int x = x0; x < x1; x++)
            Row[x] = Pixel;
    }
}

uint32_t VidBlend(uint32_t Background, uint32_t Foreground, uint32_t Alpha)
{
    if (Alpha >= 255)
        return Foreground;
    if (Alpha == 0)
        return Background;
    uint32_t Out = 0;
    for (int Shift = 0; Shift <= 16; Shift += 8) {
        int32_t b = (int32_t)((Background >> Shift) & 0xFF);
        int32_t f = (int32_t)((Foreground >> Shift) & 0xFF);
        int32_t c = b + ((f - b) * (int32_t)Alpha + (f >= b ? 127 : -127)) / 255;
        Out |= (uint32_t)c << Shift;
    }
    return Out;
}

void VidFillVerticalGradient(int X, int Y, int Width, int Height, uint32_t Top, uint32_t Bottom)
{
    if (Height <= 0)
        return;
    for (int i = 0; i < Height; i++) {
        uint32_t Alpha = Height > 1 ? (uint32_t)((i * 255) / (Height - 1)) : 0;
        VidFillRect(X, Y + i, Width, 1, VidBlend(Top, Bottom, Alpha));
    }
}

/* ---- Text -------------------------------------------------------------------------------- */

static int32_t VidKerning(const BOOT_FONT *Font, char Left, char Right)
{
    uint32_t Lo = 0, Hi = Font->KerningCount;
    uint16_t Key = (uint16_t)(((uint8_t)Left << 8) | (uint8_t)Right);
    while (Lo < Hi) {
        uint32_t Mid = (Lo + Hi) / 2;
        const BOOT_KERN_PAIR *P = &Font->Kerning[Mid];
        uint16_t MidKey = (uint16_t)((P->Left << 8) | P->Right);
        if (MidKey == Key)
            return P->Adjust;
        if (MidKey < Key)
            Lo = Mid + 1;
        else
            Hi = Mid;
    }
    return 0;
}

static const BOOT_GLYPH *VidGlyph(const BOOT_FONT *Font, char *Ch)
{
    if ((uint8_t)*Ch < BOOTFONT_FIRST_CHAR || (uint8_t)*Ch > BOOTFONT_LAST_CHAR)
        *Ch = '?';
    return &Font->Glyphs[(uint8_t)*Ch - BOOTFONT_FIRST_CHAR];
}

int VidMeasureText(const BOOT_FONT *Font, const char *Text)
{
    int32_t Pen = 0;
    char Previous = 0;
    for (const char *p = Text; *p; p++) {
        char Ch = *p;
        const BOOT_GLYPH *G = VidGlyph(Font, &Ch);
        if (Previous)
            Pen += VidKerning(Font, Previous, Ch);
        Pen += G->Advance;
        Previous = Ch;
    }
    return (Pen + 32) >> 6;
}

int VidDrawText(const BOOT_FONT *Font, int X, int BaselineY, const char *Text, uint32_t Foreground,
                uint32_t Background)
{
    int32_t Pen = X << 6;
    char Previous = 0;
    for (const char *p = Text; *p; p++) {
        char Ch = *p;
        const BOOT_GLYPH *G = VidGlyph(Font, &Ch);
        if (Previous)
            Pen += VidKerning(Font, Previous, Ch);
        Previous = Ch;

        if (Vid.Ready && G->Width && G->Height) {
            int gx = ((Pen + 32) >> 6) + G->OffsetX;
            int gy = BaselineY + G->OffsetY;
            const uint8_t *Alpha = Font->Bitmap + G->BitmapOffset;
            for (int y = 0; y < G->Height; y++) {
                int py = gy + y;
                if (py < Vid.ClipY0 || py >= Vid.ClipY1)
                    continue;
                uint32_t *Row = VidRow(py);
                for (int x = 0; x < G->Width; x++) {
                    int px = gx + x;
                    uint8_t a = Alpha[y * G->Width + x];
                    if (a == 0 || px < Vid.ClipX0 || px >= Vid.ClipX1)
                        continue;
                    Row[px] = VidEncode(VidBlend(Background, Foreground, a));
                }
            }
        }
        Pen += G->Advance;
    }
    return (Pen + 32) >> 6;
}

/* ---- Logo ------------------------------------------------------------------------------ */

/* The boot mark follows assets/branding/nexora-os-logo.svg. */
static bool VidLogoContains(int32_t X, int32_t Y)
{
    static const int32_t Points[][2] = {
        {216, 96}, {240, 96}, {336, 288}, {336, 96}, {400, 96}, {411, 101}, {416, 112}, {416, 296},
        {408, 342}, {381, 381}, {342, 408}, {296, 416}, {272, 416}, {176, 224}, {176, 416}, {112, 416},
        {101, 411}, {96, 400}, {96, 216}, {101, 170}, {131, 131}, {170, 101},
    };
    bool Inside = false;
    for (size_t I = 0, J = HZ_ARRAYSIZE(Points) - 1; I < HZ_ARRAYSIZE(Points); J = I++) {
        int32_t X1 = Points[I][0], Y1 = Points[I][1];
        int32_t X2 = Points[J][0], Y2 = Points[J][1];
        if ((Y1 > Y) != (Y2 > Y)) {
            int64_t Left = (int64_t)(X - X1) * (Y2 - Y1);
            int64_t Right = (int64_t)(X2 - X1) * (Y - Y1);
            if ((Y2 > Y1 && Left < Right) || (Y2 < Y1 && Left > Right))
                Inside = !Inside;
        }
    }
    return Inside;
}

static uint32_t VidLogoSample(int32_t U, int32_t V, int32_t N, uint32_t Background, uint32_t Foreground)
{
    return VidLogoContains(U * 512 / N, V * 512 / N) ? Foreground : Background;
}

void VidDrawLogoTinted(int X, int Y, int Size, uint32_t Background, uint32_t Foreground)
{
    if (!Vid.Ready || Size <= 0)
        return;
    const int32_t N = Size * 8;
    for (int py = 0; py < Size; py++) {
        int y = Y + py;
        if (y < Vid.ClipY0 || y >= Vid.ClipY1)
            continue;
        uint32_t *Row = VidRow(y);
        for (int px = 0; px < Size; px++) {
            int x = X + px;
            if (x < Vid.ClipX0 || x >= Vid.ClipX1)
                continue;
            uint32_t r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    int32_t U = px * 8 + sx * 2 + 1;
                    int32_t V = py * 8 + sy * 2 + 1;
                    uint32_t c = VidLogoSample(U, V, N, Background, Foreground);
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                }
            }

            Row[x] = VidEncode(((r / 16) << 16) | ((g / 16) << 8) | (b / 16));
        }
    }

}

void VidDrawLogo(int X, int Y, int Size, uint32_t Background)
{
    VidDrawLogoTinted(X, Y, Size, Background, 0xC5CDD4);
}
