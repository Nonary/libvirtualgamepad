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
    [switch] $SkipInfVerif,
    [string] $Inf2CatPath,
    [string] $CatalogGenerationEvidencePath,
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

# The WDK's Visual Studio integration is versioned against one MSBuild and one
# set of platform toolsets. A newer Visual Studio, or an install without the
# Spectre-mitigated runtime libraries, otherwise fails the driver project with
# errors that say nothing about drivers. Detect those exact mismatches and pass
# the corresponding overrides instead of asking the caller to guess them.
function Resolve-WdkContentRoot {
    foreach ($key in @(
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Windows Kits\Installed Roots'
    )) {
        $root = (Get-ItemProperty -Path $key -Name 'KitsRoot10' -ErrorAction SilentlyContinue).KitsRoot10
        if (-not [string]::IsNullOrWhiteSpace($root)) {
            return $root.TrimEnd('\')
        }
    }
    return $null
}

function Get-DriverProjectOverrides {
    param(
        [Parameter(Mandatory = $true)][string] $MSBuildPath,
        [Parameter(Mandatory = $true)][string] $Platform,
        [Parameter(Mandatory = $true)][string] $KitVersion
    )

    $overrides = @()
    $msbuildMajor = (Get-Item -LiteralPath $MSBuildPath).VersionInfo.FileMajorPart

    $wdkRoot = Resolve-WdkContentRoot
    if ($null -ne $wdkRoot) {
        $taskDir = Join-Path $wdkRoot "build\$KitVersion\bin"
        if (Test-Path -LiteralPath $taskDir -PathType Container) {
            $expected = Join-Path $taskDir "Microsoft.DriverKit.Build.Tasks.$msbuildMajor.0.dll"
            if (-not (Test-Path -LiteralPath $expected -PathType Leaf)) {
                $available = @(Get-ChildItem -LiteralPath $taskDir -Filter 'Microsoft.DriverKit.Build.Tasks.*.dll' -ErrorAction SilentlyContinue |
                    ForEach-Object { if ($_.Name -match 'Tasks\.(\d+)\.0\.dll$') { [int] $Matches[1] } } |
                    Sort-Object -Descending)
                if ($available.Count -eq 0) {
                    throw "The Windows Kit at '$wdkRoot' has no MSBuild task assembly for driver projects. Install the WDK build tools for kit $KitVersion."
                }
                Write-Warning "MSBuild $msbuildMajor has no matching WDK task assembly; building the driver as Visual Studio $($available[0]).0."
                $overrides += "/p:VisualStudioVersion=$($available[0]).0"
            }
        }
    }

    # The WDK toolset imports v143 (or v142) from the Visual Studio it shipped
    # for. A Visual Studio that only carries a newer toolset needs that toolset
    # named explicitly, otherwise the import fails before the driver compiles.
    $msbuildRoot = (Get-Item -LiteralPath $MSBuildPath).Directory
    while ($null -ne $msbuildRoot -and $msbuildRoot.Name -ne 'MSBuild') {
        $msbuildRoot = $msbuildRoot.Parent
    }
    if ($null -eq $msbuildRoot) {
        # An unrecognized layout is not worth guessing at: build with defaults.
        return $overrides
    }
    $platformToolsets = @(Get-ChildItem -Path (Join-Path $msbuildRoot.FullName 'Microsoft\VC') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "Platforms\$Platform\PlatformToolsets" } |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'WindowsUserModeDriver10.0') -PathType Container })
    foreach ($toolsetDir in $platformToolsets) {
        if ((Test-Path -LiteralPath (Join-Path $toolsetDir 'v143') -PathType Container) -or
            (Test-Path -LiteralPath (Join-Path $toolsetDir 'v142') -PathType Container)) {
            continue
        }
        $fallback = @(Get-ChildItem -LiteralPath $toolsetDir -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^v\d+$' } | Sort-Object Name -Descending | Select-Object -First 1)
        if ($fallback.Count -eq 0) {
            continue
        }
        Write-Warning "The WDK toolset expects the v143 platform toolset; building the driver with $($fallback[0].Name) instead."
        $overrides += "/p:V143PropsFile=$(Join-Path $fallback[0].FullName 'Toolset.props')"
        $overrides += "/p:V143TargetsFile=$(Join-Path $fallback[0].FullName 'Toolset.targets')"
    }

    # Driver projects default to Spectre-mitigated libraries. Report the missing
    # component instead of failing with MSB8040, and keep local test builds
    # possible on a host that never installed it.
    $spectre = @(Get-ChildItem -Path (Join-Path $msbuildRoot.Parent.FullName 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "lib\spectre\$Platform") -PathType Container })
    if ($spectre.Count -eq 0) {
        Write-Warning 'Spectre-mitigated libraries are not installed; building the driver without Spectre mitigation. Install them before producing a release package.'
        $overrides += '/p:SpectreMitigation=false'
    }

    return $overrides
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$driverProject = Join-Path $repoRoot 'driver/VibeshineVhfGamepad.vcxproj'
$deviceSetupProject = Join-Path $repoRoot 'tools/device_setup/VibeshineVhfGamepadDeviceSetup.vcxproj'
$driverVer = Resolve-DriverVer -ExplicitDriverVer $DriverVer -RepositoryRoot $repoRoot
$msbuild = Resolve-MSBuild -ExplicitPath $MSBuildPath

