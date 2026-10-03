# Native HID identity build validation

This change gives native DualShock 4, DualSense, and Switch Pro profiles explicit
PnP VID/PID hardware IDs. Xbox profiles are unchanged. The API and Steam evidence
for the separate Xbox duplicate is in
[the diagnostic README](../driver/tests/gameinput/README.md).

## Build the current upstream merge

Build the PR merged with current upstream, rather than an older fork checkout.
The affected Vibepollo 2.0.0 installation uses protocol 2 and driver 0.1.0.39.
The older fork ancestor uses protocol 1; artifacts made from that ancestor cannot
be used with the installed protocol-2 client. Check `k_protocol_version` in
`include/libvirtualgamepad/protocol.h` and the package manifest against the
consumer before installation.

The initial `native-hid-pr-aca38f6-*` local artifacts were built from the older
fork and are superseded. The corrected test package was built from merge
`c4120b46afc06d000d1e8bcab5095ef8296d17c5`, whose parents are upstream
`4b56fb9da177f320fb2d7ddb1b6262e5d55d2750` and PR commit
`aca38f6dee5acb8a23be32ba87b0941f204f02ef`. Its manifest records protocol 2
and DriverVer `10/03/2026,0.1.0.40`.

## Verified on 2026-10-03

- x64 Release UMDF clean build: zero warnings and zero errors.
- Root-device setup executable builds.
- DLL exports `FxDriverEntryUm` and imports `VhfUm.DLL`.
- All three existing driver contract/descriptor tests pass.
- Standalone native identity regression passes for all five profiles.
- Read-only Windows identity inventory builds.
- WDK `InfVerif /w /v` validates the INF.
- `Inf2Cat` generates a catalog with no errors or warnings.
- Both local catalog and setup executable CMS signatures pass cryptographic
  verification; all signed manifest hashes match.

The environment was Visual Studio Community 2026 18.10.3, MSVC 14.51.36231,
Spectre libraries, Windows SDK 26100.8249, and WDK x64 NuGet 26100.6584.
The Visual Studio WDK integration was present but the machine-wide WDK payload
was absent. Microsoft's WDK NuGet package was unpacked locally; no installed
Visual Studio or SDK files were modified.

This is local build and signing evidence. Certificate trust, full Windows policy
verification, driver installation, GameInput identity after installation, Steam
deduplication, and live reconnect behavior have not yet been verified.

## Reproduce the local WDK build

On a fully installed matching SDK/WDK, use the repository's
`tools/build-driver.ps1`. When supplying the WDK through NuGet, its `c` directory
contains the headers, framework libraries, and build tools. See Microsoft's
[WDK NuGet instructions](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget).

The following overrides reproduced this local build. Replace the WDK path and
MSBuild path with the installed locations:

```powershell
$ErrorActionPreference = 'Stop'
$driverMsbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$wdkRoot = 'C:\build-tools\wdk-26100.6584\c\'
$driverArgs = @(
    'driver/VibeshineVhfGamepad.vcxproj',
    '/p:Configuration=Release',
    '/p:Platform=x64',
    "/p:WDKContentRoot=$wdkRoot",
    '/p:VisualStudioVersion=17.0'
)
$libraryPath = (& $driverMsbuild @driverArgs '-getProperty:LibraryPath').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Library path evaluation failed' }
$libraryOverride = '/p:LibraryPath=' + (
    "$($wdkRoot)Lib\10.0.26100.0\um\x64;" + $libraryPath
).Replace(';', '%3B')
& $driverMsbuild @driverArgs $libraryOverride /t:Rebuild /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Driver rebuild failed' }
& $driverMsbuild tools/device_setup/VibeshineVhfGamepadDeviceSetup.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Setup executable build failed' }
```

`VisualStudioVersion=17.0` selects the task assembly shipped in that WDK
package; prepending its UM library directory makes `VhfUm.lib` available while
retaining the installed SDK libraries. These are local compatibility overrides,
not a claim that this combination has release qualification.

## Reproduce the regressions

```powershell
cmake -S driver/tests -B build/descriptor-tests -G "Visual Studio 18 2026" -A x64
cmake --build build/descriptor-tests --config Release --target test_ds4_usb test_ds5_usb test_pid_descriptor --parallel 4
ctest --test-dir build/descriptor-tests -C Release --output-on-failure
cmake -S driver/tests/gameinput -B build/identity-tests -G "Visual Studio 18 2026" -A x64
cmake --build build/identity-tests --config Release --target test_profile_identity probe_controller_identity --parallel 4
ctest --test-dir build/identity-tests -C Release --output-on-failure
```

The `probe_ds5_usb` path filter accepts both the original generic VHF identity
and the explicit `vid_054c&pid_0ce6` identity. That probe creates a controller on
the installed driver; it is not run as part of these regression tests. The
separate `probe_controller_identity` inventory is read-only.

## Prepare a local test package

Use a new empty output directory and a DriverVer appropriate for the installed
driver. The version below is the value used in this investigation, not a new
upstream release. Set `$wdkRoot` as in the build command:

```powershell
$testPackage = 'C:\driver-test-package'
./tools/prepare-driver-package.ps1 -Platform x64 -Configuration Release `
    -DriverVer '10/03/2026,0.1.0.40' -OutputDir $testPackage `
    -SigningMode LocalTest `
    -InfVerifPath "$($wdkRoot)tools\10.0.26100.0\x64\infverif.exe" `
    -Inf2CatPath "$($wdkRoot)bin\10.0.26100.0\x86\Inf2Cat.exe" `
    -SignToolPath 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
```

The script signs the catalog and setup executable and bundles the public
certificate. In this investigation an explicitly selected non-exportable local
key was used via `-SigningThumbprint`; no private key is included in the package.
Never publish the private key or distribute a test package as a production
release. Follow [installation and rollback](NATIVE_HID_TESTING.md) separately.
