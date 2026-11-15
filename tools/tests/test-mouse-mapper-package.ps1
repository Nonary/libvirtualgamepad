[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('vhf-mapper-package-test-' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $root 'package'
$evidencePath = Join-Path $root 'catalog-generation.json'
$verify = Join-Path (Split-Path -Parent $PSScriptRoot) 'verify-driver-package.ps1'
$driverVer = '11/14/2026,0.1.0.999'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'release-preparation.ps1')
$files = @('driver/VibeshineVhfGamepad.inf', 'driver/VibeshineVhfGamepad.dll', 'driver/VibeshineVhfGamepad.cat',
    'mouse_mapper/VibeshineVhfMouseMapper.inf', 'mouse_mapper/VibeshineVhfMouseMapper.sys', 'mouse_mapper/VibeshineVhfMouseMapper.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe')
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
# Mock Authenticode only for these inert fixture bytes. Native signature and
# kernel policy verification remains a separate Windows qualification gate.
function Get-AuthenticodeSignature { param($LiteralPath); return [pscustomobject]@{ Status = 'NotSigned'; SignerCertificate = $null } }
function Expect-Failure([scriptblock]$Action, [string]$Pattern) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    if ($message -notmatch $Pattern) { throw "Expected failure '$Pattern', got '$message'." }
}
$previousLog = $env:VHF_MAPPER_TEST_SIGNLOG
$previousGit = $env:VHF_MAPPER_TEST_GIT
try {
    foreach ($relative in $files) {
        $path = Join-Path $package $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        $contents = if ($relative.EndsWith('.inf')) { "DriverVer=$driverVer" } else { "inert fixture:$relative" }
        if ($relative -ceq 'mouse_mapper/VibeshineVhfMouseMapper.inf') { $contents += "`nModel=Mapper,HID\VID_ABCD&PID_5678&REV_0100&Col02" }
        [IO.File]::WriteAllText($path, $contents)
    }
    $membership = [ordered]@{}
    foreach ($relative in @($files | Where-Object { -not $_.StartsWith('tools/') })) { $membership[$relative] = Hash (Join-Path $package $relative) }
    [ordered]@{ schema_version = 1; generator = 'Inf2Cat'; package_dir = $package; platform = 'x64'; driver_ver = $driverVer; files = $membership } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $evidencePath
    $arguments = @{ PackageDir = $package; Platform = 'x64'; DriverVer = $driverVer; SourceRevision = ('a' * 40); ProtocolVersion = 2; UnsignedForMsiSigning = $true; CatalogGenerationEvidencePath = $evidencePath }
    & $verify @arguments | Out-Null
    $manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.features.mouse_mapper -ne 1 -or $manifest.mouse_mapper.kernel_signing -cne 'requires-microsoft-signing' -or
        (@($manifest.files.path | Sort-Object) -join ',') -cne (@($files | Sort-Object) -join ',')) { throw 'Unsigned candidate feature/file contract is incorrect.' }
    $stateRoot = Join-Path $root 'unsigned-state'
    New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null
    Copy-Item -LiteralPath $package -Destination (Join-Path $stateRoot 'package') -Recurse
    Copy-Item -LiteralPath $evidencePath -Destination (Join-Path $stateRoot 'catalog-generation.json')
    $preparationMetadata = [ordered]@{ tag='v0.1.0-beta.999'; tag_target=('a'*40); source_revision=('a'*40); driver_ver=$driverVer; protocol_version=2; platform='x64'; vendor_id=43981; product_id=22136; preparation_run_id='42' }
    Write-UnsignedReleasePreparation -Root $stateRoot -Metadata $preparationMetadata
    $preparedArchive = Join-Path $root 'unsigned.zip'
    $preparedHash = Export-UnsignedReleasePreparation -Root $stateRoot -ArchivePath $preparedArchive
    $imported = Join-Path $root 'imported-state'
    $null = Import-UnsignedReleasePreparation -ArchivePath $preparedArchive -ExpectedArchiveSha256 $preparedHash -Root $imported -ExpectedMetadata $preparationMetadata
    $importedSys = Join-Path $imported 'package/mouse_mapper/VibeshineVhfMouseMapper.sys'
    $originalSysHash = Hash $importedSys
    [IO.File]::WriteAllText($importedSys, 'tampered persisted submission')
    Expect-Failure { Assert-UnsignedReleasePreparation -Root $imported -ExpectedMetadata $preparationMetadata } 'preparation hash mismatch'
    foreach ($field in @('preparation_run_id','source_revision','driver_ver','vendor_id','product_id')) {
        $wrong = [ordered]@{}
        foreach ($entry in $preparationMetadata.GetEnumerator()) { $wrong[$entry.Key] = $entry.Value }
        $wrong[$field] = 'mismatched-original-metadata'
        Expect-Failure { Assert-UnsignedReleasePreparation -Root $stateRoot -ExpectedMetadata $wrong } 'preparation metadata mismatch'
    }
    Expect-Failure { Import-UnsignedReleasePreparation -ArchivePath $preparedArchive -ExpectedArchiveSha256 ('0'*64) -Root (Join-Path $root 'bad-pin') -ExpectedMetadata $preparationMetadata } 'explicitly pinned SHA-256'

    $mapperSys = Join-Path $package 'mouse_mapper/VibeshineVhfMouseMapper.sys'
    $original = [IO.File]::ReadAllBytes($mapperSys)
    Remove-Item -LiteralPath $mapperSys
    Expect-Failure { & $verify @arguments } 'missing'
    [IO.File]::WriteAllText($mapperSys, 'changed mapper bytes')
    Expect-Failure { & $verify @arguments } 'evidence hash mismatch'
    [IO.File]::WriteAllBytes($mapperSys, $original)
    $signedCatalog = Join-Path $root 'returned.cat'
    [IO.File]::WriteAllText($signedCatalog, 'inert returned catalog fixture')
    $fakeSignTool = Join-Path $root 'signtool-fixture.ps1'
    $env:VHF_MAPPER_TEST_SIGNLOG = Join-Path $root 'verify-invocations.txt'
    @'
