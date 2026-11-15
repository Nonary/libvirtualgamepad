[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$MSBuildPath)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('vhf-identity-msbuild-' + [guid]::NewGuid().ToString('N'))
$props = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../driver/MouseIdentity.props'))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $project = Join-Path $root 'identity.proj'
    $escapedProps = [Security.SecurityElement]::Escape($props)
    @"
<Project DefaultTargets="ClCompile" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup><ClCompile Include="inert-fixture.cpp"><PreprocessorDefinitions>ORIGINAL=1</PreprocessorDefinitions></ClCompile></ItemGroup>
  <Import Project="$escapedProps" />
  <Target Name="ClCompile">
    <Error Condition="'%(ClCompile.PreprocessorDefinitions)' != 'LVG_KM_VENDOR_ID=`$(ExpectedVendorId);LVG_KM_PRODUCT_ID=`$(ExpectedProductId);ORIGINAL=1'" Text="The shared guard did not normalize both compile definitions." />
  </Target>
</Project>
"@ | Set-Content -LiteralPath $project
    function Check-Identity([string]$Vendor, [string]$Product, [bool]$Allowed, [int]$ExpectedVendor = 1, [int]$ExpectedProduct = 2) {
        $output = @(& $MSBuildPath $project '/nologo' '/v:minimal' "/p:LvgKeyboardMouseVendorId=$Vendor" "/p:LvgKeyboardMouseProductId=$Product" "/p:ExpectedVendorId=$ExpectedVendor" "/p:ExpectedProductId=$ExpectedProduct" 2>&1)
        if (($LASTEXITCODE -eq 0) -ne $Allowed) { throw "Identity result mismatch for '$Vendor'/'$Product': $($output -join [Environment]::NewLine)" }
    }
    Check-Identity '43981' '22136' $true 43981 22136
    Check-Identity '0xABCD' '0x5678' $true 43981 22136
    Check-Identity '00001' '00002' $true
    Check-Identity '65535' '65535' $true 65535 65535
    Check-Identity '0xFFFF' '0Xffff' $true 65535 65535
    foreach ($zero in @('0', '00', '00000', '0x0', '0X0000')) {
        Check-Identity $zero $zero $true 0 0
        Check-Identity $zero '2' $true 0 2
        Check-Identity '1' $zero $true 1 0
    }
    foreach ($invalid in @('65536', '99999', '0x10000', '-1', '+1', '1.0', 'unallocated')) {
        Check-Identity $invalid '2' $false
        Check-Identity '1' $invalid $false
    }
    $output = @(& $MSBuildPath $project '/nologo' '/v:minimal' '/p:ExpectedVendorId=0' '/p:ExpectedProductId=0' 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "Default zero identity failed: $($output -join [Environment]::NewLine)" }
    Write-Output 'Shared MSBuild identity guard passed: default/explicit zero, decimal/hex normalization and 16-bit bounds for both projects. No driver was compiled.'
} finally { Remove-Item -LiteralPath $root -Recurse -Force }
