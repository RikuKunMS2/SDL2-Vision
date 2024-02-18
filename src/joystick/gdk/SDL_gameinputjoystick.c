/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_GAMEINPUT

#include "../SDL_sysjoystick.h"
#include "../usb_ids.h"

#include <stdbool.h>
#define COBJMACROS
#include <GameInput.h>


typedef struct GAMEINPUT_InternalDevice
{
    IGameInputDevice *device;
    char path[(APP_LOCAL_DEVICE_ID_SIZE * 2) + 1];
    char *name;
    SDL_JoystickGUID guid;          /* generated by SDL */
    SDL_JoystickID device_instance; /* generated by SDL */
    const GameInputDeviceInfo *info;
    SDL_bool isAdded;
    SDL_bool isDeleteRequested;
} GAMEINPUT_InternalDevice;

typedef struct GAMEINPUT_InternalList
{
    GAMEINPUT_InternalDevice **devices;
    int count;
} GAMEINPUT_InternalList;

typedef struct joystick_hwdata
{
    GAMEINPUT_InternalDevice *devref;
    SDL_bool report_sensors;
    GameInputRumbleParams rumbleParams;
} GAMEINPUT_InternalJoystickHwdata;


static GAMEINPUT_InternalList g_GameInputList = { NULL };
static void *g_hGameInputDLL = NULL;
static IGameInput *g_pGameInput = NULL;
static GameInputCallbackToken g_GameInputCallbackToken = GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE;


static int GAMEINPUT_InternalAddOrFind(IGameInputDevice *pDevice)
{
    GAMEINPUT_InternalDevice **devicelist = NULL;
    GAMEINPUT_InternalDevice *elem = NULL;
    const GameInputDeviceInfo *info = NULL;
    Uint16 bus = SDL_HARDWARE_BUS_USB;
    Uint16 vendor = 0;
    Uint16 product = 0;
    Uint16 version = 0;
    const char *manufacturer_string = NULL;
    const char *product_string = NULL;
    char tmp[4];
    int idx = 0;

    info = IGameInputDevice_GetDeviceInfo(pDevice);
    if (info->capabilities & GameInputDeviceCapabilityWireless) {
        bus = SDL_HARDWARE_BUS_BLUETOOTH;
    } else {
        bus = SDL_HARDWARE_BUS_USB;
    }
    vendor = info->vendorId;
    product = info->productId;
    version = (info->firmwareVersion.major << 8) | info->firmwareVersion.minor;

    if (SDL_JoystickHandledByAnotherDriver(&SDL_GAMEINPUT_JoystickDriver, vendor, product, version, "")) {
        return 0;
    }

    for (idx = 0; idx < g_GameInputList.count; ++idx) {
        elem = g_GameInputList.devices[idx];
        if (elem && elem->device == pDevice) {
            /* we're already added */
            return 0;
        }
    }

    elem = (GAMEINPUT_InternalDevice *)SDL_calloc(1, sizeof(*elem));
    if (!elem) {
        return SDL_OutOfMemory();
    }

    devicelist = (GAMEINPUT_InternalDevice **)SDL_realloc(g_GameInputList.devices, sizeof(elem) * (g_GameInputList.count + 1LL));
    if (!devicelist) {
        SDL_free(elem);
        return SDL_OutOfMemory();
    }

    /* Generate a device path */
    for (idx = 0; idx < APP_LOCAL_DEVICE_ID_SIZE; ++idx) {
        SDL_snprintf(tmp, SDL_arraysize(tmp), "%02hhX", info->deviceId.value[idx]);
        SDL_strlcat(elem->path, tmp, SDL_arraysize(tmp));
    }

    if (info->deviceStrings) {
        /* In theory we could get the manufacturer and product strings here, but they're NULL for all the controllers I've tested */
    }

    if (info->displayName) {
        /* This could give us a product string, but it's NULL for all the controllers I've tested */
    }

    IGameInputDevice_AddRef(pDevice);
    elem->device = pDevice;
    elem->name = SDL_CreateJoystickName(vendor, product, manufacturer_string, product_string);
    elem->guid = SDL_CreateJoystickGUID(bus, vendor, product, version, manufacturer_string, product_string, 'g', 0);
    elem->device_instance = SDL_GetNextObjectID();
    elem->info = info;

    g_GameInputList.devices = devicelist;
    g_GameInputList.devices[g_GameInputList.count++] = elem;

    return 0;
}

