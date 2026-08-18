[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [string] $PackageDir,

    [switch] $Remove
)

$ErrorActionPreference = 'Stop'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated PowerShell session because it changes the LocalMachine certificate stores.'
    }
}

$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)
$certificatePath = Join-Path $PackageDir 'driver/VibeshineVhfGamepad.cer'
$catalogPath = Join-Path $PackageDir 'driver/VibeshineVhfGamepad.cat'
if (-not (Test-Path -LiteralPath $certificatePath -PathType Leaf)) {
    throw "The local-test package does not contain a public certificate: $certificatePath"
}
if (-not (Test-Path -LiteralPath $catalogPath -PathType Leaf)) {
    throw "The local-test package does not contain a catalog: $catalogPath"
}

Assert-Administrator
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
$thumbprint = $certificate.Thumbprint.Replace(' ', '').ToUpperInvariant()
$catalogSignature = Get-AuthenticodeSignature -LiteralPath $catalogPath
if ($null -eq $catalogSignature.SignerCertificate) {
    throw 'The package catalog has no signer certificate.'
}
if ($catalogSignature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) {
    throw 'The package catalog has a hash mismatch.'
}
if ($catalogSignature.SignerCertificate.Thumbprint.Replace(' ', '').ToUpperInvariant() -ne $thumbprint) {
    throw 'The package public certificate does not match the catalog signer.'
}
$stores = @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')

foreach ($store in $stores) {
    $destination = "$store\$thumbprint"
    $existing = Get-ChildItem -Path $store -ErrorAction Stop | Where-Object {
        $_.Thumbprint.Replace(' ', '').ToUpperInvariant() -eq $thumbprint
    }

    if ($Remove) {
        if ($null -eq $existing) {
            Write-Output "No matching certificate is installed in $store."
            continue
        }
        if ($PSCmdlet.ShouldProcess($destination, 'Remove the explicit local-test certificate')) {
            Remove-Item -LiteralPath $destination -Force
            Write-Output "Removed the local-test certificate from $store."
        }
        continue
    }

    if ($null -ne $existing) {
        Write-Output "The local-test certificate is already trusted in $store."
        continue
    }
    if ($PSCmdlet.ShouldProcess($store, "Trust $($certificate.Subject) for local VHF gamepad testing")) {
        Import-Certificate -FilePath $certificatePath -CertStoreLocation $store | Out-Null
        Write-Output "Trusted the local-test certificate in $store."
    }
}

if ($Remove) {
    Write-Output 'Removed only the certificate explicitly bundled with this local-test package.'
} else {
    Write-Output 'The test certificate is now trusted in Root and TrustedPublisher. This does not install the driver or create a virtual controller.'
}
