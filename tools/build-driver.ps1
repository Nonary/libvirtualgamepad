[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [ValidateSet('x64', 'ARM64')]
    [string] $Platform = 'x64',

    [ValidatePattern('^\d{2}/\d{2}/\d{4},\d+\.\d+\.\d+\.\d+$')]
    [string] $DriverVer,

    [string] $PackageDir,

    [ValidateSet('LocalTest', 'Release')]
    [string] $SigningMode = 'LocalTest',

    [string] $MSBuildPath,
    [string] $InfVerifPath,
    [string] $Inf2CatPath,
    [string] $SignToolPath,

    [ValidatePattern('^[0-9a-fA-F ]{40,64}$')]
    [string] $SigningThumbprint
)

$ErrorActionPreference = 'Stop'

function Resolve-DriverVer {
    param(
        [string] $ExplicitDriverVer,
        [string] $RepositoryRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitDriverVer)) {
        return $ExplicitDriverVer
    }

    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -eq $git) {
        throw 'git.exe was not found. Pass -DriverVer explicitly when Git metadata is unavailable.'
    }

    $dirtyEntries = @(& $git.Source -C $RepositoryRoot status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not inspect the Git worktree for a generated DriverVer (exit code $LASTEXITCODE). Pass -DriverVer explicitly."
    }
    if (@($dirtyEntries | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
        throw 'Refusing to derive DriverVer from a dirty worktree. Commit the source or pass an explicit newer -DriverVer.'
    }

    $date = (& $git.Source -C $RepositoryRoot log -1 '--format=%cd' '--date=format:%m/%d/%Y').Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($date)) {
        throw "Could not derive the DriverVer date from Git (exit code $LASTEXITCODE). Pass -DriverVer explicitly."
    }

    $revisionText = (& $git.Source -C $RepositoryRoot rev-list --count HEAD).Trim()
    $revision = 0
    if ($LASTEXITCODE -ne 0 -or -not [int]::TryParse($revisionText, [ref] $revision)) {
        throw "Could not derive the DriverVer revision from Git (exit code $LASTEXITCODE). Pass -DriverVer explicitly."
    }
    if ($revision -gt 65535) {
        throw "The Git revision count $revision exceeds the DriverVer field limit. Pass -DriverVer explicitly."
    }

    return "$date,0.1.0.$revision"
}

function Resolve-MSBuild {
    param([string] $ExplicitPath)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "msbuild.exe was not found at '$ExplicitPath'."
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $candidate = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($null -eq $candidate) {
        throw 'msbuild.exe was not found. Open a Developer PowerShell with the Windows 11 WDK installed, or pass -MSBuildPath.'
    }
    return $candidate.Source
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot 'driver/VibeshineVhfGamepad.vcxproj'
$driverVer = Resolve-DriverVer -ExplicitDriverVer $DriverVer -RepositoryRoot $repoRoot
$msbuild = Resolve-MSBuild -ExplicitPath $MSBuildPath

if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $safeDriverVer = $driverVer -replace '[/,: ]', '-'
    $PackageDir = Join-Path $repoRoot "artifacts/vhf-gamepad-$Platform-$Configuration-$safeDriverVer"
}

& $msbuild $project '/t:Build' "/p:Configuration=$Configuration" "/p:Platform=$Platform" '/m'
if ($LASTEXITCODE -ne 0) {
    throw "UMDF build failed with exit code $LASTEXITCODE."
}

$driver = Join-Path $repoRoot "build/$Platform/$Configuration/VibeshineVhfGamepad.dll"
if (-not (Test-Path -LiteralPath $driver -PathType Leaf)) {
    throw "Build completed without the expected driver DLL: $driver"
}

$prepare = Join-Path $PSScriptRoot 'prepare-driver-package.ps1'
$prepareParameters = @{
    Platform = $Platform
    DriverVer = $driverVer
    OutputDir = $PackageDir
    Configuration = $Configuration
    SigningMode = $SigningMode
}
if (-not [string]::IsNullOrWhiteSpace($InfVerifPath)) {
    $prepareParameters.InfVerifPath = $InfVerifPath
}
if (-not [string]::IsNullOrWhiteSpace($Inf2CatPath)) {
    $prepareParameters.Inf2CatPath = $Inf2CatPath
}
if (-not [string]::IsNullOrWhiteSpace($SignToolPath)) {
    $prepareParameters.SignToolPath = $SignToolPath
}
if (-not [string]::IsNullOrWhiteSpace($SigningThumbprint)) {
    $prepareParameters.SigningThumbprint = $SigningThumbprint
}

& $prepare @prepareParameters
if ($LASTEXITCODE -ne 0) {
    throw "Driver package preparation failed with exit code $LASTEXITCODE."
}

Write-Output "Built $driver"
Write-Output "Prepared $SigningMode package: $([System.IO.Path]::GetFullPath($PackageDir))"
if ($SigningMode -eq 'LocalTest') {
    Write-Output 'Next: run tools/trust-test-certificate.ps1 elevated on the test host, then run tools/verify-driver-package.ps1.'
} else {
    Write-Output 'Next: SignPath-sign only driver/VibeshineVhfGamepad.cat, then run tools/verify-driver-package.ps1.'
}
