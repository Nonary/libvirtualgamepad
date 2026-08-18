[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'ARM64')]
    [string] $Platform,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d{2}/\d{2}/\d{4},\d+\.\d+\.\d+\.\d+$')]
    [string] $DriverVer,

    [Parameter(Mandatory = $true)]
    [string] $OutputDir,

    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [string] $InfVerifPath,
    [string] $Inf2CatPath,

    [ValidateSet('Release', 'LocalTest')]
    [string] $SigningMode = 'Release',

    [string] $SignToolPath,

    [ValidatePattern('^[0-9a-fA-F ]{40,64}$')]
    [string] $SigningThumbprint
)

$ErrorActionPreference = 'Stop'

function Resolve-SdkTool {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,
        [string] $ExplicitPath
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "$Name was not found at '$ExplicitPath'."
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $sdkRoots = @(
        'D:\Software\WinSDK\bin',
        (Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)) 'Windows Kits\10\bin')
    )
    foreach ($sdkRoot in $sdkRoots) {
        if (-not (Test-Path -LiteralPath $sdkRoot -PathType Container)) {
            continue
        }

        foreach ($architecture in @('x64', 'x86')) {
            $directCandidate = Join-Path $sdkRoot "$architecture/$Name"
            if (Test-Path -LiteralPath $directCandidate -PathType Leaf) {
                return (Resolve-Path -LiteralPath $directCandidate).Path
            }
        }

        $versionDirectories = @(Get-ChildItem -LiteralPath $sdkRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)
        foreach ($versionDirectory in $versionDirectories) {
            foreach ($architecture in @('x64', 'x86')) {
                $candidate = Join-Path $versionDirectory.FullName "$architecture/$Name"
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    return (Resolve-Path -LiteralPath $candidate).Path
                }
            }
        }
    }

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "$Name was not found. Install a matching Windows SDK/WDK or pass -$($Name -replace '\.exe$','')Path."
    }
    return $command.Source
}

