/*
 * Nexora OS - win32k/compose.cpp
 * The compositor.
 *
 * Every visible window has a surface; the compositor draws the desktop into a system-memory
 * back buffer and copies changed rectangles to the framebuffer (write-combined, never read back).
 *
 *   - Changes are queued as a dirty region (CmpInvalidate*). A compositor thread wakes up,
 *     takes the region, snapshots the visible windows (bottom to top, each surface referenced)
 *     and redraws each dirty rectangle with the painter's algorithm: wallpaper, then for every
 *     window its soft shadow, its body and its rounded corners, then the cursor. Drawing starts at
 *     the topmost window whose opaque area covers the whole rectangle, so the common case of a
 *     change inside one window touches only that window.
 *   - Frames are paced to at most one per CMP_FRAME_US.
 *   - Everything is integer arithmetic (the kernel never uses floating point): the wallpaper is
 *     procedural, the Mica-like backdrop is a box-blurred, tinted copy of it, corner coverage and
 *     the shadow profile are precomputed tables.
 *   - Surfaces are 0xAARRGGBB. Ordinary windows are opaque (alpha ignored); HZ_WS_MICA windows
 *     blend their pixels over the backdrop, so a transparent caption shows the blurred wallpaper.
 *   - Every window has rounded corners and a 1 px border, except maximized windows (square,
 *     borderless). The cursor is a sprite chosen by shape (input routing decides which).
 *   - Until CmpShowDesktop the boot screen stays visible: frames are composed (tests can read
 *     pixels back) but not copied to the framebuffer.
 */
#include "w32p.h"
#include "inbv.h"

#define CMP_TAG HZ_POOL_TAG('W', 'C', 'm', 'p')
#define CMP_FRAME_US 16000u
#define CMP_MAX_LAYERS 256
#define CMP_MICA_CELL 16
#define CMP_MICA_TINT 0x1C1C24u
#define CMP_MICA_TINT_ALPHA 196u
#define CMP_BORDER_COLOR 0xFFFFFFu
#define CMP_BORDER_ALPHA 28u
/* Every cursor sprite lies within this box around its hotspot (dirty-region bounds). */
#define CMP_CURSOR_BOX_BEFORE 11
#define CMP_CURSOR_BOX_AFTER 21

namespace {

struct CMP_LAYER {
    HZ_RECT Rect;
    uint32_t Style;
    bool Active;
    bool Square;      /* maximized, frameless or transparent: no corners, no border */
    bool Transparent; /* per-pixel alpha over what is below */
    bool NoShadow;
    const uint32_t *Pixels;
    uint32_t Width, Height;
    MM_SECTION *Surface;
};

struct {
    bool Available;
    bool DesktopShown;
    uint64_t DesktopFadeStartUs;
    int32_t Width, Height;
    uint8_t *Framebuffer;
    uint32_t Pitch;
    uint32_t *Back;
    uint32_t *Wallpaper;
    uint32_t *Mica;
    KSPIN_LOCK DirtyLock;
    KMUTEX ComposeLock;  /* held while composing; wallpaper swaps take it */
    uint32_t WallpaperIndex;
    uint32_t BootWallpaperSeed;
    RGN *Dirty;
    RGN *Working;
    KEVENT Wake;
    volatile bool Composing;
    volatile uint64_t Frames;
    uint8_t Corner[HZ_WINDOW_RADIUS][HZ_WINDOW_RADIUS]; /* coverage of the top-left corner, 0..255 */
    uint8_t Shadow[CMP_SHADOW_EXTENT + 1];             /* alpha falloff with distance */
    CMP_LAYER Layers[CMP_MAX_LAYERS];
    HZ_POINT Cursor;                                   /* snapshot with the layers */
    uint32_t CursorShape;
} Cmp;

/* Cursor art: 'X' black outline, '.' white fill. Some shapes reuse another's art transposed
   (vertical from horizontal) or mirrored (one diagonal from the other). */
const char *const CmpArrowArt[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
    "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      X..X  ",
    "       XX   ",
};
const char *const CmpIBeamArt[] = {
    "XXX XXX", "X..X..X", "XXX.XXX", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ",
    "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "XXX.XXX", "X..X..X", "XXX XXX",
};
const char *const CmpHandArt[] = {
    "     XX         ", "    X..X        ", "    X..X        ", "    X..X        ", "    X..XXX      ",
    "    X..X..XXX   ", "    X..X..X..XX ", " XX X..X..X..X.X", "X..XX........X.X", "X...X..........X",
    " X.............X", "  X............X", "  X...........X ", "   X..........X ", "   X.........X  ",
    "    X........X  ", "    X........X  ", "    XXXXXXXXXX  ",
};
const char *const CmpSizeWeArt[] = {
    "    X           X    ", "   XX           XX   ", "  X.X           X.X  ", " X..XXXXXXXXXXXXX..X ",
    "X...................X", " X..XXXXXXXXXXXXX..X ", "  X.X           X.X  ", "   XX           XX   ",
    "    X           X    ",
};
const char *const CmpSizeDiagonalArt[] = {
    "XXXXXX         ", "X....X         ", "X...X          ", "X....X         ", "X.XX..X        ",
    "XX  X.X        ", "     X.X       ", "      X.X      ", "       X.X     ", "        X.X  XX",
    "        X..XX.X", "         X....X", "          X...X", "         X....X", "         XXXXXX",
};

struct CMP_CURSOR_SPRITE {
    const char *const *Art;
    int32_t Width, Height; /* of the art */
    int32_t HotX, HotY;    /* in screen orientation */
    bool Transpose;
    bool Mirror;
};

const CMP_CURSOR_SPRITE CmpCursors[HZ_CURSOR_COUNT] = {
    {CmpArrowArt, 12, 19, 0, 0, false, false},       /* HZ_CURSOR_ARROW */
    {CmpIBeamArt, 7, 17, 3, 8, false, false},        /* HZ_CURSOR_IBEAM */
    {CmpHandArt, 16, 18, 5, 0, false, false},        /* HZ_CURSOR_HAND */
    {CmpSizeWeArt, 21, 9, 10, 4, false, false},      /* HZ_CURSOR_SIZE_WE */
    {CmpSizeWeArt, 21, 9, 4, 10, true, false},       /* HZ_CURSOR_SIZE_NS */
    {CmpSizeDiagonalArt, 15, 15, 7, 7, false, false}, /* HZ_CURSOR_SIZE_NWSE */
    {CmpSizeDiagonalArt, 15, 15, 7, 7, false, true},  /* HZ_CURSOR_SIZE_NESW */
};

inline uint32_t Blend(uint32_t Below, uint32_t Above, uint32_t Alpha)
{
    if (Alpha >= 255)
        return Above & 0xFFFFFF;
    if (Alpha == 0)
        return Below & 0xFFFFFF;
    uint32_t Inverse = 255 - Alpha;
    uint32_t R = (((Above >> 16) & 0xFF) * Alpha + ((Below >> 16) & 0xFF) * Inverse) * 257 >> 16;
    uint32_t G = (((Above >> 8) & 0xFF) * Alpha + ((Below >> 8) & 0xFF) * Inverse) * 257 >> 16;
    uint32_t B = ((Above & 0xFF) * Alpha + (Below & 0xFF) * Inverse) * 257 >> 16;
    return (R << 16) | (G << 8) | B;
}

bool CmpLoadWallpaperBitmap(const char *Path, uint32_t *Out, uint32_t *Random);

bool CmpLoadWallpaper(uint32_t Index, uint32_t *Out)
{
    char Path[128];
    RtlFormatString(Path, sizeof(Path), "\\??\\C:\\Nexora\\Wallpapers\\wallpaper-%03u.bmp", Index);
    uint32_t Random = Cmp.BootWallpaperSeed ^ (Index + 1u);
    if (!Random)
        Random = 0xA341316Cu;
    return CmpLoadWallpaperBitmap(Path, Out, &Random);
}

inline uint32_t Darken(uint32_t Pixel, uint32_t Alpha)
{
    return Blend(Pixel, 0, Alpha);
}

/* ---- Tables and backgrounds -------------------------------------------------------------- */

void BuildCornerTable()
{
    /* 4x4 supersampling of a quarter circle centred at (R, R), in eighths of a pixel. */
    const int32_t R = HZ_WINDOW_RADIUS, C = R * 8, R2 = (R * 8) * (R * 8);
    for (int32_t y = 0; y < R; y++) {
        for (int32_t x = 0; x < R; x++) {
            int Inside = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    int32_t px = x * 8 + sx * 2 + 1 - C, py = y * 8 + sy * 2 + 1 - C;
                    Inside += px * px + py * py <= R2;
                }
            }
            Cmp.Corner[y][x] = (uint8_t)(Inside * 255 / 16);
        }
    }
}

