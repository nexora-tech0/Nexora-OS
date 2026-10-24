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

}  // namespace hz::wallpaper

#endif
