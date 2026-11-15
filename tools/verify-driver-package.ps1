[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $PackageDir,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'ARM64')]
    [string] $Platform,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d{2}/\d{2}/\d{4},\d+\.\d+\.\d+\.\d+$')]
    [string] $DriverVer,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{40,64}$')]
    [string] $SourceRevision,

    [ValidateRange(1, 65535)]
    [uint16] $ProtocolVersion,

    [string] $CatalogGenerationEvidencePath,

    [string] $SignToolPath,

    # Attestation/HLK-signed catalog for the exact locally built mapper INF/SYS.
    [string] $KernelSignedMapperCatalog,
    [switch] $RequireKernelSigning,

    [switch] $AllowLocalTestCertificate,

    # Write a manifest for a package that is shipped unsigned on purpose,
    # because its catalog is signed downstream by the consuming installer's
    # signing request. SignPath is authorised for Nonary/vibeshine and not for
    # this repository, so a release here cannot carry a production signature.
    #
    # This mode requires fresh Inf2Cat generation evidence and proves that every
    # signable producer payload is unsigned. An unsigned catalog cannot use
    # SignTool's signed-catalog membership check; the evidence instead binds the
    # exact INF, DLL, and CAT hashes generated together in the fresh package.
    # The CAT and setup-tool hashes will change when the consumer signs them.
    [switch] $UnsignedForMsiSigning
)

if ($UnsignedForMsiSigning -and ($RequireKernelSigning -or -not [string]::IsNullOrWhiteSpace($KernelSignedMapperCatalog))) {
    throw 'The unsigned producer release cannot import or require a signed mapper catalog. Future signed-package verification is a separate explicit mode.'
}
if ($UnsignedForMsiSigning -and $AllowLocalTestCertificate) {
    throw 'A package signed downstream cannot also be a local-test package.'
}
if (-not $UnsignedForMsiSigning -and -not [string]::IsNullOrWhiteSpace($CatalogGenerationEvidencePath)) {
    throw '-CatalogGenerationEvidencePath is valid only with -UnsignedForMsiSigning.'
}

$ErrorActionPreference = 'Stop'

function Resolve-SignTool {
    param([string] $ExplicitPath)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "signtool.exe was not found at '$ExplicitPath'."
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $command = Get-Command 'signtool.exe' -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw 'signtool.exe was not found. Install a matching Windows SDK or pass -SignToolPath.'
    }
    return $command.Source
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-UnsignedAuthenticode {
    param([Parameter(Mandatory = $true)][string] $Path)

    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::NotSigned -or
        $null -ne $signature.SignerCertificate) {
        $signer = if ($null -eq $signature.SignerCertificate) {
            '<none>'
        } else {
            $signature.SignerCertificate.Subject
        }
        throw "Unsigned release payload '$Path' has Authenticode status '$($signature.Status)' and signer '$signer'."
    }
}

$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)
$driverDir = Join-Path $PackageDir 'driver'
$inf = Join-Path $driverDir 'VibeshineVhfGamepad.inf'
$dll = Join-Path $driverDir 'VibeshineVhfGamepad.dll'
$catalog = Join-Path $driverDir 'VibeshineVhfGamepad.cat'
$mapperInf = Join-Path $PackageDir 'mouse_mapper/VibeshineVhfMouseMapper.inf'
$mapperSys = Join-Path $PackageDir 'mouse_mapper/VibeshineVhfMouseMapper.sys'
$mapperCatalog = Join-Path $PackageDir 'mouse_mapper/VibeshineVhfMouseMapper.cat'
$certificatePath = Join-Path $driverDir 'VibeshineVhfGamepad.cer'
$deviceSetup = Join-Path $PackageDir 'tools/VibeshineVhfGamepadDeviceSetup.exe'

foreach ($file in @($inf, $dll, $catalog, $deviceSetup, $mapperInf, $mapperSys, $mapperCatalog)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Driver package is missing '$file'."
    }
}

foreach ($stagedInf in @($inf, $mapperInf)) {
    $infDriverVer = Select-String -LiteralPath $stagedInf -Pattern '^DriverVer=(.+)$' | Select-Object -First 1
    if ($null -eq $infDriverVer -or $infDriverVer.Matches[0].Groups[1].Value.Trim() -ne $DriverVer) {
        throw "The staged INF DriverVer does not match -DriverVer '$DriverVer'."
    }
}