void BuildShadowTable()
{
    /* Smooth falloff: (1 - d/E)^2 scaled to 255. */
    for (int32_t d = 0; d <= CMP_SHADOW_EXTENT; d++) {
        int32_t t = CMP_SHADOW_EXTENT - d;
        Cmp.Shadow[d] = (uint8_t)(t * t * 255 / (CMP_SHADOW_EXTENT * CMP_SHADOW_EXTENT));
    }
}

uint32_t Lerp(uint32_t A, uint32_t B, int32_t T, int32_t Scale)
{
    uint32_t Alpha = (uint32_t)(HZ_MIN(HZ_MAX(T, 0), Scale) * 255 / Scale);
    return Blend(A, B, Alpha);
}

uint32_t AddColor(uint32_t Base, uint32_t Add, uint32_t Amount)
{
    uint32_t R = ((Base >> 16) & 0xFF) + (((Add >> 16) & 0xFF) * Amount >> 8);
    uint32_t G = ((Base >> 8) & 0xFF) + (((Add >> 8) & 0xFF) * Amount >> 8);
    uint32_t B = (Base & 0xFF) + ((Add & 0xFF) * Amount >> 8);
    return (HZ_MIN(R, 255u) << 16) | (HZ_MIN(G, 255u) << 8) | HZ_MIN(B, 255u);
}

/* Fallback palettes used only when the bundled image catalog cannot be loaded at boot. */
struct CMP_PALETTE {
    uint32_t SkyTop, SkyMiddle, SkyBottom, Glow, Band;
    uint32_t WaveTop, WaveMiddle, WaveBottom;
};

uint16_t CmpReadLe16(const uint8_t *Data)
{
    return (uint16_t)(Data[0] | ((uint16_t)Data[1] << 8));
}

uint32_t CmpReadLe32(const uint8_t *Data)
{
    return (uint32_t)Data[0] | ((uint32_t)Data[1] << 8) | ((uint32_t)Data[2] << 16) |
           ((uint32_t)Data[3] << 24);
}

uint32_t CmpNextRandom(uint32_t *State)
{
    uint32_t Value = *State;
    Value ^= Value << 13;
    Value ^= Value >> 17;
    Value ^= Value << 5;
    *State = Value ? Value : 0xA341316Cu;
    return *State;
}

