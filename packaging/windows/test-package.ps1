# SPDX-License-Identifier: MIT
# Synthetic inventories exercise validation, not binary/architecture correctness.
$ErrorActionPreference = 'Stop'
$Root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
New-Item -ItemType Directory $Root | Out-Null
try {
    foreach ($Backend in 'openssl','bcrypt') {
        $Variants = Join-Path $Root $Backend
        foreach ($Configuration in 'Debug','Release') {
            foreach ($Platform in 'Win32','x64','Arm64') {
                $Variant = "$Variants/sdk-$Configuration-$Platform"
                $Lib = "$Variant/lib/$Configuration-$Platform"
                New-Item -ItemType Directory $Lib,"$Variant/include/srt" -Force | Out-Null
                foreach ($File in @("$Lib/robotweax-srt.lib", "$Variant/include/srt/srt.h", "$Variant/LICENSE.txt", "$Variant/srt.props")) {
                    'fixture' | Set-Content $File
                }
                @{platform=$Platform; configuration=$Configuration; crypto_backend=$Backend} |
                    ConvertTo-Json | Set-Content "$Lib/build.json"
                "<Project><PropertyGroup><RobotweaxSrtCryptoBackend>$Backend</RobotweaxSrtCryptoBackend></PropertyGroup></Project>" |
                    Set-Content "$Variant/srt-backend.props"
                if ($Backend -eq 'openssl') {
                    'fixture' | Set-Content "$Lib/libcrypto.lib"
                    'fixture' | Set-Content "$Variant/OpenSSL-LICENSE.txt"
                }
            }
        }
        & "$PSScriptRoot/package.ps1" -Variants $Variants -Destination "$Root/valid-$Backend"
        $Rejected = $false
        $Other = if ($Backend -eq 'openssl') { 'bcrypt' } else { 'openssl' }
        try { & "$PSScriptRoot/package.ps1" -Variants $Variants -Destination "$Root/wrong-$Backend" -ExpectedBackend $Other }
        catch {
            if ($_.Exception.Message -notmatch 'Unexpected crypto backend') { throw }
            $Rejected = $true
        }
        if (!$Rejected) { throw 'Incorrect installer backend accepted' }
        foreach ($Variant in Get-ChildItem $Variants -Directory) {
            Rename-Item $Variant.FullName ($Variant.Name -replace '^sdk-', "sdk-$Backend-")
        }
        & "$PSScriptRoot/package.ps1" -Variants $Variants -Destination "$Root/prefixed-$Backend" -ArtifactPrefix "sdk-$Backend" -ExpectedBackend $Backend
        foreach ($Variant in Get-ChildItem $Variants -Directory) {
            Rename-Item $Variant.FullName ($Variant.Name -replace "^sdk-$Backend-", 'sdk-')
        }
    }
    function Expect-Rejection([string]$Name, [string]$Pattern) {
        $Rejected = $false
        try { & "$PSScriptRoot/package.ps1" -Variants "$Root/bcrypt" -Destination "$Root/$Name" }
        catch {
            if ($_.Exception.Message -notmatch $Pattern) { throw }
            $Rejected = $true
        }
        if (!$Rejected) { throw "Invalid package accepted: $Name" }
    }
    $MetadataPath = "$Root/bcrypt/sdk-Release-Arm64/lib/Release-Arm64/build.json"
    $Original = Get-Content $MetadataPath -Raw
    $Metadata = $Original | ConvertFrom-Json
    $Metadata.crypto_backend = 'openssl'
    $Metadata | ConvertTo-Json | Set-Content $MetadataPath
    Expect-Rejection 'mixed' 'Mixed crypto backends'
    $Original | Set-Content $MetadataPath
    'unexpected' | Set-Content "$Root/bcrypt/sdk-Release-Arm64/lib/Release-Arm64/libcrypto.lib"
    Expect-Rejection 'contaminated' 'must not contain OpenSSL'
    Write-Output 'Package validation tests passed'
} finally {
    # Only this invocation's generated temporary fixtures are removed.
    Remove-Item -Recurse -Force $Root
}
