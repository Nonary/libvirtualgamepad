# Install and test the native HID identity fix

Use a locally test-signed package built from the current upstream/PR merge.
For Vibepollo 2.0.0, the package must have protocol 2. The earlier direct-fork
`native-hid-pr-aca38f6-*` protocol-1 artifacts are unsuitable. See
[build validation](NATIVE_HID_BUILD_VALIDATION.md) for the corrected build.

These are manual test-host instructions. They change certificate trust and the
installed gamepad driver. No installation was performed during the investigation.

## Back up and prepare

Disconnect streaming sessions, exit Steam on the host, and open PowerShell as
administrator. Adapt these paths to your checkout and extracted test package:

```powershell
$ErrorActionPreference = 'Stop'
$testRepo = 'C:\work\libvirtualgamepad'
$testPackage = 'C:\driver-test-package'
$backupDir = 'C:\driver-test-backup'
$signTool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$rootId = (Get-PnpDevice -PresentOnly | Where-Object InstanceId -Like 'ROOT\VIBESHINEVIRTUALGAMEPAD\*' | Select-Object -First 1).InstanceId
if (-not $rootId) { throw 'Expected an existing Vibepollo gamepad root device' }
$originalInf = (Get-PnpDeviceProperty -InstanceId $rootId -KeyName DEVPKEY_Device_DriverInfPath).Data
$originalVersion = (Get-PnpDeviceProperty -InstanceId $rootId -KeyName DEVPKEY_Device_DriverVersion).Data
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
pnputil /export-driver $originalInf $backupDir
if ($LASTEXITCODE -ne 0) { throw 'Original driver backup failed' }
[ordered]@{ root_id=$rootId; inf=$originalInf; version=$originalVersion } |
    ConvertTo-Json | Set-Content "$backupDir\original-driver.json"
```

The affected machine originally used `oem63.inf`, version `0.1.0.39`, and the
service `ApolloService`. Use the corresponding service name on another host.
Do not replace that host's original INF name with this machine's example.

## Trust and verify the package

```powershell
& "$testRepo\tools\trust-test-certificate.ps1" -PackageDir $testPackage
& $signTool verify /pa "$testPackage\driver\VibeshineVhfGamepad.cat"
if ($LASTEXITCODE -ne 0) { throw 'Catalog trust check failed' }
foreach ($payload in @('VibeshineVhfGamepad.inf', 'VibeshineVhfGamepad.dll')) {
    & $signTool verify /pa /c "$testPackage\driver\VibeshineVhfGamepad.cat" "$testPackage\driver\$payload"
    if ($LASTEXITCODE -ne 0) { throw "Catalog binding failed: $payload" }
}
& $signTool verify /pa "$testPackage\tools\VibeshineVhfGamepadDeviceSetup.exe"
if ($LASTEXITCODE -ne 0) { throw 'Setup executable trust check failed' }
```

The trust script validates that the catalog and setup executable match the
bundled public certificate, then imports it into LocalMachine Root and
TrustedPublisher. It supports `-Remove` for cleanup. These trust imports were
not performed during build/signing validation.

## Install and confirm the selected version

For this investigation, the test package DriverVer is
`10/03/2026,0.1.0.40`. Use the version actually recorded in your package manifest:

```powershell
$manifest = Get-Content "$testPackage\manifest.json" -Raw | ConvertFrom-Json
if ($manifest.protocol_version -ne 2) { throw 'This Vibepollo client requires protocol 2' }
$expectedVersion = ($manifest.driver_ver -split ',')[1]
Stop-Service ApolloService
& "$testPackage\tools\VibeshineVhfGamepadDeviceSetup.exe" install --inf "$testPackage\driver\VibeshineVhfGamepad.inf"
$installResult = $LASTEXITCODE
if ($installResult -notin @(0, 3010)) { throw "Driver installation failed: $installResult" }
```

Exit 3010 requires a reboot. After reboot, restore the path variables and root
instance ID before continuing. Exit 0 permits immediate verification:

