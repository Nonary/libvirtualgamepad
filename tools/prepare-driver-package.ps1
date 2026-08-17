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
    [string] $Inf2CatPath
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

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "$Name was not found. Install a matching Windows SDK/WDK or pass -$($Name -replace '\.exe$','')Path."
    }
    return $command.Source
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

Write-Output "Prepared unsigned $Platform driver package in $OutputDir"
Write-Output 'Next: SignPath-sign only driver/VibeshineVhfGamepad.cat, then run tools/verify-driver-package.ps1 to verify final catalog membership and write manifest.json.'
