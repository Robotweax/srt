# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory)][string]$Installers,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Publisher,
    [switch]$WriteManifest,
    [switch]$RequireManifest
)
. "$PSScriptRoot/signing.ps1"
if ($WriteManifest -and $RequireManifest) { throw 'Choose one manifest operation' }
$Files = @(Get-SdkInstallerPair $Installers $Version)
$SignTool = Get-SdkSignTool
foreach ($File in $Files) { Assert-SdkInstallerSignature $File.FullName $Publisher $SignTool }
$Lines = @(Get-SdkChecksumLines $Files)
$Manifest = Join-Path $Installers 'SHA256SUMS'
if ($RequireManifest) {
    Assert-SdkChecksumManifest $Manifest $Lines
}
if ($WriteManifest) {
    if (Test-Path -LiteralPath $Manifest) { throw 'Refusing to replace an existing checksum manifest' }
    [IO.File]::WriteAllLines($Manifest, $Lines, [Text.Encoding]::ASCII)
}
Write-Output 'Both signed installers have the approved publisher and trusted timestamps'