```powershell
$original = Get-Content "$backupDir\original-driver.json" -Raw | ConvertFrom-Json
$rootId = $original.root_id
$originalInf = $original.inf
$manifest = Get-Content "$testPackage\manifest.json" -Raw | ConvertFrom-Json
$expectedVersion = ($manifest.driver_ver -split ',')[1]
& "$testPackage\tools\VibeshineVhfGamepadDeviceSetup.exe" status
$selectedVersion = (Get-PnpDeviceProperty -InstanceId $rootId -KeyName DEVPKEY_Device_DriverVersion).Data
$testInf = (Get-PnpDeviceProperty -InstanceId $rootId -KeyName DEVPKEY_Device_DriverInfPath).Data
if ($selectedVersion -ne $expectedVersion -or $testInf -eq $originalInf) {
    throw 'The test driver has not been selected; stop before testing'
}
[ordered]@{ root_id=$rootId; inf=$testInf; version=$selectedVersion } |
    ConvertTo-Json | Set-Content "$backupDir\test-driver.json"
Start-Service ApolloService
```

Expect one root device, `source_interface_ready:true`, and `problem_code:0`.
These steps do not change Secure Boot, Memory Integrity, or Test Mode. If
Windows rejects installation/loading, retain the actual error and logs before
deciding whether any boot-policy change is required.

## Live verification

Start Steam and reconnect the Deck with its DualSense paired. Run:

```powershell
& "$testRepo\build\identity-tests\Release\probe_controller_identity.exe" |
    Tee-Object "$backupDir\identity-after-install.txt"
```

Check these separately:

1. GameInput reports DualSense as `054c:0ce6`, matching HID, DirectInput, and
   WGI; the former `0000:0000` entry is gone.
2. Steam exposes one DualSense. Buttons and sticks produce one action per
   input; separate controllers retain their intended player slots.
3. The Deck's virtual Xbox still works. Windows should have one native Xbox
   and one OS XInput slot for it when no other XInput devices are attached.
   Steam's GameInput/XInput Xbox pair may remain: the production driver patch
   does not claim to fix that separate SDL/Steam behavior.
4. Five stream disconnect/reconnect cycles do not accumulate controllers.
   Transient controller children disappear on disconnect; the source root
   normally remains installed.
5. Restart Steam and reboot the host, then repeat the inventory and input test.

For Xbox timing investigation, run each of these separately before connecting
and reconnecting the client during the 60-second inventory:

```powershell
python "$testRepo\driver\tests\gameinput\probe_sdl_identity.py" 'C:\Program Files (x86)\Steam\SDL3.dll' --gameinput 1 --gameinput-raw 0 --samples 60
python "$testRepo\driver\tests\gameinput\probe_sdl_identity.py" 'C:\Program Files (x86)\Steam\SDL3.dll' --gameinput 0 --gameinput-raw 1 --samples 60
```

The hints affect only the diagnostic process, not Steam's configuration. The
diagnostic README explains the installed SDL bug and remaining hypotheses.

Useful logs are Steam's `logs/controller.txt`, Windows
`C:\Windows\INF\setupapi.dev.log`, and the CodeIntegrity and
DriverFrameworks-UserMode Operational event logs, where available.

## Roll back

Disconnect the stream, exit Steam, and open an elevated shell. Set the path
variables again, then verify that the selected package is exactly the test
package recorded above before removing it:

```powershell
$original = Get-Content "$backupDir\original-driver.json" -Raw | ConvertFrom-Json
$test = Get-Content "$backupDir\test-driver.json" -Raw | ConvertFrom-Json
$selectedInf = (Get-PnpDeviceProperty -InstanceId $test.root_id -KeyName DEVPKEY_Device_DriverInfPath).Data
$selectedVersion = (Get-PnpDeviceProperty -InstanceId $test.root_id -KeyName DEVPKEY_Device_DriverVersion).Data
if ($test.inf -notmatch '^oem\d+\.inf$' -or $test.inf -eq $original.inf -or
    $selectedInf -ne $test.inf -or $selectedVersion -ne $test.version) {
    throw 'Selected driver no longer matches the recorded test package'
}
Stop-Service ApolloService
pnputil /delete-driver $test.inf /uninstall
if ($LASTEXITCODE -notin @(0, 3010)) { throw 'Removing the test package failed' }
$originalPackageInf = 'C:\Program Files\Apollo\drivers\vhf-gamepad\driver\VibeshineVhfGamepad.inf'
& "$testPackage\tools\VibeshineVhfGamepadDeviceSetup.exe" install --inf $originalPackageInf
if ($LASTEXITCODE -notin @(0, 3010)) { throw 'Restoring the original driver failed' }
```

Use the matching INF from the exported backup if the production package's
original path is unavailable. Reboot if requested, confirm the original driver
version is selected, then start the service. After restoring the original driver:

```powershell
& "$testRepo\tools\trust-test-certificate.ps1" -PackageDir $testPackage -Remove
```
