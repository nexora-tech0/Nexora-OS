/*
 * Nexora OS - user/include/hzimage.h
 * hz::image decodes the picture files the Photo Viewer opens (user/lib/hzrt/hzimage.cpp). The
 * decoder is the vendored stb_image (third_party/stb, public domain / MIT), compiled here in
 * freestanding user mode with its allocation, assertion and thread-local hooks mapped onto the
 * Nexora runtime.
 *
 * JPEG, PNG, BMP, GIF, TGA and PNM (PBM/PGM/PPM) decode. Photoshop documents and Radiance HDR are
 * recognised from their signature so the viewer can name what it found, but they are not compiled
 * in and Decode refuses them. Video is a different problem: a container demuxer plus an open codec,
 * and for sound an audio device the kernel does not have yet, so no video format is claimed here.
 *
 * A decoded Bitmap holds 0xAARRGGBB pixels, first row first, which is what Painter::DrawBitmap
 * takes; the alpha byte is 0xFF everywhere except for images that carry transparency.
 */
#ifndef HZIMAGE_H
#define HZIMAGE_H

#include "hzfile.h"

namespace hz::image {

enum class Format : uint8_t { Unknown, Bmp, Png, Jpeg, Gif, Tga, Pnm, Psd, Hdr };

struct Bitmap {
    uint32_t *Pixels = nullptr; /* Width * Height, malloc'd */
    int32_t Width = 0, Height = 0;

    /* Frees the pixels and zeroes the bitmap. Safe on an empty one, and on the same bitmap twice. */
    void Release();
};

/* The format of a file, from its leading bytes; a file whose signature is not one we know is
   named from its extension instead (TGA has no signature, and a .tga is only a .tga by name). */
Format Detect(const String &Path);
const char *FormatName(Format Value);
/* Whether Decode reads this format. False for Unknown, and for the recognised-but-not-compiled
   formats above, so a caller can tell "not an image" from "an image we cannot open". */
bool IsSupported(Format Value);

/* Reads and decodes Path. STATUS_NOT_SUPPORTED when the bytes are not a format we read,
   STATUS_FILE_TOO_LARGE past kMaxFileBytes, STATUS_UNSUCCESSFUL when the file is damaged. */
STATUS Decode(const String &Path, Bitmap *Out);
/* The same, from bytes already in memory. */
STATUS DecodeMemory(const uint8_t *Data, size_t Size, Bitmap *Out);
/* The same with a caller-selected decoded-pixel budget, checked before allocating pixel storage. */
STATUS DecodeMemoryBounded(const uint8_t *Data, size_t Size, uint64_t MaximumPixels, Bitmap *Out);

/* Why the last Decode call failed, in the decoder's own words. Never null. */
const char *LastError();

/* A copy of Source turned by Turns quarter turns clockwise (Turns is taken modulo 4, and 0 still
   copies). Rotation cannot be done with Painter::DrawBitmap, which only stretches. */
STATUS RotateQuarter(const Bitmap &Source, int Turns, Bitmap *Out);

/* The largest file Decode reads (a decoded bitmap is bounded separately, in the decoder). */
constexpr uint64_t kMaxFileBytes = 64u * 1024 * 1024;

}  // namespace hz::image

#endif /* HZIMAGE_H */
