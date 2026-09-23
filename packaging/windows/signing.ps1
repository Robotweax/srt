# SPDX-License-Identifier: MIT
# Shared validation for final installer bytes. No signing credentials are used here.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SdkInstallerPair([string]$Directory, [string]$Version) {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid SDK version' }
    $Files = @(Get-ChildItem -LiteralPath $Directory -File)
    $Expected = @('openssl', 'bcrypt' | ForEach-Object { "robotweax-srt-$Version-windows-sdk-$_.exe" })
    $Executables = @($Files | Where-Object Extension -eq '.exe')
    if ($Executables.Count -ne 2) { throw 'Expected exactly two installer executables' }
    foreach ($Name in $Expected) {
        $Match = @($Executables | Where-Object { $_.Name -ceq $Name })
        if ($Match.Count -ne 1 -or $Match[0].Length -eq 0) { throw "Missing or invalid installer: $Name" }
        if ($Match[0].Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Installer must be a regular file' }
        $Match[0]
    }
}

function Get-SdkSignTool {
    $Tools = @(Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin/*/x64/signtool.exe" |
        Sort-Object FullName -Descending)
    if (!$Tools.Count) { throw 'Windows SDK SignTool is required' }
    $Tools[0].FullName
}

function Assert-SdkSignatureRecord($Signature, [string]$Publisher) {
    if ([string]::IsNullOrWhiteSpace($Publisher)) { throw 'Approved publisher is required' }
    if ($null -eq $Signature -or [string]$Signature.Status -cne 'Valid' -or
        [string]$Signature.SignatureType -cne 'Authenticode' -or $null -eq $Signature.SignerCertificate) {
        throw 'Invalid Authenticode signature'
    }
    $Certificate = $Signature.SignerCertificate
    $Name = $Certificate.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if (![string]::Equals($Name, $Publisher, [StringComparison]::Ordinal)) { throw 'Unapproved signer' }
    # Require an actual CN, so GetNameInfo cannot silently fall back to another field.
    $Subject = $Certificate.SubjectName.Decode([Security.Cryptography.X509Certificates.X500DistinguishedNameFlags]::UseNewLines)
    $CommonNames = @($Subject -split '\r?\n' | Where-Object { $_ -match '^CN=' })
    if ($CommonNames.Count -ne 1 -or $CommonNames[0] -cne "CN=$Publisher") { throw 'Unapproved signer subject' }
    $CodeSigning = @($Certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' } |
        ForEach-Object { $_.EnhancedKeyUsages } | Where-Object { $_.Value -eq '1.3.6.1.5.5.7.3.3' })
    if (!$CodeSigning.Count) { throw 'Code Signing EKU is required' }
    if ($null -eq $Signature.TimeStamperCertificate) { throw 'Trusted timestamp is required' }
}

function Assert-SdkInstallerSignature([string]$Path, [string]$Publisher, [string]$SignTool) {
    Assert-SdkSignatureRecord (Get-AuthenticodeSignature -LiteralPath $Path) $Publisher
    # /all checks every embedded signature; /tw returns a warning for missing timestamps.
    # Treat warnings as failures, too. SignTool verifies the timestamp and trust chain.
    & $SignTool verify /pa /all /v /tw $Path | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "SignTool verification failed: $Path (exit $LASTEXITCODE)" }
}

function Get-SdkChecksumLines($Files) {
    foreach ($File in $Files) {
        $Hash = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$Hash  $($File.Name)"
    }
}

function Write-SdkChecksumManifest([string]$Manifest, [string[]]$Lines) {
    if (Test-Path -LiteralPath $Manifest) { throw 'Refusing to replace an existing checksum manifest' }
    # GNU sha256sum in Git Bash treats CR as part of the filename.
    [IO.File]::WriteAllText($Manifest, (($Lines -join "`n") + "`n"), [Text.Encoding]::ASCII)
}

function Assert-SdkChecksumManifest([string]$Manifest, [string[]]$ExpectedLines) {
    $Actual = @(Get-Content -LiteralPath $Manifest)
    if ($ExpectedLines.Count -ne 2 -or $Actual.Count -ne 2 -or
        ($Actual -join "`n") -cne ($ExpectedLines -join "`n")) {
        throw 'Signed installer checksum manifest mismatch'
    }
}
