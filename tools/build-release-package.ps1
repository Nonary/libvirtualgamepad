[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v0\.1\.0-beta\.[1-9][0-9]*$')]
    [string] $Tag,

    [ValidateSet('Prepare', 'Resume')][string] $Stage = 'Prepare',
    [string] $OutputDir,
    [Parameter(Mandatory = $true)][ValidatePattern('^[1-9][0-9]*$')][string] $PreparationRunId,
    [string] $PreparedArchivePath,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string] $ExpectedPreparedArchiveSha256,
    [string] $MSBuildPath,
    [string] $InfVerifPath,
    [string] $Inf2CatPath,
    [string] $SignToolPath,
    [string] $KernelSignedMapperCatalog,
    [uint16] $KeyboardMouseVendorId = 0,
    [uint16] $KeyboardMouseProductId = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'release-preparation.ps1')
if ($Stage -ceq 'Resume' -and ([string]::IsNullOrWhiteSpace($PreparedArchivePath) -or [string]::IsNullOrWhiteSpace($ExpectedPreparedArchiveSha256) -or [string]::IsNullOrWhiteSpace($KernelSignedMapperCatalog))) {
    throw 'Resume requires the exact prepared archive, its explicit SHA-256, original preparation run ID, and the returned Microsoft mapper catalog.'
}
if ($KeyboardMouseVendorId -eq 0 -or $KeyboardMouseProductId -eq 0) {
    throw 'The ordered mouse mapper requires explicit nonzero product-owned -KeyboardMouseVendorId and -KeyboardMouseProductId. No production identity is allocated by this script.'
}


function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string[]] $Arguments
    )

    $output = @(& $script:Git.Source -C $script:RepoRoot @Arguments)
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
    return ($output -join "`n").Trim()
}

