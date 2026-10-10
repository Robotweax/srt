# SPDX-License-Identifier: MIT
# Small real installers qualify recipe behavior, not library binary correctness.
# The separate full SDK test upgrades the pinned real release and runs a consumer.
$ErrorActionPreference = 'Stop'
$TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('SDK installer recipes ' + [guid]::NewGuid())
$Compiler = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (!(Test-Path $Compiler)) { throw 'Inno Setup 6.4 or newer is required' }
$InstalledPaths = @()
function New-TestSdk([string]$Directory, [string]$Backend, [string]$Version, [bool]$Obsolete) {
    New-Item -ItemType Directory "$Directory/include/srt" -Force | Out-Null
    foreach ($File in 'LICENSE.txt','srt.props','include/srt/srt.h') { $Version | Set-Content "$Directory/$File" }
    '<Project />' | Set-Content "$Directory/srt-backend.props"
    if ($Obsolete) {
        'obsolete fixture' | Set-Content "$Directory/include/srt/a-retired.h","$Directory/include/srt/b-retired.h"
    }
    foreach ($Configuration in 'Debug','Release') {
        foreach ($Platform in 'Win32','x64','Arm64') {
            $Dir = "$Directory/lib/$Configuration-$Platform"
            New-Item -ItemType Directory $Dir -Force | Out-Null
            $Version | Set-Content "$Dir/robotweax-srt.lib"
            @{configuration=$Configuration;platform=$Platform;crypto_backend=$Backend} |
                ConvertTo-Json | Set-Content "$Dir/build.json"
        }
    }
    $Hashes = @{}
    foreach ($File in Get-ChildItem $Directory -Recurse -File) {
        $Hashes[$File.FullName.Substring($Directory.Length + 1)] = (Get-FileHash $File.FullName).Hash
    }
    $Hashes | ConvertTo-Json | Set-Content "$Directory/checksums.json"
}
function Compile-Setup([string]$Sdk, [string]$Backend, [string]$Version) {
    $Out = Join-Path $TestRoot "$Backend-$Version"
    New-Item -ItemType Directory $Out | Out-Null
    & $Compiler "/DSdkRoot=$Sdk" "/DProductVersion=$Version" "/DCryptoBackend=$Backend" "/O$Out" "$PSScriptRoot/sdk.iss" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Installer recipe compilation failed' }
    return "$Out/robotweax-srt-$Version-windows-sdk-$Backend.exe"
}
function Run-Setup([string]$Executable, [string]$Extra = '', [int]$Expected = 0) {
    $Log = Join-Path $TestRoot ([guid]::NewGuid().ToString() + '.log')
    $Process = Start-Process -Wait -PassThru $Executable -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=`"$Log`" $Extra"
    if (($Expected -eq -1 -and $Process.ExitCode -eq 0) -or
        ($Expected -ge 0 -and $Process.ExitCode -ne $Expected)) {
        if (Test-Path $Log) { Get-Content $Log -Tail 80 | Out-Host }
        throw "Installer exit $($Process.ExitCode), expected $Expected"
    }
}
try {
    New-Item -ItemType Directory $TestRoot | Out-Null
    foreach ($Backend in 'bcrypt','openssl') {
        if ([Environment]::GetEnvironmentVariable(('ROBOTWEAX_SRT_' + $Backend.ToUpperInvariant()),'Machine')) {
            throw 'Run installer qualification on a fresh Windows host'
        }
        $Stage1 = Join-Path $TestRoot "stage-1-$Backend"
        $Stage2 = Join-Path $TestRoot "stage-2-$Backend"
        $Destination = Join-Path $TestRoot "custom SDK $Backend"
        $InstalledPaths += $Destination
        New-TestSdk $Stage1 $Backend '0.0.1' $true
        New-TestSdk $Stage2 $Backend '0.0.2' $false
        $First = Compile-Setup $Stage1 $Backend '0.0.1'
        $Second = Compile-Setup $Stage2 $Backend '0.0.2'
        Run-Setup $First "/DIR=`"$Destination`""
        'user file' | Set-Content "$Destination/include/srt/user-owned.h"
        $Retired = "$Destination/include/srt/a-retired.h"
        $Locked = "$Destination/include/srt/b-retired.h"
        $OriginalHash = (Get-FileHash $Retired).Hash
        $LibraryHash = (Get-FileHash "$Destination/lib/Release-x64/robotweax-srt.lib").Hash
        'changed user content' | Set-Content $Retired
        Run-Setup $Second '' 7
        if ((Get-FileHash "$Destination/lib/Release-x64/robotweax-srt.lib").Hash -ne $LibraryHash) { throw 'Preflight failure changed SDK' }
        'obsolete fixture' | Set-Content $Retired
        $Handle = [IO.File]::Open($Locked,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        try {
            Run-Setup $Second '' -1
            if (!(Test-Path $Retired) -or (Get-FileHash $Retired).Hash -ne $OriginalHash) { throw 'Retired file was not restored' }
        } finally { $Handle.Dispose() }
        if ((Get-FileHash "$Destination/lib/Release-x64/robotweax-srt.lib").Hash -ne $LibraryHash) { throw 'Retirement failure changed SDK' }
        Run-Setup $Second
        if ((Test-Path $Retired) -or (Test-Path $Locked)) { throw 'Obsolete files remain after upgrade' }
        if ((Get-FileHash "$Destination/lib/Release-x64/robotweax-srt.lib").Hash -ne (Get-FileHash "$Stage2/lib/Release-x64/robotweax-srt.lib").Hash) { throw 'Payload was not upgraded' }
        $Key = "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Robotweax.SRT.SDK.${Backend}_is1"
        if ((Get-ItemProperty $Key).DisplayVersion -ne '0.0.2') { throw 'SDK registration was not upgraded' }
        Remove-Item "$Destination/srt.props"
        Run-Setup $Second
        if (!(Test-Path "$Destination/srt.props")) { throw 'Repair did not replace missing file' }
        Run-Setup $First '' 7
        Run-Setup $Second "/DIR=`"$TestRoot/another directory`"" 7
        Run-Setup "$Destination/unins000.exe"
        if ((Test-Path "$Destination/srt.props") -or (Test-Path $Key)) { throw 'SDK uninstall is incomplete' }
        if ([Environment]::GetEnvironmentVariable(('ROBOTWEAX_SRT_' + $Backend.ToUpperInvariant()),'Machine')) { throw 'Stale SDK variable' }
        if ((Get-Content "$Destination/include/srt/user-owned.h" -Raw).Trim() -ne 'user file') { throw 'Untracked file was removed' }
    }
    Write-Output 'Both native installer recipes passed upgrade, repair, downgrade, path, obsolete-file restoration and uninstall'
} finally {
    foreach ($Directory in $InstalledPaths) {
        if (Test-Path "$Directory/unins000.exe") { Run-Setup "$Directory/unins000.exe" }
    }
    if (Test-Path $TestRoot) { Remove-Item -Recurse -Force $TestRoot }
}