if (($args -join '|') -notmatch '^verify\|/v\|/kp\|/c\|') { throw 'Expected exact kernel catalog policy invocation.' }
Add-Content -LiteralPath $env:VHF_MAPPER_TEST_SIGNLOG -Value ($args -join '|')
$global:LASTEXITCODE = 0
'@ | Set-Content -LiteralPath $fakeSignTool
    & $verify @arguments -RequireKernelSigning -KernelSignedMapperCatalog $signedCatalog -SignToolPath $fakeSignTool | Out-Null
    $manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
    $mapperCatName = 'mouse_mapper/VibeshineVhfMouseMapper.cat'
    $mapperEntry = @($manifest.files | Where-Object path -ceq $mapperCatName)
    if ($manifest.mouse_mapper.kernel_signing -cne 'windows-kernel-policy' -or
        $mapperEntry.Count -ne 1 -or $mapperEntry[0].sha256 -cne (Hash $signedCatalog) -or
        $manifest.signing.catalog_membership.files.PSObject.Properties[$mapperCatName].Value -cne (Hash $signedCatalog) -or
        @((Get-Content -LiteralPath $env:VHF_MAPPER_TEST_SIGNLOG)).Count -ne 2 -or
        @($manifest.signing.signed_downstream) -contains $mapperCatName) { throw 'Catalog import lost policy checks or final hash binding.' }
    $fakeGit = Join-Path $root 'git-fixture.ps1'
    @'
switch ($args[2]) {
    'rev-parse' { 'a' * 40 }
    'status' { }
    'tag' { 'v0.1.0-beta.999' }
    'log' { '11/14/2026' }
    'rev-list' { '999' }
    default { throw "Unexpected fixture Git operation: $($args -join ' ')" }
}
$global:LASTEXITCODE = 0
'@ | Set-Content -LiteralPath $fakeGit
    $env:VHF_MAPPER_TEST_GIT = $fakeGit
    function Get-Command {
        param([string]$Name, $ErrorAction)
        if ($Name -ceq 'git.exe') { return [pscustomobject]@{ Source = $env:VHF_MAPPER_TEST_GIT } }
        Microsoft.PowerShell.Core\Get-Command @PSBoundParameters
    }
    $releaseDir = Join-Path $root 'resumed-release'
    & (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-release-package.ps1') -Stage Resume -Tag 'v0.1.0-beta.999' `
        -PreparationRunId 42 -PreparedArchivePath $preparedArchive -ExpectedPreparedArchiveSha256 $preparedHash `
        -KernelSignedMapperCatalog $signedCatalog -KeyboardMouseVendorId 43981 -KeyboardMouseProductId 22136 `
        -SignToolPath $fakeSignTool -MSBuildPath '/must-not-rebuild-on-resume' -OutputDir $releaseDir | Out-Null
    $releaseArchive = Join-Path $releaseDir 'libvirtualgamepad-0.1.0-beta.999-windows-x64.zip'
    $resumedRoot = Join-Path $root 'resumed-extracted'
    [IO.Compression.ZipFile]::ExtractToDirectory($releaseArchive, $resumedRoot)
    $releaseLock = Get-Content -LiteralPath (Join-Path $releaseDir 'libvirtualgamepad-0.1.0-beta.999-windows-x64.release-lock.json') -Raw | ConvertFrom-Json
    if ((Hash (Join-Path $resumedRoot 'mouse_mapper/VibeshineVhfMouseMapper.sys')) -cne $originalSysHash -or
        $releaseLock.preparation.unsigned_archive_sha256 -cne $preparedHash -or $releaseLock.preparation.run_id -cne '42') { throw 'Resume rebuilt/changed original mapper bytes or lost preparation provenance.' }
    Write-Output 'Prepare/resume fixture passed: explicit archive SHA, source/DriverVer/VID/PID/run binding, tamper rejection and exact original SYS reuse; no MSBuild path invoked. Git/signature verification was mocked.'
    Write-Output 'Mapper package fixture passed: missing/tampered SYS rejected, unsigned candidate recorded, kernel verifier called for both members, returned CAT hashes pinned. Signature verification was mocked.'
} finally {
    $env:VHF_MAPPER_TEST_SIGNLOG = $previousLog
    $env:VHF_MAPPER_TEST_GIT = $previousGit
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