static int GAMEINPUT_InternalRemoveByIndex(int idx)
{
    GAMEINPUT_InternalDevice **devicelist = NULL;
    GAMEINPUT_InternalDevice *elem;
    int bytes = 0;

    if (idx < 0 || idx >= g_GameInputList.count) {
        return SDL_SetError("GAMEINPUT_InternalRemoveByIndex argument idx %d is out of range", idx);
    }

    elem = g_GameInputList.devices[idx];
    if (elem) {
        IGameInputDevice_Release(elem->device);
        SDL_free(elem->name);
        SDL_free(elem);
    }
    g_GameInputList.devices[idx] = NULL;

    if (g_GameInputList.count == 1) {
        /* last element in the list, free the entire list then */
        SDL_free(g_GameInputList.devices);
        g_GameInputList.devices = NULL;
    } else {
        if (idx != g_GameInputList.count - 1) {
            bytes = sizeof(*devicelist) * (g_GameInputList.count - idx);
            SDL_memmove(&g_GameInputList.devices[idx], &g_GameInputList.devices[idx + 1], bytes);
        }
    }

    /* decrement the count and return */
    return g_GameInputList.count--;
}

static GAMEINPUT_InternalDevice *GAMEINPUT_InternalFindByIndex(int idx)
{
    /* We're guaranteed that the index is in range when this is called */
    return g_GameInputList.devices[idx];
}

static void CALLBACK GAMEINPUT_InternalJoystickDeviceCallback(
    _In_ GameInputCallbackToken callbackToken,
    _In_ void* context,
    _In_ IGameInputDevice* device,
    _In_ uint64_t timestamp,
    _In_ GameInputDeviceStatus currentStatus,
    _In_ GameInputDeviceStatus previousStatus)
{
    int idx = 0;
    GAMEINPUT_InternalDevice *elem = NULL;

    if (!device) {
        /* This should never happen, but ignore it if it does */
        return;
    }

    if (currentStatus & GameInputDeviceConnected) {
        GAMEINPUT_InternalAddOrFind(device);
    } else {
        for (idx = 0; idx < g_GameInputList.count; ++idx) {
            elem = g_GameInputList.devices[idx];
            if (elem && elem->device == device) {
                /* will be deleted on the next Detect call */
                elem->isDeleteRequested = SDL_TRUE;
                break;
            }
        }
    }
}

static void GAMEINPUT_JoystickDetect(void);

static int GAMEINPUT_JoystickInit(void)
{
    HRESULT hR;

    if (!g_hGameInputDLL) {
        g_hGameInputDLL = SDL_LoadObject("gameinput.dll");
        if (!g_hGameInputDLL) {
            return -1;
        }
    }

    if (!g_pGameInput) {
        typedef HRESULT (WINAPI *GameInputCreate_t)(IGameInput * *gameInput);
        GameInputCreate_t GameInputCreateFunc = (GameInputCreate_t)SDL_LoadFunction(g_hGameInputDLL, "GameInputCreate");
        if (!GameInputCreateFunc) {
            return -1;
        }

        hR = GameInputCreateFunc(&g_pGameInput);
        if (FAILED(hR)) {
            return SDL_SetError("GameInputCreate failure with HRESULT of %08X", hR);
        }
    }

    hR = IGameInput_RegisterDeviceCallback(g_pGameInput,
                                           NULL,
                                           GameInputKindGamepad,
                                           GameInputDeviceConnected,
                                           GameInputBlockingEnumeration,
                                           NULL,
                                           GAMEINPUT_InternalJoystickDeviceCallback,
                                           &g_GameInputCallbackToken);
    if (FAILED(hR)) {
        return SDL_SetError("IGameInput::RegisterDeviceCallback failure with HRESULT of %08X", hR);
    }

    GAMEINPUT_JoystickDetect();

    return 0;
}