function Test-CodeSigningCertificate {
    param(
        [Parameter(Mandatory = $true)]
        [System.Security.Cryptography.X509Certificates.X509Certificate2] $Certificate
    )

    if (-not $Certificate.HasPrivateKey -or $Certificate.NotAfter -le (Get-Date)) {
        return $false
    }

    foreach ($extension in $Certificate.Extensions) {
        if ($extension.Oid.Value -ne '2.5.29.37') {
            continue
        }
        $usage = [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($extension, $false)
        foreach ($oid in $usage.EnhancedKeyUsages) {
            if ($oid.Value -eq '1.3.6.1.5.5.7.3.3') {
                return $true
            }
        }
    }

    return $false
}

function Find-LocalTestSigningCertificate {
    param([string] $RequestedThumbprint)

    $stores = @('Cert:\CurrentUser\My', 'Cert:\LocalMachine\My')
    if (-not [string]::IsNullOrWhiteSpace($RequestedThumbprint)) {
        $normalizedThumbprint = $RequestedThumbprint.Replace(' ', '').ToUpperInvariant()
        $matchingCertificates = @(
            foreach ($store in $stores) {
                Get-ChildItem -Path $store -ErrorAction SilentlyContinue | Where-Object {
                    $_.Thumbprint.Replace(' ', '').ToUpperInvariant() -eq $normalizedThumbprint
                }
            }
        )
        $certificate = @($matchingCertificates | Select-Object -First 1)
        if ($certificate.Count -eq 0) {
            throw "No certificate with thumbprint '$RequestedThumbprint' was found in CurrentUser\\My or LocalMachine\\My."
        }
        if (-not (Test-CodeSigningCertificate -Certificate $certificate[0])) {
            throw "Certificate '$RequestedThumbprint' must be valid, have a private key, and allow Code Signing."
        }
        return $certificate[0]
    }

    $subject = 'CN=Vibeshine VHF Gamepad Test'
    $certificate = @(Get-ChildItem -Path 'Cert:\CurrentUser\My' -ErrorAction SilentlyContinue | Where-Object {
        $_.Subject -eq $subject -and (Test-CodeSigningCertificate -Certificate $_)
    } | Sort-Object NotAfter -Descending | Select-Object -First 1)
    if ($certificate.Count -ne 0) {
        return $certificate[0]
    }

    return New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -KeyUsage DigitalSignature -KeyExportPolicy Exportable -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(5)
}

function Get-CertificateStoreScope {
    param(
        [Parameter(Mandatory = $true)]
        [System.Security.Cryptography.X509Certificates.X509Certificate2] $Certificate
    )

    if ($Certificate.PSParentPath -like '*LocalMachine\My*') {
        return 'LocalMachine'
    }
    return 'CurrentUser'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$template = Join-Path $repoRoot 'driver/VibeshineVhfGamepad.inf.in'
$dll = Join-Path $repoRoot "build/$Platform/$Configuration/VibeshineVhfGamepad.dll"
$arch = if ($Platform -eq 'x64') { 'amd64' } else { 'arm64' }
$inf2CatTarget = if ($Platform -eq 'x64') { '10_X64' } else { '10_ARM64' }

if (-not (Test-Path -LiteralPath $template -PathType Leaf)) {
    throw "INF template is missing: $template"
}
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "Driver DLL is missing: $dll. Run tools/build-driver.ps1 first."
}

$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) {
    $existing = Get-ChildItem -LiteralPath $OutputDir -Force | Select-Object -First 1
    if ($null -ne $existing) {
        throw "Refusing to overwrite a non-empty package directory: $OutputDir"
    }
} else {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

$driverDir = Join-Path $OutputDir 'driver'
New-Item -ItemType Directory -Path $driverDir -Force | Out-Null

$inf = Join-Path $driverDir 'VibeshineVhfGamepad.inf'
$stagedDll = Join-Path $driverDir 'VibeshineVhfGamepad.dll'
$contents = Get-Content -LiteralPath $template -Raw
$contents = $contents.Replace('$ARCH$', $arch).Replace('08/16/2026,0.1.0.0', $DriverVer)
Set-Content -LiteralPath $inf -Value $contents -NoNewline -Encoding utf8
Copy-Item -LiteralPath $dll -Destination $stagedDll

$infVerif = Resolve-SdkTool -Name 'InfVerif.exe' -ExplicitPath $InfVerifPath
$inf2Cat = Resolve-SdkTool -Name 'Inf2Cat.exe' -ExplicitPath $Inf2CatPath

& $infVerif '/w' '/v' $inf
if ($LASTEXITCODE -ne 0) {
    throw "InfVerif failed with exit code $LASTEXITCODE."
}

& $inf2Cat "/driver:$driverDir" "/os:$inf2CatTarget"
if ($LASTEXITCODE -ne 0) {
    throw "Inf2Cat failed with exit code $LASTEXITCODE."
}

$catalog = Join-Path $driverDir 'VibeshineVhfGamepad.cat'
if (-not (Test-Path -LiteralPath $catalog -PathType Leaf)) {
    throw "Inf2Cat completed without the expected catalog: $catalog"
}

if ($SigningMode -eq 'Release') {
    Write-Output "Prepared unsigned $Platform driver package in $OutputDir"
    Write-Output 'Next: SignPath-sign only driver/VibeshineVhfGamepad.cat, then run tools/verify-driver-package.ps1 to verify final catalog membership and write manifest.json.'
    return
}

$signTool = Resolve-SdkTool -Name 'signtool.exe' -ExplicitPath $SignToolPath
$certificate = Find-LocalTestSigningCertificate -RequestedThumbprint $SigningThumbprint
$certificateThumbprint = $certificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
$certificatePath = Join-Path $driverDir 'VibeshineVhfGamepad.cer'
Export-Certificate -Cert $certificate -FilePath $certificatePath -Force | Out-Null

$signArguments = @('sign', '/fd', 'SHA256')
if ((Get-CertificateStoreScope -Certificate $certificate) -eq 'LocalMachine') {
    $signArguments += '/sm'
}
$signArguments += @('/sha1', $certificateThumbprint, $catalog)
& $signTool @signArguments
if ($LASTEXITCODE -ne 0) {
    throw "Local catalog signing failed with exit code $LASTEXITCODE."
}

$catalogSignature = Get-AuthenticodeSignature -LiteralPath $catalog
if ($null -eq $catalogSignature.SignerCertificate) {
    throw 'Local catalog signing completed without a signer certificate.'
}
if ($catalogSignature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) {
    throw 'Local catalog signing produced a catalog hash mismatch.'
}
if ($catalogSignature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant() -ne $certificateThumbprint) {
    throw 'The local catalog signer does not match the exported public certificate.'
}

Write-Output "Prepared locally signed $Platform driver package in $OutputDir"
Write-Output "Exported the public test certificate: $certificatePath"
Write-Output 'The public certificate is not trusted automatically. On each test host, run tools/trust-test-certificate.ps1 elevated, then run tools/verify-driver-package.ps1.'
