/*
 * Nexora OS - user/lib/hzuser/font.cpp
 * TrueType text: fonts are read from files into the heap and rasterized with stb_truetype
 * (third_party/stb, public domain / MIT). Sizes are em sizes in pixels (like CSS font-size).
 * Glyph bitmaps for code points below 256 are cached per font, and (M11) up to 192 others in a
 * small hash table; beyond that they are rasterized on every use. The pen advances in fractional pixels (with kerning); each glyph is placed at the
 * nearest whole pixel. A font file is read once and shared by every size loaded from it.
 */
#include "hzmath.h"
#include "hzuser.h"

#define STBTT_ifloor(x) ((int)HzFloor(x))
#define STBTT_iceil(x) ((int)HzCeil(x))
#define STBTT_sqrt(x) HzSqrt(x)
#define STBTT_pow(x, y) HzPow(x, y)
#define STBTT_fmod(x, y) HzFmod(x, y)
#define STBTT_cos(x) HzCos(x)
#define STBTT_acos(x) HzAcos(x)
#define STBTT_fabs(x) HzFabs(x)
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u) ((void)(u), free(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STB_TRUETYPE_IMPLEMENTATION

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../../../third_party/stb/stb_truetype.h"
#pragma clang diagnostic pop

namespace {

struct Glyph {
    bool Ready;
    uint8_t *Bitmap; /* Width x Height coverage, NULL for blank glyphs */
    int Width, Height, OffsetX, OffsetY;
    float Advance;   /* pixels */
};

/* Glyphs above U+00FF (M11): an open-addressed table, filled to three quarters at most; the rest
   are rasterized on every use as before. Entries never move once inserted. */
constexpr uint32_t kExtraSlots = 256;
struct ExtraGlyph {
    uint32_t CodePoint; /* 0 = free */
    Glyph G;
};

}  // namespace

struct _HZ_FONT {
    stbtt_fontinfo Info;
    uint8_t *Data;
    float Scale;
    int32_t Ascent, Descent, LineGap; /* pixels */
    volatile uint32_t Lock;
    Glyph Cache[256];
    ExtraGlyph *Extra; /* kExtraSlots entries, allocated on first use */
    uint32_t ExtraCount;
};

namespace {

void FontLock(HZ_FONT *Font)
{
    while (__atomic_exchange_n(&Font->Lock, 1, __ATOMIC_ACQUIRE))
        NtYieldExecution();
}

void FontUnlock(HZ_FONT *Font)
{
    __atomic_store_n(&Font->Lock, 0, __ATOMIC_RELEASE);
}

uint8_t *ReadWholeFile(const char *Path, uint64_t *Size)
{
    HANDLE File;
    if (!HZ_SUCCESS(NtCreateFile(&File, GENERIC_READ, Path, FILE_OPEN, FILE_NON_DIRECTORY_FILE, nullptr)))
        return nullptr;
    HZ_FILE_BASIC_INFORMATION Basic = {};
    uint8_t *Data = nullptr;
    if (HZ_SUCCESS(NtQueryInformationFile(File, HZ_FILE_BASIC_INFORMATION_CLASS, &Basic, sizeof(Basic))) &&
        Basic.EndOfFile > 0 && Basic.EndOfFile < 32 * 1024 * 1024) {
        Data = (uint8_t *)malloc(Basic.EndOfFile);
        uint64_t Offset = 0;
        while (Data && Offset < Basic.EndOfFile) {
            uint32_t Chunk = (uint32_t)HZ_MIN(Basic.EndOfFile - Offset, (uint64_t)65536), Got = 0;
            if (!HZ_SUCCESS(NtReadFile(File, Data + Offset, Chunk, &Offset, &Got)) || Got == 0) {
                free(Data);
                Data = nullptr;
                break;
            }
            Offset += Got;
        }
    }
    NtClose(File);
    *Size = Basic.EndOfFile;
    return Data;
}

/* Rasterizes one glyph (caller holds the font lock for cached glyphs). */
void Rasterize(HZ_FONT *Font, uint32_t CodePoint, Glyph *G)
{
    int Index = stbtt_FindGlyphIndex(&Font->Info, (int)CodePoint);
    int Advance = 0, Bearing = 0;
    stbtt_GetGlyphHMetrics(&Font->Info, Index, &Advance, &Bearing);
    G->Advance = Advance * Font->Scale;
    G->Bitmap = nullptr;
    G->Width = G->Height = G->OffsetX = G->OffsetY = 0;
    if (!stbtt_IsGlyphEmpty(&Font->Info, Index))
        G->Bitmap = stbtt_GetGlyphBitmap(&Font->Info, Font->Scale, Font->Scale, Index, &G->Width, &G->Height,
                                         &G->OffsetX, &G->OffsetY);
    G->Ready = true;
}

/* The cached glyph of a code point above U+00FF, or NULL when the table is full. */
Glyph *CachedExtra(HZ_FONT *Font, uint32_t C)
{
    Glyph *Found = nullptr;
    FontLock(Font);
    if (!Font->Extra)
        Font->Extra = (ExtraGlyph *)calloc(kExtraSlots, sizeof(ExtraGlyph));
    if (Font->Extra) {
        uint32_t i = (C * 2654435761u) & (kExtraSlots - 1);
        for (uint32_t n = 0; n < kExtraSlots; n++, i = (i + 1) & (kExtraSlots - 1)) {
            ExtraGlyph &E = Font->Extra[i];
            if (E.CodePoint == C) {
                Found = &E.G;
                break;
            }
            if (E.CodePoint == 0) {
                if (Font->ExtraCount < kExtraSlots * 3 / 4) {
                    Rasterize(Font, C, &E.G);
                    E.CodePoint = C;
                    Font->ExtraCount++;
                    Found = &E.G;
                }
                break;
            }
        }
    }
    FontUnlock(Font);
    return Found;
}

/* Next code point of UTF-8 text (invalid bytes decode as U+FFFD). */
uint32_t NextCodePoint(const char **Text)
{
    const uint8_t *P = (const uint8_t *)*Text;
    uint32_t C = *P++;
    int Extra = 0;
    if (C >= 0xF0 && C < 0xF8) {
        C &= 0x07;
        Extra = 3;
    } else if (C >= 0xE0) {
        C &= 0x0F;
        Extra = 2;
    } else if (C >= 0xC0) {
        C &= 0x1F;
        Extra = 1;
    } else if (C >= 0x80) {
        *Text = (const char *)P;
        return 0xFFFD;
    }
    for (; Extra > 0; Extra--) {
        if ((*P & 0xC0) != 0x80) {
            *Text = (const char *)P;
            return 0xFFFD;
        }
        C = (C << 6) | (*P++ & 0x3F);
    }
    *Text = (const char *)P;
    return C;
}

/* Walks the code points in [Text, End) (End = NULL: up to the terminating NUL). */
template <typename Visit>
float WalkText(HZ_FONT *Font, const char *Text, const char *End, Visit &&Visitor)
{
    float Pen = 0;
    int Previous = 0;
    while (End ? Text < End : *Text != '\0') {
        uint32_t C = NextCodePoint(&Text);
        int Index = stbtt_FindGlyphIndex(&Font->Info, (int)C);
        if (Previous)
            Pen += stbtt_GetGlyphKernAdvance(&Font->Info, Previous, Index) * Font->Scale;
        Glyph Temporary;
        Glyph *G;
        if (C < 256) {
            G = &Font->Cache[C];
            if (!G->Ready) {
                FontLock(Font);
                if (!G->Ready)
                    Rasterize(Font, C, G);
                FontUnlock(Font);
            }
        } else if (!(G = CachedExtra(Font, C))) {
            Rasterize(Font, C, &Temporary);
            G = &Temporary;
        }
        Visitor(*G, Pen);
        Pen += G->Advance;
        if (G == &Temporary && Temporary.Bitmap)
            stbtt_FreeBitmap(Temporary.Bitmap, nullptr);
        Previous = Index;
    }
    return Pen;
}

HZ_FONT *g_Standard[HZ_FONT_COUNT];
bool g_StandardTried[HZ_FONT_COUNT];

/* Font files already read (shared by every size; never freed). */
struct LoadedFile {
    char Path[96];
    uint8_t *Data;
};
LoadedFile g_Files[8];
volatile uint32_t g_FilesLock;

uint8_t *SharedFile(const char *Path)
{
    while (__atomic_exchange_n(&g_FilesLock, 1, __ATOMIC_ACQUIRE))
        NtYieldExecution();
    uint8_t *Data = nullptr;
    for (LoadedFile &F : g_Files) {
        if (F.Data && strcmp(F.Path, Path) == 0) {
            Data = F.Data;
            break;
        }
    }
    if (!Data) {
        uint64_t Size = 0;
        Data = ReadWholeFile(Path, &Size);
        size_t Length = strlen(Path);
        for (LoadedFile &F : g_Files) {
            if (Data && !F.Data && Length < sizeof(F.Path)) {
                memcpy(F.Path, Path, Length + 1);
                F.Data = Data;
                break;
            }
        }
    }
    __atomic_store_n(&g_FilesLock, 0, __ATOMIC_RELEASE);
    return Data;
}

}  // namespace

extern "C" HZ_FONT *HzLoadFont(const char *Path, int32_t PixelHeight)
{
    uint8_t *Data = SharedFile(Path);
    if (!Data)
        return nullptr;
    HZ_FONT *Font = (HZ_FONT *)calloc(1, sizeof(HZ_FONT));
    if (!Font || !stbtt_InitFont(&Font->Info, Data, stbtt_GetFontOffsetForIndex(Data, 0))) {
        free(Font);
        return nullptr;
    }
    Font->Data = Data;
    Font->Scale = stbtt_ScaleForMappingEmToPixels(&Font->Info, (float)PixelHeight);
    int Ascent, Descent, LineGap;
    stbtt_GetFontVMetrics(&Font->Info, &Ascent, &Descent, &LineGap);
    Font->Ascent = (int32_t)HzCeil(Ascent * Font->Scale);
    Font->Descent = (int32_t)HzCeil(-Descent * Font->Scale);
    Font->LineGap = (int32_t)HzCeil(LineGap * Font->Scale);
    return Font;
}

extern "C" HZ_FONT *HzGetFont(int Which)
{
    static const struct {
        const char *Path;
        int32_t Size;
        const char *FallbackPath;
    } Standard[HZ_FONT_COUNT] = {
        {"C:\\Nexora\\Fonts\\Inter-Regular.ttf", 14, "C:\\Nexora\\Fonts\\Inter-SemiBold.ttf"},
        {"C:\\Nexora\\Fonts\\Inter-SemiBold.ttf", 14, "C:\\Nexora\\Fonts\\Inter-Regular.ttf"},
        {"C:\\Nexora\\Fonts\\Inter-SemiBold.ttf", 28, "C:\\Nexora\\Fonts\\Inter-Regular.ttf"},
        {"C:\\Nexora\\Fonts\\Inter-Regular.ttf", 13, "C:\\Nexora\\Fonts\\Inter-SemiBold.ttf"},
        {"C:\\Nexora\\Fonts\\Inter-SemiBold.ttf", 20, "C:\\Nexora\\Fonts\\Inter-Regular.ttf"},
        {"C:\\Nexora\\Fonts\\Inter-Regular.ttf", 12, "C:\\Nexora\\Fonts\\Inter-SemiBold.ttf"},
        {"C:\\Nexora\\Fonts\\JetBrainsMono-Regular.ttf", 14, "C:\\Nexora\\Fonts\\Inter-Regular.ttf"},
        {"C:\\Nexora\\Fonts\\Inter-Regular.ttf", 18, "C:\\Nexora\\Fonts\\Inter-SemiBold.ttf"},
        {"C:\\Nexora\\Fonts\\InterDisplay-SemiBold.ttf", 72, "C:\\Nexora\\Fonts\\Inter-SemiBold.ttf"},
    };
    if (Which < 0 || Which >= HZ_FONT_COUNT)
        return nullptr;
    if (!g_StandardTried[Which]) {
        g_Standard[Which] = HzLoadFont(Standard[Which].Path, Standard[Which].Size);
        if (!g_Standard[Which]) {
            HzPrint("hzuser: falling back from %s", Standard[Which].Path);
            g_Standard[Which] = HzLoadFont(Standard[Which].FallbackPath, Standard[Which].Size);
        }
        g_StandardTried[Which] = true;
        if (!g_Standard[Which])
            HzPrint("hzuser: cannot load %s", Standard[Which].Path);
    }
    return g_Standard[Which];
}

extern "C" HZ_FONT *HzIconFont(int32_t PixelHeight)
{
    /* The icon font is a normal TrueType font whose glyphs are pictures; each size is its own
       HZ_FONT (the file itself is read once by SharedFile and shared). Snap to a small set of sizes
       so a rounded layout does not load a new font for every pixel. */
    static const char *const Path = "C:\\Nexora\\Fonts\\MaterialIcons-Regular.ttf";
    static const int32_t Steps[] = {12, 14, 16, 18, 20, 24, 28, 32, 40, 48, 64, 96, 128};
    struct Entry {
        int32_t Size;
        HZ_FONT *Font;
    };
    static Entry Cache[8];
    static int32_t Count;

    if (PixelHeight < Steps[0])
        PixelHeight = Steps[0];
    int32_t Size = Steps[sizeof(Steps) / sizeof(Steps[0]) - 1];
    for (int32_t Step : Steps) {
        if (Step >= PixelHeight) {
            Size = Step;
            break;
        }
    }
    for (int32_t i = 0; i < Count; i++) {
        if (Cache[i].Size == Size)
            return Cache[i].Font;
    }
    HZ_FONT *Font = HzLoadFont(Path, Size);
    if (!Font)
        HzPrint("hzuser: cannot load %s", Path);
    if (Count < (int32_t)(sizeof(Cache) / sizeof(Cache[0]))) {
        Cache[Count].Size = Size;
        Cache[Count].Font = Font;
        Count++;
    }
    return Font;
}

extern "C" int32_t HzFontAscent(const HZ_FONT *Font)
{
    return Font ? Font->Ascent : 0;
}

extern "C" int32_t HzFontDescent(const HZ_FONT *Font)
{
    return Font ? Font->Descent : 0;
}

extern "C" int32_t HzFontLineHeight(const HZ_FONT *Font)
{
    return Font ? Font->Ascent + Font->Descent + Font->LineGap : 0;
}

extern "C" int32_t HzMeasureText(HZ_FONT *Font, const char *Text)
{
    if (!Font || !Text)
        return 0;
    return (int32_t)HzCeil(WalkText(Font, Text, nullptr, [](const Glyph &, float) {}));
}

extern "C" int32_t HzMeasureTextN(HZ_FONT *Font, const char *Text, size_t Length)
{
    if (!Font || !Text || !Length)
        return 0;
    return (int32_t)HzCeil(WalkText(Font, Text, Text + Length, [](const Glyph &, float) {}));
}

extern "C" int32_t HzMeasureTextFixed(HZ_FONT *Font, const char *Text, size_t Length)
{
    if (!Font || !Text || !Length)
        return 0;
    return (int32_t)HzFloor(WalkText(Font, Text, Text + Length, [](const Glyph &, float) {}) * 64 + 0.5);
}

namespace {

int32_t DrawTextRange(HZ_CANVAS *Canvas, HZ_FONT *Font, int32_t X, int32_t Baseline, const char *Text,
                      const char *End, uint32_t Color)
{
    float Width = WalkText(Font, Text, End, [&](const Glyph &G, float Pen) {
        if (!G.Bitmap)
            return;
        int32_t Left = X + (int32_t)HzFloor(Pen + 0.5) + G.OffsetX, Top = Baseline + G.OffsetY;
        if (Left >= Canvas->Clip.Right || Left + G.Width <= Canvas->Clip.Left || Top >= Canvas->Clip.Bottom ||
            Top + G.Height <= Canvas->Clip.Top)
            return; /* outside the clip */
        for (int y = 0; y < G.Height; y++)
            for (int x = 0; x < G.Width; x++)
                HzBlendPixel(Canvas, Left + x, Top + y, Color, G.Bitmap[y * G.Width + x]);
    });
    return X + (int32_t)HzCeil(Width);
}

}  // namespace

extern "C" int32_t HzDrawText(HZ_CANVAS *Canvas, HZ_FONT *Font, int32_t X, int32_t Baseline, const char *Text,
                              uint32_t Color)
{
    if (!Font || !Text)
        return X;
    return DrawTextRange(Canvas, Font, X, Baseline, Text, nullptr, Color);
}

extern "C" int32_t HzDrawTextN(HZ_CANVAS *Canvas, HZ_FONT *Font, int32_t X, int32_t Baseline, const char *Text,
                               size_t Length, uint32_t Color)
{
    if (!Font || !Text || !Length)
        return X;
    return DrawTextRange(Canvas, Font, X, Baseline, Text, Text + Length, Color);
}

extern "C" void HzDrawGlyphCentered(HZ_CANVAS *Canvas, HZ_FONT *Font, uint32_t CodePoint, int32_t CenterX,
                                    int32_t CenterY, uint32_t Color)
{
    if (!Font || !Canvas)
        return;
    Glyph Temporary;
    Glyph *G;
    if (CodePoint < 256) {
        G = &Font->Cache[CodePoint];
        if (!G->Ready) {
            FontLock(Font);
            if (!G->Ready)
                Rasterize(Font, CodePoint, G);
            FontUnlock(Font);
        }
    } else if (!(G = CachedExtra(Font, CodePoint))) {
        Rasterize(Font, CodePoint, &Temporary);
        G = &Temporary;
    }
    if (G->Bitmap) {
        /* OffsetX/OffsetY place the ink relative to the pen origin on the baseline, so putting the
           ink's centre on (CenterX, CenterY) centres the picture itself, not the pen box. */
        int32_t Left = CenterX - (G->OffsetX + G->Width / 2);
        int32_t Top = CenterY - (G->OffsetY + G->Height / 2);
        for (int32_t y = 0; y < G->Height; y++) {
            int32_t Py = Top + y;
            if (Py < Canvas->Clip.Top || Py >= Canvas->Clip.Bottom)
                continue;
            for (int32_t x = 0; x < G->Width; x++) {
                int32_t Px = Left + x;
                if (Px < Canvas->Clip.Left || Px >= Canvas->Clip.Right)
                    continue;
                HzBlendPixel(Canvas, Px, Py, Color, G->Bitmap[y * G->Width + x]);
            }
        }
    }
    if (G == &Temporary && Temporary.Bitmap)
        stbtt_FreeBitmap(Temporary.Bitmap, nullptr);
}