static int GAMEINPUT_JoystickGetCount(void)
{
    return g_GameInputList.count;
}

static void GAMEINPUT_JoystickDetect(void)
{
    int idx = 0;
    GAMEINPUT_InternalDevice *elem = NULL;

    for (idx = 0; idx < g_GameInputList.count; ++idx) {
        elem = g_GameInputList.devices[idx];
        if (!elem) {
            continue;
        }

        if (!elem->isAdded) {
            SDL_PrivateJoystickAdded(elem->device_instance);
            elem->isAdded = SDL_TRUE;
        }

        if (elem->isDeleteRequested || !(IGameInputDevice_GetDeviceStatus(elem->device) & GameInputDeviceConnected)) {
            SDL_PrivateJoystickRemoved(elem->device_instance);
            GAMEINPUT_InternalRemoveByIndex(idx--);
        }
    }
}

static SDL_bool GAMEINPUT_JoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    int idx = 0;
    GAMEINPUT_InternalDevice *elem = NULL;

    if (vendor_id == USB_VENDOR_MICROSOFT &&
        product_id == USB_PRODUCT_XBOX_ONE_XBOXGIP_CONTROLLER) {
        /* The Xbox One controller shows up as a hardcoded raw input VID/PID, which we definitely handle */
        return SDL_TRUE;
    }

    for (idx = 0; idx < g_GameInputList.count; ++idx) {
        elem = g_GameInputList.devices[idx];
        if (elem && vendor_id == elem->info->vendorId && product_id == elem->info->productId) {
            return SDL_TRUE;
        }
    }
    return SDL_FALSE;
}

static const char *GAMEINPUT_JoystickGetDeviceName(int device_index)
{
    return GAMEINPUT_InternalFindByIndex(device_index)->name;
}

static const char *GAMEINPUT_JoystickGetDevicePath(int device_index)
{
    /* APP_LOCAL_DEVICE_ID as a hex string, since it's required for some association callbacks */
    return GAMEINPUT_InternalFindByIndex(device_index)->path;
}

static int GAMEINPUT_JoystickGetDeviceSteamVirtualGamepadSlot(int device_index)
{
    /* Steamworks API is not available in GDK */
    return -1;
}

static int GAMEINPUT_JoystickGetDevicePlayerIndex(int device_index)
{
    return -1;
}

static void GAMEINPUT_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
}

static SDL_JoystickGUID GAMEINPUT_JoystickGetDeviceGUID(int device_index)
{
    return GAMEINPUT_InternalFindByIndex(device_index)->guid;
}

static SDL_JoystickID GAMEINPUT_JoystickGetDeviceInstanceID(int device_index)
{
    return GAMEINPUT_InternalFindByIndex(device_index)->device_instance;
}

static SDL_JoystickPowerLevel GAMEINPUT_InternalGetPowerLevel(IGameInputDevice *device)
{
    GameInputBatteryState battery_state;

    SDL_zero(battery_state);
    IGameInputDevice_GetBatteryState(device, &battery_state);

    if (battery_state.status == GameInputBatteryDischarging) {
        /* FIXME: What are the units for remainingCapacity? */
    }
    return SDL_JOYSTICK_POWER_UNKNOWN;
}

