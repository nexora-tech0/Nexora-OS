/*
 * Nexora OS - user/lib/hzrt/hzimage.cpp
 * The picture decoder behind user/include/hzimage.h: the vendored stb_image (third_party/stb,
 * public domain / MIT) plus the Nexora side of it - reading the file, deciding what it is from
 * its signature, and handing back 0xAARRGGBB pixels.
 *
 * stb_image is compiled here rather than in a library that would carry it into every program.
 * Freestanding user mode has no <stdlib.h> or <string.h>, so third_party/stb/compat supplies the
 * declarations the header includes, and the STBI_* hooks below point its allocation, assertion and
 * thread-local uses at the Nexora runtime (the process heap, and plain globals because the PE
 * loader does not set up a TLS directory). The formats that need floating point (HDR, and its
 * linear variant) and PSD are switched off with STBI_NO_*, so no libm is needed either and the
 * recognised-but-unsupported cases are reported by Detect instead of half-decoding.
 */
#include "hzimage.h"

#define STBI_NO_STDIO           /* pictures arrive as memory, not through a FILE */
#define STBI_NO_THREAD_LOCALS   /* no TLS directory in a Nexora PE image */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_MALLOC(Size) malloc(Size)
#define STBI_REALLOC(Pointer, NewSize) realloc(Pointer, NewSize)
#define STBI_REALLOC_SIZED(Pointer, OldSize, NewSize) realloc(Pointer, NewSize)
#define STBI_FREE(Pointer) free(Pointer)
#define STBI_ASSERT(Condition) ((void)0)
#define STB_IMAGE_IMPLEMENTATION

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "../../../third_party/stb/stb_image.h"
#pragma clang diagnostic pop