bool CmpLoadWallpaperBitmap(const char *Path, uint32_t *Out, uint32_t *Random)
{
    void *Raw;
    uint64_t Size;
    STATUS Status = IoReadWholeFile(Path, &Raw, &Size, 16 * 1024 * 1024);
    if (!HZ_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Desktop: cannot read wallpaper %s (0x%08X)\n", Path, (uint32_t)Status);
        return false;
    }

    const uint8_t *Data = (const uint8_t *)Raw;
    bool Valid = Size >= 54 && Data[0] == 'B' && Data[1] == 'M';
    uint32_t PixelOffset = Valid ? CmpReadLe32(Data + 10) : 0;
    uint32_t HeaderSize = Valid ? CmpReadLe32(Data + 14) : 0;
    int32_t SourceWidth = Valid ? (int32_t)CmpReadLe32(Data + 18) : 0;
    int32_t SignedHeight = Valid ? (int32_t)CmpReadLe32(Data + 22) : 0;
    uint16_t Planes = Valid ? CmpReadLe16(Data + 26) : 0;
    uint16_t BitsPerPixel = Valid ? CmpReadLe16(Data + 28) : 0;
    uint32_t Compression = Valid ? CmpReadLe32(Data + 30) : 1;
    bool HeightValid = SignedHeight != (-2147483647 - 1);
    int32_t SourceHeight = HeightValid && SignedHeight < 0 ? -SignedHeight : SignedHeight;
    Valid = Valid && HeightValid && HeaderSize >= 40 && HeaderSize <= Size - 14 && PixelOffset >= 14 + HeaderSize &&
            PixelOffset <= Size && Planes == 1 && BitsPerPixel == 24 && Compression == 0 && SourceWidth > 0 &&
            SourceHeight > 0 && SourceWidth <= 4096 && SourceHeight <= 4096;

    uint64_t RowBytes = Valid ? ((uint64_t)SourceWidth * 3 + 3) & ~3ull : 0;
    Valid = Valid && RowBytes * (uint64_t)SourceHeight <= Size - PixelOffset;
    if (!Valid) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Desktop: invalid wallpaper bitmap %s\n", Path);
        ExFreePool(Raw);
        return false;
    }

    int64_t CropWidth = SourceWidth, CropHeight = SourceHeight;
    if (CropWidth * Cmp.Height > CropHeight * Cmp.Width)
        CropWidth = CropHeight * Cmp.Width / Cmp.Height;
    else
        CropHeight = CropWidth * Cmp.Height / Cmp.Width;

    uint32_t Zoom = 100 + CmpNextRandom(Random) % 21;
    CropWidth = HZ_MAX(CropWidth * 100 / Zoom, 1);
    CropHeight = HZ_MAX(CropHeight * 100 / Zoom, 1);
    int64_t MaxX = SourceWidth - CropWidth, MaxY = SourceHeight - CropHeight;
    int64_t StartX = MaxX * CmpNextRandom(Random) / UINT32_MAX;
    int64_t StartY = MaxY * CmpNextRandom(Random) / UINT32_MAX;

    for (int32_t y = 0; y < Cmp.Height; y++) {
        int32_t SourceY = (int32_t)(StartY + (int64_t)y * CropHeight / Cmp.Height);
        int32_t RowY = SignedHeight < 0 ? SourceY : SourceHeight - 1 - SourceY;
        const uint8_t *SourceRow = Data + PixelOffset + RowBytes * (uint32_t)RowY;
        uint32_t *Dest = Out + (size_t)y * Cmp.Width;
        for (int32_t x = 0; x < Cmp.Width; x++) {
            int32_t SourceX = (int32_t)(StartX + (int64_t)x * CropWidth / Cmp.Width);
            const uint8_t *Pixel = SourceRow + (size_t)SourceX * 3;
            Dest[x] = ((uint32_t)Pixel[2] << 16) | ((uint32_t)Pixel[1] << 8) | Pixel[0];
        }
    }
    ExFreePool(Raw);
    return true;
}

bool CmpLoadBootWallpaper(uint32_t *Out)
{
    void *RawCount;
    uint64_t CountSize;
    STATUS Status = IoReadWholeFile("\\??\\C:\\Nexora\\Wallpapers\\count.txt", &RawCount, &CountSize, 16);
    if (!HZ_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Desktop: wallpaper catalog unavailable (0x%08X)\n", (uint32_t)Status);
        return false;
    }

    const char *CountText = (const char *)RawCount;
    uint32_t Count = 0;
    bool Valid = CountSize > 0;
    for (uint64_t i = 0; Valid && i < CountSize && CountText[i] != '\n' && CountText[i] != '\r'; i++) {
        if (CountText[i] < '0' || CountText[i] > '9' || Count > 25) {
            Valid = false;
            break;
        }
        Count = Count * 10 + (uint32_t)(CountText[i] - '0');
    }
    ExFreePool(RawCount);
    if (!Valid || Count == 0 || Count > 256) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Desktop: invalid wallpaper catalog\n");
        return false;
    }

    uint32_t Random = Cmp.BootWallpaperSeed;
    uint32_t Selected = CmpNextRandom(&Random) % Count;
    char Path[128];
    RtlFormatString(Path, sizeof(Path), "\\??\\C:\\Nexora\\Wallpapers\\wallpaper-%03u.bmp", Selected);
    if (!CmpLoadWallpaperBitmap(Path, Out, &Random))
        return false;

    Cmp.WallpaperIndex = HZ_WALLPAPER_AUTO;
    DbgPrint("Desktop: wallpaper %u of %u selected for this boot\n", Selected + 1, Count);
    return true;
}

const CMP_PALETTE CmpPalettes[HZ_MAX_WALLPAPER + 1] = {
    {0x071A3D, 0x164B8C, 0x09255A, 0x74C8FF, 0xD2E9FF, 0x2B65A7, 0x5C91C3, 0xAFCBE4},
    {0x050D22, 0x0F2A52, 0x1C4E74, 0x4CC8FF, 0xA8DCFF, 0x123B67, 0x23608D, 0x72AFCF},
    {0x161028, 0x4E2E58, 0x9A5466, 0xFFC27A, 0xFFDCC0, 0x633F65, 0xA76579, 0xE6A981},
    {0x10241D, 0x285C43, 0x102C35, 0x85D7A6, 0xD9F0DC, 0x316E53, 0x639C73, 0xAAC9AC},
    {0x171F2D, 0x40566E, 0x17263D, 0xF0B9A2, 0xF6E0D6, 0x38516A, 0x73889B, 0xBBC6CD},
};