$identityMatches = [regex]::Matches((Get-Content -LiteralPath $mapperInf -Raw), 'HID\\VID_([0-9A-Fa-f]{4})&PID_([0-9A-Fa-f]{4})&REV_0100&Col02')
if ($identityMatches.Count -ne 1) { throw 'Mapper INF must contain exactly one owned revision/collection identity.' }
$mapperVendorId = [Convert]::ToUInt16($identityMatches[0].Groups[1].Value, 16)
$mapperProductId = [Convert]::ToUInt16($identityMatches[0].Groups[2].Value, 16)

$catalogMembership = $null
if ($UnsignedForMsiSigning) {
    if ([string]::IsNullOrWhiteSpace($CatalogGenerationEvidencePath)) {
        throw '-UnsignedForMsiSigning requires -CatalogGenerationEvidencePath from the successful Inf2Cat invocation.'
    }
    $CatalogGenerationEvidencePath = [System.IO.Path]::GetFullPath($CatalogGenerationEvidencePath)
    if (-not (Test-Path -LiteralPath $CatalogGenerationEvidencePath -PathType Leaf)) {
        throw "Catalog generation evidence is missing: $CatalogGenerationEvidencePath"
    }
    $generationEvidence = Get-Content -LiteralPath $CatalogGenerationEvidencePath -Raw | ConvertFrom-Json
    $evidencePackageDir = [System.IO.Path]::GetFullPath([string] $generationEvidence.package_dir)
    if ($generationEvidence.schema_version -ne 1 -or
        $generationEvidence.generator -cne 'Inf2Cat' -or
        -not $evidencePackageDir.Equals($PackageDir, [System.StringComparison]::OrdinalIgnoreCase) -or
        $generationEvidence.platform -cne $Platform -or
        $generationEvidence.driver_ver -cne $DriverVer) {
        throw 'Catalog generation evidence does not match this package, platform, or DriverVer.'
    }

    $membershipFiles = [ordered]@{
        'mouse_mapper/VibeshineVhfMouseMapper.inf' = $mapperInf
        'mouse_mapper/VibeshineVhfMouseMapper.sys' = $mapperSys
        'mouse_mapper/VibeshineVhfMouseMapper.cat' = $mapperCatalog
        'driver/VibeshineVhfGamepad.inf' = $inf
        'driver/VibeshineVhfGamepad.dll' = $dll
        'driver/VibeshineVhfGamepad.cat' = $catalog
    }
    $membershipHashes = [ordered]@{}
    $evidenceFileNames = @($generationEvidence.files.PSObject.Properties.Name | Sort-Object)
    $expectedMembershipNames = @($membershipFiles.Keys | Sort-Object)
    if (($evidenceFileNames -join "`n") -cne ($expectedMembershipNames -join "`n")) {
        throw 'Catalog generation evidence contains an unexpected file set.'
    }
    foreach ($entry in $membershipFiles.GetEnumerator()) {
        $actualHash = Get-Sha256 -Path $entry.Value
        $evidenceHash = [string] $generationEvidence.files.PSObject.Properties[$entry.Key].Value
        if ($actualHash -cne $evidenceHash) {
            throw "Catalog generation evidence hash mismatch for '$($entry.Key)'."
        }
        $membershipHashes[$entry.Key] = $actualHash
    }
    $catalogMembership = [ordered]@{
        basis = 'fresh-inf2cat'
        generator = 'Inf2Cat'
        files = $membershipHashes
    }
}

