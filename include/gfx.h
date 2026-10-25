/*
 * Nexora OS - include/gfx.h
 * Graphics device API (Gfx*): a small, bounded abstraction over an accelerated display device
 * that the compositor (win32k/compose.cpp) presents through. The implementation lives in
 * drivers/gpu/virtio_gpu.c (VirtIO-GPU, PCI 1AF4:1050 / legacy 1AF4:1010). The UEFI/GOP
 * framebuffer stays the fallback: the compositor keeps drawing into it whenever this API reports
 * no device or a failed acquisition.
 *
 * Ownership and limits (read before using):
 *   - A display driver publishes its device once, at driver-entry time, with
 *     GfxRegisterDisplayDevice. GfxFindDisplayDevice returns that device, or NULL when only the
 *     UEFI framebuffer exists.
 *   - The compositor calls GfxAcquireDisplay exactly once, when the desktop takes over the
 *     screen (after InbvHandOffDisplay). That call allocates a framebuffer the compositor draws
 *     into and points the device's scanout at it; only if every step succeeds does display
 *     ownership move to the device (the caller must then draw only through the returned
 *     framebuffer). On any failure the call returns an error and the caller keeps the UEFI
 *     framebuffer untouched. There is at most one owner, and one acquired framebuffer, at a time.
 *   - GfxPresent flushes a bounded dirty rectangle (screen coordinates) from the framebuffer to
 *     the device. The caller must not write the framebuffer while a present is in flight; the
 *     compositor serializes frames on its compose lock. Rectangles are clamped to the mode.
 *   - All calls run at PASSIVE_LEVEL. Control commands block with a real timeout, so a wedged
 *     device makes GfxAcquireDisplay/GfxPresent fail instead of hanging forever.
 *   - Framebuffer memory is normal cacheable RAM owned by the device; the caller reads only its
 *     pixels/pitch and must return it with GfxReleaseFramebuffer (never ExFreePool).
 */
#ifndef GFX_H
#define GFX_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

/* Half-open screen rectangle: [Left, Right) x [Top, Bottom). */
typedef struct _GFX_RECT {
    int32_t Left, Top, Right, Bottom;
} GFX_RECT;

/* A display mode as the device presents it (or as the compositor requests it). */
typedef struct _GFX_MODE {
    uint32_t Width;
    uint32_t Height;
    uint32_t Pitch; /* bytes per scanline */
    uint16_t Bpp;   /* bits per pixel (32 for every device this API exposes) */
} GFX_MODE;

typedef struct _GFX_DEVICE GFX_DEVICE;
typedef struct _GFX_FRAMEBUFFER GFX_FRAMEBUFFER;

/* Publishes (or clears, with NULL) the display device a driver has taken over. Called by the
   driver; the compositor only reads it through GfxFindDisplayDevice. */
void GfxRegisterDisplayDevice(GFX_DEVICE *Device, const char *Name);

/* The display device an accelerated present can go through, or NULL. */
GFX_DEVICE *GfxFindDisplayDevice(void);
const char *GfxDeviceName(GFX_DEVICE *Device);

/* Describes the mode the device currently scans out (its rect even before we own it). */
STATUS GfxQueryMode(GFX_DEVICE *Device, GFX_MODE *Mode);

/*
 * Takes over the display: allocates a Width x Height framebuffer the compositor draws into,
 * creates the device resource, attaches the framebuffer as its backing and points scanout 0 at
 * it. Only when all of that succeeds is the device marked as the display owner and *Framebuffer
 * returned; on any failure nothing is changed and the caller must keep the UEFI framebuffer.
 * Width/Height must match the compositor's screen size.
 */
STATUS GfxAcquireDisplay(GFX_DEVICE *Device, uint32_t Width, uint32_t Height, GFX_FRAMEBUFFER **Framebuffer);

/* Releases a framebuffer from GfxAcquireDisplay: detaches its backing, drops the resource and
   frees the pages. Also releases the display back to the caller (the UEFI framebuffer is only a
   fallback path, so nothing draws to it here). */
void GfxReleaseFramebuffer(GFX_FRAMEBUFFER *Framebuffer);

/* Accessors the compositor draws through. */
uint32_t *GfxFramebufferPixels(GFX_FRAMEBUFFER *Framebuffer);
uint32_t GfxFramebufferWidth(GFX_FRAMEBUFFER *Framebuffer);
uint32_t GfxFramebufferHeight(GFX_FRAMEBUFFER *Framebuffer);
uint32_t GfxFramebufferPitch(GFX_FRAMEBUFFER *Framebuffer);

/* Copies a dirty region (clamped to the mode) to the device and flushes it. */
STATUS GfxPresent(GFX_FRAMEBUFFER *Framebuffer, const GFX_RECT *Dirty);

/* Runs the graphics self-tests (drivers/gpu/virtio_gpu.c): bounded command validation and
   virtqueue ring tests always; a live display-info probe only when a device is present. */
bool GfxRunSelfTests(void);

HZ_EXTERN_C_END

#endif /* GFX_H */
