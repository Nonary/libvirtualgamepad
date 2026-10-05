# Native controller identity regression

See [build validation](../../../docs/NATIVE_HID_BUILD_VALIDATION.md) and
[installation, live testing, and rollback](../../../docs/NATIVE_HID_TESTING.md).
Build with current upstream's protocol version before installing alongside
Vibepollo; older fork artifacts may not match the consumer's protocol.

Build the standalone profile regression without loading a driver:

The parent `driver/tests` CMake project also includes this regression, so the
existing controller protocol CI runs it alongside the descriptor tests.

```powershell
cmake -S driver/tests/gameinput -B build/identity-tests -G "Visual Studio 18 2026" -A x64
cmake --build build/identity-tests --config Release
ctest --test-dir build/identity-tests -C Release --output-on-failure
```

On Windows, an optional read-only inventory compares HID, DirectInput,
Windows.Gaming.Input, and GameInput IDs, plus connected OS XInput slots:

```powershell
cmake --build build/identity-tests --config Release --target probe_controller_identity
.\build\identity-tests\Release\probe_controller_identity.exe
```

The inventory needs Windows SDK headers for GameInput and C++/WinRT. It does
not create controllers, send input/output reports, install drivers, or change
the existing devices. Run it while the controller is connected.

The affected DualSense reports `054c:0ce6` through HID, DirectInput, and
Windows.Gaming.Input, but `0000:0000` through GameInput. After rebuilding and
installing a driver with explicit native HID PnP IDs, verify that GameInput
also reports `054c:0ce6` and Steam/SDL expose only one controller. Repeat the
test across session reconnects; a profile regression cannot prove device
removal or application deduplication.

## Xbox GameInput/XInput duplication

With a Steam Deck and paired DualSense, the expected host inventory is one
virtual Xbox plus one virtual DualSense. Two PnP nodes for the Xbox (VHF source
and HID child) are one device stack; Steam Input's reserved slots are also not
evidence of additional driver controllers.

The affected machine's installed driver exposes one native Xbox Series entry
through GameInput (`045e:0b12`) and one connected OS XInput slot (slot 0).
Steam nevertheless logged two Xbox entries with these exact SDL GUIDs:

| Backend | GUID | VID/PID | SDL version field |
| --- | --- | --- | --- |
| GameInput | `03005e485e040000120b000000006701` | `045e:0b12` | `0000` |
| XInput | `0300fa675e040000120b000009057801` | `045e:0b12` | `0509` |

This differs from the native DualSense's missing GameInput VID/PID. The Xbox
already publishes VID/PID and `IG_00` PnP IDs and has `xinputhid` attached. The
production patch here does not change its profile. SDL intentionally leaves
the GameInput GUID's version field at zero; the differing version alone does
not defeat its GameInput presence check, which matches VID/PID.

An optional Python 3 probe loads an explicitly selected SDL3 DLL in a separate
process and inventories only GameInput and XInput. It disables HIDAPI,
RawInput, DirectInput, and WGI; therefore the total count is **not** the full
controller inventory, especially when GameInput is disabled. No joystick is
opened and no rumble/output API is called. Hints affect only the probe process.

```powershell
python driver/tests/gameinput/probe_sdl_identity.py "C:\Program Files (x86)\Steam\SDL3.dll" --gameinput 1
python driver/tests/gameinput/probe_sdl_identity.py "C:\Program Files (x86)\Steam\SDL3.dll" --gameinput 0 --gameinput-raw 0
python driver/tests/gameinput/probe_sdl_identity.py "C:\Program Files (x86)\Steam\SDL3.dll" --gameinput 0 --gameinput-raw 1
python driver/tests/gameinput/probe_sdl_identity.py "C:\Program Files (x86)\Steam\SDL3.dll" --gameinput default --gameinput-raw default
```

On 2026-10-03, the affected installation's DLL reported SDL `3005000` (3.5.0),
revision `SDL-release-3.4.0-1359-g70e9cc86d`. With controllers already connected,
each of the four cases above exposed **one Xbox**, stable across three samples:

| Controller hint | Raw GameInput hint | Xbox backend | Other entry |
| --- | --- | --- | --- |
| 1 | 0 | GameInput | unidentified DualSense via GameInput |
| 0 | 0 | XInput | none (other backends disabled) |
| 0 | 1 | GameInput | unidentified DualSense via GameInput |
| library default | library default | GameInput | unidentified DualSense via GameInput |

The `0/1` case proves this DLL still admits ordinary gamepads through raw
GameInput despite disabling GameInput controller handling. SDL subsequently
fixed that behavior in
[c4cfb739ae66dd256ea1c16273a93994a51225db](https://github.com/libsdl-org/SDL/commit/c4cfb739ae66dd256ea1c16273a93994a51225db)
(2026-09-21), after the installed revision. This is a relevant fix for Steam
to incorporate, but the cold-start probe did **not** reproduce the Xbox pair,
so it is not yet verified as the complete solution.

A separate late-arrival path warrants testing in
[the installed SDL revision's XInput backend](https://github.com/libsdl-org/SDL/blob/70e9cc86ddbe0f6b580c3c02d1391b3dde4d11fb/src/joystick/windows/SDL_xinputjoystick.c):
`AddXInputDevice` retains an existing slot before checking whether a higher
priority backend now handles it. If XInput discovers the Xbox before a raw
GameInput callback admits it, subsequent detection can preserve both entries.
This is a source-based hypothesis; Steam's actual hints and callback timing
have not been captured. The Xbox's lower SDL instance ID in the Steam log is
consistent with earlier XInput allocation, but is not proof of this race.

For a controlled follow-up, start the probe before connecting a streaming
session and add `--samples 60`. Test initial connection and repeated reconnects
with the `0/1` and `1/0` settings, then compare an SDL build containing the fix
above. If duplicate entries survive, revalidate higher-priority ownership before
retaining an existing XInput entry and test removal of the old entry. Any such
change belongs in SDL/Steam and must preserve multiple real Xbox controllers.
Do not remove the driver's `IG_00` identity to hide this symptom: that identity
enables its intended XInput compatibility.
