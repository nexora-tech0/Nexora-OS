/* Shared input event injection for PS/2, USB HID, and future input drivers. */
#ifndef INPUT_H
#define INPUT_H

#include "uapi/hzapi.h"

HZ_EXTERN_C_BEGIN

void InputSubmitKeyEvent(const HZ_KEY_EVENT *Event);
void InputSubmitMouseEvent(const HZ_MOUSE_EVENT *Event);

HZ_EXTERN_C_END

#endif