function Get-RelativeFiles {
    param([Parameter(Mandatory = $true)][string] $Root)

    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    return @(
        Get-ChildItem -LiteralPath $rootPath -Recurse -File | ForEach-Object {
            $_.FullName.Substring($rootPath.Length).TrimStart('\', '/').Replace('\', '/')
        } | Sort-Object
    )
}

function Assert-ExactList {
    param(
        [Parameter(Mandatory = $true)][object[]] $Actual,
        [Parameter(Mandatory = $true)][object[]] $Expected,
        [Parameter(Mandatory = $true)][string] $Name
    )

    $actualText = @($Actual | ForEach-Object { [string] $_ }) -join "`n"
    $expectedText = @($Expected | ForEach-Object { [string] $_ }) -join "`n"
    if ($actualText -cne $expectedText) {
        throw "$Name does not match the release contract.`nExpected:`n$expectedText`nActual:`n$actualText"
    }
}

function Get-ProtocolVersion {
    param([Parameter(Mandatory = $true)][string] $HeaderPath)

    $match = [regex]::Match(
        (Get-Content -LiteralPath $HeaderPath -Raw),
        'k_protocol_version\s*=\s*(\d+)'
    )
    if (-not $match.Success) {
        throw "Could not read k_protocol_version from '$HeaderPath'."
    }
    return [uint16] $match.Groups[1].Value
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($null -eq $Git) {
    throw 'git.exe was not found.'
}

$head = Invoke-Git -Arguments @('rev-parse', 'HEAD')
$dirty = Invoke-Git -Arguments @('status', '--porcelain', '--untracked-files=all')
if (-not [string]::IsNullOrWhiteSpace($dirty)) {
    throw "Release packaging requires a clean worktree. Commit or remove these entries:`n$dirty"
}

$tagsAtHead = @(
    (Invoke-Git -Arguments @('tag', '--points-at', 'HEAD')) -split "`n" |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
)
Assert-ExactList -Actual $tagsAtHead -Expected @($Tag) -Name 'Tags at HEAD'

$tagTarget = Invoke-Git -Arguments @('rev-parse', "refs/tags/$Tag^{commit}")
if ($tagTarget -cne $head) {
    throw "Tag '$Tag' resolves to '$tagTarget', not HEAD '$head'."
}

$version = $Tag.Substring(1)
$assetBase = "libvirtualgamepad-$version-windows-x64"
$archiveName = "$assetBase.zip"
$checksumName = "$archiveName.sha256"
$lockName = "$assetBase.release-lock.json"
$evidenceName = "$assetBase-evidence.json"

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $RepoRoot $(if ($Stage -ceq 'Prepare') { 'artifacts/unsigned-preparation' } else { 'artifacts/release' })
}
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) {
    $existingOutput = Get-ChildItem -LiteralPath $OutputDir -Force | Select-Object -First 1
    if ($null -ne $existingOutput) {
        throw "Refusing to overwrite a non-empty release output directory: $OutputDir"
    }
} else {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$commitDate = Invoke-Git -Arguments @('log', '-1', '--format=%cd', '--date=format:%m/%d/%Y')
$revisionText = Invoke-Git -Arguments @('rev-list', '--count', 'HEAD')
$revision = 0
if (-not [int]::TryParse($revisionText, [ref] $revision) -or $revision -gt 65535) {
    throw "Git revision count '$revisionText' cannot be represented in DriverVer."
}
$driverVer = "$commitDate,0.1.0.$revision"
$protocolVersion = Get-ProtocolVersion -HeaderPath (Join-Path $RepoRoot 'include/libvirtualgamepad/protocol.h')

$preparationMetadata = [ordered]@{
    tag = $Tag; tag_target = $tagTarget.ToLowerInvariant(); source_revision = $head.ToLowerInvariant()
    driver_ver = $driverVer; protocol_version = $protocolVersion; platform = 'x64'
    vendor_id = $KeyboardMouseVendorId; product_id = $KeyboardMouseProductId; preparation_run_id = $PreparationRunId
}

$expectedPayload = @(
    'mouse_mapper/VibeshineVhfMouseMapper.cat',
    'mouse_mapper/VibeshineVhfMouseMapper.inf',
    'mouse_mapper/VibeshineVhfMouseMapper.sys',
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.inf',
    'manifest.json',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$expectedManifestFiles = @(
    'mouse_mapper/VibeshineVhfMouseMapper.cat',
    'mouse_mapper/VibeshineVhfMouseMapper.inf',
    'mouse_mapper/VibeshineVhfMouseMapper.sys',
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.inf',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$signedDownstream = @(
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$unsignedPayloads = @(
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)

$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("libvirtualgamepad-release-" + [guid]::NewGuid().ToString('N'))
$packageDir = Join-Path $workRoot 'package'
$catalogGenerationEvidencePath = Join-Path $workRoot 'catalog-generation.json'
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null

try {
    if ($Stage -ceq 'Prepare') {
    $stateRoot = Join-Path $workRoot 'unsigned-state'
    $packageDir = Join-Path $stateRoot 'package'
    $catalogGenerationEvidencePath = Join-Path $stateRoot 'catalog-generation.json'
    New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null
    $buildParameters = @{
        Configuration = 'Release'
        Platform = 'x64'
        DriverVer = $driverVer
        PackageDir = $packageDir
        SigningMode = 'Release'
        KeyboardMouseVendorId = $KeyboardMouseVendorId
        KeyboardMouseProductId = $KeyboardMouseProductId
        CatalogGenerationEvidencePath = $catalogGenerationEvidencePath
    }
    foreach ($parameter in @('MSBuildPath', 'InfVerifPath', 'Inf2CatPath')) {
        $value = Get-Variable -Name $parameter -ValueOnly
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            $buildParameters[$parameter] = $value
        }
    }

    & (Join-Path $PSScriptRoot 'build-driver.ps1') @buildParameters

    & (Join-Path $PSScriptRoot 'verify-driver-package.ps1') -PackageDir $packageDir -Platform x64 -DriverVer $driverVer `
        -SourceRevision $head -ProtocolVersion $protocolVersion -CatalogGenerationEvidencePath $catalogGenerationEvidencePath -UnsignedForMsiSigning
    Write-UnsignedReleasePreparation -Root $stateRoot -Metadata $preparationMetadata
    $null = Assert-UnsignedReleasePreparation -Root $stateRoot -ExpectedMetadata $preparationMetadata
    $unsignedName = "libvirtualgamepad-$version-unsigned-run-$PreparationRunId.zip"
    $unsignedPath = Join-Path $OutputDir $unsignedName
    $unsignedHash = Export-UnsignedReleasePreparation -Root $stateRoot -ArchivePath $unsignedPath
    "$unsignedHash  $unsignedName`n" | Set-Content -LiteralPath "$unsignedPath.sha256" -NoNewline -Encoding ascii
    Write-Output "Prepared exact unsigned submission archive: $unsignedPath"
    Write-Output "Unsigned archive SHA-256: $unsignedHash"
    Write-Output 'Submit package/mouse_mapper from this archive for Microsoft signing; resume with this exact archive/hash/run ID and returned CAT. Resume does not rebuild.'
    return
    }

    $stateRoot = Join-Path $workRoot 'imported-unsigned-state'
    $preparation = Import-UnsignedReleasePreparation -ArchivePath $PreparedArchivePath -ExpectedArchiveSha256 $ExpectedPreparedArchiveSha256 `
        -Root $stateRoot -ExpectedMetadata $preparationMetadata
    # Work on a copy. No compiler or Inf2Cat invocation occurs in Resume.
    Copy-Item -LiteralPath (Join-Path $stateRoot 'package') -Destination $packageDir -Recurse
    $originalGenerationEvidence = Get-Content -LiteralPath (Join-Path $stateRoot 'catalog-generation.json') -Raw | ConvertFrom-Json
    # The artifact hash authenticates the original evidence. Only its absolute
    # scratch-directory locator changes after moving the same immutable bytes.
    $originalGenerationEvidence.package_dir = $packageDir
    $originalGenerationEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $catalogGenerationEvidencePath -NoNewline -Encoding utf8

    $verifyParameters = @{
        PackageDir = $packageDir
        Platform = 'x64'
        DriverVer = $driverVer
        SourceRevision = $head
        ProtocolVersion = $protocolVersion
        CatalogGenerationEvidencePath = $catalogGenerationEvidencePath
        UnsignedForMsiSigning = $true
        RequireKernelSigning = $true
        KernelSignedMapperCatalog = $KernelSignedMapperCatalog
    }
    if (-not [string]::IsNullOrWhiteSpace($SignToolPath)) {
        $verifyParameters.SignToolPath = $SignToolPath
    }
    & (Join-Path $PSScriptRoot 'verify-driver-package.ps1') @verifyParameters

    $actualPayload = Get-RelativeFiles -Root $packageDir
    Assert-ExactList -Actual $actualPayload -Expected @($expectedPayload | Sort-Object) -Name 'Package layout'
    if (@(Get-ChildItem -LiteralPath $packageDir -Recurse -Filter '*.cer' -File).Count -ne 0) {
        throw 'Release packages must not contain a certificate.'
    }

    $manifestPath = Join-Path $packageDir 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema_version -ne 1 -or
        $manifest.source_revision -cne $head.ToLowerInvariant() -or
        $manifest.driver_ver -cne $driverVer -or
        $manifest.platform -cne 'x64' -or
        $manifest.protocol_version -ne $protocolVersion -or
        $manifest.signing.channel -cne 'msi-request-signing') {
        throw 'manifest.json metadata does not match the tagged source and release contract.'
    }
    Assert-ExactList -Actual @($manifest.signing.signed_downstream) -Expected $signedDownstream -Name 'Manifest signed_downstream'
    Assert-ExactList -Actual @($manifest.signing.unsigned_payloads) -Expected $unsignedPayloads -Name 'Manifest unsigned_payloads'
    if ($manifest.signing.catalog_membership.basis -cne 'fresh-inf2cat' -or
        $manifest.signing.catalog_membership.generator -cne 'Inf2Cat') {
        throw 'manifest.json does not record fresh Inf2Cat catalog membership evidence.'
    }
    $expectedMembershipFiles = @(
        'mouse_mapper/VibeshineVhfMouseMapper.cat',
        'mouse_mapper/VibeshineVhfMouseMapper.inf',
        'mouse_mapper/VibeshineVhfMouseMapper.sys',
        'driver/VibeshineVhfGamepad.cat',
        'driver/VibeshineVhfGamepad.dll',
        'driver/VibeshineVhfGamepad.inf'
    )
    $membershipFileNames = @($manifest.signing.catalog_membership.files.PSObject.Properties.Name | Sort-Object)
    Assert-ExactList -Actual $membershipFileNames -Expected @($expectedMembershipFiles | Sort-Object) -Name 'Catalog membership file list'

    $manifestFiles = @($manifest.files | ForEach-Object { [string] $_.path } | Sort-Object)
    Assert-ExactList -Actual $manifestFiles -Expected @($expectedManifestFiles | Sort-Object) -Name 'Manifest file list'
    foreach ($file in $manifest.files) {
        $payloadPath = Join-Path $packageDir ([string] $file.path)
        $actualHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -cne [string] $file.sha256) {
            throw "Manifest hash mismatch for '$($file.path)'."
        }
        if ([string] $file.path -cin $expectedMembershipFiles) {
            $membershipHash = [string] $manifest.signing.catalog_membership.files.PSObject.Properties[[string] $file.path].Value
            if ($membershipHash -cne $actualHash) {
                throw "Catalog membership hash mismatch for '$($file.path)'."
            }
        }
    }

    $manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $archivePath = Join-Path $OutputDir $archiveName
    if (Test-Path -LiteralPath $archivePath) {
        throw "Refusing to recreate release archive '$archivePath'."
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $packageDir,
        $archivePath,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false
    )

    $zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $archiveLayout = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object)
        Assert-ExactList -Actual $archiveLayout -Expected @($expectedPayload | Sort-Object) -Name 'Archive layout'
        $archivedManifest = $zip.GetEntry('manifest.json')
        if ($null -eq $archivedManifest) {
            throw 'The release archive is missing manifest.json.'
        }
        $manifestStream = $archivedManifest.Open()
        $sha256 = [System.Security.Cryptography.SHA256]::Create()
        try {
            $archivedManifestHash = ([System.BitConverter]::ToString(
                $sha256.ComputeHash($manifestStream)
            )).Replace('-', '').ToLowerInvariant()
        } finally {
            $sha256.Dispose()
            $manifestStream.Dispose()
        }
        if ($archivedManifestHash -cne $manifestHash) {
            throw 'The archived manifest hash changed while creating the ZIP.'
        }
    } finally {
        $zip.Dispose()
    }

    $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $checksumPath = Join-Path $OutputDir $checksumName
    "$archiveHash  $archiveName`n" | Set-Content -LiteralPath $checksumPath -NoNewline -Encoding ascii

    $releaseMetadata = [ordered]@{
        schema_version = 1
        tag = $Tag
        tag_target = $tagTarget.ToLowerInvariant()
        source_revision = $head.ToLowerInvariant()
        driver_ver = $driverVer
        protocol_version = $protocolVersion
        platform = 'x64'
        archive = [ordered]@{
            name = $archiveName
            sha256 = $archiveHash
        }
        manifest = [ordered]@{
            path = 'manifest.json'
            sha256 = $manifestHash
        }
        preparation = [ordered]@{ run_id = $PreparationRunId; unsigned_archive_sha256 = $ExpectedPreparedArchiveSha256.ToLowerInvariant(); source_revision = $head.ToLowerInvariant() }
        layout = @($expectedPayload | Sort-Object)
        features = $manifest.features
        mouse_mapper = $manifest.mouse_mapper
        signing = [ordered]@{
            channel = 'msi-request-signing'
            signed_downstream = $signedDownstream
            unsigned_payloads = $unsignedPayloads
            catalog_membership = $manifest.signing.catalog_membership
        }
    }
    $releaseMetadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDir $lockName) -NoNewline -Encoding utf8

    $evidence = [ordered]@{}
    foreach ($entry in $releaseMetadata.GetEnumerator()) {
        $evidence[$entry.Key] = $entry.Value
    }
    $evidence.verification = [ordered]@{
        clean_tagged_head = $true
        infverif_required = $true
        inf2cat_required = $true
        fresh_inf2cat_evidence = $true
        exact_prepared_artifact_reused_without_rebuild = $true
        mapper_kernel_policy_verified = $true
        package_verified_unsigned_for_msi_signing = $true
        unsigned_authenticode_checked = $true
        certificate_absent = $true
        archive_created_once = $true
    }
    $evidence | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDir $evidenceName) -NoNewline -Encoding utf8

    $expectedOutputs = @($archiveName, $checksumName, $evidenceName, $lockName) | Sort-Object
    Assert-ExactList -Actual (Get-RelativeFiles -Root $OutputDir) -Expected $expectedOutputs -Name 'Release output set'

    Write-Output "Release package ready: $archivePath"
    Write-Output "Tag target: $tagTarget"
    Write-Output "DriverVer: $driverVer"
    Write-Output "Protocol version: $protocolVersion"
    Write-Output "SHA-256: $archiveHash"
} finally {
    $resolvedWorkRoot = [System.IO.Path]::GetFullPath($workRoot)
    $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    if ($resolvedWorkRoot.StartsWith($resolvedTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedWorkRoot) -like 'libvirtualgamepad-release-*') {
        Remove-Item -LiteralPath $resolvedWorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
