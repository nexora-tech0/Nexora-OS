/*
 * Nexora OS - include/bootvid.h
 * Boot video driver (Vid*): direct framebuffer drawing used before win32k exists and by the
 * stop screen afterwards. Never reads the framebuffer back (it is write-combined and slow to
 * read); antialiased drawing blends against a caller-supplied background colour instead.
 * Colours are 0xRRGGBB.
 */
#ifndef BOOTVID_H
#define BOOTVID_H

#include "bootfont.h"
#include "bootinfo.h"
#include "hzbase.h"

HZ_EXTERN_C_BEGIN

bool VidInitialize(const FRAMEBUFFER_INFO *Framebuffer);
bool VidIsReady(void);
int VidWidth(void);
int VidHeight(void);

/* Restricts all drawing to a rectangle (intersected with the screen). */
void VidSetClip(int X, int Y, int Width, int Height);
void VidResetClip(void);

void VidPutPixel(int X, int Y, uint32_t Color);
void VidFillRect(int X, int Y, int Width, int Height, uint32_t Color);
void VidFillVerticalGradient(int X, int Y, int Width, int Height, uint32_t Top, uint32_t Bottom);

/* Linear blend: Alpha 0 = Background, 255 = Foreground. */
uint32_t VidBlend(uint32_t Background, uint32_t Foreground, uint32_t Alpha);

/*
 * Fills an antialiased disc. Centre and Radius are Q16.16 pixels, so callers can place a dot at a
 * sub-pixel position and animate it without it jumping from pixel to pixel. Edge pixels blend from
 * Background to Color; nothing is read back from the framebuffer.
 */
void VidFillDiscAA(int32_t CenterX, int32_t CenterY, int32_t Radius, uint32_t Color, uint32_t Background);

/* Width in pixels of Text rendered with Font (includes kerning). */
int VidMeasureText(const BOOT_FONT *Font, const char *Text);

/*
 * Draws Text with its baseline at BaselineY, blending glyph coverage from Background to
 * Foreground. Returns the pen X position after the last glyph.
 */
int VidDrawText(const BOOT_FONT *Font, int X, int BaselineY, const char *Text, uint32_t Foreground,
                uint32_t Background);

/* Draws the Nexora OS logo (rounded tile, sunrise over water) as a Size x Size square. */
void VidDrawLogo(int X, int Y, int Size, uint32_t Background);
void VidDrawLogoTinted(int X, int Y, int Size, uint32_t Background, uint32_t Foreground);

HZ_EXTERN_C_END

#endif /* BOOTVID_H */
