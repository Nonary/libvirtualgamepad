[CmdletBinding()]
param()

# Capture-SteamController.ps1 runs an elevated Ctrl+C helper against USBPcapCMD. It must be
# passed inline (-EncodedCommand with the target PID substituted into the helper text), never
# through a file in %TEMP% that a same-user non-elevated process could replace between write
# and run (D23-01). This pins the helper's shape; the capture itself needs hardware.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script = Join-Path $PSScriptRoot '..\capture\Capture-SteamController.ps1'
$text = Get-Content -LiteralPath $script -Raw
$failures = 0
function Check([bool] $ok, [string] $name) {
    if ($ok) { Write-Host "PASS: $name" } else { Write-Host "FAIL: $name"; $script:failures++ }
}

$helperStart = $text.IndexOf("`$script:CtrlCHelper = @'")
$helperEnd = $text.IndexOf("'@", $helperStart + 1)
Check ($helperStart -ge 0 -and $helperEnd -gt $helperStart) 'the Ctrl+C helper is a single-quoted here-string (no expansion of its text)'
$helper = $text.Substring($helperStart, $helperEnd - $helperStart)
Check ($helper -match '__TARGET_PID__') 'the helper takes its target PID through a placeholder'
Check ($helper -notmatch 'param\(') 'the helper has no parameter block (it is not run as a file)'

$fnStart = $text.IndexOf('function Stop-ConsoleProcess')
$fnEnd = $text.IndexOf('function ', $fnStart + 1)
Check ($fnStart -ge 0 -and $fnEnd -gt $fnStart) 'Stop-ConsoleProcess is present'
$fn = $text.Substring($fnStart, $fnEnd - $fnStart)
Check ($fn -match "-replace '__TARGET_PID__', \(\[string\] \`$Process\.Id\)") 'the PID is substituted as a plain string'
Check ($fn -match '-EncodedCommand') 'the helper runs from -EncodedCommand'
Check ($fn -notmatch "'-File'" -and $fn -notmatch 'Set-Content' -and $fn -notmatch 'Bypass' -and $fn -notmatch '\$env:TEMP') 'no helper file is written to a temp directory'

# The substituted helper must still be valid PowerShell with the PID in place.
$sample = ($helper -replace '__TARGET_PID__', '4242')
$sample = $sample.Substring($sample.IndexOf("@'") + 2)
$tokens = $null; $errors = $null
[void][System.Management.Automation.Language.Parser]::ParseInput($sample, [ref]$tokens, [ref]$errors)
Check ($errors.Count -eq 0 -and $sample -match "\`$TargetPid = \[int\] '4242'") 'the substituted helper parses and carries the PID'

# The raw hub captures (D25-05): random directory, never adopted, restricted ACL, and only
# created by live captures (D26-02: a -FromPcap reprocess runs unelevated and used to leave an
# empty Administrators-only directory in %TEMP%).
$tmpStart = $text.IndexOf('$tmp = Join-Path $env:TEMP')
$tmpBlock = if ($tmpStart -ge 0) { $text.Substring($tmpStart, [Math]::Min(600, $text.Length - $tmpStart)) } else { '' }
Check ($tmpBlock -match 'GetRandomFileName') 'the capture directory name is random'
$newItem = [regex]::Match($tmpBlock, 'New-Item[^\r\n]*\$tmp[^\r\n]*').Value
Check ($newItem -match '-ItemType Directory' -and $newItem -notmatch '-Force') 'an existing capture directory is not adopted (no -Force, whatever the operand order)'
Check ($tmpBlock -match 'icacls \$tmp /inheritance:r') 'the capture directory ACL is restricted to Administrators and SYSTEM'
$guardStart = if ($tmpStart -ge 0) { $text.LastIndexOf('if (-not $FromPcap)', $tmpStart) } else { -1 }
$guard = if ($guardStart -ge 0) { $text.Substring($guardStart, $tmpStart - $guardStart) } else { 'missing' }
Check ($guardStart -ge 0 -and $guard -notmatch '\n\}' -and $guard -match '\{\s*$') 'the capture directory is only created when not reprocessing a pcap'

