# SPDX-License-Identifier: MIT
# Run in a matching Visual Studio developer environment.
param(
    [Parameter(Mandatory)][ValidateSet('Win32','x64','Arm64')][string]$Platform,
    [Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration,
    [Parameter(Mandatory)][string]$OpenSSLRoot,
    [Parameter(Mandatory)][string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed: $LASTEXITCODE" }
}
$Source = (Resolve-Path "$PSScriptRoot/../..").Path
$Crypto = (Resolve-Path "$OpenSSLRoot/lib/libcrypto.lib").Path
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$Build = Join-Path $OutputRoot "build-$Configuration-$Platform"
$Stage = Join-Path $OutputRoot "sdk-$Configuration-$Platform"
if ((Test-Path $Build) -or (Test-Path $Stage)) { throw 'Use a fresh output directory; existing files are never deleted.' }
$Runtime = if ($Configuration -eq 'Debug') { 'MultiThreadedDebugDLL' } else { 'MultiThreadedDLL' }
Invoke-Checked cmake @('-S',$Source,'-B',$Build,'-G','Ninja',
    "-DCMAKE_BUILD_TYPE=$Configuration","-DCMAKE_MSVC_RUNTIME_LIBRARY=$Runtime",
    '-DBUILD_SHARED_LIBS=OFF','-DROBOTWEAX_SRT_BUILD_TESTS=OFF',
    '-DROBOTWEAX_SRT_BUILD_TOOLS=OFF','-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF',
    '-DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF','-DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced',
    '-DOPENSSL_USE_STATIC_LIBS=ON',"-DOPENSSL_INCLUDE_DIR=$OpenSSLRoot/include",
    "-DOPENSSL_CRYPTO_LIBRARY=$Crypto")
Invoke-Checked cmake @('--build',$Build,'--parallel','2','--target','robotweax_srt')
New-Item -ItemType Directory -Path "$Stage/lib/$Configuration-$Platform" -Force | Out-Null
Copy-Item "$Build/robotweax-srt.lib" "$Stage/lib/$Configuration-$Platform/"
Copy-Item $Crypto "$Stage/lib/$Configuration-$Platform/"
New-Item -ItemType Directory -Path "$Stage/include/srt" -Force | Out-Null
Copy-Item "$Source/include/srt.h","$Source/include/robotweax_srt.h" "$Stage/include/"
Copy-Item "$Source/include/srt/*.h" "$Stage/include/srt/"
Copy-Item "$Source/LICENSE" "$Stage/LICENSE.txt"
Copy-Item "$Source/THIRD_PARTY.md" "$Stage/THIRD_PARTY.md"
Copy-Item "$PSScriptRoot/srt.props" "$Stage/srt.props"
Copy-Item "$PSScriptRoot/README.md" "$Stage/README.md"
# The producer must supply the exact dependency license next to its build.
Copy-Item "$OpenSSLRoot/LICENSE.txt" "$Stage/OpenSSL-LICENSE.txt"
@{platform=$Platform; configuration=$Configuration; runtime=$Runtime;
  source=(git -C $Source rev-parse HEAD); crypto_sha256=(Get-FileHash $Crypto).Hash
} | ConvertTo-Json | Set-Content "$Stage/lib/$Configuration-$Platform/build.json"
