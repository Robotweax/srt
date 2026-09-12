# SPDX-License-Identifier: MIT
param([Parameter(Mandatory)][string]$Variants, [Parameter(Mandatory)][string]$Destination,
      [string]$ArtifactPrefix = 'sdk', [ValidateSet('openssl','bcrypt')][string]$ExpectedBackend)
$ErrorActionPreference = 'Stop'
if (Test-Path $Destination) { throw 'Destination must not exist' }
New-Item -ItemType Directory $Destination | Out-Null
$Hashes = @{}
$Backend = $null
foreach ($Configuration in @('Debug','Release')) {
    foreach ($Platform in @('Win32','x64','Arm64')) {
        $Variant = Join-Path $Variants "$ArtifactPrefix-$Configuration-$Platform"
        $MetadataPath = "$Variant/lib/$Configuration-$Platform/build.json"
        $Metadata = Get-Content $MetadataPath -Raw | ConvertFrom-Json
        if ($Metadata.crypto_backend -notin @('openssl','bcrypt')) { throw 'Missing or invalid crypto backend' }
        if ($ExpectedBackend -and $Metadata.crypto_backend -ne $ExpectedBackend) { throw 'Unexpected crypto backend' }
        if ($Metadata.platform -ne $Platform -or $Metadata.configuration -ne $Configuration) { throw 'Variant metadata mismatch' }
        if ($Backend -and $Backend -ne $Metadata.crypto_backend) { throw 'Mixed crypto backends are not supported' }
        $Backend = $Metadata.crypto_backend
        $RequiredFiles = @("lib/$Configuration-$Platform/robotweax-srt.lib", 'include/srt/srt.h','LICENSE.txt','srt.props','srt-backend.props')
        if ($Backend -eq 'openssl') {
            $RequiredFiles += @("lib/$Configuration-$Platform/libcrypto.lib", 'OpenSSL-LICENSE.txt')
        } elseif (@(Get-ChildItem $Variant -Recurse -File | Where-Object { $_.Name -match '(?i)libcrypto|openssl' }).Count) {
            throw 'BCrypt SDK must not contain OpenSSL files'
        }
        foreach ($Required in $RequiredFiles) {
            if (!(Test-Path "$Variant/$Required")) { throw "Missing $Variant/$Required" }
        }
        [xml]$Props = Get-Content "$Variant/srt-backend.props" -Raw
        if ($Props.Project.PropertyGroup.RobotweaxSrtCryptoBackend -ne $Backend) { throw 'Backend props mismatch' }
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