$signTool = $null
if (-not $UnsignedForMsiSigning) {
    $signTool = Resolve-SignTool -ExplicitPath $SignToolPath
}
# Importing a Microsoft-signed CAT never changes the catalog-bound INF/SYS.
# Verify the replacement against those exact bytes before publishing hashes.
$kernelSigning = 'unsigned-producer'
if (-not [string]::IsNullOrWhiteSpace($KernelSignedMapperCatalog)) {
    $signTool = Resolve-SignTool -ExplicitPath $SignToolPath
    foreach ($payload in @($mapperInf, $mapperSys)) {
        & $signTool verify '/v' '/kp' '/c' $KernelSignedMapperCatalog $payload
        if ($LASTEXITCODE -ne 0) { throw "Microsoft kernel catalog policy/membership verification failed for '$payload'." }
    }
    Copy-Item -LiteralPath $KernelSignedMapperCatalog -Destination $mapperCatalog -Force
    $kernelSigning = 'windows-kernel-policy'
    if ($null -ne $catalogMembership) {
        $catalogMembership.files['mouse_mapper/VibeshineVhfMouseMapper.cat'] = Get-Sha256 -Path $mapperCatalog
    }
} elseif ($RequireKernelSigning -or (-not $UnsignedForMsiSigning -and -not $AllowLocalTestCertificate)) {
    $signTool = Resolve-SignTool -ExplicitPath $SignToolPath
    foreach ($payload in @($mapperInf, $mapperSys)) {
        & $signTool verify '/v' '/kp' '/c' $mapperCatalog $payload
        if ($LASTEXITCODE -ne 0) { throw "Production mapper requires a Microsoft attestation/HLK-signed catalog bound to '$payload'. Ordinary MSI signing is insufficient." }
    }
    $kernelSigning = 'windows-kernel-policy'
}
$testCertificate = $null
if (Test-Path -LiteralPath $certificatePath -PathType Leaf) {
    if (-not $AllowLocalTestCertificate) {
        throw 'Refusing a self-signed local-test package without -AllowLocalTestCertificate.'
    }
    $testCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
} elseif ($AllowLocalTestCertificate) {
    throw '-AllowLocalTestCertificate requires driver/VibeshineVhfGamepad.cer.'
}
$usesLocalTestCertificate = $null -ne $testCertificate

if (-not $PSBoundParameters.ContainsKey('ProtocolVersion')) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $protocolHeader = Join-Path $repoRoot 'include/libvirtualgamepad/protocol.h'
    $protocolMatch = [regex]::Match(
        (Get-Content -LiteralPath $protocolHeader -Raw),
        'k_protocol_version\s*=\s*(\d+)'
    )
    if (-not $protocolMatch.Success) {
        throw "Could not derive the protocol version from '$protocolHeader'. Pass -ProtocolVersion explicitly."
    }
    $ProtocolVersion = [uint16] $protocolMatch.Groups[1].Value
}

# A signed catalog permits SignTool to verify both the signature and membership
# of the exact final INF/DLL. The unsigned producer path cannot use that signed
# membership check; it requires the fresh Inf2Cat hash binding above instead.
# Do not re-sign the DLL after the catalog has been created.
if (-not $usesLocalTestCertificate -and -not $UnsignedForMsiSigning) {
    & $signTool verify '/v' '/pa' $catalog
    if ($LASTEXITCODE -ne 0) {
        throw "Catalog signature verification failed with exit code $LASTEXITCODE."
    }

    foreach ($payload in @($inf, $dll)) {
        & $signTool verify '/v' '/pa' '/c' $catalog $payload
        if ($LASTEXITCODE -ne 0) {
            throw "Catalog membership verification failed for '$payload' with exit code $LASTEXITCODE."
        }
    }
}

$catalogSignature = if ($UnsignedForMsiSigning) { $null } else { Get-AuthenticodeSignature -LiteralPath $catalog }
if (-not $UnsignedForMsiSigning -and $null -eq $catalogSignature.SignerCertificate) {
    throw 'Catalog verification succeeded without an inspectable signer certificate.'
}
if (-not $UnsignedForMsiSigning -and
    $catalogSignature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) {
    throw 'Catalog inspection reported a hash mismatch.'
}
if ($usesLocalTestCertificate -and $catalogSignature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant() -ne $testCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant()) {
    throw 'The package public certificate does not match the catalog signer.'
}

# This executable creates/removes only the package-owned root device. It is
# intentionally outside the catalog, so it must carry an embedded signature.
if (-not $usesLocalTestCertificate -and -not $UnsignedForMsiSigning) {
    & $signTool verify '/v' '/pa' $deviceSetup
    if ($LASTEXITCODE -ne 0) {
        throw "Root-device setup tool signature verification failed with exit code $LASTEXITCODE."
    }
}
$deviceSetupSignature = if ($UnsignedForMsiSigning) { $null } else { Get-AuthenticodeSignature -LiteralPath $deviceSetup }
if (-not $UnsignedForMsiSigning) {
    if ($null -eq $deviceSetupSignature.SignerCertificate) {
        throw 'Root-device setup tool verification succeeded without an inspectable signer certificate.'
    }
    if ($deviceSetupSignature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) {
        throw 'Root-device setup tool inspection reported a hash mismatch.'
    }
}
if ($usesLocalTestCertificate -and $deviceSetupSignature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant() -ne $testCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant()) {
    throw 'The package public certificate does not match the root-device setup tool signer.'
}

