# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)][string]$Variants, [Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference = 'Stop'
if (Test-Path $Destination) { throw 'Destination must not exist' }
New-Item -ItemType Directory $Destination | Out-Null
$Hashes = @{}
foreach ($Configuration in @('Debug','Release')) {
    foreach ($Platform in @('Win32','x64','Arm64')) {
        $Variant = Join-Path $Variants "sdk-$Configuration-$Platform"
        foreach ($Required in @("lib/$Configuration-$Platform/robotweax-srt.lib", "lib/$Configuration-$Platform/libcrypto.lib", 'include/srt/srt.h','LICENSE.txt','OpenSSL-LICENSE.txt','srt.props')) {
            if (!(Test-Path "$Variant/$Required")) { throw "Missing $Variant/$Required" }
        }
        foreach ($File in Get-ChildItem $Variant -Recurse -File) {
            $Relative = $File.FullName.Substring((Resolve-Path $Variant).Path.Length + 1)
            $Hash = (Get-FileHash $File.FullName).Hash
            if ($Hashes.ContainsKey($Relative) -and $Hashes[$Relative] -ne $Hash) { throw "Conflicting shared file $Relative" }
            $Hashes[$Relative] = $Hash
            $Target = Join-Path $Destination $Relative
            New-Item -ItemType Directory (Split-Path $Target) -Force | Out-Null
            Copy-Item $File.FullName $Target
        }
    }
}
$Hashes | ConvertTo-Json | Set-Content "$Destination/checksums.json"
