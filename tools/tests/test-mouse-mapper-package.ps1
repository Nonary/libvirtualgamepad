[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('vhf-mapper-package-test-' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $root 'package'
$evidencePath = Join-Path $root 'catalog-generation.json'
$verify = Join-Path (Split-Path -Parent $PSScriptRoot) 'verify-driver-package.ps1'
$driverVer = '11/14/2026,0.1.0.999'
$files = @('driver/VibeshineVhfGamepad.inf', 'driver/VibeshineVhfGamepad.dll', 'driver/VibeshineVhfGamepad.cat',
    'mouse_mapper/VibeshineVhfMouseMapper.inf', 'mouse_mapper/VibeshineVhfMouseMapper.sys', 'mouse_mapper/VibeshineVhfMouseMapper.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe')
$expectedUnsigned = @('driver/VibeshineVhfGamepad.cat', 'driver/VibeshineVhfGamepad.dll',
    'mouse_mapper/VibeshineVhfMouseMapper.cat', 'mouse_mapper/VibeshineVhfMouseMapper.sys', 'tools/VibeshineVhfGamepadDeviceSetup.exe')
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
$fixtureState = @{ SignedName = '' }
# Inert bytes only: mock Authenticode status; no signature, trust or driver operation.
function Get-AuthenticodeSignature {
    param($LiteralPath)
    if ($fixtureState.SignedName -and $LiteralPath.EndsWith($fixtureState.SignedName)) { return [pscustomobject]@{ Status='Valid'; SignerCertificate=[pscustomobject]@{ Subject='fixture'; Thumbprint=('A'*40) } } }
    return [pscustomobject]@{ Status='NotSigned'; SignerCertificate=$null }
}
function Expect-Failure([scriptblock]$Action, [string]$Pattern) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    if ($message -notmatch $Pattern) { throw "Expected failure '$Pattern', got '$message'." }
}
try {
    foreach ($relative in $files) {
        $path = Join-Path $package $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        $contents = if ($relative.EndsWith('.inf')) { "DriverVer=$driverVer" } else { "inert fixture:$relative" }
        if ($relative -ceq 'mouse_mapper/VibeshineVhfMouseMapper.inf') { $contents += "`nModel=Mapper,HID\VID_0000&PID_0000&REV_0100&Col02" }
        [IO.File]::WriteAllText($path, $contents)
    }
    $membership = [ordered]@{}
    foreach ($relative in @($files | Where-Object { -not $_.StartsWith('tools/') })) { $membership[$relative] = Hash (Join-Path $package $relative) }
    [ordered]@{ schema_version=1; generator='Inf2Cat'; package_dir=$package; platform='x64'; driver_ver=$driverVer; files=$membership } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $evidencePath
    $arguments = @{ PackageDir=$package; Platform='x64'; DriverVer=$driverVer; SourceRevision=('a'*40); ProtocolVersion=2; UnsignedForMsiSigning=$true; CatalogGenerationEvidencePath=$evidencePath }
    & $verify @arguments | Out-Null
    $manifest = Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.features.mouse_mapper -ne 1 -or $manifest.mouse_mapper.kernel_signing -cne 'unsigned-producer' -or
        $manifest.mouse_mapper.vendor_id -ne 0 -or $manifest.mouse_mapper.product_id -ne 0 -or
        (@($manifest.files.path | Sort-Object) -join ',') -cne (@($files | Sort-Object) -join ',') -or
        (@($manifest.signing.unsigned_payloads) -join ',') -cne ($expectedUnsigned -join ',') -or
        @($manifest.signing.signed_downstream) -contains 'mouse_mapper/VibeshineVhfMouseMapper.cat') { throw 'Unsigned mapper identity, layout or signing ownership is incorrect.' }
    # Run the real publication validator against inert files; no GitHub API is
    # invoked. Mutating a single sidecar must not survive coherent archive hashes.
    $publisher = Join-Path (Split-Path -Parent $PSScriptRoot) 'publish-release.ps1'
    $tokens=$null; $errors=$null
    $publisherAst = [Management.Automation.Language.Parser]::ParseFile($publisher, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw 'Publisher fixture parser failed.' }
    foreach ($name in @('Get-Sha256', 'Assert-ExactList', 'Get-ValidatedMouseMapperMetadata', 'Assert-MouseMapperMetadataMatch', 'Assert-MouseMapperInfIdentity', 'Assert-ReleaseSet')) {
        $definition = $publisherAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }.GetNewClosure(), $true)
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    $published = Join-Path $root 'publication-fixture'
    New-Item -ItemType Directory -Path $published | Out-Null
    $archiveName='fixture.zip'; $checksumName='fixture.zip.sha256'; $lockName='lock.json'; $evidenceName='evidence.json'
    $archivePath=Join-Path $published $archiveName
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($package, $archivePath)
    $archiveHash=Hash $archivePath
    "$archiveHash  $archiveName`n" | Set-Content -LiteralPath (Join-Path $published $checksumName) -NoNewline
    $metadata=[ordered]@{schema_version=1;tag='v0.1.0-beta.999';tag_target=('a'*40);source_revision=('a'*40);driver_ver=$driverVer;protocol_version=2;platform='x64';features=$manifest.features;mouse_mapper=$manifest.mouse_mapper;
        archive=@{name=$archiveName;sha256=$archiveHash};manifest=@{path='manifest.json';sha256=(Hash (Join-Path $package 'manifest.json'))};layout=@($files+'manifest.json'|Sort-Object);signing=$manifest.signing}
    $releaseLock=$metadata|ConvertTo-Json -Depth 8|ConvertFrom-Json
    $releaseEvidence=$metadata|ConvertTo-Json -Depth 8|ConvertFrom-Json
    $releaseEvidence|Add-Member verification ([pscustomobject]@{clean_tagged_head=$true;infverif_required=$true;inf2cat_required=$true;fresh_inf2cat_evidence=$true;package_verified_unsigned_for_msi_signing=$true;unsigned_authenticode_checked=$true;certificate_absent=$true;archive_created_once=$true})
    function Save-Sidecars {
        $releaseLock|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $published $lockName)
        $releaseEvidence|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $published $evidenceName)
    }
    Save-Sidecars
    $publicationArgs=@{Root=$published;ExpectedTag='v0.1.0-beta.999';ExpectedRevision=('a'*40);ExpectedFiles=@($archiveName,$checksumName,$lockName,$evidenceName|Sort-Object);ArchiveName=$archiveName;ChecksumName=$checksumName;LockName=$lockName;EvidenceName=$evidenceName}
    $null=Assert-ReleaseSet @publicationArgs
    $releaseEvidence.mouse_mapper.vendor_id=1;Save-Sidecars
    Expect-Failure { Assert-ReleaseSet @publicationArgs } 'metadata mismatch'
    $releaseEvidence.mouse_mapper.vendor_id=0
    $releaseLock.mouse_mapper.product_id=1;Save-Sidecars
    Expect-Failure { Assert-ReleaseSet @publicationArgs } 'metadata mismatch'
    $releaseEvidence.mouse_mapper.product_id=1;Save-Sidecars
    Expect-Failure { Assert-ReleaseSet @publicationArgs } 'metadata mismatch'
    $releaseLock.mouse_mapper.product_id=0;$releaseEvidence.mouse_mapper.product_id=0
    $releaseEvidence.mouse_mapper.vendor_id=65536;Save-Sidecars
    Expect-Failure { Assert-ReleaseSet @publicationArgs } '0..65535'
    $releaseEvidence.mouse_mapper.vendor_id=0;Save-Sidecars
    $null=Assert-ReleaseSet @publicationArgs
    Write-Output 'Publication mapper coherence fixture passed: lock/evidence/manifest identity agreement, sidecar-only tamper and out-of-range rejection; archived INF identity checked. No publication occurred.'
    foreach ($relative in @('mouse_mapper/VibeshineVhfMouseMapper.sys', 'mouse_mapper/VibeshineVhfMouseMapper.cat')) {
        $path = Join-Path $package $relative
        $original = [IO.File]::ReadAllBytes($path)
        Remove-Item -LiteralPath $path
        Expect-Failure { & $verify @arguments } 'missing'
        [IO.File]::WriteAllText($path, 'changed mapper bytes')
        Expect-Failure { & $verify @arguments } 'evidence hash mismatch'
        [IO.File]::WriteAllBytes($path, $original)
        $fixtureState.SignedName = Split-Path -Leaf $relative
        Expect-Failure { & $verify @arguments } 'Unsigned release payload'
        $fixtureState.SignedName = ''
    }
    Expect-Failure { & $verify @arguments -RequireKernelSigning } 'unsigned producer release cannot'
    $workflow = Get-Content -LiteralPath (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '.github/workflows/release-windows.yml') -Raw
    if ($workflow -match 'prepare_run_id|signed_mapper_catalog_url|PreparationRunId|Stage Resume|Stage Prepare' -or
        $workflow -notmatch 'build-release-package.ps1' -or $workflow -notmatch 'publish-release.ps1') { throw 'Release workflow is not the original single-stage unsigned build/publish contract.' }
    Write-Output 'Unsigned mapper package fixture passed: 0/0 identity, six fresh catalog-bound hashes, exact unsigned CAT/SYS ownership, missing/tampered/signed mapper rejection, single-stage release without signing inputs. Authenticode status was mocked.'
} finally { if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force } }
