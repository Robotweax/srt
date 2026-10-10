# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)][string]$Installers)
$ErrorActionPreference = 'Stop'
$Root = Join-Path $env:RUNNER_TEMP ('SDK coexistence ' + [guid]::NewGuid())
$Legacy = [Environment]::GetEnvironmentVariable('ROBOTWEAX_SRT','Machine')
$Paths = @{}
$Executables = @{}
$OlderExecutables = @{}
$Version = [regex]::Match((Get-Content "$PSScriptRoot/../../CMakeLists.txt" -Raw), 'project\(robotweax_srt VERSION ([0-9.]+)').Groups[1].Value
$Compiler = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (!(Test-Path $Compiler)) { throw 'Inno Setup 6 is required' }
$VS = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$VS) { throw 'Visual Studio C++ tools are required' }
$MSBuild = Join-Path $VS 'MSBuild/Current/Bin/MSBuild.exe'
function Run-Installer([string]$Executable, [string]$Arguments, [bool]$Reject = $false, [int]$ExpectedExit = -1) {
    $Process = Start-Process -Wait -PassThru $Executable -ArgumentList $Arguments
    if ($ExpectedExit -ge 0 -and $Process.ExitCode -ne $ExpectedExit) { throw "Unexpected installer exit: $($Process.ExitCode), wanted $ExpectedExit" }
    if ($Reject) {
        if ($Process.ExitCode -eq 0) { throw 'Conflicting installation was accepted' }
    } elseif ($Process.ExitCode -ne 0) { throw "Installer failed: $($Process.ExitCode)" }
}
function Assert-Installed([string]$Backend) {
    $Destination = $Paths[$Backend]
    $Variable = 'ROBOTWEAX_SRT_' + $Backend.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($Variable,'Machine') -ne $Destination) { throw 'Incorrect SDK root' }
    $Key = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Robotweax.SRT.SDK.${Backend}_is1"
    if ((Get-ItemProperty $Key).DisplayVersion -ne $Version) { throw 'Incorrect installed SDK version' }
    [xml]$Props = Get-Content "$Destination/srt-backend.props" -Raw
    if ($Props.Project.PropertyGroup.RobotweaxSrtCryptoBackend -ne $Backend) { throw 'Incorrect installed backend' }
    $Inventory = Get-Content "$Destination/checksums.json" -Raw | ConvertFrom-Json
    foreach ($Entry in $Inventory.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath (Join-Path $Destination $Entry.Name)).Hash -ne $Entry.Value) { throw "Installed payload differs: $($Entry.Name)" }
    }
}
function Test-Upgrade([string]$Backend) {
    $Destination = $Paths[$Backend]
    $Other = if ($Backend -eq 'openssl') { 'bcrypt' } else { 'openssl' }
    $OtherHashes = Get-Content "$($Paths[$Other])/checksums.json" -Raw
    $BeforeLibrary = (Get-FileHash "$Destination/lib/Release-x64/robotweax-srt.lib").Hash
    $Inventory = Get-Content "$Destination/checksums.json" -Raw | ConvertFrom-Json
    $Hashes = @{}
    foreach ($Entry in $Inventory.PSObject.Properties) { $Hashes[$Entry.Name] = $Entry.Value }
    # Fault fixtures augment the real earlier-release payload inventory.
    $Retired = "$Destination/include/srt/a-retired.h"
    $Locked = "$Destination/include/srt/b-retired.h"
    'obsolete fixture' | Set-Content $Retired,$Locked
    $Hashes['include\srt\a-retired.h'] = (Get-FileHash $Retired).Hash
    $Hashes['include\srt\b-retired.h'] = (Get-FileHash $Locked).Hash
    $Hashes | ConvertTo-Json | Set-Content "$Destination/checksums.json"
    'user-owned file' | Set-Content "$Destination/include/srt/user-owned.h"
    'modified obsolete fixture' | Set-Content $Retired
    Run-Installer $Executables[$Backend] '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' $true 7
    if ((Get-FileHash "$Destination/lib/Release-x64/robotweax-srt.lib").Hash -ne $BeforeLibrary) { throw 'Rejected preflight changed SDK' }
    'obsolete fixture' | Set-Content $Retired
    # Fail during retirement after the first obsolete file was backed up/deleted.
    $Handle = [IO.File]::Open($Locked, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        Run-Installer $Executables[$Backend] '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' $true
        if (!(Test-Path $Retired) -or (Get-FileHash $Retired).Hash -ne $Hashes['include\srt\a-retired.h']) { throw 'Failed install did not restore retired file' }
    } finally { $Handle.Dispose() }
    if ((Get-FileHash "$Destination/lib/Release-x64/robotweax-srt.lib").Hash -ne $BeforeLibrary) { throw 'Failed retirement changed SDK' }
    # No /DIR: the registered custom directory (including spaces) is retained.
    Run-Installer $Executables[$Backend] '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
    Assert-Installed $Backend
    if ((Test-Path $Retired) -or (Test-Path $Locked)) { throw 'Obsolete SDK files survived upgrade' }
    if ((Get-Content "$Destination/include/srt/user-owned.h" -Raw).Trim() -ne 'user-owned file') { throw 'Untracked file changed' }
    if ((Get-Content "$($Paths[$Other])/checksums.json" -Raw) -cne $OtherHashes) { throw 'Other backend inventory changed' }
    $OtherInventory = $OtherHashes | ConvertFrom-Json
    foreach ($Entry in $OtherInventory.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath (Join-Path $Paths[$Other] $Entry.Name)).Hash -ne $Entry.Value) { throw 'Other backend payload changed' }
    }
    if ([Environment]::GetEnvironmentVariable(('ROBOTWEAX_SRT_' + $Other.ToUpperInvariant()),'Machine') -ne $Paths[$Other]) { throw 'Other backend variable changed during upgrade' }
    # Exercise this recipe's downgrade guard, not the older installer's guard.
    $FixtureRoot = Join-Path $Root ('downgrade-' + $Backend + '-' + [guid]::NewGuid())
    $FixtureSdk = Join-Path $FixtureRoot 'sdk'
    New-Item -ItemType Directory $FixtureSdk -Force | Out-Null
    $Inventory = Get-Content "$Destination/checksums.json" -Raw | ConvertFrom-Json
    foreach ($Entry in $Inventory.PSObject.Properties) {
        $Target = Join-Path $FixtureSdk $Entry.Name
        New-Item -ItemType Directory (Split-Path $Target) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $Destination $Entry.Name) -Destination $Target
    }
    Copy-Item "$Destination/checksums.json" $FixtureSdk
    & $Compiler "/DSdkRoot=$FixtureSdk" '/DProductVersion=0.0.1' "/DCryptoBackend=$Backend" "/O$FixtureRoot" "$PSScriptRoot/sdk.iss"
    if ($LASTEXITCODE -ne 0) { throw 'Downgrade fixture compilation failed' }
    Run-Installer "$FixtureRoot/robotweax-srt-0.0.1-windows-sdk-$Backend.exe" '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' $true 7
    Assert-Installed $Backend
    # Same-version repair overwrites a damaged current file.
    'damaged props fixture' | Set-Content "$Destination/srt.props"
    Run-Installer $Executables[$Backend] '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
    Assert-Installed $Backend
}
try {
    New-Item -ItemType Directory "$Root/older" -Force | Out-Null
    # Immutable, signed, real earlier SDKs: verifies migration from released inventory.
    $OlderHashes = @{
        openssl='6c9d64f8df22b4bb170a1fab43bfa0d0286b145839e6f49d55a27dacc7dd20f2'
        bcrypt='b636a03c69fd6ec66e9f870a78ff80b71ce7c98ea103362353e21a0519c61494'
    }
    foreach ($Backend in 'openssl','bcrypt') {
        if ([Environment]::GetEnvironmentVariable(('ROBOTWEAX_SRT_' + $Backend.ToUpperInvariant()),'Machine')) { throw 'Run installer qualification on a fresh Windows host' }
        $Matches = @(Get-ChildItem "$Installers/*-windows-sdk-$Backend.exe")
        if ($Matches.Count -ne 1) { throw "Expected one $Backend installer" }
        $Executables[$Backend] = $Matches[0].FullName
        $Paths[$Backend] = Join-Path $Root $Backend
        $Older = "$Root/older/robotweax-srt-0.2.7-windows-sdk-$Backend.exe"
        Invoke-WebRequest "https://github.com/Robotweax/srt/releases/download/v0.2.7/robotweax-srt-0.2.7-windows-sdk-$Backend.exe" -OutFile $Older
        if ((Get-FileHash $Older).Hash.ToLowerInvariant() -cne $OlderHashes[$Backend]) { throw 'Earlier SDK release digest mismatch' }
        if ((Get-AuthenticodeSignature $Older).Status -ne 'Valid') { throw 'Earlier SDK signature is invalid' }
        $OlderExecutables[$Backend] = $Older
    }
    foreach ($First in 'openssl','bcrypt') {
        $Second = if ($First -eq 'openssl') { 'bcrypt' } else { 'openssl' }
        foreach ($Backend in $First,$Second) {
            $Destination = $Paths[$Backend]
            Run-Installer $OlderExecutables[$Backend] "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=`"$Destination`""
        }
        foreach ($Backend in $First,$Second) { Test-Upgrade $Backend }
        foreach ($Backend in $First,$Second) {
            # Rebuild through the installed props: no staged SDK or implicit backend selection.
            & $MSBuild "$PSScriptRoot/consumer/consumer.vcxproj" /nologo /t:Rebuild /p:Configuration=Release /p:Platform=x64 "/p:ROBOTWEAX_SRT=$($Paths[$Backend])"
            if ($LASTEXITCODE -ne 0) { throw 'Installed SDK consumer build failed' }
            & "$PSScriptRoot/consumer/out/Release-x64/sdk-consumer.exe"
            if ($LASTEXITCODE -ne 0) { throw 'Installed SDK consumer failed' }
        }
        $Hash = (Get-FileHash "$($Paths[$First])/srt.props").Hash
        Run-Installer "$($Paths[$Second])/unins000.exe" '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
        # Different backend must not overwrite the remaining SDK's directory.
        Run-Installer $Executables[$Second] "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=`"$($Paths[$First])`"" $true
        if ((Get-FileHash "$($Paths[$First])/srt.props").Hash -ne $Hash) { throw 'Remaining SDK changed' }
        $Variable = 'ROBOTWEAX_SRT_' + $First.ToUpperInvariant()
        if ([Environment]::GetEnvironmentVariable($Variable,'Machine') -ne $Paths[$First]) { throw 'Remaining SDK variable changed' }
        Run-Installer "$($Paths[$First])/unins000.exe" '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
        foreach ($Backend in 'openssl','bcrypt') {
            $Variable = 'ROBOTWEAX_SRT_' + $Backend.ToUpperInvariant()
            if ([Environment]::GetEnvironmentVariable($Variable,'Machine')) { throw 'Stale backend environment variable' }
            if (Test-Path "$($Paths[$Backend])/srt.props") { throw 'SDK payload survived uninstall' }
            if ((Get-Content "$($Paths[$Backend])/include/srt/user-owned.h" -Raw).Trim() -ne 'user-owned file') { throw 'Uninstall removed untracked file' }
            Remove-Item "$($Paths[$Backend])/include/srt/user-owned.h"
        }
    }
    if ([Environment]::GetEnvironmentVariable('ROBOTWEAX_SRT','Machine') -ne $Legacy) { throw 'Legacy SDK selection modified' }
    Write-Output 'Released SDK upgrade, repair, downgrade, obsolete-file recovery, both installation orders and independent uninstall passed'
} finally {
    foreach ($Path in $Paths.Values) {
        if (Test-Path "$Path/unins000.exe") {
            Run-Installer "$Path/unins000.exe" '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
        }
    }
    if (Test-Path $Root) { Remove-Item -Recurse -Force $Root }
}