static int GAMEINPUT_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    GAMEINPUT_InternalDevice *elem = GAMEINPUT_InternalFindByIndex(device_index);
    GAMEINPUT_InternalJoystickHwdata *hwdata = NULL;

    if (!elem) {
        return -1;
    }

    hwdata = (GAMEINPUT_InternalJoystickHwdata *)SDL_calloc(1, sizeof(*hwdata));
    if (!hwdata) {
        return SDL_OutOfMemory();
    }

    hwdata->devref = elem;

    joystick->hwdata = hwdata;
    joystick->naxes = 6;
    joystick->nbuttons = 11;
    joystick->nhats = 1;

    if (elem->info->supportedRumbleMotors & (GameInputRumbleLowFrequency | GameInputRumbleHighFrequency)) {
        SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, SDL_TRUE);
    }
    if (elem->info->supportedRumbleMotors & (GameInputRumbleLeftTrigger | GameInputRumbleRightTrigger)) {
        SDL_SetBooleanProperty(SDL_GetJoystickProperties(joystick), SDL_PROP_JOYSTICK_CAP_TRIGGER_RUMBLE_BOOLEAN, SDL_TRUE);
    }

    if (elem->info->supportedInput & GameInputKindTouch) {
        SDL_PrivateJoystickAddTouchpad(joystick, elem->info->touchPointCount);
    }

    if (elem->info->supportedInput & GameInputKindMotion) {
        /* FIXME: What's the sensor update rate? */
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 250.0f);
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 250.0f);
    }

    if (elem->info->capabilities & GameInputDeviceCapabilityWireless) {
        joystick->epowerlevel = GAMEINPUT_InternalGetPowerLevel(elem->device);
    } else {
        joystick->epowerlevel = SDL_JOYSTICK_POWER_WIRED;
    }
    return 0;
}

static int GAMEINPUT_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
    /* don't check for caps here, since SetRumbleState doesn't return any result - we don't need to check it */
    GAMEINPUT_InternalJoystickHwdata *hwdata = joystick->hwdata;
    GameInputRumbleParams *params = &hwdata->rumbleParams;
    params->lowFrequency = (float)low_frequency_rumble / (float)SDL_MAX_UINT16;
    params->highFrequency = (float)high_frequency_rumble / (float)SDL_MAX_UINT16;
    IGameInputDevice_SetRumbleState(hwdata->devref->device, params);
    return 0;
}

static int GAMEINPUT_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    /* don't check for caps here, since SetRumbleState doesn't return any result - we don't need to check it */
    GAMEINPUT_InternalJoystickHwdata *hwdata = joystick->hwdata;
    GameInputRumbleParams *params = &hwdata->rumbleParams;
    params->leftTrigger = (float)left_rumble / (float)SDL_MAX_UINT16;
    params->rightTrigger = (float)right_rumble / (float)SDL_MAX_UINT16;
    IGameInputDevice_SetRumbleState(hwdata->devref->device, params);
    return 0;
}

static int GAMEINPUT_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static int GAMEINPUT_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static int GAMEINPUT_JoystickSetSensorsEnabled(SDL_Joystick *joystick, SDL_bool enabled)
{
    joystick->hwdata->report_sensors = enabled;
    return 0;
}

