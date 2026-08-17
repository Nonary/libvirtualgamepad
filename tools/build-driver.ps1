[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [ValidateSet('x64', 'ARM64')]
    [string] $Platform = 'x64',

    [string] $MSBuildPath
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repoRoot 'driver/VibeshineVhfGamepad.vcxproj'

if ([string]::IsNullOrWhiteSpace($MSBuildPath)) {
    $candidate = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($null -eq $candidate) {
        throw 'msbuild.exe was not found. Open a Developer PowerShell with the Windows 11 WDK installed, or pass -MSBuildPath.'
    }
    $MSBuildPath = $candidate.Source
}

& $MSBuildPath $project '/t:Build' "/p:Configuration=$Configuration" "/p:Platform=$Platform" '/m'
if ($LASTEXITCODE -ne 0) {
    throw "UMDF build failed with exit code $LASTEXITCODE."
}

$driver = Join-Path $repoRoot "build/$Platform/$Configuration/VibeshineVhfGamepad.dll"
if (-not (Test-Path -LiteralPath $driver -PathType Leaf)) {
    throw "Build completed without the expected driver DLL: $driver"
}

Write-Output "Built $driver"
Write-Output 'Release packaging is intentionally separate: copy final INF/DLL, run Inf2Cat, SignPath-sign the catalog, verify catalog membership, then publish the package.'