if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $safeDriverVer = $driverVer -replace '[/,: ]', '-'
    $PackageDir = Join-Path $repoRoot "artifacts/vhf-gamepad-$Platform-$Configuration-$safeDriverVer"
}

$kitVersion = '10.0.26100.0'
$driverOverrides = Get-DriverProjectOverrides -MSBuildPath $msbuild -Platform $Platform -KitVersion $kitVersion

foreach ($project in @($driverProject, $deviceSetupProject)) {
    if (-not (Test-Path -LiteralPath $project -PathType Leaf)) {
        throw "Build project is missing: $project"
    }
    # Only the UMDF driver project goes through the WDK toolset, so only it
    # needs the host-compatibility overrides.
    $projectArguments = @($project, '/t:Build', "/p:Configuration=$Configuration", "/p:Platform=$Platform", '/m')
    if ($project -eq $driverProject) {
        $projectArguments += $driverOverrides
    }
    & $msbuild @projectArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for '$project' with exit code $LASTEXITCODE."
    }
}

$driver = Join-Path $repoRoot "build/$Platform/$Configuration/VibeshineVhfGamepad.dll"
$deviceSetup = Join-Path $repoRoot "build/$Platform/$Configuration/VibeshineVhfGamepadDeviceSetup.exe"
if (-not (Test-Path -LiteralPath $driver -PathType Leaf)) {
    throw "Build completed without the expected driver DLL: $driver"
}
if (-not (Test-Path -LiteralPath $deviceSetup -PathType Leaf)) {
    throw "Build completed without the expected root-device setup tool: $deviceSetup"
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
if ($SkipInfVerif) {
    $prepareParameters.SkipInfVerif = $true
}
if (-not [string]::IsNullOrWhiteSpace($Inf2CatPath)) {
    $prepareParameters.Inf2CatPath = $Inf2CatPath
}
if (-not [string]::IsNullOrWhiteSpace($CatalogGenerationEvidencePath)) {
    $prepareParameters.CatalogGenerationEvidencePath = $CatalogGenerationEvidencePath
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
Write-Output "Built $deviceSetup"
Write-Output "Prepared $SigningMode package: $([System.IO.Path]::GetFullPath($PackageDir))"
if ($SigningMode -eq 'LocalTest') {
    Write-Output 'Next: run tools/trust-test-certificate.ps1 elevated on the test host, then run tools/verify-driver-package.ps1.'
} else {
    Write-Output 'Next: SignPath-sign driver/VibeshineVhfGamepad.cat and tools/VibeshineVhfGamepadDeviceSetup.exe, then run tools/verify-driver-package.ps1.'
}
