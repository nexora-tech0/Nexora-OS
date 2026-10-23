/*
 * Nexora OS - user/lib/hzui/hzuip.h
 * hzui internals shared between its source files.
 */
#ifndef HZUIP_H
#define HZUIP_H

#include "hzui.h"

namespace hzui {

/* menu.cpp: a tooltip popup showing Text near the screen point (kept on screen). */
Window *CreateToolTip(const char *Text, Point Screen);

/* window.cpp */
Rect WorkArea();

constexpr uint64_t kAnimationTimer = 1;
constexpr uint64_t kToolTipTimer = 2;
constexpr uint32_t kToolTipDelayMs = 600;

}  // namespace hzui

#endif /* HZUIP_H */