void BuildWallpaper(uint32_t *Out, uint32_t Index)
{
    const CMP_PALETTE &P = CmpPalettes[Index <= HZ_MAX_WALLPAPER ? Index : 0];
    const int32_t W = Cmp.Width, H = Cmp.Height;
    const int64_t SunX = W / 2, SunY = H + H / 5;
    const int64_t Radius = (int64_t)H * 7 / 10;
    const int64_t WaveOffset[3] = {-(int64_t)H / 10, (int64_t)H / 14, (int64_t)H / 4};
    const uint32_t WaveColor[3] = {P.WaveTop, P.WaveMiddle, P.WaveBottom};
    const int32_t Edge = HZ_MAX(H / 18, 1);
    for (int32_t y = 0; y < H; y++) {
        uint32_t Sky = y < H * 6 / 10 ? Lerp(P.SkyTop, P.SkyMiddle, y, H * 6 / 10)
                                      : Lerp(P.SkyMiddle, P.SkyBottom, y - H * 6 / 10, H * 4 / 10);
        uint32_t *Row = Out + (size_t)y * W;
        for (int32_t x = 0; x < W; x++) {
            uint32_t Pixel = Sky;
            /* Elliptical glow: (dx^2 + (1.4 dy)^2) / r^2 in 16.16 fixed point. */
            int64_t dx = x - SunX, dy = (y - SunY) * 14 / 10;
            int64_t D = ((dx * dx + dy * dy) << 16) / (Radius * Radius);
            if (D < 65536) {
                int64_t t = 65536 - D;           /* 1 - d^2 */
                uint32_t Glow = (uint32_t)((t * t >> 16) * 200 >> 16);
                Pixel = AddColor(Pixel, P.Glow, Glow);
            }
            int64_t Dx = x - W / 2;
            int64_t Base = (int64_t)H * 72 / 100 + Dx * H / (2 * W) - Dx * Dx * H / (2LL * W * W);
            for (int Layer = 0; Layer < 3; Layer++) {
                int64_t Curve = Base + WaveOffset[Layer];
                int64_t Distance = (int64_t)y - Curve;
                if (Distance > 0) {
                    uint32_t Alpha = (uint32_t)HZ_MIN(Distance * 180 / Edge, 180);
                    Pixel = Blend(Pixel, WaveColor[Layer], Alpha);
                }
                int64_t BandDistance = Distance < 0 ? -Distance : Distance;
                int64_t BandWidth = H / 120 + 1;
                if (BandDistance < BandWidth)
                    Pixel = Blend(Pixel, P.Band, (uint32_t)((BandWidth - BandDistance) * 34 / BandWidth));
            }
            Row[x] = Pixel;
        }
    }
}

void BuildMica(const uint32_t *Wall, uint32_t *Out)
{
    /* Average 16x16 cells, blur the small image twice with a 3x3 box, then upsample bilinearly
       and tint. */
    const int32_t W = Cmp.Width, H = Cmp.Height, C = CMP_MICA_CELL;
    const int32_t SW = (W + C - 1) / C + 1, SH = (H + C - 1) / C + 1;
    uint32_t *Small = (uint32_t *)ExAllocatePoolWithTag(NonPagedPool, (size_t)SW * SH * 4, CMP_TAG);
    uint32_t *Temp = (uint32_t *)ExAllocatePoolWithTag(NonPagedPool, (size_t)SW * SH * 4, CMP_TAG);
    if (!Small || !Temp) {
        for (int32_t i = 0; i < W * H; i++)
            Out[i] = Blend(Wall[i], CMP_MICA_TINT, CMP_MICA_TINT_ALPHA);
        if (Small)
            ExFreePoolWithTag(Small, CMP_TAG);
        if (Temp)
            ExFreePoolWithTag(Temp, CMP_TAG);
        return;
    }
    for (int32_t sy = 0; sy < SH; sy++) {
        for (int32_t sx = 0; sx < SW; sx++) {
            uint32_t R = 0, G = 0, B = 0, N = 0;
            for (int32_t y = sy * C; y < HZ_MIN(sy * C + C, H); y += 4) {
                for (int32_t x = sx * C; x < HZ_MIN(sx * C + C, W); x += 4) {
                    uint32_t P = Wall[(size_t)y * W + x];
                    R += (P >> 16) & 0xFF;
                    G += (P >> 8) & 0xFF;
                    B += P & 0xFF;
                    N++;
                }
            }
            if (!N) {
                uint32_t P = Wall[(size_t)HZ_MIN(sy * C, H - 1) * W + HZ_MIN(sx * C, W - 1)];
                R = (P >> 16) & 0xFF, G = (P >> 8) & 0xFF, B = P & 0xFF, N = 1;
            }
            Small[sy * SW + sx] = ((R / N) << 16) | ((G / N) << 8) | (B / N);
        }
    }
    for (int Pass = 0; Pass < 2; Pass++) {
        for (int32_t sy = 0; sy < SH; sy++) {
            for (int32_t sx = 0; sx < SW; sx++) {
                uint32_t R = 0, G = 0, B = 0, N = 0;
                for (int32_t oy = -1; oy <= 1; oy++) {
                    for (int32_t ox = -1; ox <= 1; ox++) {
                        int32_t yy = sy + oy, xx = sx + ox;
                        if (yy < 0 || yy >= SH || xx < 0 || xx >= SW)
                            continue;
                        uint32_t P = Small[yy * SW + xx];
                        R += (P >> 16) & 0xFF, G += (P >> 8) & 0xFF, B += P & 0xFF, N++;
                    }
                }
                Temp[sy * SW + sx] = ((R / N) << 16) | ((G / N) << 8) | (B / N);
            }
        }
        memcpy(Small, Temp, (size_t)SW * SH * 4);
    }
    for (int32_t y = 0; y < H; y++) {
        int32_t sy = y / C, fy = y % C;
        for (int32_t x = 0; x < W; x++) {
            int32_t sx = x / C, fx = x % C;
            uint32_t P00 = Small[sy * SW + sx], P10 = Small[sy * SW + sx + 1];
            uint32_t P01 = Small[(sy + 1) * SW + sx], P11 = Small[(sy + 1) * SW + sx + 1];
            uint32_t Top = Blend(P00, P10, (uint32_t)(fx * 255 / C));
            uint32_t Bottom = Blend(P01, P11, (uint32_t)(fx * 255 / C));
            Out[(size_t)y * W + x] = Blend(Blend(Top, Bottom, (uint32_t)(fy * 255 / C)), CMP_MICA_TINT,
                                                CMP_MICA_TINT_ALPHA);
        }
    }
    ExFreePoolWithTag(Small, CMP_TAG);
    ExFreePoolWithTag(Temp, CMP_TAG);
}

