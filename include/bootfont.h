/*
 * Nexora OS - include/bootfont.h
 * Pre-rasterized grayscale fonts for kernel-drawn text (boot splash, boot log, stop screen).
 * Generated at build time by tools/fontgen from the vendored Inter TTFs, so the kernel needs
 * neither floating point nor a TrueType rasterizer.
 */
#ifndef BOOTFONT_H
#define BOOTFONT_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

#define BOOTFONT_FIRST_CHAR 32
#define BOOTFONT_LAST_CHAR 126
#define BOOTFONT_GLYPH_COUNT (BOOTFONT_LAST_CHAR - BOOTFONT_FIRST_CHAR + 1)

typedef struct _BOOT_GLYPH {
    int32_t Advance;  /* 26.6 fixed point pixels */
    int16_t OffsetX;  /* bitmap left edge relative to pen position */
    int16_t OffsetY;  /* bitmap top edge relative to baseline (negative = above) */
    uint16_t Width;
    uint16_t Height;
    uint32_t BitmapOffset; /* into BOOT_FONT.Bitmap; Width*Height alpha bytes */
} BOOT_GLYPH;

typedef struct _BOOT_KERN_PAIR {
    uint8_t Left;
    uint8_t Right;
    int16_t Adjust; /* 26.6 fixed point pixels */
} BOOT_KERN_PAIR;

typedef struct _BOOT_FONT {
    const char *Name;
    uint16_t PixelSize; /* em size in pixels */
    int16_t Ascent;     /* pixels above baseline */
    int16_t Descent;    /* pixels below baseline (positive) */
    int16_t LineHeight; /* baseline to baseline */
    const BOOT_GLYPH *Glyphs; /* BOOTFONT_GLYPH_COUNT entries */
    const BOOT_KERN_PAIR *Kerning; /* sorted by (Left, Right) */
    uint32_t KerningCount;
    const uint8_t *Bitmap;
} BOOT_FONT;

/* Fonts linked into the kernel (see Makefile BOOTFONTS). */
extern const BOOT_FONT FontUi14;       /* Inter Regular 14 px: boot log */
extern const BOOT_FONT FontUi16;       /* Inter Regular 16 px: body text */
extern const BOOT_FONT FontUiSemi16;   /* Inter SemiBold 16 px: sub-headings */
extern const BOOT_FONT FontUiSemi24;   /* Inter SemiBold 24 px: headings */
extern const BOOT_FONT FontDisplay44;  /* Inter Display SemiBold 44 px: product name */
extern const BOOT_FONT FontMono13;     /* JetBrains Mono Regular 13 px: registers, traces */

HZ_EXTERN_C_END

#endif /* BOOTFONT_H */