if ($UnsignedForMsiSigning) {
    if ($kernelSigning -ceq 'unsigned-producer') {
        Assert-UnsignedAuthenticode -Path $mapperCatalog
        Assert-UnsignedAuthenticode -Path $mapperSys
    }
    foreach ($unsignedPayload in @($catalog, $dll, $deviceSetup)) {
        Assert-UnsignedAuthenticode -Path $unsignedPayload
    }
}

$signingChannel = if ($UnsignedForMsiSigning) { 'msi-request-signing' } else { 'external-catalog-signing' }
if ($usesLocalTestCertificate) {
    $catalogThumbprint = $catalogSignature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
    $certificateThumbprint = $testCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
    if ($catalogThumbprint -ne $certificateThumbprint) {
        throw 'The package public certificate does not match the catalog signer.'
    }
    $deviceSetupThumbprint = $deviceSetupSignature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
    if ($deviceSetupThumbprint -ne $certificateThumbprint) {
        throw 'The package public certificate does not match the root-device setup tool signer.'
    }
    $signingChannel = 'self-signed-local-test'
    $kernelSigning = 'self-signed-local-test'
    foreach ($payload in @($mapperCatalog, $mapperSys)) {
        $signature = Get-AuthenticodeSignature -LiteralPath $payload
        if ($null -eq $signature.SignerCertificate -or $signature.Status -eq 'HashMismatch' -or $signature.SignerCertificate.Thumbprint -ine $certificateThumbprint) { throw "Local mapper signer mismatch: $payload" }
    }
}

$relativeFiles = @(
    'mouse_mapper/VibeshineVhfMouseMapper.inf',
    'mouse_mapper/VibeshineVhfMouseMapper.sys',
    'mouse_mapper/VibeshineVhfMouseMapper.cat',
    'driver/VibeshineVhfGamepad.inf',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.cat',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
if (Test-Path -LiteralPath $certificatePath -PathType Leaf) {
    $relativeFiles += 'driver/VibeshineVhfGamepad.cer'
}

$files = $relativeFiles | ForEach-Object {
    $relativePath = $_
    $fullPath = Join-Path $PackageDir $relativePath
    [ordered]@{
        path = $relativePath.Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$manifest = [ordered]@{
    schema_version = 1
    features = [ordered]@{ mouse_mapper = 1 }
    mouse_mapper = [ordered]@{ transport_version = 1; vendor_id = $mapperVendorId; product_id = $mapperProductId; kernel_signing = $kernelSigning }
    source_revision = $SourceRevision.ToLowerInvariant()
    driver_ver = $DriverVer
    platform = $Platform
    protocol_version = $ProtocolVersion
    signing = if ($UnsignedForMsiSigning) {
        # No signer exists yet. Naming the files that will be signed keeps the
        # manifest self-describing, so a consumer does not have to infer which
        # hashes above are expected to go stale.
        [ordered]@{
            channel = $signingChannel
            signed_downstream = @(
                'driver/VibeshineVhfGamepad.cat',
                'tools/VibeshineVhfGamepadDeviceSetup.exe'
            )
            unsigned_payloads = @(
                'driver/VibeshineVhfGamepad.cat',
                'driver/VibeshineVhfGamepad.dll',
                'mouse_mapper/VibeshineVhfMouseMapper.cat',
                'mouse_mapper/VibeshineVhfMouseMapper.sys',
                'tools/VibeshineVhfGamepadDeviceSetup.exe'
            )
            catalog_membership = $catalogMembership
        }
    } else {
        [ordered]@{
            channel = $signingChannel
            signer_subject = $catalogSignature.SignerCertificate.Subject
            signer_thumbprint = $catalogSignature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
            device_setup_signer_subject = $deviceSetupSignature.SignerCertificate.Subject
            device_setup_signer_thumbprint = $deviceSetupSignature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
        }
    }
    files = $files
}
$manifestPath = Join-Path $PackageDir 'manifest.json'
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -NoNewline -Encoding utf8

if ($UnsignedForMsiSigning) {
    Write-Output "Wrote an unsigned manifest for $PackageDir"
    Write-Output 'The consuming installer must sign driver/VibeshineVhfGamepad.cat and tools/VibeshineVhfGamepadDeviceSetup.exe in its own signing request.'
} elseif ($usesLocalTestCertificate) {
    Write-Output "Verified local signer identity for $PackageDir"
} else {
    Write-Output "Verified signed catalog and final payload binding for $PackageDir"
}
Write-Output "Wrote final manifest: $manifestPath"
