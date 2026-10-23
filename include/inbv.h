/*
 * Nexora OS - include/inbv.h
 * Boot display (Inbv*): the boot splash, the on-screen boot log and the stop screen, drawn
 * through the boot video driver.
 */
#ifndef INBV_H
#define INBV_H

#include "hzbase.h"
#include "ke.h"

HZ_EXTERN_C_BEGIN

/* Sets up the boot video driver from KeLoaderBlock and draws the splash. */
bool InbvInitialize(void);
bool InbvIsDisplayReady(void);

/* The boot log remains available to serial output; the minimalist splash has no text. */
void InbvDisplayString(const char *Text);
void InbvEnableDisplayString(bool Enable);
/* Advances the framebuffer boot animation until the desktop takes over. */
void InbvAnimate(void);
/* The desktop (win32k) owns the screen from now on: the splash, boot log and status line stop
   drawing (the serial log continues). Stop screens still draw directly. */
void InbvHandOffDisplay(void);

/* Kept for boot callers; the minimalist splash has no status text. */
void InbvSetStatusText(const char *Text);

typedef struct _STOP_SCREEN_INFO {
    uint32_t Code;
    const char *CodeName;
    uint64_t Parameters[4];
    const char *Message; /* optional */
    const char *File;    /* optional */
    uint32_t Line;
    const KTRAP_FRAME *Frame; /* optional: registers at the trap */
    const char *ThreadName;   /* optional: thread running when the system stopped */
    uint32_t ThreadId;
    KIRQL Irql;               /* IRQL when the bug check was raised */
    uint64_t Cr2;
    uint64_t Cr3;
    const uint64_t *Frames; /* return addresses, innermost first */
    uint32_t FrameCount;
} STOP_SCREEN_INFO;

/* Paints the full-screen stop screen. Safe to call from a bug check. */
void InbvDrawStopScreen(const STOP_SCREEN_INFO *Info);

HZ_EXTERN_C_END

#endif /* INBV_H */
