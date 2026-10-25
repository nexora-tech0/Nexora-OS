/*
 * Nexora OS - user/include/hzwallpaper.h
 * Shared wallpaper labels and packaged preview loading for Settings and the shell.
 */
#ifndef HZWALLPAPER_H
#define HZWALLPAPER_H

#include "hzcore.h"
#include "hzuser.h"

namespace hz::wallpaper {

struct Info {
    const char *Name;
    const char *Description;
};

constexpr uint32_t Count = HZ_MAX_WALLPAPER + 1;

const Info *GetInfo(uint32_t Index);
STATUS LoadPreview(uint32_t Index, uint32_t **Pixels, int32_t *Width, int32_t *Height);

/* Decodes an uncompressed 24-bit BMP (top-down or bottom-up) whose sides are at most MaxSize into
   0xAARRGGBB pixels; *Pixels is malloc'd and the caller frees it. Errors when the file is not a
   BMP of that shape. Wallpaper previews use it, and so does the profile picture. */
STATUS LoadBmp(const char *Path, int32_t MaxSize, uint32_t **Pixels, int32_t *Width, int32_t *Height);

/* Renders Path as a circular profile picture: Size x Size pixels, centre-cropped and box-averaged
   from the source, blended onto Matte outside the disc so it can be drawn with an opaque blit.
   Out holds Size * Size pixels. Errors when the file cannot be decoded, so the caller can fall
   back to the initial of the account's name. */
STATUS MakeRoundPicture(const char *Path, int32_t Size, uint32_t Matte, uint32_t *Out);

}  // namespace hz::wallpaper

#endif