# -VendorId is spliced into tshark display filters and PnP regexes: four hex digits only.
$hidScript = Join-Path $PSScriptRoot '..\capture\Get-SteamControllerHid.ps1'
$hidText = Get-Content -LiteralPath $hidScript -Raw
foreach ($pair in @(@('Capture-SteamController.ps1', $text), @('Get-SteamControllerHid.ps1', $hidText))) {
    Check ($pair[1] -match "ValidatePattern\('\^\[0-9A-Fa-f\]\{4\}\$'\)\]\s*\r?\n\s*\[string\] \`$VendorId") "$($pair[0]) validates -VendorId as four hex digits"
}

# Raw captures and attribution: the full hub capture is never copied under captures/, the Steam
# phase never falls back to an unfiltered "usb" display filter, the final hint stages an explicit
# file list instead of the directory, and the raw names are ignored by git.
Check ($text -notmatch 'Copy-Item \$Pcap') 'the full hub capture is not copied into the output directory'
Check ($text -notmatch "'usb'\s*\}") 'no unfiltered "usb" display filter fallback remains'
Check ($text -notmatch 'git add captures') 'the staging hint does not add the whole captures directory'
Check ($text -match "-notlike '\*\.pcap'") 'raw .pcap files are excluded from the staging hint'
Check ($text -match '\$script:Emitted' -and $text -notmatch 'Get-ChildItem -LiteralPath \$OutputDir -File') 'the staging hint lists the files this run registered, not a directory scan'
$ignore = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\.gitignore') -Raw
Check ($ignore -match '(?m)^/captures/\*\.pcap\s*$') 'raw .pcap hub captures under captures/ are gitignored'

