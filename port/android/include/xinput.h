/* xinput.h on Android: XInput as the port reads controllers, fed by Android's
 * input events (src/android_input.c). XINPUT_STATE, XINPUT_GAMEPAD and the
 * button bits come from xboxrecomp's shim (platform/win32_compat.h). */
#pragma once
#include <windows.h>

#ifndef XUSER_MAX_COUNT
#define XUSER_MAX_COUNT 4
#endif
#define XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE  7849
#define XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE 8689
#define XINPUT_GAMEPAD_TRIGGER_THRESHOLD    30
#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167
#endif

typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XINPUT_VIBRATION;

#ifdef __cplusplus
extern "C" {
#endif
DWORD android_XInputGetState(DWORD user, XINPUT_STATE *s);
DWORD android_XInputSetState(DWORD user, XINPUT_VIBRATION *v);
#ifdef __cplusplus
}
#endif
#define XInputGetState android_XInputGetState
#define XInputSetState android_XInputSetState
#define XInputEnable(on) ((void)(on))