/* ---- Drawing ---------------------------------------------------------------------------------- */

inline uint32_t *BackRow(int32_t Y)
{
    return Cmp.Back + (size_t)Y * Cmp.Width;
}

void DrawWallpaper(const HZ_RECT &R)
{
    for (int32_t y = R.Top; y < R.Bottom; y++)
        memcpy(BackRow(y) + R.Left, Cmp.Wallpaper + (size_t)y * Cmp.Width + R.Left, (size_t)(R.Right - R.Left) * 4);
}

inline uint32_t ShapeCoverage(const CMP_LAYER &L, int32_t x, int32_t y);

int32_t ShadowExtent(const CMP_LAYER &L)
{
    return (L.Style & HZ_WS_POPUP) ? CMP_POPUP_SHADOW_EXTENT : CMP_SHADOW_EXTENT;
}

void DrawShadow(const CMP_LAYER &L, const HZ_RECT &R)
{
    int32_t E = ShadowExtent(L);
    int32_t Offset = (L.Style & HZ_WS_POPUP) ? 2 : CMP_SHADOW_OFFSET_Y;
    uint32_t Strength = L.Active ? 150 : 90;
    HZ_RECT Caster = {L.Rect.Left, L.Rect.Top + Offset, L.Rect.Right, L.Rect.Bottom + Offset};
    HZ_RECT Area = WmIntersect(R, HZ_RECT{Caster.Left - E, Caster.Top - E, Caster.Right + E, Caster.Bottom + E});
    for (int32_t y = Area.Top; y < Area.Bottom; y++) {
        int32_t dy = y < Caster.Top ? Caster.Top - y : (y >= Caster.Bottom ? y - Caster.Bottom + 1 : 0);
        if (dy > E)
            continue;
        uint32_t *Row = BackRow(y);
        bool RowInWindow = y >= L.Rect.Top && y < L.Rect.Bottom;
        for (int32_t x = Area.Left; x < Area.Right; x++) {
            /* Under the opaque body there is nothing to darken; the rounded corners' cut-outs
               must keep the shadow, or the unshadowed wallpaper shows as a light square there. */
            if (RowInWindow && x >= L.Rect.Left && x < L.Rect.Right &&
                ShapeCoverage(L, x - L.Rect.Left, y - L.Rect.Top) == 255)
                continue;
            int32_t dx = x < Caster.Left ? Caster.Left - x : (x >= Caster.Right ? x - Caster.Right + 1 : 0);
            if (dx > E)
                continue;
            /* Scale the table to this extent, then combine both axes. */
            uint32_t Ax = Cmp.Shadow[dx * CMP_SHADOW_EXTENT / E], Ay = Cmp.Shadow[dy * CMP_SHADOW_EXTENT / E];
            uint32_t Alpha = Ax * Ay / 255 * Strength / 255;
            if (Alpha)
                Row[x] = Darken(Row[x], Alpha);
        }
    }
}

/* Coverage of the window shape at window-relative (x, y): rounded corners. */
inline uint32_t ShapeCoverage(const CMP_LAYER &L, int32_t x, int32_t y)
{
    const int32_t Rad = HZ_WINDOW_RADIUS;
    int32_t W = L.Rect.Right - L.Rect.Left, H = L.Rect.Bottom - L.Rect.Top;
    if (L.Square)
        return 255;
    int32_t cx = x < Rad ? x : (x >= W - Rad ? W - 1 - x : -1);
    int32_t cy = y < Rad ? y : (y >= H - Rad ? H - 1 - y : -1);
    if (cx < 0 || cy < 0)
        return 255;
    return Cmp.Corner[cy][cx];
}

inline uint32_t SurfacePixel(const CMP_LAYER &L, int32_t x, int32_t y)
{
    if ((uint32_t)x >= L.Width || (uint32_t)y >= L.Height)
        return 0xFF202020;
    return L.Pixels[(size_t)y * L.Width + x];
}

inline uint32_t BodyColor(const CMP_LAYER &L, int32_t x, int32_t y, int32_t ScreenX, int32_t ScreenY)
{
    uint32_t Src = SurfacePixel(L, x, y);
    if (!(L.Style & HZ_WS_MICA))
        return Src & 0xFFFFFF;
    return Blend(Cmp.Mica[(size_t)ScreenY * Cmp.Width + ScreenX], Src, Src >> 24);
}

void DrawBody(const CMP_LAYER &L, const HZ_RECT &R)
{
    HZ_RECT Area = WmIntersect(R, L.Rect);
    if (WmRectEmpty(Area))
        return;
    const int32_t Rad = HZ_WINDOW_RADIUS;
    int32_t W = L.Rect.Right - L.Rect.Left, H = L.Rect.Bottom - L.Rect.Top;
    bool Mica = (L.Style & HZ_WS_MICA) != 0;
    for (int32_t y = Area.Top; y < Area.Bottom; y++) {
        int32_t wy = y - L.Rect.Top;
        uint32_t *Row = BackRow(y);
        bool CornerRow = wy < Rad || wy >= H - Rad;
        bool EdgeRow = wy == 0 || wy == H - 1;
        if (!Mica && !L.Transparent && !CornerRow && !EdgeRow && (uint32_t)wy < L.Height) {
            /* Fast path: an opaque row segment straight from the surface. */
            int32_t From = Area.Left - L.Rect.Left, To = Area.Right - L.Rect.Left;
            int32_t Copy = HZ_MIN(To, (int32_t)L.Width) - From;
            if (Copy > 0) {
                const uint32_t *Src = L.Pixels + (size_t)wy * L.Width + From;
                for (int32_t i = 0; i < Copy; i++)
                    Row[Area.Left + i] = Src[i] & 0xFFFFFF;
            }
            for (int32_t x = Area.Left + HZ_MAX(Copy, 0); x < Area.Right; x++)
                Row[x] = 0x202020;
            if (Area.Left == L.Rect.Left && !L.Square)
                Row[Area.Left] = Blend(Row[Area.Left], CMP_BORDER_COLOR, CMP_BORDER_ALPHA);
            if (Area.Right == L.Rect.Right && !L.Square)
                Row[Area.Right - 1] = Blend(Row[Area.Right - 1], CMP_BORDER_COLOR, CMP_BORDER_ALPHA);
            continue;
        }
        for (int32_t x = Area.Left; x < Area.Right; x++) {
            int32_t wx = x - L.Rect.Left;
            uint32_t Coverage = ShapeCoverage(L, wx, wy);
            if (!Coverage)
                continue;
            uint32_t Color;
            if (L.Transparent) {
                uint32_t Src = SurfacePixel(L, wx, wy);
                Color = Blend(Row[x], Src, Src >> 24);
            } else {
                Color = BodyColor(L, wx, wy, x, y);
            }
            bool Edge = !L.Square && (wx == 0 || wx == W - 1 || EdgeRow || Coverage < 255);
            if (Edge)
                Color = Blend(Color, CMP_BORDER_COLOR, CMP_BORDER_ALPHA);
            Row[x] = Blend(Row[x], Color, Coverage);
        }
    }
}