# Device attribution over synthetic tshark descriptor rows (address|idProduct|bcdDevice|
# bNumInterfaces): one device is selected, none selects nothing, and a second device of the
# vendor on the same hub is refused instead of the first one being taken.
$selStart = $text.IndexOf('function Select-ControllerDevice')
$selEnd = $text.IndexOf('function ', $selStart + 1)
Check ($selStart -ge 0 -and $selEnd -gt $selStart) 'Select-ControllerDevice is present'
$VendorId = '28DE'
$ProductId = '1302'
Invoke-Expression $text.Substring($selStart, $selEnd - $selStart)
# Product identity: the vendor filter alone would accept a sole other Valve device as the controller.
Check ($text -match "ValidatePattern\('\^\[0-9A-Fa-f\]\{4\}\$'\)\]\s*\r?\n\s*\[string\] \`$ProductId = '1302'") 'the script takes the controller product id (default 1302) validated as four hex digits'
Check ($text -match 'USB\\\\VID_\$VendorId&PID_\$ProductId') 'the PnP presence check matches the product, not the vendor alone'
Check ($text -match 'if \(-not \(Find-ControllerAddress -Pcap \$raw\)\) \{ throw') 'the -FromPcap Plugin reprocess refuses a capture without the controller descriptor'
$WarningPreference = 'SilentlyContinue'
Check ($null -eq (Select-ControllerDevice -Rows @('4|0x1304|0x0100|1'))) 'a sole other product of the vendor (the puck) is not selected'
Check ($null -eq (Select-ControllerDevice -Rows @('4|0x1304|0x0100|1', '5|0x1205|0x0100|1'))) 'several other products of the vendor select nothing'
$mixed = Select-ControllerDevice -Rows @('4|0x1304|0x0100|1', '3|0x1302|0x0307|1')
Check ($null -ne $mixed -and $mixed.Address -eq 3) 'the controller is selected next to another product of the vendor'
Check ($null -ne (Select-ControllerDevice -Rows @('3|1302|0x0307|1'))) 'the bare hex product form is accepted'
$WarningPreference = 'Continue'
$one = Select-ControllerDevice -Rows @('3|0x1302|0x0307|1', '3|0x1302|0x0307|1')
Check ($null -ne $one -and $one.Address -eq 3 -and $one.ProductId -eq '0x1302' -and $one.BcdDevice -eq '0x0307') 'a single device whose descriptor was seen twice is selected once'
Check ($null -eq (Select-ControllerDevice -Rows @())) 'no descriptor rows select nothing'
Check ($null -eq (Select-ControllerDevice -Rows @('', '|0x1302|0x0307|1', '7'))) 'rows without an address or incomplete rows select nothing'
$mixedOut = @(Select-ControllerDevice -Rows @('3|0x1302|0x0307|1', '5|0x1142|0x0100|1') 3>&1)
$warned = @($mixedOut | Where-Object { $_ -is [System.Management.Automation.WarningRecord] })
$beside = @($mixedOut | Where-Object { $_ -isnot [System.Management.Automation.WarningRecord] })
Check ($beside.Count -eq 1 -and $beside[0].Address -eq 3 -and $warned.Count -eq 1 -and "$($warned[0])" -match 'address 5 idProduct 0x1142') 'another product of the vendor beside the controller is named in a warning and never selected'
$refused = $false
try { Select-ControllerDevice -Rows @('3|0x1302|0x0307|1', '4|0x1302|0x0307|1') | Out-Null } catch { $refused = $_.Exception.Message -match 'USB addresses 3, 4' }
Check $refused 'the same product under two addresses (re-enumeration) is refused too'
# The staging list is built from the files this run wrote. A stale .json.zip from an earlier run
# next to this run's small .json must neither be listed nor left behind.
foreach ($fn in @('Register-Artifact', 'Unregister-Artifact', 'Complete-JsonExport')) {
    $s = $text.IndexOf("function $fn")
    $e = $text.IndexOf("`nfunction ", $s + 1)
    if ($e -lt 0) { $e = $text.IndexOf("`n`n", $s) }
    Check ($s -ge 0 -and $e -gt $s) "$fn is present"
    Invoke-Expression $text.Substring($s, $e - $s)
}
$script:Emitted = New-Object System.Collections.Generic.List[string]
$InformationPreference = 'SilentlyContinue'
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("capture-helper-test-" + [System.IO.Path]::GetRandomFileName().Replace('.', ''))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    $json = Join-Path $stage 'sc26-steam.json'
    Set-Content -LiteralPath "$json.zip" -Value 'stale archive from the unfiltered days' -Encoding ascii
    Set-Content -LiteralPath $json -Value '[]' -Encoding ascii
    $WarningPreference = 'SilentlyContinue'
    Complete-JsonExport -JsonPath $json
    $WarningPreference = 'Continue'
    Check ((-not (Test-Path -LiteralPath "$json.zip")) -and (Test-Path -LiteralPath $json)) 'a small fresh export removes the stale .json.zip of an earlier run'
    Check ($script:Emitted.Count -eq 1 -and $script:Emitted[0] -eq 'sc26-steam.json') 'only the fresh .json is registered for staging'
    Register-Artifact (Join-Path $stage 'sc26-steam.pcapng'); Register-Artifact (Join-Path $stage 'sc26-steam.pcapng')
    Check ($script:Emitted.Count -eq 2) 'registering the same artifact twice lists it once'
    Unregister-Artifact (Join-Path $stage 'sc26-steam.pcapng')
    Check ($script:Emitted.Count -eq 1 -and -not $script:Emitted.Contains('sc26-steam.pcapng')) 'an artifact can be withdrawn from the list'
}
finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    $InformationPreference = 'Continue'
}

$steamStart = $text.IndexOf("'Steam' {")
$steamBlock = $text.Substring($steamStart)
Check ($steamBlock -notmatch '\$candidates\.Count -eq 1 -or') 'the single Plugin-phase hub is not accepted without identifying the controller'
Check ($steamBlock -match '\$chosen\.Count -gt 1') 'several hubs seeing the vendor are refused in the Steam phase'

if ($failures -ne 0) { throw "$failures capture-helper gate(s) failed" }
Write-Host 'capture helper gates passed'
