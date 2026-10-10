# SPDX-License-Identifier: MIT
# Embedded in Setup; never execute code from an installed SDK.
param([string]$Root, [string]$Manifest, [AllowEmptyString()][string]$OldVersion,
      [string]$NewVersion, [ValidateSet('openssl','bcrypt')][string]$Backend,
      [string]$PlanPath, [string]$ErrorPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-SdkPath([string]$Directory, [string]$Relative) {
    $Parts = $Relative -split '\\'
    if ($Relative.Length -gt 240 -or $Parts.Count -eq 0) { throw 'Invalid SDK inventory path' }
    foreach ($Part in $Parts) {
        if ($Part -notmatch '^[A-Za-z0-9_.-]+$' -or $Part -in '.','..' -or
            $Part.EndsWith('.') -or $Part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "Unsafe SDK inventory path: $Relative"
        }
    }
    if ($Relative -notmatch '^(?i:LICENSE\.txt|OpenSSL-LICENSE\.txt|THIRD_PARTY\.md|README\.md|srt\.props|srt-backend\.props)$' -and
        $Relative -notmatch '^(?i:include\\|lib\\(?:Debug|Release)-(?:Win32|x64|Arm64)\\)') {
        throw "Path outside the SDK payload: $Relative"
    }
    # Reject junctions/symlinks in the root, its ancestors and the payload path.
    $Path = [IO.Path]::GetFullPath((Join-Path $Directory $Relative))
    $Current = $Path
    while ($Current) {
        $Item = Get-Item -LiteralPath $Current -Force -ErrorAction SilentlyContinue
        if ($Item) {
            if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "SDK path crosses a reparse point: $Current" }
        }
        $Parent = [IO.Path]::GetDirectoryName($Current)
        if ($Parent -eq $Current) { break }
        $Current = $Parent
    }
    return $Path
}

function Read-SdkInventory([string]$Path, [string]$Directory) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -gt 4194304) {
        throw 'SDK checksums.json is missing or too large; uninstall before installing again.'
    }
    if ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'SDK inventory must be a regular file'
    }
    $Json = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($Json -isnot [System.Management.Automation.PSCustomObject]) { throw 'Invalid SDK inventory object' }
    $Entries = @($Json.PSObject.Properties)
    if ($Entries.Count -eq 0 -or $Entries.Count -gt 4096) { throw 'Invalid SDK inventory size' }
    $Inventory = @{}
    foreach ($Entry in $Entries) {
        if ($Entry.Value -isnot [string] -or $Entry.Value -notmatch '^[0-9a-fA-F]{64}$' -or $Inventory.ContainsKey($Entry.Name)) {
            throw 'Invalid or duplicate SDK inventory entry'
        }
        $null = Assert-SdkPath $Directory $Entry.Name
        $Inventory[$Entry.Name] = $Entry.Value
    }
    foreach ($Required in 'LICENSE.txt','srt.props','srt-backend.props','include\srt\srt.h') {
        if (!$Inventory.ContainsKey($Required)) { throw "Incomplete SDK inventory: $Required" }
    }
    return $Inventory
}

function Get-SdkUpgradePlan([string]$Directory, [string]$Incoming, [string]$PreviousVersion,
                            [string]$Version, [string]$CryptoBackend) {
    if ($Version -notmatch '^\d+\.\d+\.\d+$' -or
        ($PreviousVersion -and $PreviousVersion -notmatch '^\d+\.\d+\.\d+$')) { throw 'Invalid SDK version' }
    if ($PreviousVersion -and [version]$PreviousVersion -gt [version]$Version) {
        throw "SDK downgrade from $PreviousVersion to $Version is not supported; uninstall first."
    }
    $New = Read-SdkInventory $Incoming $Directory
    if (!$PreviousVersion) {
        foreach ($Relative in $New.Keys) {
            if (Test-Path -LiteralPath (Assert-SdkPath $Directory $Relative)) {
                throw 'Unregistered SDK files occupy this directory; select a separate directory.'
            }
        }
        return
    }
    $Old = Read-SdkInventory (Join-Path $Directory 'checksums.json') $Directory
    # Verify the old backend identity from its checksummed build metadata.
    foreach ($Configuration in 'Debug','Release') {
        foreach ($Platform in 'Win32','x64','Arm64') {
            $Relative = "lib\$Configuration-$Platform\build.json"
            $Path = Assert-SdkPath $Directory $Relative
            if (!$Old.ContainsKey($Relative) -or !(Test-Path -LiteralPath $Path -PathType Leaf) -or
                (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Old[$Relative]) {
                throw 'Existing SDK backend metadata is missing or modified; uninstall first.'
            }
            $Metadata = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
            if ($Metadata.crypto_backend -ne $CryptoBackend -or $Metadata.platform -ne $Platform -or
                $Metadata.configuration -ne $Configuration) { throw 'Existing SDK backend metadata mismatch' }
        }
    }
    foreach ($Relative in ($Old.Keys | Sort-Object)) {
        if ($New.ContainsKey($Relative)) { continue }
        $Path = Assert-SdkPath $Directory $Relative
        if (!(Test-Path -LiteralPath $Path)) { continue }
        if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Old[$Relative]) {
            throw "Obsolete SDK file was modified; preserve it and uninstall first: $Relative"
        }
        # Fixed digest + two spaces + validated relative path, read by Setup.
        $Old[$Relative].ToLowerInvariant() + '  ' + $Relative
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        $Lines = @(Get-SdkUpgradePlan $Root $Manifest $OldVersion $NewVersion $Backend)
        # Write even an empty plan; LoadStringsFromFile can read it successfully.
        [IO.File]::WriteAllLines($PlanPath, [string[]]$Lines, [Text.UTF8Encoding]::new($true))
        exit 0
    } catch {
        [IO.File]::WriteAllText($ErrorPath, $_.Exception.Message, [Text.UTF8Encoding]::new($true))
        exit 1
    }
}
