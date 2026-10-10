# SPDX-License-Identifier: MIT
# Runs in Windows PowerShell 5.1 as well as pwsh; no installer or admin rights.
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/upgrade-check.ps1"
$TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('SDK upgrade validation ' + [guid]::NewGuid())
New-Item -ItemType Directory $TestRoot | Out-Null
function Expect-Rejection([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw }
        return
    }
    throw "Expected rejection: $Pattern"
}
try {
    $Installed = Join-Path $TestRoot 'installed'
    $Incoming = Join-Path $TestRoot 'incoming.json'
    New-Item -ItemType Directory "$Installed/include/srt" -Force | Out-Null
    foreach ($File in 'LICENSE.txt','srt.props','srt-backend.props','include/srt/srt.h','include/srt/obsolete.h') {
        'fixture' | Set-Content "$Installed/$File"
    }
    foreach ($Configuration in 'Debug','Release') {
        foreach ($Platform in 'Win32','x64','Arm64') {
            $Dir = "$Installed/lib/$Configuration-$Platform"
            New-Item -ItemType Directory $Dir -Force | Out-Null
            @{configuration=$Configuration;platform=$Platform;crypto_backend='bcrypt'} |
                ConvertTo-Json | Set-Content "$Dir/build.json"
        }
    }
    $Hashes = @{}
    foreach ($File in Get-ChildItem $Installed -Recurse -File) {
        $Hashes[$File.FullName.Substring($Installed.Length + 1)] = (Get-FileHash $File.FullName).Hash
    }
    $Hashes | ConvertTo-Json | Set-Content "$Installed/checksums.json"
    $NewHashes = $Hashes.Clone()
    $NewHashes.Remove('include\srt\obsolete.h')
    $NewHashes | ConvertTo-Json | Set-Content $Incoming
    if ((Get-SdkFileHash "$Installed/srt.props") -ne (Get-FileHash "$Installed/srt.props").Hash) { throw 'Incorrect SDK SHA256' }
    $Plan = @(Get-SdkUpgradePlan $Installed $Incoming '0.2.7' '0.2.8' 'bcrypt')
    if ($Plan.Count -ne 1 -or $Plan[0].Substring(66) -ne 'include\srt\obsolete.h') { throw 'Incorrect obsolete-file plan' }
    if (@(Get-SdkUpgradePlan $Installed $Incoming '0.2.8' '0.2.8' 'bcrypt').Count -ne 1) { throw 'Repair rejected' }
    Expect-Rejection { Get-SdkUpgradePlan $Installed $Incoming '0.2.9' '0.2.8' 'bcrypt' } 'downgrade'
    Expect-Rejection { Get-SdkUpgradePlan $Installed $Incoming 'broken' '0.2.8' 'bcrypt' } 'version'
    Expect-Rejection { Get-SdkUpgradePlan $Installed $Incoming '0.2.7' '0.2.8' 'openssl' } 'backend metadata mismatch'
    'modified' | Set-Content "$Installed/include/srt/obsolete.h"
    Expect-Rejection { Get-SdkUpgradePlan $Installed $Incoming '0.2.7' '0.2.8' 'bcrypt' } 'Obsolete SDK file was modified'
    'fixture' | Set-Content "$Installed/include/srt/obsolete.h"
    'user content' | Set-Content "$Installed/include/srt/user.h"
    if (@(Get-SdkUpgradePlan $Installed $Incoming '0.2.7' '0.2.8' 'bcrypt').Count -ne 1) { throw 'Untracked user file included' }
    $Conflicting = $NewHashes.Clone()
    $Conflicting['include\srt\user.h'] = 'a' * 64
    $Conflicting | ConvertTo-Json | Set-Content $Incoming
    Expect-Rejection { Get-SdkUpgradePlan $Installed $Incoming '0.2.7' '0.2.8' 'bcrypt' } 'overwrite an untracked file'
    if ((Get-Content "$Installed/include/srt/user.h" -Raw).Trim() -ne 'user content') { throw 'Collision changed user file' }
    $NewHashes | ConvertTo-Json | Set-Content $Incoming
    Remove-Item "$Installed/include/srt/obsolete.h"
    if (@(Get-SdkUpgradePlan $Installed $Incoming '0.2.7' '0.2.8' 'bcrypt').Count -ne 0) { throw 'Missing obsolete file rejected' }
    foreach ($Bad in '..\outside.txt','include\..\outside.txt','include\srt\NUL.h','C:\outside.txt','include\srt\srt.h:stream','include\srt\bad.','arbitrary.txt','include/srt/bad.h') {
        $Malformed = $NewHashes.Clone()
        $Malformed[$Bad] = 'a' * 64
        $Malformed | ConvertTo-Json | Set-Content $Incoming
        Expect-Rejection { Read-SdkInventory $Incoming $Installed } 'path'
    }
    $NewHashes | ConvertTo-Json | Set-Content $Incoming
    # A reparse point is rejected even when an old inventory names its target.
    $Outside = Join-Path $TestRoot 'outside'
    New-Item -ItemType Directory $Outside | Out-Null
    New-Item -ItemType Junction -Path "$Installed/include/redirect" -Target $Outside | Out-Null
    Expect-Rejection { Assert-SdkPath $Installed 'include\redirect\file.h' } 'reparse point'
    Remove-Item "$Installed/include/redirect" -Force
    $NewHashes['srt.props'] = 'not-a-digest'
    $NewHashes | ConvertTo-Json | Set-Content $Incoming
    Expect-Rejection { Read-SdkInventory $Incoming $Installed } 'inventory entry'
    Write-Output 'SDK upgrade preflight negative tests passed'
} finally {
    if (Test-Path "$Installed/include/redirect") { Remove-Item "$Installed/include/redirect" -Force }
    Remove-Item -Recurse -Force $TestRoot
}
