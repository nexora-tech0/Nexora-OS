/*
 * Nexora OS - user/lib/hzui/wallpaper.cpp
 * Shared metadata and checked BMP preview decoding for wallpaper selectors.
 */
#include "hzfile.h"
#include "hzwallpaper.h"

namespace hz::wallpaper {

namespace {

constexpr Info kWallpapers[Count] = {
    {"Mountain falls", "Waterfall beneath a mountain"},
    {"Coastal cliffs", "Cliffs at sunset"},
    {"City at dusk", "City lights at twilight"},
    {"Red fox", "A red fox in winter"},
    {"Coastal lock screen", "Purple mountain coast"},
};

uint16_t ReadLe16(const uint8_t *Data)
{
    return (uint16_t)(Data[0] | ((uint16_t)Data[1] << 8));
}

uint32_t ReadLe32(const uint8_t *Data)
{
    return (uint32_t)Data[0] | ((uint32_t)Data[1] << 8) | ((uint32_t)Data[2] << 16) |
           ((uint32_t)Data[3] << 24);
}

}  // namespace

const Info *GetInfo(uint32_t Index)
{
    return Index < Count ? &kWallpapers[Index] : nullptr;
}

STATUS LoadBmp(const char *Path, int32_t MaxSize, uint32_t **Pixels, int32_t *Width, int32_t *Height)
{
    if (!Path || !Pixels || !Width || !Height || MaxSize <= 0)
        return STATUS_INVALID_PARAMETER;
    *Pixels = nullptr;
    *Width = 0;
    *Height = 0;

    hz::String File;
    STATUS Status = hz::fs::ReadAll(hz::String(Path), File, 4 * 1024 * 1024);
    if (!HZ_SUCCESS(Status))
        return Status;
    const uint8_t *Data = (const uint8_t *)File.CStr();
    if (File.Length() < 54 || Data[0] != 'B' || Data[1] != 'M')
        return STATUS_INVALID_PARAMETER;

    uint32_t PixelOffset = ReadLe32(Data + 10), HeaderSize = ReadLe32(Data + 14);
    int32_t SourceWidth = (int32_t)ReadLe32(Data + 18), SignedHeight = (int32_t)ReadLe32(Data + 22);
    uint16_t Planes = ReadLe16(Data + 26), BitsPerPixel = ReadLe16(Data + 28);
    uint32_t Compression = ReadLe32(Data + 30);
    if (HeaderSize < 40 || (uint64_t)HeaderSize + 14 > File.Length() || PixelOffset < HeaderSize + 14 ||
        PixelOffset > File.Length() || SourceWidth <= 0 || SourceWidth > MaxSize || SignedHeight == INT32_MIN ||
        SignedHeight == 0 || Planes != 1 || BitsPerPixel != 24 || Compression != 0)
        return STATUS_INVALID_PARAMETER;

    int32_t SourceHeight = SignedHeight < 0 ? -SignedHeight : SignedHeight;
    if (SourceHeight > MaxSize)
        return STATUS_INVALID_PARAMETER;
    uint64_t RowBytes = ((uint64_t)SourceWidth * 3 + 3) & ~3ull;
    if (RowBytes * (uint32_t)SourceHeight > File.Length() - PixelOffset)
        return STATUS_INVALID_PARAMETER;

    uint64_t PixelCount = (uint64_t)SourceWidth * SourceHeight;
    uint32_t *Decoded = (uint32_t *)malloc((size_t)PixelCount * sizeof(uint32_t));
    if (!Decoded)
        return STATUS_NO_MEMORY;
    for (int32_t Y = 0; Y < SourceHeight; Y++) {
        int32_t SourceY = SignedHeight < 0 ? Y : SourceHeight - 1 - Y;
        const uint8_t *Row = Data + PixelOffset + RowBytes * (uint32_t)SourceY;
        for (int32_t X = 0; X < SourceWidth; X++) {
            const uint8_t *Pixel = Row + (size_t)X * 3;
            Decoded[(size_t)Y * SourceWidth + X] =
                0xFF000000u | ((uint32_t)Pixel[2] << 16) | ((uint32_t)Pixel[1] << 8) | Pixel[0];
        }
    }
    *Pixels = Decoded;
    *Width = SourceWidth;
    *Height = SourceHeight;
    return STATUS_SUCCESS;
}

STATUS LoadPreview(uint32_t Index, uint32_t **Pixels, int32_t *Width, int32_t *Height)
{
    if (!Pixels || !Width || !Height || Index >= Count)
        return STATUS_INVALID_PARAMETER;
    hz::String Path = hz::String::Format("C:\\Nexora\\Wallpapers\\wallpaper-preview-%03u.bmp", Index);
    return LoadBmp(Path.CStr(), 512, Pixels, Width, Height);
}

STATUS MakeRoundPicture(const char *Path, int32_t Size, uint32_t Matte, uint32_t *Out)
{
    if (!Path || !Out || Size <= 0 || Size > 256)
        return STATUS_INVALID_PARAMETER;
    uint32_t *Source = nullptr;
    int32_t SourceWidth = 0, SourceHeight = 0;
    STATUS Status = LoadBmp(Path, 512, &Source, &SourceWidth, &SourceHeight);
    if (!HZ_SUCCESS(Status))
        return Status;

    /* Centre-crop the source to a square, then box-average each disc pixel from its share of it. */
    int32_t Side = SourceWidth < SourceHeight ? SourceWidth : SourceHeight;
    int32_t OriginX = (SourceWidth - Side) / 2, OriginY = (SourceHeight - Side) / 2;
    float Radius = Size * 0.5f;
    uint32_t MatteR = Matte & 0xFF, MatteG = (Matte >> 8) & 0xFF, MatteB = (Matte >> 16) & 0xFF;
    for (int32_t Y = 0; Y < Size; Y++) {
        for (int32_t X = 0; X < Size; X++) {
            /* Coverage: 4x4 subsamples of the pixel against the disc, so the rim is antialiased. */
            int Covered = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float Px = X + (sx + 0.5f) * 0.25f, Py = Y + (sy + 0.5f) * 0.25f;
                    float Dx = Px - Radius, Dy = Py - Radius;
                    if (Dx * Dx + Dy * Dy <= Radius * Radius)
                        Covered++;
                }
            }
            int32_t Sx0 = OriginX + X * Side / Size, Sx1 = OriginX + (X + 1) * Side / Size;
            int32_t Sy0 = OriginY + Y * Side / Size, Sy1 = OriginY + (Y + 1) * Side / Size;
            if (Sx1 <= Sx0)
                Sx1 = Sx0 + 1;
            if (Sy1 <= Sy0)
                Sy1 = Sy0 + 1;
            uint32_t R = 0, G = 0, B = 0, Count = 0;
            for (int32_t Sy = Sy0; Sy < Sy1 && Sy < SourceHeight; Sy++) {
                for (int32_t Sx = Sx0; Sx < Sx1 && Sx < SourceWidth; Sx++) {
                    uint32_t C = Source[(size_t)Sy * SourceWidth + Sx];
                    R += C & 0xFF;
                    G += (C >> 8) & 0xFF;
                    B += (C >> 16) & 0xFF;
                    Count++;
                }
            }
            if (!Count) {
                R = MatteR;
                G = MatteG;
                B = MatteB;
                Count = 1;
            }
            uint32_t Cr = R / Count, Cg = G / Count, Cb = B / Count;
            uint32_t Or = (Cr * (uint32_t)Covered + MatteR * (uint32_t)(16 - Covered)) / 16;
            uint32_t Og = (Cg * (uint32_t)Covered + MatteG * (uint32_t)(16 - Covered)) / 16;
            uint32_t Ob = (Cb * (uint32_t)Covered + MatteB * (uint32_t)(16 - Covered)) / 16;
            Out[(size_t)Y * Size + X] = 0xFF000000u | (Ob << 16) | (Og << 8) | Or;
        }
    }
    free(Source);
    return STATUS_SUCCESS;
}

}  // namespace hz::wallpaper