void DrawCursor(const HZ_RECT &R, HZ_POINT Cursor, uint32_t Shape)
{
    if (Shape == HZ_CURSOR_HIDDEN)
        return; /* the mouse is locked */
    const CMP_CURSOR_SPRITE &S = CmpCursors[Shape < HZ_CURSOR_COUNT ? Shape : HZ_CURSOR_ARROW];
    int32_t Width = S.Transpose ? S.Height : S.Width, Height = S.Transpose ? S.Width : S.Height;
    int32_t Left = Cursor.X - S.HotX, Top = Cursor.Y - S.HotY;
    HZ_RECT Area = WmIntersect(R, HZ_RECT{Left, Top, Left + Width, Top + Height});
    for (int32_t y = Area.Top; y < Area.Bottom; y++) {
        for (int32_t x = Area.Left; x < Area.Right; x++) {
            int32_t sx = x - Left, sy = y - Top;
            if (S.Mirror)
                sx = Width - 1 - sx;
            char c = S.Transpose ? S.Art[sx][sy] : S.Art[sy][sx];
            if (c == 'X')
                BackRow(y)[x] = 0x000000;
            else if (c == '.')
                BackRow(y)[x] = 0xFFFFFF;
        }
    }
}

/* True if the window's opaque area (everything but the rounded corners) covers R. */
bool CoversOpaquely(const CMP_LAYER &L, const HZ_RECT &R)
{
    const int32_t Rad = L.Square ? 0 : HZ_WINDOW_RADIUS;
    HZ_RECT Wide = {L.Rect.Left, L.Rect.Top + Rad, L.Rect.Right, L.Rect.Bottom - Rad};
    HZ_RECT Tall = {L.Rect.Left + Rad, L.Rect.Top, L.Rect.Right - Rad, L.Rect.Bottom};
    auto Inside = [&](const HZ_RECT &O) {
        return R.Left >= O.Left && R.Right <= O.Right && R.Top >= O.Top && R.Bottom <= O.Bottom;
    };
    return !L.Transparent && (Inside(Wide) || Inside(Tall));
}

void ComposeRect(const HZ_RECT &R, int LayerCount)
{
    /* Start at the topmost window that hides everything below it in R. */
    int Start = 0;
    bool Covered = false;
    for (int i = LayerCount - 1; i >= 0; i--) {
        if (CoversOpaquely(Cmp.Layers[i], R)) {
            Start = i;
            Covered = true;
            break;
        }
    }
    if (!Covered)
        DrawWallpaper(R);
    for (int i = Start; i < LayerCount; i++) {
        const CMP_LAYER &L = Cmp.Layers[i];
        int32_t E = ShadowExtent(L);
        HZ_RECT Outer = {L.Rect.Left - E, L.Rect.Top - E, L.Rect.Right + E, L.Rect.Bottom + E + CMP_SHADOW_OFFSET_Y};
        if (WmRectEmpty(WmIntersect(R, Outer)))
            continue;
        if (!(Covered && i == Start)) /* a covering window's shadow lies outside R */
            if (!L.NoShadow)
                DrawShadow(L, R);
        DrawBody(L, R);
    }
    DrawCursor(R, Cmp.Cursor, Cmp.CursorShape);
}

void FlushRect(const HZ_RECT &R, uint32_t FadeAlpha)
{
    for (int32_t y = R.Top; y < R.Bottom; y++) {
        uint32_t *Destination = (uint32_t *)(Cmp.Framebuffer + (size_t)y * Cmp.Pitch + (size_t)R.Left * 4);
        const uint32_t *Source = BackRow(y) + R.Left;
        if (FadeAlpha == 255) {
            memcpy(Destination, Source, (size_t)(R.Right - R.Left) * 4);
        } else {
            for (int32_t x = R.Left; x < R.Right; x++) {
                uint32_t Pixel = *Source++;
                uint32_t Red = ((Pixel >> 16) & 0xFF) * FadeAlpha / 255;
                uint32_t Green = ((Pixel >> 8) & 0xFF) * FadeAlpha / 255;
                uint32_t Blue = (Pixel & 0xFF) * FadeAlpha / 255;
                *Destination++ = (Red << 16) | (Green << 8) | Blue;
            }
        }
    }
}