static void GAMEINPUT_JoystickUpdate(SDL_Joystick *joystick)
{
    static WORD s_XInputButtons[] = {
        GameInputGamepadA,                  /* SDL_GAMEPAD_BUTTON_SOUTH */
        GameInputGamepadB,                  /* SDL_GAMEPAD_BUTTON_EAST */
        GameInputGamepadX,                  /* SDL_GAMEPAD_BUTTON_WEST */
        GameInputGamepadY,                  /* SDL_GAMEPAD_BUTTON_NORTH */
        GameInputGamepadView,               /* SDL_GAMEPAD_BUTTON_BACK */
        0, /* The guide button is not available */
        GameInputGamepadMenu,               /* SDL_GAMEPAD_BUTTON_START */
        GameInputGamepadLeftThumbstick,     /* SDL_GAMEPAD_BUTTON_LEFT_STICK */
        GameInputGamepadRightThumbstick,    /* SDL_GAMEPAD_BUTTON_RIGHT_STICK */
        GameInputGamepadLeftShoulder,       /* SDL_GAMEPAD_BUTTON_LEFT_SHOULDER */
        GameInputGamepadRightShoulder,      /* SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER */
    };
    Uint8 btnidx = 0, btnstate = 0, hat = 0;
    GAMEINPUT_InternalJoystickHwdata *hwdata = joystick->hwdata;
    IGameInputDevice *device = hwdata->devref->device;
    const GameInputDeviceInfo *info = hwdata->devref->info;
    IGameInputReading *reading = NULL;
    Uint64 timestamp = SDL_GetTicksNS();
    GameInputGamepadState state;
    HRESULT hR;


    hR = IGameInput_GetCurrentReading(g_pGameInput, GameInputKindGamepad, device, &reading);
    if (FAILED(hR)) {
        /* don't SetError here since there can be a legitimate case when there's no reading avail */
        return;
    }

    /* FIXME: See if we can get the delta between the reading timestamp and current time and apply the offset to timestamp */

    if (IGameInputReading_GetGamepadState(reading, &state)) {
        for (btnidx = 0; btnidx < SDL_arraysize(s_XInputButtons); ++btnidx) {
            btnstate = (state.buttons & s_XInputButtons[btnidx]) ? SDL_PRESSED : SDL_RELEASED;
            SDL_SendJoystickButton(timestamp, joystick, btnidx, btnstate);
        }

        if (state.buttons & GameInputGamepadDPadUp) {
            hat |= SDL_HAT_UP;
        }
        if (state.buttons & GameInputGamepadDPadDown) {
            hat |= SDL_HAT_DOWN;
        }
        if (state.buttons & GameInputGamepadDPadLeft) {
            hat |= SDL_HAT_LEFT;
        }
        if (state.buttons & GameInputGamepadDPadRight) {
            hat |= SDL_HAT_RIGHT;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);

#define CONVERT_AXIS(v) (Sint16)(((v) < 0.0f) ? ((v)*32768.0f) : ((v)*32767.0f))
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, CONVERT_AXIS(state.leftThumbstickX));
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, CONVERT_AXIS(-state.leftThumbstickY));
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, CONVERT_AXIS(state.rightThumbstickX));
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, CONVERT_AXIS(-state.rightThumbstickY));
#undef CONVERT_AXIS
#define CONVERT_TRIGGER(v) (Sint16)((v)*65535.0f - 32768.0f)
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, CONVERT_TRIGGER(state.leftTrigger));
        SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, CONVERT_TRIGGER(state.rightTrigger));
#undef CONVERT_TRIGGER
    }

    if (info->supportedInput & GameInputKindTouch) {
        GameInputTouchState *touch_state = SDL_stack_alloc(GameInputTouchState, info->touchPointCount);
        if (touch_state) {
            uint32_t i;
            uint32_t touch_count = IGameInputReading_GetTouchState(reading, info->touchPointCount, touch_state);
            for (i = 0; i < touch_count; ++i) {
                GameInputTouchState *touch = &touch_state[i];
                /* FIXME: We should use touch->touchId to track fingers instead of using i below */
                SDL_SendJoystickTouchpad(timestamp, joystick, 0, i, SDL_PRESSED, touch->positionX * info->touchSensorInfo[i].resolutionX, touch->positionY * info->touchSensorInfo[0].resolutionY, touch->pressure);
            }
            SDL_stack_free(touch_state);
        }
    }

    if (hwdata->report_sensors) {
        GameInputMotionState motion_state;

        if (IGameInputReading_GetMotionState(reading, &motion_state)) {
            /* FIXME: How do we interpret the motion data? */
        }
    }

    IGameInputReading_Release(reading);

    if (joystick->epowerlevel != SDL_JOYSTICK_POWER_WIRED) {
        /* FIXME: We can poll this at a much lower rate */
        SDL_SendJoystickBatteryLevel(joystick, GAMEINPUT_InternalGetPowerLevel(device));
    }
}

static void GAMEINPUT_JoystickClose(SDL_Joystick* joystick)
{
    SDL_free(joystick->hwdata);
    joystick->hwdata = NULL;
}

static void GAMEINPUT_JoystickQuit(void)
{
    if (g_pGameInput) {
        /* free the callback */
        IGameInput_UnregisterCallback(g_pGameInput, g_GameInputCallbackToken, /*timeoutInUs:*/ 10000);
        g_GameInputCallbackToken = GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE;

        /* free the list */
        while (g_GameInputList.count > 0) {
            GAMEINPUT_InternalRemoveByIndex(0);
        }

        IGameInput_Release(g_pGameInput);
        g_pGameInput = NULL;
    }

    if (g_hGameInputDLL) {
        SDL_UnloadObject(g_hGameInputDLL);
        g_hGameInputDLL = NULL;
    }
}

