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

    [string] $SignToolPath
)

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

foreach ($file in @($inf, $dll, $catalog)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Driver package is missing '$file'."
    }
}

$infDriverVer = Select-String -LiteralPath $inf -Pattern '^DriverVer=(.+)$' | Select-Object -First 1
if ($null -eq $infDriverVer -or $infDriverVer.Matches[0].Groups[1].Value.Trim() -ne $DriverVer) {
    throw "The staged INF DriverVer does not match -DriverVer '$DriverVer'."
}

$signTool = Resolve-SignTool -ExplicitPath $SignToolPath

# A catalog is the release signature. Verify both the catalog itself and its
# binding to the exact final INF and UMDF DLL. Do not re-sign this DLL after
# the catalog has been created.
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

$files = @(
    'driver/VibeshineVhfGamepad.inf',
    'driver/VibeshineVhfGamepad.dll',
    'driver/VibeshineVhfGamepad.cat'
) | ForEach-Object {
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
    files = $files
}
$manifestPath = Join-Path $PackageDir 'manifest.json'
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -NoNewline -Encoding utf8

Write-Output "Verified signed catalog and final payload binding for $PackageDir"
Write-Output "Wrote final manifest: $manifestPath"