int SnapshotLayers()
{
    int Count = 0;
    WmAcquire();
    for (WINDOW *W = WmBottom; W && Count < CMP_MAX_LAYERS; W = W->Above) {
        if (!W->Visible)
            continue;
        CMP_LAYER &L = Cmp.Layers[Count++];
        L.Rect = W->Rect;
        L.Style = W->Style;
        L.Active = W == WmActive;
        L.Transparent = (W->Style & HZ_WS_TRANSPARENT) != 0;
        L.NoShadow = (W->Style & (HZ_WS_TRANSPARENT | HZ_WS_NOFRAME)) != 0;
        L.Square = L.NoShadow || (W->State & HZ_WINDOW_STATE_MAXIMIZED) != 0;
        L.Pixels = W->Pixels;
        L.Width = W->SurfaceWidth;
        L.Height = W->SurfaceHeight;
        L.Surface = W->Surface;
        ObReferenceObject(L.Surface);
    }
    Cmp.Cursor = InpGetCursor();
    Cmp.CursorShape = InpGetCursorShape();
    WmRelease();
    return Count;
}

void CompositorThread(void *Context)
{
    uint64_t LastFrame = 0;
    for (;;) {
        KeWaitForSingleObject(&Cmp.Wake, NULL);
        uint64_t Now = KeQueryUptimeMicroseconds();
        if (Now - LastFrame < CMP_FRAME_US)
            KeDelayExecutionThread(CMP_FRAME_US - (Now - LastFrame)); /* coalesce bursts */

        KIRQL OldIrql;
        KeAcquireSpinLock(&Cmp.DirtyLock, &OldIrql);
        Cmp.Composing = true;
        RGN *Swap = Cmp.Working;
        Cmp.Working = Cmp.Dirty;
        Cmp.Dirty = Swap;
        RgnSetRect(Cmp.Dirty, NULL);
        KeReleaseSpinLock(&Cmp.DirtyLock, OldIrql);

        RGN_RECT Screen = {0, 0, Cmp.Width, Cmp.Height};
        RgnCombineRect(Cmp.Working, &Screen, RGN_OP_INTERSECT);
        if (!RgnIsEmpty(Cmp.Working)) {
            int Count = SnapshotLayers();
            KeWaitForSingleObject(&Cmp.ComposeLock, NULL);
            uint64_t FadeElapsed = KeQueryUptimeMicroseconds() - Cmp.DesktopFadeStartUs;
            uint32_t FadeAlpha = 255;
            if (Cmp.DesktopFadeStartUs != 0 && FadeElapsed < 240000) {
                uint64_t T = FadeElapsed * 65535 / 240000;
                uint64_t Eased = T * T * (3 * 65535 - 2 * T) / (65535ull * 65535ull);
                FadeAlpha = (uint32_t)(Eased * 255 / 65535);
            }
            const RGN_RECT *Rects;
            size_t N = RgnGetRects(Cmp.Working, &Rects);
            for (size_t i = 0; i < N; i++) {
                HZ_RECT R = {Rects[i].Left, Rects[i].Top, Rects[i].Right, Rects[i].Bottom};
                ComposeRect(R, Count);
                if (Cmp.DesktopShown)
                    FlushRect(R, FadeAlpha);
            }
            KeReleaseMutex(&Cmp.ComposeLock);
            for (int i = 0; i < Count; i++)
                ObDereferenceObject(Cmp.Layers[i].Surface);
            Cmp.Frames = Cmp.Frames + 1;
        }
        Cmp.Composing = false;
        LastFrame = KeQueryUptimeMicroseconds();
        if (Cmp.DesktopShown && Cmp.DesktopFadeStartUs != 0 && LastFrame - Cmp.DesktopFadeStartUs < 240000) {
            HZ_RECT FullScreen = {0, 0, Cmp.Width, Cmp.Height};
            CmpInvalidateRect(&FullScreen);
        }
    }
}

}  // namespace

/* ---- Interface ------------------------------------------------------------------------------ */

bool CmpInitialize()
{
    KeInitializeSpinLock(&Cmp.DirtyLock);
    KeInitializeMutex(&Cmp.ComposeLock);
    KeInitializeEvent(&Cmp.Wake, SynchronizationEvent, false);
    Cmp.Dirty = RgnCreate();
    Cmp.Working = RgnCreate();
    const FRAMEBUFFER_INFO *Fb = &KeLoaderBlock.Framebuffer;
    if (!KeLoaderBlock.HasFramebuffer || Fb->Bpp != 32 || !Fb->Width || !Fb->Height) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Desktop: no 32-bpp framebuffer; composition disabled\n");
        return false;
    }
    Cmp.Width = (int32_t)Fb->Width;
    Cmp.Height = (int32_t)Fb->Height;
    uint64_t Clock = KeQuerySystemTime() ^ KeQueryUptimeMicroseconds();
    Cmp.BootWallpaperSeed = (uint32_t)(Clock ^ (Clock >> 32) ^ (uint64_t)Cmp.Width << 16 ^ Cmp.Height);
    if (!Cmp.BootWallpaperSeed)
        Cmp.BootWallpaperSeed = 0x9E3779B9u;
    Cmp.Framebuffer = (uint8_t *)Fb->Address;
    Cmp.Pitch = Fb->Pitch;
    size_t Bytes = (size_t)Cmp.Width * Cmp.Height * 4;
    Cmp.Back = (uint32_t *)ExAllocatePoolWithTag(NonPagedPool, Bytes, CMP_TAG);
    Cmp.Wallpaper = (uint32_t *)ExAllocatePoolWithTag(NonPagedPool, Bytes, CMP_TAG);
    Cmp.Mica = (uint32_t *)ExAllocatePoolWithTag(NonPagedPool, Bytes, CMP_TAG);
    if (!Cmp.Back || !Cmp.Wallpaper || !Cmp.Mica) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "Desktop: not enough memory for %dx%d buffers; composition disabled\n",
                   Cmp.Width, Cmp.Height);
        return false;
    }
    uint64_t Start = KeQueryUptimeMicroseconds();
    BuildCornerTable();
    BuildShadowTable();
    if (!CmpLoadBootWallpaper(Cmp.Wallpaper))
        BuildWallpaper(Cmp.Wallpaper, 0);
    BuildMica(Cmp.Wallpaper, Cmp.Mica);
    memcpy(Cmp.Back, Cmp.Wallpaper, Bytes);
    Cmp.Available = true;
    STATUS Status = PsCreateSystemThread(CompositorThread, NULL, KI_THREAD_PRIORITY_DEFAULT + 3, "Compositor", NULL);
    if (!HZ_SUCCESS(Status)) {
        Cmp.Available = false;
        return false;
    }
    char Size[24];
    RtlFormatByteSize(Size, sizeof(Size), Bytes * 3);
    DbgPrint("Desktop: compositor at %dx%d, %s of buffers, wallpaper and backdrop built in %llu ms\n", Cmp.Width,
             Cmp.Height, Size, (unsigned long long)((KeQueryUptimeMicroseconds() - Start) / 1000));
    return true;
}

