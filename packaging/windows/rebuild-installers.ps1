# SPDX-License-Identifier: MIT
# Fresh source rebuild for the signing boundary. Never consume producer artifacts.
param(
    [Parameter(Mandatory)][string]$Revision,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Destination,
    [Parameter(Mandatory)][string]$WorkRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Invoke-RebuildChecked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "$Program failed: $LASTEXITCODE" }
}
$Source = (Resolve-Path "$PSScriptRoot/../..").Path
if ($Revision -cnotmatch '^[0-9a-f]{40}$' -or $Version -notmatch '^\d+\.\d+\.\d+$') {
    throw 'Invalid rebuild source identity'
}
$ActualRevision = & git -C $Source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $ActualRevision -cne $Revision) { throw 'Rebuild checkout differs from event commit' }
$ActualVersion = [regex]::Match((Get-Content "$Source/CMakeLists.txt" -Raw), 'project\(robotweax_srt VERSION ([0-9.]+)').Groups[1].Value
if ($ActualVersion -cne $Version) { throw 'Rebuild version differs from source' }
$OpenSSLRevision = & git -C "$Source/openssl-source" rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $OpenSSLRevision -cne 'aae016bfd52fcad2bc9657c2c782cfdf73b1ed5f') { throw 'Unexpected OpenSSL rebuild source' }
if ((Test-Path $WorkRoot) -or (Test-Path $Destination)) { throw 'Rebuild requires fresh directories' }
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory $WorkRoot, $Destination | Out-Null
$VisualStudio = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$VisualStudio) { throw 'Visual Studio is required' }
$Compiler = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (!(Test-Path $Compiler)) { throw 'Inno Setup 6 is required' }
$PowerShell = Join-Path $PSHOME 'pwsh.exe'
foreach ($Backend in @('openssl','bcrypt')) {
    $Variants = Join-Path $WorkRoot "variants-$Backend"
    New-Item -ItemType Directory $Variants | Out-Null
    foreach ($Configuration in @('Debug','Release')) {
        foreach ($Platform in @('Win32','x64','Arm64')) {
            $VariantRoot = Join-Path $WorkRoot "$Backend-$Configuration-$Platform"
            Invoke-RebuildChecked git @('-C',$Source,'worktree','add','--detach',$VariantRoot,$Revision)
            if ($Backend -eq 'openssl') {
                New-Item -ItemType Directory "$VariantRoot/openssl-source" | Out-Null
                Get-ChildItem "$Source/openssl-source" -Force | Where-Object Name -ne '.git' |
                    Copy-Item -Destination "$VariantRoot/openssl-source" -Recurse
            }
            $Arch = @{Win32='x86'; x64='amd64'; Arm64='amd64_arm64'}[$Platform]
            $Script = "$VariantRoot/packaging/windows/build-ci.ps1"
            Push-Location $VariantRoot
            try {
                Invoke-RebuildChecked cmd @('/c', "`"$VisualStudio\VC\Auxiliary\Build\vcvarsall.bat`" $Arch && `"$PowerShell`" -NoProfile -File `"$Script`" -Platform $Platform -Configuration $Configuration -CryptoBackend $Backend")
                Move-Item "$VariantRoot/output/sdk-$Configuration-$Platform" "$Variants/sdk-$Configuration-$Platform"
            } finally { Pop-Location }
        }
    }
    $Sdk = Join-Path $WorkRoot "sdk-$Backend"
    & "$PSScriptRoot/package.ps1" -Variants $Variants -Destination $Sdk -ExpectedBackend $Backend
    if (!$?) { throw 'Rebuilt SDK packaging failed' }
    Invoke-RebuildChecked $Compiler @("/DSdkRoot=$Sdk", "/DProductVersion=$Version", "/DCryptoBackend=$Backend", "/O$Destination", "$PSScriptRoot/sdk.iss")
}
. "$PSScriptRoot/signing.ps1"
$Pair = @(Get-SdkInstallerPair $Destination $Version)
foreach ($File in $Pair) {
    if ([string](Get-AuthenticodeSignature -LiteralPath $File.FullName).Status -cne 'NotSigned') {
        throw 'Expected unsigned rebuilt installers'
    }
}