static SDL_bool GAMEINPUT_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    out->a.kind = EMappingKind_Button;
    out->a.target = SDL_GAMEPAD_BUTTON_SOUTH;

    out->b.kind = EMappingKind_Button;
    out->b.target = SDL_GAMEPAD_BUTTON_EAST;

    out->x.kind = EMappingKind_Button;
    out->x.target = SDL_GAMEPAD_BUTTON_WEST;

    out->y.kind = EMappingKind_Button;
    out->y.target = SDL_GAMEPAD_BUTTON_NORTH;

    out->back.kind = EMappingKind_Button;
    out->back.target = SDL_GAMEPAD_BUTTON_BACK;

    /* The guide button isn't available, so don't map it */

    out->start.kind = EMappingKind_Button;
    out->start.target = SDL_GAMEPAD_BUTTON_START;

    out->leftstick.kind = EMappingKind_Button;
    out->leftstick.target = SDL_GAMEPAD_BUTTON_LEFT_STICK;

    out->rightstick.kind = EMappingKind_Button;
    out->rightstick.target = SDL_GAMEPAD_BUTTON_RIGHT_STICK;

    out->leftshoulder.kind = EMappingKind_Button;
    out->leftshoulder.target = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;

    out->rightshoulder.kind = EMappingKind_Button;
    out->rightshoulder.target = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;

    out->dpup.kind = EMappingKind_Hat;
    out->dpup.target = SDL_HAT_UP;

    out->dpdown.kind = EMappingKind_Hat;
    out->dpdown.target = SDL_HAT_DOWN;

    out->dpleft.kind = EMappingKind_Hat;
    out->dpleft.target = SDL_HAT_LEFT;

    out->dpright.kind = EMappingKind_Hat;
    out->dpright.target = SDL_HAT_RIGHT;

    out->leftx.kind = EMappingKind_Axis;
    out->leftx.target = SDL_GAMEPAD_AXIS_LEFTX;

    out->lefty.kind = EMappingKind_Axis;
    out->lefty.target = SDL_GAMEPAD_AXIS_LEFTY;

    out->rightx.kind = EMappingKind_Axis;
    out->rightx.target = SDL_GAMEPAD_AXIS_RIGHTX;

    out->righty.kind = EMappingKind_Axis;
    out->righty.target = SDL_GAMEPAD_AXIS_RIGHTY;

    out->lefttrigger.kind = EMappingKind_Axis;
    out->lefttrigger.target = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;

    out->righttrigger.kind = EMappingKind_Axis;
    out->righttrigger.target = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;

    return SDL_TRUE;
}


SDL_JoystickDriver SDL_GAMEINPUT_JoystickDriver =
{
    GAMEINPUT_JoystickInit,
    GAMEINPUT_JoystickGetCount,
    GAMEINPUT_JoystickDetect,
    GAMEINPUT_JoystickIsDevicePresent,
    GAMEINPUT_JoystickGetDeviceName,
    GAMEINPUT_JoystickGetDevicePath,
    GAMEINPUT_JoystickGetDeviceSteamVirtualGamepadSlot,
    GAMEINPUT_JoystickGetDevicePlayerIndex,
    GAMEINPUT_JoystickSetDevicePlayerIndex,
    GAMEINPUT_JoystickGetDeviceGUID,
    GAMEINPUT_JoystickGetDeviceInstanceID,
    GAMEINPUT_JoystickOpen,
    GAMEINPUT_JoystickRumble,
    GAMEINPUT_JoystickRumbleTriggers,
    GAMEINPUT_JoystickSetLED,
    GAMEINPUT_JoystickSendEffect,
    GAMEINPUT_JoystickSetSensorsEnabled,
    GAMEINPUT_JoystickUpdate,
    GAMEINPUT_JoystickClose,
    GAMEINPUT_JoystickQuit,
    GAMEINPUT_JoystickGetGamepadMapping
};


#endif /* SDL_JOYSTICK_GAMEINPUT */