STATUS CmpSetWallpaper(uint32_t Index)
{
    if (!Cmp.Available)
        return STATUS_DEVICE_NOT_READY;
    if (Index > HZ_WALLPAPER_AUTO)
        return STATUS_INVALID_PARAMETER;
    size_t Bytes = (size_t)Cmp.Width * Cmp.Height * 4;
    uint32_t *Wallpaper = (uint32_t *)ExAllocatePoolWithTag(NonPagedPool, Bytes, CMP_TAG);
    uint32_t *Mica = (uint32_t *)ExAllocatePoolWithTag(NonPagedPool, Bytes, CMP_TAG);
    if (!Wallpaper || !Mica) {
        if (Wallpaper)
            ExFreePoolWithTag(Wallpaper, CMP_TAG);
        if (Mica)
            ExFreePoolWithTag(Mica, CMP_TAG);
        return STATUS_NO_MEMORY;
    }
    if (Index == HZ_WALLPAPER_AUTO) {
        if (!CmpLoadBootWallpaper(Wallpaper)) {
            BuildWallpaper(Wallpaper, 0);
            Index = 0;
        }
    } else if (!CmpLoadWallpaper(Index, Wallpaper)) {
        ExFreePoolWithTag(Wallpaper, CMP_TAG);
        ExFreePoolWithTag(Mica, CMP_TAG);
        return STATUS_NOT_FOUND;
    }
    BuildMica(Wallpaper, Mica);
    KeWaitForSingleObject(&Cmp.ComposeLock, NULL); /* not in the middle of a frame */
    uint32_t *OldWallpaper = Cmp.Wallpaper, *OldMica = Cmp.Mica;
    Cmp.Wallpaper = Wallpaper;
    Cmp.Mica = Mica;
    Cmp.WallpaperIndex = Index;
    KeReleaseMutex(&Cmp.ComposeLock);
    ExFreePoolWithTag(OldWallpaper, CMP_TAG);
    ExFreePoolWithTag(OldMica, CMP_TAG);
    HZ_RECT Screen = {0, 0, Cmp.Width, Cmp.Height};
    CmpInvalidateRect(&Screen);
    return STATUS_SUCCESS;
}

uint32_t CmpGetWallpaper()
{
    return Cmp.WallpaperIndex;
}

bool CmpIsAvailable()
{
    return Cmp.Available;
}

int32_t CmpScreenWidth()
{
    return Cmp.Width;
}

int32_t CmpScreenHeight()
{
    return Cmp.Height;
}

void CmpInvalidateRect(const HZ_RECT *Rect)
{
    if (!Cmp.Available || WmRectEmpty(*Rect))
        return;
    RGN_RECT R = WmToRgn(*Rect);
    KIRQL OldIrql;
    KeAcquireSpinLock(&Cmp.DirtyLock, &OldIrql);
    RgnCombineRect(Cmp.Dirty, &R, RGN_OP_UNION);
    KeReleaseSpinLock(&Cmp.DirtyLock, OldIrql);
    KeSetEvent(&Cmp.Wake);
}

void CmpInvalidateCursor(int32_t OldX, int32_t OldY, int32_t NewX, int32_t NewY)
{
    HZ_RECT A = {OldX - CMP_CURSOR_BOX_BEFORE, OldY - CMP_CURSOR_BOX_BEFORE, OldX + CMP_CURSOR_BOX_AFTER,
                 OldY + CMP_CURSOR_BOX_AFTER};
    HZ_RECT B = {NewX - CMP_CURSOR_BOX_BEFORE, NewY - CMP_CURSOR_BOX_BEFORE, NewX + CMP_CURSOR_BOX_AFTER,
                 NewY + CMP_CURSOR_BOX_AFTER};
    CmpInvalidateRect(&A);
    CmpInvalidateRect(&B);
}

void CmpShowDesktop()
{
    if (!Cmp.Available)
        return;
    InbvHandOffDisplay();
    Cmp.DesktopFadeStartUs = KeQueryUptimeMicroseconds();
    Cmp.DesktopShown = true;
    HZ_RECT Screen = {0, 0, Cmp.Width, Cmp.Height};
    CmpInvalidateRect(&Screen);
}

bool CmpIsDesktopShown()
{
    return Cmp.DesktopShown;
}

STATUS CmpFlush(uint32_t TimeoutMs)
{
    if (!Cmp.Available)
        return STATUS_DEVICE_NOT_READY;
    uint64_t Deadline = KeQueryUptimeMicroseconds() + (uint64_t)TimeoutMs * 1000;
    for (;;) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&Cmp.DirtyLock, &OldIrql);
        bool Idle = RgnIsEmpty(Cmp.Dirty) && !Cmp.Composing;
        KeReleaseSpinLock(&Cmp.DirtyLock, OldIrql);
        if (Idle)
            return STATUS_SUCCESS;
        if (KeQueryUptimeMicroseconds() >= Deadline)
            return STATUS_TIMEOUT;
        KeDelayExecutionThread(2000);
    }
}

STATUS CmpGetPixel(int32_t X, int32_t Y, uint32_t *Color)
{
    if (!Cmp.Available)
        return STATUS_DEVICE_NOT_READY;
    if (X < 0 || Y < 0 || X >= Cmp.Width || Y >= Cmp.Height)
        return STATUS_INVALID_PARAMETER;
    *Color = Cmp.Back[(size_t)Y * Cmp.Width + X] & 0xFFFFFF;
    return STATUS_SUCCESS;
}
