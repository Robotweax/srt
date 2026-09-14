# SPDX-License-Identifier: MIT
# Real local Authenticode fixtures plus deterministic timestamp/publisher policy tests.
# No Azure identity, signing service, external timestamp server or release file is used.
. "$PSScriptRoot/signing.ps1"
$Root = Join-Path ([IO.Path]::GetTempPath()) ('sdk-signing-tests-' + [guid]::NewGuid())
$Certificate = $null
function Expect-Rejection([scriptblock]$Action, [string]$Pattern) {
    try { & $Action } catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw }
        return
    }
    throw "Invalid signature input accepted (expected $Pattern)"
}
New-Item -ItemType Directory $Root | Out-Null
try {
    $Source = Join-Path $Root 'fixture.cs'
    'class Fixture { static int Main() { return 0; } }' | Set-Content $Source
    $Compiler = "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
    $Unsigned = Join-Path $Root 'unsigned.exe'
    Write-Output 'Compiling Authenticode fixture'
    & $Compiler /nologo /target:exe "/out:$Unsigned" $Source
    if ($LASTEXITCODE -ne 0) { throw 'Fixture compilation failed' }
    Write-Output 'Locating SignTool and checking unsigned fixture'
    $SignTool = Get-SdkSignTool
    Expect-Rejection { Assert-SdkInstallerSignature $Unsigned 'SDK Signing Test' $SignTool } 'Invalid Authenticode'
    Write-Output 'Creating temporary signing certificate'
    $Certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=SDK Signing Test' `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddDays(1)
    foreach ($StoreName in 'Root', 'TrustedPublisher') {
        Write-Output "Importing temporary certificate into LocalMachine/$StoreName"
        $Store = [Security.Cryptography.X509Certificates.X509Store]::new($StoreName, 'LocalMachine')
        try { $Store.Open('ReadWrite'); $Store.Add($Certificate) } finally { $Store.Close() }
    }
    $Signed = Join-Path $Root 'signed.exe'
    Copy-Item $Unsigned $Signed
    Write-Output 'Signing local fixture without a timestamp'
    & $SignTool sign /fd SHA256 /sha1 $Certificate.Thumbprint /s My $Signed | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Fixture signing failed' }
    Write-Output 'Checking real Authenticode rejection cases'
    $Signature = Get-AuthenticodeSignature -LiteralPath $Signed
    if ([string]$Signature.Status -cne 'Valid') { throw 'Local test certificate was not trusted' }
    Expect-Rejection { Assert-SdkInstallerSignature $Signed 'Other Publisher' $SignTool } 'Unapproved signer'
    Expect-Rejection { Assert-SdkInstallerSignature $Signed 'SDK Signing Test' $SignTool } 'timestamp is required'
    $Tampered = Join-Path $Root 'tampered.exe'
    $Bytes = [IO.File]::ReadAllBytes($Signed)
    $Bytes[64] = $Bytes[64] -bxor 1
    [IO.File]::WriteAllBytes($Tampered, $Bytes)
    Expect-Rejection { Assert-SdkInstallerSignature $Tampered 'SDK Signing Test' $SignTool } 'Invalid Authenticode'
    Write-Output 'Checking signature policy and exact pair/checksum cases'
    # This record only tests policy; it is not a cryptographic timestamp test.
    $Record = [pscustomobject]@{Status='Valid'; SignatureType='Authenticode';
        SignerCertificate=$Certificate; TimeStamperCertificate=$Certificate}
    Assert-SdkSignatureRecord $Record 'SDK Signing Test'
    $Record.Status = 'HashMismatch'
    Expect-Rejection { Assert-SdkSignatureRecord $Record 'SDK Signing Test' } 'Invalid Authenticode'
    $Record.Status = 'Valid'; $Record.SignatureType = 'Catalog'
    Expect-Rejection { Assert-SdkSignatureRecord $Record 'SDK Signing Test' } 'Invalid Authenticode'
    $Pair = Join-Path $Root 'pair'
    New-Item -ItemType Directory $Pair | Out-Null
    Copy-Item $Unsigned "$Pair/robotweax-srt-0.0.0-windows-sdk-openssl.exe"
    Expect-Rejection { Get-SdkInstallerPair $Pair '0.0.0' } 'exactly two'
    Copy-Item $Unsigned "$Pair/robotweax-srt-0.0.0-windows-sdk-bcrypt.exe"
    $Files = @(Get-SdkInstallerPair $Pair '0.0.0')
    if ($Files.Count -ne 2) { throw 'Valid installer pair rejected' }
    $Before = @(Get-SdkChecksumLines $Files)
    $Manifest = Join-Path $Pair 'SHA256SUMS'
    [IO.File]::WriteAllLines($Manifest, $Before)
    Assert-SdkChecksumManifest $Manifest $Before
    Copy-Item $Tampered $Files[1].FullName -Force
    $After = @(Get-SdkChecksumLines $Files)
    if ($Before[1] -ceq $After[1]) { throw 'Changed installer bytes were not detected' }
    Expect-Rejection { Assert-SdkChecksumManifest $Manifest $After } 'manifest mismatch'
    [IO.File]::WriteAllLines($Manifest, @($Before[0]))
    Expect-Rejection { Assert-SdkChecksumManifest $Manifest $Before } 'manifest mismatch'
    [IO.File]::WriteAllLines($Manifest, @($Before[0], $Before[0]))
    Expect-Rejection { Assert-SdkChecksumManifest $Manifest $Before } 'manifest mismatch'
    [IO.File]::WriteAllLines($Manifest, @($Before[0], $Before[1], 'unexpected'))
    Expect-Rejection { Assert-SdkChecksumManifest $Manifest $Before } 'manifest mismatch'
    Copy-Item $Unsigned "$Pair/unexpected.exe"
    Expect-Rejection { Get-SdkInstallerPair $Pair '0.0.0' } 'exactly two'
    Write-Output 'Unsigned, tampered, unapproved-signer, missing-timestamp and pair validation tests passed'
} finally {
    if ($null -ne $Certificate) {
        foreach ($StoreName in 'Root', 'TrustedPublisher', 'My') {
            $Location = if ($StoreName -eq 'My') { 'CurrentUser' } else { 'LocalMachine' }
            $Store = [Security.Cryptography.X509Certificates.X509Store]::new($StoreName, $Location)
            try { $Store.Open('ReadWrite'); $Store.Remove($Certificate) } finally { $Store.Close() }
        }
    }
    Remove-Item -LiteralPath $Root -Recurse -Force
}
