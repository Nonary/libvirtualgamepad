# libvirtualgamepad

`libvirtualgamepad` is Vibeshine's independently authored Windows virtual
gamepad driver and protocol. It uses Microsoft's in-box Virtual HID Framework
(VHF) from a UMDF2 source driver; it does not ship a custom kernel bus driver
and does not contain LizardByte/libvirtualhid source or assets.

## Design

```text
Vibeshine
  |  versioned, METHOD_BUFFERED control IOCTLs
  v
Vibeshine VHF UMDF source driver
  |  VhfUm.dll + Vhf.sys (inbox Windows components)
  v
HID child gamepad(s)
  |  HID input/output/feature reports
  v
Windows HID clients
```

The private source-device interface handles controller lifetime and normalized
input. VHF handles HID child enumeration. HID output reports are parsed in the
driver and returned to Vibeshine as bounded feedback events. A client closing
its control handle releases every controller it owns, so an application crash
cannot leave an owned virtual controller active.

The protocol lives in `include/libvirtualgamepad/protocol.h`. It is deliberately
small, fixed-size, versioned, and `METHOD_BUFFERED`; it is the only ABI shared
with Vibeshine.

## Profile contract

The target profile set is deliberately explicit:

| Profile | Contract |
| --- | --- |
| Generic HID | Standard HID Game Pad; implemented first. |
| Xbox 360 | Requires a separately validated XUSB-compatible path. VHF alone is HID, not XInput. |
| Xbox One | Requires a separately validated XUSB/GIP-compatible path. |
| Xbox Series | Requires a separately validated XUSB/GIP-compatible path. |
| DualShock 4 | Requires an independently captured and tested HID descriptor plus output/feature behavior. |
| DualSense | Requires an independently captured and tested HID descriptor plus output/feature behavior. |
| Switch Pro | Requires an independently captured and tested HID descriptor plus output/feature behavior. |

The repository must not label an HID descriptor as an Xbox, PlayStation, or
Nintendo device merely by changing a VID/PID. Each profile becomes available
only after its descriptor, input mapping, output mapping, application behavior,
and legal provenance have all been tested. This avoids falsely promising
ViGEm/XInput compatibility from a VHF HID child.

## Driver package and signing

Each architecture has its own production release package:

```text
driver/
  VibeshineVhfGamepad.inf
  VibeshineVhfGamepad.dll
  VibeshineVhfGamepad.cat
tools/
  VibeshineVhfGamepadDeviceSetup.exe
manifest.json
```

The release order is final architecture-matched INF/DLL -> `Inf2Cat` ->
SignPath-sign the generated catalog and the separately packaged setup tool ->
verify catalog membership and both signatures -> write the final manifest ->
publish. `tools/prepare-driver-package.ps1` creates an unsigned staging
directory and refuses to overwrite an existing one.
`tools/verify-driver-package.ps1` verifies the signed catalog against the final
INF/DLL, then verifies the setup tool's embedded signature, and only then
writes `manifest.json`. The tool is intentionally outside the catalog because
it is not a driver payload. Vibeshine must consume a pinned signed package and
must not rewrite either the catalog-bound DLL or the manifest-hashed setup tool
inside its MSI. Local test packages use a clearly separate local test
certificate.

For a local test build, `tools/build-driver.ps1` follows the same package model
as Vibeshine's virtual-display driver: it builds the UMDF DLL and the owned
root-device setup tool, generates the final INF/catalog, creates or reuses
`CN=Vibeshine VHF Gamepad Test` in `CurrentUser\My`, signs the catalog and the
setup tool, and exports the public certificate as
`driver/VibeshineVhfGamepad.cer`. The private key never enters the package.
Trust that public certificate explicitly on the test host before installation:

```powershell
.\tools\build-driver.ps1 -Platform x64
.\tools\trust-test-certificate.ps1 -PackageDir .\artifacts\vhf-gamepad-x64-Release-<DriverVer>
.\tools\verify-driver-package.ps1 -PackageDir .\artifacts\vhf-gamepad-x64-Release-<DriverVer> -Platform x64 -DriverVer <DriverVer> -SourceRevision <git-sha>
```

On a clean Git worktree, the build script derives `DriverVer` from the latest
commit date and revision count. While iterating on uncommitted source, pass an
explicit monotonic newer `-DriverVer` so Windows cannot select an older staged
driver package.

The trust command must run elevated and installs the public certificate in
`LocalMachine\Root` and `LocalMachine\TrustedPublisher`. It does not install
the driver or create a virtual controller. A local test host might also need
Windows test-signing configuration; the script deliberately does not alter that
machine-wide setting. Use `tools/build-driver.ps1 -SigningMode Release` for a
release staging package. That mode deliberately omits the `.cer` and leaves the
final `.cat` and setup tool unsigned for their separate SignPath requests.
Never publish the local-test certificate as part of a SignPath release archive.

The setup tool creates or removes only `ROOT\VIBESHINEVIRTUALGAMEPAD`; it does
not enumerate physical HID devices or manage third-party virtual-controller
packages. Installing the INF into the Driver Store is not enough for this
root-enumerated source driver—the setup tool creates the source node and waits
for its private control interface before reporting success.

When Windows asks for a restart during installation, the setup tool first tries
to activate the exact owned source node in the current session: it
re-enumerates it, requests a PnP property-change restart, and then performs one
owned-node disable/enable cycle only if the private interface is still absent.
It never restarts shared UMDF hosts or unrelated HID devices. A reboot is
reported only when that bounded recovery cannot reload the owned node and
prove both the source interface and the selected driver version are live.

## Build baseline

The UMDF driver targets x64 and ARM64 Windows 10 or newer, uses UMDF 2.15 or
newer, and links the inbox VHF user-mode import library (`VhfUm.lib`). It must
not redistribute `VhfUm.dll` or `Vhf.sys`.

The initial generic profile is intentionally a narrow proof of the VHF driver,
control protocol, controller cleanup, and generic HID feedback path. It
currently exposes 32 HID buttons, a hat switch, four signed axes, and two
triggers. Adding a new profile is a driver change with descriptor and
compatibility tests, not a configuration-only branding change.
