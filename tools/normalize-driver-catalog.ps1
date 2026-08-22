[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $DriverDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$canonicalName = 'VibeshineVhfGamepad.cat'
$DriverDir = [System.IO.Path]::GetFullPath($DriverDir)
if (-not (Test-Path -LiteralPath $DriverDir -PathType Container)) {
    throw "The driver package directory does not exist: $DriverDir"
}

$catalogs = @(
    Get-ChildItem -LiteralPath $DriverDir -File -Force | Where-Object {
        $_.Extension -ieq '.cat'
    }
)
if ($catalogs.Count -ne 1) {
    throw "Expected exactly one generated catalog in '$DriverDir'; found $($catalogs.Count)."
}

$generatedCatalog = $catalogs[0]
if ($generatedCatalog.Name -ine $canonicalName) {
    throw "Inf2Cat generated unexpected catalog '$($generatedCatalog.Name)'; expected '$canonicalName' ignoring case."
}

$canonicalPath = Join-Path $DriverDir $canonicalName
if ($generatedCatalog.Name -cne $canonicalName) {
    $temporaryPath = Join-Path $DriverDir ('.catalog-case-' + [guid]::NewGuid().ToString('N') + '.tmp')
    [System.IO.File]::Move($generatedCatalog.FullName, $temporaryPath)
    try {
        [System.IO.File]::Move($temporaryPath, $canonicalPath)
    } catch {
        $renameError = $_
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            [System.IO.File]::Move($temporaryPath, $generatedCatalog.FullName)
        }
        throw $renameError
    }
}

$canonicalCatalogs = @(
    Get-ChildItem -LiteralPath $DriverDir -File -Force | Where-Object {
        $_.Name -ceq $canonicalName
    }
)
if ($canonicalCatalogs.Count -ne 1) {
    throw "The generated catalog does not have the canonical release name '$canonicalName'."
}

return $canonicalCatalogs[0].FullName
