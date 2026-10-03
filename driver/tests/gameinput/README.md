# Native controller identity regression

Build the standalone profile regression without loading a driver:

```powershell
cmake -S driver/tests/gameinput -B build/identity-tests -G "Visual Studio 18 2026" -A x64
cmake --build build/identity-tests --config Release
ctest --test-dir build/identity-tests -C Release --output-on-failure
```

On Windows, an optional read-only inventory compares HID, DirectInput,
Windows.Gaming.Input, and GameInput IDs:

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
