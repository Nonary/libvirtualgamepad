# Pure file/evidence helpers shared by prepare, resume and portable fixtures.
# A preparation archive is the exact unsigned compiler output submitted for
# attestation. Resume authenticates its caller-pinned SHA-256 before extraction.
function Get-UnsignedPreparationPayloadPaths {
    return @('catalog-generation.json',
        'package/driver/VibeshineVhfGamepad.inf', 'package/driver/VibeshineVhfGamepad.dll', 'package/driver/VibeshineVhfGamepad.cat',
        'package/mouse_mapper/VibeshineVhfMouseMapper.inf', 'package/mouse_mapper/VibeshineVhfMouseMapper.sys', 'package/mouse_mapper/VibeshineVhfMouseMapper.cat',
        'package/tools/VibeshineVhfGamepadDeviceSetup.exe', 'package/manifest.json')
}
function Get-PreparationSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Write-UnsignedReleasePreparation {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)]$Metadata)
    $record = [ordered]@{ schema_version = 1; stage = 'unsigned-kernel-submission' }
    foreach ($entry in $Metadata.GetEnumerator()) { $record[$entry.Key] = $entry.Value }
    $hashes = [ordered]@{}
    foreach ($relative in @(Get-UnsignedPreparationPayloadPaths | Sort-Object)) { $hashes[$relative] = Get-PreparationSha256 (Join-Path $Root $relative) }
    $record.files = $hashes
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Root 'preparation.json') -Encoding utf8 -NoNewline
}
function Assert-UnsignedReleasePreparation {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)]$ExpectedMetadata)
    $record = Get-Content -LiteralPath (Join-Path $Root 'preparation.json') -Raw | ConvertFrom-Json
    if ($record.schema_version -ne 1 -or $record.stage -cne 'unsigned-kernel-submission') { throw 'Unsupported unsigned preparation record.' }
    foreach ($entry in $ExpectedMetadata.GetEnumerator()) {
        $property = $record.PSObject.Properties[$entry.Key]
        if ($null -eq $property -or [string]$property.Value -cne [string]$entry.Value) { throw "Unsigned preparation metadata mismatch: $($entry.Key)." }
    }
    $expectedPaths = @(Get-UnsignedPreparationPayloadPaths | Sort-Object)
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $actualPaths = @(Get-ChildItem -LiteralPath $Root -Recurse -File | ForEach-Object { $_.FullName.Substring($rootPath.Length).TrimStart('\', '/').Replace('\', '/') } | Sort-Object)
    if (($actualPaths -join ',') -cne (@($expectedPaths + 'preparation.json' | Sort-Object) -join ',') -or
        (@($record.files.PSObject.Properties.Name | Sort-Object) -join ',') -cne ($expectedPaths -join ',')) { throw 'Unsigned preparation file set is not exact.' }
    foreach ($relative in $expectedPaths) {
        $expectedHash = [string]$record.files.PSObject.Properties[$relative].Value
        if ($expectedHash -cnotmatch '^[0-9a-f]{64}$' -or (Get-PreparationSha256 (Join-Path $Root $relative)) -cne $expectedHash) { throw "Unsigned preparation hash mismatch: $relative." }
    }
    $manifest = Get-Content -LiteralPath (Join-Path $Root 'package/manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.source_revision -cne $record.source_revision -or $manifest.driver_ver -cne $record.driver_ver -or
        $manifest.protocol_version -ne $record.protocol_version -or $manifest.platform -cne $record.platform -or
        $manifest.features.mouse_mapper -ne 1 -or $manifest.mouse_mapper.transport_version -ne 1 -or
        $manifest.mouse_mapper.vendor_id -ne $record.vendor_id -or $manifest.mouse_mapper.product_id -ne $record.product_id -or
        $manifest.mouse_mapper.kernel_signing -cne 'requires-microsoft-signing' -or $manifest.signing.channel -cne 'msi-request-signing') { throw 'Unsigned preparation manifest does not match its source/version/identity record.' }
    return $record
}
function Export-UnsignedReleasePreparation {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$ArchivePath)
    if (Test-Path -LiteralPath $ArchivePath) { throw 'Refusing to overwrite an unsigned preparation archive.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($Root, $ArchivePath, [IO.Compression.CompressionLevel]::Optimal, $false)
    return Get-PreparationSha256 $ArchivePath
}
function Import-UnsignedReleasePreparation {
    param([Parameter(Mandatory = $true)][string]$ArchivePath, [Parameter(Mandatory = $true)][string]$ExpectedArchiveSha256,
        [Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)]$ExpectedMetadata)
    if ($ExpectedArchiveSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        (Get-PreparationSha256 $ArchivePath) -cne $ExpectedArchiveSha256.ToLowerInvariant()) { throw 'Unsigned preparation archive does not match its explicitly pinned SHA-256.' }
    if (Test-Path -LiteralPath $Root) { throw 'Unsigned preparation import requires a fresh directory.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $names = @($zip.Entries | ForEach-Object FullName | Sort-Object)
        $expected = @(@(Get-UnsignedPreparationPayloadPaths) + 'preparation.json' | Sort-Object)
        if (($names -join ',') -cne ($expected -join ',')) { throw 'Unsigned preparation archive has an unexpected or unsafe path/file set.' }
    } finally { $zip.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($ArchivePath, $Root)
    return Assert-UnsignedReleasePreparation -Root $Root -ExpectedMetadata $ExpectedMetadata
}
