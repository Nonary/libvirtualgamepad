#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inventory GameInput/XInput through an explicitly selected SDL3 DLL.

Hints apply only to this process. HIDAPI, RawInput, DirectInput, and WGI are
disabled to isolate these two backends. No joystick is opened, no rumble or
output API is called, and Steam's configuration is not modified.
"""
import argparse
import ctypes
from pathlib import Path
import time


class GUID(ctypes.Structure):
    _fields_ = [("data", ctypes.c_ubyte * 16)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path, help="absolute path to the SDL3 DLL to inspect")
    parser.add_argument("--gameinput", choices=("default", "0", "1"), default="default")
    parser.add_argument("--gameinput-raw", choices=("default", "0", "1"), default="0")
    parser.add_argument("--samples", type=int, default=3, help="number of one-second inventory samples")
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("--samples must be positive")
    library = args.library.resolve(strict=True)
    # Resolve dependencies only in the supplied DLL's directory and System32.
    sdl = ctypes.CDLL(str(library), winmode=0x100 | 0x800)
    signatures = {
        "SDL_GetVersion": (ctypes.c_int, []),
        "SDL_GetRevision": (ctypes.c_char_p, []),
        "SDL_SetHintWithPriority": (ctypes.c_bool, [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]),
        "SDL_GetHint": (ctypes.c_char_p, [ctypes.c_char_p]),
        "SDL_Init": (ctypes.c_bool, [ctypes.c_uint32]),
        "SDL_GetError": (ctypes.c_char_p, []),
        "SDL_GetJoysticks": (ctypes.POINTER(ctypes.c_uint32), [ctypes.POINTER(ctypes.c_int)]),
        "SDL_GetJoystickNameForID": (ctypes.c_char_p, [ctypes.c_uint32]),
        "SDL_GetJoystickPathForID": (ctypes.c_char_p, [ctypes.c_uint32]),
        "SDL_GetJoystickGUIDForID": (GUID, [ctypes.c_uint32]),
        "SDL_GetJoystickVendorForID": (ctypes.c_uint16, [ctypes.c_uint32]),
        "SDL_GetJoystickProductForID": (ctypes.c_uint16, [ctypes.c_uint32]),
        "SDL_GetJoystickProductVersionForID": (ctypes.c_uint16, [ctypes.c_uint32]),
        "SDL_UpdateJoysticks": (None, []),
        "SDL_free": (None, [ctypes.c_void_p]),
        "SDL_Quit": (None, []),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(sdl, name)
        function.restype = result
        function.argtypes = arguments
    print(f"Library: {library}", flush=True)
    print(f"SDL version: {sdl.SDL_GetVersion()}; revision: {sdl.SDL_GetRevision().decode()}", flush=True)
    hints = {
        "SDL_JOYSTICK_HIDAPI": "0",
        "SDL_JOYSTICK_RAWINPUT": "0",
        "SDL_JOYSTICK_DIRECTINPUT": "0",
        "SDL_JOYSTICK_WGI": "0",
        "SDL_XINPUT_ENABLED": "1",
    }
    for hint, value in (("SDL_JOYSTICK_GAMEINPUT", args.gameinput),
                        ("SDL_JOYSTICK_GAMEINPUT_RAW", args.gameinput_raw)):
        if value != "default":
            hints[hint] = value
    for hint, value in hints.items():
        if not sdl.SDL_SetHintWithPriority(hint.encode(), value.encode(), 2):
            raise RuntimeError(f"Failed to set process-local hint {hint}")
    for hint in ("SDL_JOYSTICK_GAMEINPUT", "SDL_JOYSTICK_GAMEINPUT_RAW"):
        print(f"{hint}={sdl.SDL_GetHint(hint.encode())!r} (None means library default)", flush=True)
    if not sdl.SDL_Init(0x00000200):  # SDL_INIT_JOYSTICK
        raise RuntimeError(sdl.SDL_GetError().decode())
    try:
        for sample in range(args.samples):
            time.sleep(1)
            sdl.SDL_UpdateJoysticks()
            count = ctypes.c_int()
            devices = sdl.SDL_GetJoysticks(ctypes.byref(count))
            if not devices:
                raise RuntimeError(sdl.SDL_GetError().decode())
            try:
                print(f"Sample {sample + 1}: {count.value} joysticks", flush=True)
                for index in range(count.value):
                    device = devices[index]
                    guid = bytes(sdl.SDL_GetJoystickGUIDForID(device).data).hex()
                    print(f"  id={device} name={sdl.SDL_GetJoystickNameForID(device)!r} "
                          f"VID/PID={sdl.SDL_GetJoystickVendorForID(device):04x}:"
                          f"{sdl.SDL_GetJoystickProductForID(device):04x} "
                          f"version={sdl.SDL_GetJoystickProductVersionForID(device):04x} "
                          f"guid={guid} path={sdl.SDL_GetJoystickPathForID(device)!r}", flush=True)
            finally:
                sdl.SDL_free(devices)
    finally:
        sdl.SDL_Quit()


if __name__ == "__main__":
    main()
