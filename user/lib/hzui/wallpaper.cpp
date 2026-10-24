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

STATUS LoadPreview(uint32_t Index, uint32_t **Pixels, int32_t *Width, int32_t *Height)
{
    if (!Pixels || !Width || !Height || Index >= Count)
        return STATUS_INVALID_PARAMETER;
    *Pixels = nullptr;
    *Width = 0;
    *Height = 0;

    hz::String Path = hz::String::Format("C:\\Nexora\\Wallpapers\\wallpaper-preview-%03u.bmp", Index);
    hz::String File;
    STATUS Status = hz::fs::ReadAll(Path, File, 256 * 1024);
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
        PixelOffset > File.Length() || SourceWidth <= 0 || SourceWidth > 512 || SignedHeight == INT32_MIN ||
        SignedHeight == 0 || Planes != 1 || BitsPerPixel != 24 || Compression != 0)
        return STATUS_INVALID_PARAMETER;

    int32_t SourceHeight = SignedHeight < 0 ? -SignedHeight : SignedHeight;
    if (SourceHeight > 512)
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

}  // namespace hz::wallpaper
