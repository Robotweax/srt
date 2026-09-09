# SPDX-License-Identifier: MIT
$ErrorActionPreference = 'Stop'
function Gh([string[]]$Arguments) {
    $result = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'GitHub request failed' }
    return $result
}
if ($env:SDK_RUN_ID -notmatch '^\d+$') { throw 'Invalid run ID' }
if ($env:SDK_TAG -notmatch '^windows-sdk-v\d+\.\d+\.\d+-preview\.\d+$') { throw 'Invalid preview tag' }
if ($env:SDK_SIGNER -notmatch '^[A-Fa-f0-9]{40}$') { throw 'Approved signer is not configured' }
$run = (Gh @('api',"repos/$env:GH_REPO/actions/runs/$env:SDK_RUN_ID")) | ConvertFrom-Json
if ($run.conclusion -ne 'success' -or $run.status -ne 'completed' -or
    $run.head_branch -ne 'main' -or $run.event -ne 'workflow_dispatch' -or
    $run.path -ne '.github/workflows/windows-sdk.yml' -or
    $run.head_repository.full_name -ne $env:GH_REPO) { throw 'Not a successful trusted main SDK run' }
$commit = (Gh @('api',"repos/$env:GH_REPO/commits/$env:SDK_TAG")) | ConvertFrom-Json
if ($commit.sha -ne $run.head_sha) { throw 'Preview tag does not match candidate commit' }
# A commit lookup also accepts branches: require an actual existing tag.
$null = Gh @('api',"repos/$env:GH_REPO/git/ref/tags/$env:SDK_TAG")
$directory = Join-Path $env:RUNNER_TEMP "sdk-preview-$([guid]::NewGuid())"
$null = Gh @('run','download',$env:SDK_RUN_ID,'--name','windows-sdk-installer-candidate','--dir',$directory)
$files = @(Get-ChildItem $directory -File -Recurse)
if ($files.Count -ne 1 -or $files[0].Extension -ne '.exe') { throw 'Expected exactly one installer' }
$installer = $files[0]
$signature = Get-AuthenticodeSignature $installer.FullName
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $env:SDK_SIGNER -or
    !$signature.TimeStamperCertificate) { throw 'Installer requires approved, valid, timestamped signature' }
$checksum = Join-Path $directory 'SHA256SUMS.txt'
"$((Get-FileHash $installer.FullName -Algorithm SHA256).Hash.ToLower())  $($installer.Name)" | Set-Content $checksum -Encoding ascii
$notes = Join-Path $directory 'preview-notes.md'
@"
Experimental Windows SDK preview. Not a production qualification.

Source: $($run.head_sha)
Build evidence: $($run.html_url)
Signer thumbprint: $env:SDK_SIGNER

Static Debug/Release libraries for Win32, x64 and ARM64. Use /MDd or /MD respectively.
ARM64 is build/link-tested only. No throughput qualification. AES-GCM preview is disabled.
Uninstall the previous SDK before installing. No in-place upgrade support.
See packaging/windows/README.md at the source revision for integration instructions.
"@ | Set-Content $notes -Encoding utf8
# create fails for an existing release; never upload --clobber or publish here.
$null = Gh @('release','create',$env:SDK_TAG,$installer.FullName,$checksum,
    '--verify-tag','--draft','--prerelease','--title',"Windows SDK $env:SDK_TAG",'--notes-file',$notes)
