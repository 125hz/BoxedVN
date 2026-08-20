/*
 * Minimal XInput facade for an x86-64 guest running without a host controller
 * bridge. Keep the loader contract intact and report no connected device.
 *
 * Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 */

#include <windows.h>
#include <xinput.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

void WINAPI XInputEnable(BOOL enable)
{
    (void)enable;
}

DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE *state)
{
    (void)index;
    if (!state) return ERROR_BAD_ARGUMENTS;
    ZeroMemory(state, sizeof(*state));
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetStateEx(DWORD index, void *state)
{
    (void)index;
    if (!state) return ERROR_BAD_ARGUMENTS;
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    (void)index;
    if (!vibration) return ERROR_BAD_ARGUMENTS;
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags,
                                   XINPUT_CAPABILITIES *capabilities)
{
    (void)index;
    (void)flags;
    if (!capabilities) return ERROR_BAD_ARGUMENTS;
    ZeroMemory(capabilities, sizeof(*capabilities));
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilitiesEx(DWORD reserved, DWORD index, DWORD flags,
                                     void *capabilities)
{
    (void)reserved;
    (void)index;
    (void)flags;
    if (!capabilities) return ERROR_BAD_ARGUMENTS;
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE device_type,
                                         XINPUT_BATTERY_INFORMATION *battery)
{
    (void)index;
    (void)device_type;
    if (!battery) return ERROR_BAD_ARGUMENTS;
    ZeroMemory(battery, sizeof(*battery));
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved,
                                PXINPUT_KEYSTROKE keystroke)
{
    (void)index;
    (void)reserved;
    if (!keystroke) return ERROR_BAD_ARGUMENTS;
    ZeroMemory(keystroke, sizeof(*keystroke));
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetAudioDeviceIds(DWORD index, LPWSTR render_id,
                                     UINT *render_count, LPWSTR capture_id,
                                     UINT *capture_count)
{
    (void)index;
    (void)render_id;
    (void)capture_id;
    if (render_count) *render_count = 0;
    if (capture_count) *capture_count = 0;
    return ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render_guid,
                                             GUID *capture_guid)
{
    (void)index;
    if (!render_guid || !capture_guid) return ERROR_BAD_ARGUMENTS;
    ZeroMemory(render_guid, sizeof(*render_guid));
    ZeroMemory(capture_guid, sizeof(*capture_guid));
    return ERROR_DEVICE_NOT_CONNECTED;
}
