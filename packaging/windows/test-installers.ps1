# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)][string]$Installers)
$ErrorActionPreference = 'Stop'
$Root = Join-Path $env:RUNNER_TEMP ('SDK coexistence ' + [guid]::NewGuid())
$Legacy = [Environment]::GetEnvironmentVariable('ROBOTWEAX_SRT','Machine')
$Paths = @{}
$Executables = @{}
$VS = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$VS) { throw 'Visual Studio C++ tools are required' }
$MSBuild = Join-Path $VS 'MSBuild/Current/Bin/MSBuild.exe'
function Run-Installer([string]$Executable, [string]$Arguments, [bool]$Reject = $false) {
    $Process = Start-Process -Wait -PassThru $Executable -ArgumentList $Arguments
    if ($Reject) {
        if ($Process.ExitCode -eq 0) { throw 'Conflicting installation was accepted' }
    } elseif ($Process.ExitCode -ne 0) { throw "Installer failed: $($Process.ExitCode)" }
}
try {
    foreach ($Backend in 'openssl','bcrypt') {
        $Matches = @(Get-ChildItem "$Installers/*-windows-sdk-$Backend.exe")
        if ($Matches.Count -ne 1) { throw "Expected one $Backend installer" }
        $Executables[$Backend] = $Matches[0].FullName
        $Paths[$Backend] = Join-Path $Root $Backend
    }
    foreach ($First in 'openssl','bcrypt') {
        $Second = if ($First -eq 'openssl') { 'bcrypt' } else { 'openssl' }
        foreach ($Backend in $First,$Second) {
            $Destination = $Paths[$Backend]
            Run-Installer $Executables[$Backend] "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=`"$Destination`""
            $Variable = 'ROBOTWEAX_SRT_' + $Backend.ToUpperInvariant()
            if ([Environment]::GetEnvironmentVariable($Variable,'Machine') -ne $Destination) { throw 'Incorrect SDK root' }
            [xml]$Props = Get-Content "$Destination/srt-backend.props" -Raw
            if ($Props.Project.PropertyGroup.RobotweaxSrtCryptoBackend -ne $Backend) { throw 'Incorrect installed backend' }
        }
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
        }
    }
    if ([Environment]::GetEnvironmentVariable('ROBOTWEAX_SRT','Machine') -ne $Legacy) { throw 'Legacy SDK selection modified' }
    Write-Output 'Both installation orders and independent uninstall passed'
} finally {
    foreach ($Path in $Paths.Values) {
        if (Test-Path "$Path/unins000.exe") {
            Run-Installer "$Path/unins000.exe" '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART'
        }
    }
}
