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

    [string] $SignToolPath,

    [switch] $AllowLocalTestCertificate,

    # Write a manifest for a package that is shipped unsigned on purpose,
    # because its catalog is signed downstream by the consuming installer's
    # signing request. SignPath is authorised for Nonary/vibeshine and not for
    # this repository, so a release here cannot carry a production signature.
    #
    # Everything except the signature checks still runs. The recorded hashes
    # for the catalog and the root-device tool are pre-signature and will not
    # match once those files are signed; the consumer knows to skip exactly
    # those two and to require a valid signature on them instead.
    [switch] $UnsignedForMsiSigning
)

if ($UnsignedForMsiSigning -and $AllowLocalTestCertificate) {
    throw 'A package signed downstream cannot also be a local-test package.'
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

$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)
$driverDir = Join-Path $PackageDir 'driver'
$inf = Join-Path $driverDir 'VibeshineVhfGamepad.inf'
$dll = Join-Path $driverDir 'VibeshineVhfGamepad.dll'
$catalog = Join-Path $driverDir 'VibeshineVhfGamepad.cat'
$certificatePath = Join-Path $driverDir 'VibeshineVhfGamepad.cer'
$deviceSetup = Join-Path $PackageDir 'tools/VibeshineVhfGamepadDeviceSetup.exe'

foreach ($file in @($inf, $dll, $catalog, $deviceSetup)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Driver package is missing '$file'."
    }
}

$infDriverVer = Select-String -LiteralPath $inf -Pattern '^DriverVer=(.+)$' | Select-Object -First 1
if ($null -eq $infDriverVer -or $infDriverVer.Matches[0].Groups[1].Value.Trim() -ne $DriverVer) {
    throw "The staged INF DriverVer does not match -DriverVer '$DriverVer'."
}

$signTool = Resolve-SignTool -ExplicitPath $SignToolPath
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

# A catalog is the release signature. Verify both the catalog itself and its
# binding to the exact final INF and UMDF DLL. Do not re-sign this DLL after
# the catalog has been created.
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
}

$relativeFiles = @(
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
    source_revision = $SourceRevision.ToLowerInvariant()
    driver_ver = $DriverVer
    platform = $Platform
    protocol_version = 1
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