namespace hz::image {

namespace {

constexpr size_t kHeaderBytes = 32;

/* A decoded bitmap is Width * Height * 4 bytes on the heap; refuse anything past this so a
   corrupt header cannot ask for a gigabyte. 64 million pixels is well past any camera. */
constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;

/* The first kHeaderBytes of Path, zero padded. False when the file cannot be read. */
bool ReadHeader(const String &Path, uint8_t *Buffer)
{
    HANDLE File;
    if (!HZ_SUCCESS(NtCreateFile(&File, GENERIC_READ, Path.CStr(), FILE_OPEN, FILE_NON_DIRECTORY_FILE, nullptr)))
        return false;
    memset(Buffer, 0, kHeaderBytes);
    uint32_t Got = 0;
    NtReadFile(File, Buffer, (uint32_t)kHeaderBytes, nullptr, &Got);
    NtClose(File);
    return Got > 0;
}

Format FormatFromBytes(const uint8_t *D)
{
    if (memcmp(D, "\x89PNG\r\n\x1a\n", 8) == 0)
        return Format::Png;
    if (D[0] == 0xFF && D[1] == 0xD8 && D[2] == 0xFF)
        return Format::Jpeg;
    if (memcmp(D, "GIF87a", 6) == 0 || memcmp(D, "GIF89a", 6) == 0)
        return Format::Gif;
    if (D[0] == 'B' && D[1] == 'M')
        return Format::Bmp;
    if (memcmp(D, "8BPS", 4) == 0)
        return Format::Psd;
    if (memcmp(D, "#?RADIANCE", 10) == 0 || memcmp(D, "#?RGBE", 6) == 0)
        return Format::Hdr;
    if (D[0] == 'P' && D[1] >= '1' && D[1] <= '6')
        return Format::Pnm;
    return Format::Unknown;
}

/* Only the formats whose signature is unreliable (TGA has none) are named by extension, and only
   after the bytes said nothing. */
Format FormatFromExtension(const String &Path)
{
    String Ext = hz::fs::Extension(Path);
    if (Ext == "tga")
        return Format::Tga;
    if (Ext == "png")
        return Format::Png;
    if (Ext == "bmp")
        return Format::Bmp;
    if (Ext == "jpg" || Ext == "jpeg" || Ext == "jpe")
        return Format::Jpeg;
    if (Ext == "gif")
        return Format::Gif;
    if (Ext == "pnm" || Ext == "ppm" || Ext == "pgm" || Ext == "pbm")
        return Format::Pnm;
    return Format::Unknown;
}

}  // namespace

void Bitmap::Release()
{
    if (Pixels)
        free(Pixels);
    Pixels = nullptr;
    Width = 0;
    Height = 0;
}

Format Detect(const String &Path)
{
    uint8_t Header[kHeaderBytes] = {};
    Format Found = ReadHeader(Path, Header) ? FormatFromBytes(Header) : Format::Unknown;
    return Found == Format::Unknown ? FormatFromExtension(Path) : Found;
}

const char *FormatName(Format Value)
{
    switch (Value) {
    case Format::Bmp: return "BMP";
    case Format::Png: return "PNG";
    case Format::Jpeg: return "JPEG";
    case Format::Gif: return "GIF";
    case Format::Tga: return "TGA";
    case Format::Pnm: return "PNM";
    case Format::Psd: return "Photoshop (PSD)";
    case Format::Hdr: return "Radiance HDR";
    default: return "unrecognised";
    }
}

bool IsSupported(Format Value)
{
    switch (Value) {
    case Format::Bmp:
    case Format::Png:
    case Format::Jpeg:
    case Format::Gif:
    case Format::Tga:
    case Format::Pnm:
        return true;
    default:
        return false;
    }
}

const char *LastError()
{
    const char *Reason = stbi_failure_reason();
    return Reason ? Reason : "no reason given";
}

STATUS DecodeMemory(const uint8_t *Data, size_t Size, Bitmap *Out)
{
    if (!Data || !Out || Size == 0)
        return STATUS_INVALID_PARAMETER;
    Out->Release();
    if (Size > (size_t)kMaxFileBytes)
        return STATUS_FILE_TOO_LARGE;
    if (Size > 0x7FFFFFFF)
        return STATUS_INVALID_PARAMETER;

    int Width = 0, Height = 0, SourceChannels = 0;
    stbi_uc *Rgba = stbi_load_from_memory(Data, (int)Size, &Width, &Height, &SourceChannels, 4);
    if (!Rgba)
        return STATUS_UNSUCCESSFUL;
    if (Width <= 0 || Height <= 0 || (uint64_t)Width * (uint64_t)Height > kMaxPixels) {
        stbi_image_free(Rgba);
        return Width <= 0 || Height <= 0 ? STATUS_UNSUCCESSFUL : STATUS_INVALID_PARAMETER;
    }

    uint32_t *Pixels = (uint32_t *)malloc((size_t)Width * (size_t)Height * sizeof(uint32_t));
    if (!Pixels) {
        stbi_image_free(Rgba);
        return STATUS_NO_MEMORY;
    }
    /* stb hands back RGBA bytes; hzui wants 0xAARRGGBB words. */
    size_t Count = (size_t)Width * (size_t)Height;
    for (size_t i = 0; i < Count; i++) {
        const stbi_uc *P = Rgba + i * 4;
        Pixels[i] = ((uint32_t)P[3] << 24) | ((uint32_t)P[0] << 16) | ((uint32_t)P[1] << 8) | P[2];
    }
    stbi_image_free(Rgba);

    Out->Pixels = Pixels;
    Out->Width = Width;
    Out->Height = Height;
    return STATUS_SUCCESS;
}

STATUS Decode(const String &Path, Bitmap *Out)
{
    if (!Out)
        return STATUS_INVALID_PARAMETER;
    Out->Release();
    String File;
    STATUS Status = hz::fs::ReadAll(Path, File, kMaxFileBytes);
    if (!HZ_SUCCESS(Status))
        return Status;
    return DecodeMemory((const uint8_t *)File.CStr(), File.Length(), Out);
}

STATUS RotateQuarter(const Bitmap &Source, int Turns, Bitmap *Out)
{
    if (!Out || Source.Width <= 0 || Source.Height <= 0 || !Source.Pixels)
        return STATUS_INVALID_PARAMETER;
    Out->Release();
    Turns = ((Turns % 4) + 4) % 4;
    int32_t Width = Turns == 1 || Turns == 3 ? Source.Height : Source.Width;
    int32_t Height = Turns == 1 || Turns == 3 ? Source.Width : Source.Height;
    uint32_t *Pixels = (uint32_t *)malloc((size_t)Width * (size_t)Height * sizeof(uint32_t));
    if (!Pixels)
        return STATUS_NO_MEMORY;
    for (int32_t Y = 0; Y < Source.Height; Y++) {
        for (int32_t X = 0; X < Source.Width; X++) {
            /* Where the source pixel lands after turning clockwise: top-left goes to the top-right. */
            int32_t Dx = Turns == 0 ? X : Turns == 1 ? Source.Height - 1 - Y : Turns == 2 ? Source.Width - 1 - X : Y;
            int32_t Dy = Turns == 0 ? Y : Turns == 1 ? X : Turns == 2 ? Source.Height - 1 - Y : Source.Width - 1 - X;
            Pixels[(size_t)Dy * Width + Dx] = Source.Pixels[(size_t)Y * Source.Width + X];
        }
    }
    Out->Pixels = Pixels;
    Out->Width = Width;
    Out->Height = Height;
    return STATUS_SUCCESS;
}

}  // namespace hz::image
