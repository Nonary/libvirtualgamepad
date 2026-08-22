[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-ZipLayout {
    param([Parameter(Mandatory = $true)][string] $Path)

    $zip = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        return @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') } | Sort-Object)
    } finally {
        $zip.Dispose()
    }
}

function Test-ExactLayout {
    param(
        [Parameter(Mandatory = $true)][object[]] $Actual,
        [Parameter(Mandatory = $true)][object[]] $Expected
    )

    return ((@($Actual | ForEach-Object { [string] $_ }) -join "`n") -ceq
        (@($Expected | ForEach-Object { [string] $_ }) -join "`n"))
}

$expectedLayout = @(
    'driver/VibeshineVhfGamepad.cat',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.inf',
    'manifest.json',
    'tools/VibeshineVhfGamepadDeviceSetup.exe'
)
$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('libvirtualgamepad-catalog-case-' + [guid]::NewGuid().ToString('N'))
$canonicalPackage = Join-Path $testRoot 'canonical'
$lowercasePackage = Join-Path $testRoot 'lowercase'
$unexpectedPackage = Join-Path $testRoot 'unexpected'
$canonicalZip = Join-Path $testRoot 'canonical.zip'
$lowercaseZip = Join-Path $testRoot 'lowercase.zip'

try {
    foreach ($packageDir in @($canonicalPackage, $lowercasePackage)) {
        New-Item -ItemType Directory -Path (Join-Path $packageDir 'driver') -Force | Out-Null
        New-Item -ItemType Directory -Path (Join-Path $packageDir 'tools') -Force | Out-Null
        [System.IO.File]::WriteAllBytes((Join-Path $packageDir 'driver/vibeshinevhfgamepad.cat'), [byte[]] @(1, 2, 3))
        [System.IO.File]::WriteAllBytes((Join-Path $packageDir 'driver/VibeshineVhfGamepad.dll'), [byte[]] @(4))
        [System.IO.File]::WriteAllBytes((Join-Path $packageDir 'driver/VibeshineVhfGamepad.inf'), [byte[]] @(5))
        [System.IO.File]::WriteAllBytes((Join-Path $packageDir 'manifest.json'), [byte[]] @(6))
        [System.IO.File]::WriteAllBytes((Join-Path $packageDir 'tools/VibeshineVhfGamepadDeviceSetup.exe'), [byte[]] @(7))
    }

    $normalizer = Join-Path (Split-Path -Parent $PSScriptRoot) 'normalize-driver-catalog.ps1'
    $catalogHashBefore = (Get-FileHash -LiteralPath (Join-Path $canonicalPackage 'driver/vibeshinevhfgamepad.cat') -Algorithm SHA256).Hash
    $catalogPath = & $normalizer -DriverDir (Join-Path $canonicalPackage 'driver')
    $catalogName = (Get-ChildItem -LiteralPath (Join-Path $canonicalPackage 'driver') -File | Where-Object {
        $_.Extension -ieq '.cat'
    }).Name
    if ($catalogName -cne 'VibeshineVhfGamepad.cat' -or
        (Split-Path -Leaf $catalogPath) -cne 'VibeshineVhfGamepad.cat') {
        throw "Catalog normalization preserved unexpected casing '$catalogName'."
    }
    $catalogHashAfter = (Get-FileHash -LiteralPath $catalogPath -Algorithm SHA256).Hash
    if ($catalogHashAfter -cne $catalogHashBefore) {
        throw 'Catalog normalization changed the generated catalog bytes.'
    }

    New-Item -ItemType Directory -Path (Join-Path $unexpectedPackage 'driver') -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $unexpectedPackage 'driver/Other.cat'), [byte[]] @(8))
    $unexpectedNameRejected = $false
    try {
        & $normalizer -DriverDir (Join-Path $unexpectedPackage 'driver') | Out-Null
    } catch {
        if ($_.Exception.Message -like "Inf2Cat generated unexpected catalog 'Other.cat';*") {
            $unexpectedNameRejected = $true
        } else {
            throw
        }
    }
    if (-not $unexpectedNameRejected) {
        throw 'Catalog normalization accepted a different catalog name.'
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($canonicalPackage, $canonicalZip)
    [System.IO.Compression.ZipFile]::CreateFromDirectory($lowercasePackage, $lowercaseZip)

    $canonicalLayout = Get-ZipLayout -Path $canonicalZip
    if (-not (Test-ExactLayout -Actual $canonicalLayout -Expected $expectedLayout)) {
        throw "The normalized ZIP does not match the canonical case-sensitive layout: $($canonicalLayout -join ', ')"
    }

    $lowercaseLayout = Get-ZipLayout -Path $lowercaseZip
    if (Test-ExactLayout -Actual $lowercaseLayout -Expected $expectedLayout) {
        throw 'The case-sensitive ZIP contract accepted a lowercase catalog entry.'
    }

    Write-Output 'Verified canonical catalog normalization and case-sensitive ZIP layout rejection.'
} finally {
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    if ($resolvedTestRoot.StartsWith($resolvedTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolvedTestRoot) -like 'libvirtualgamepad-catalog-case-*') {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
