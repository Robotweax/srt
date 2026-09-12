# SPDX-License-Identifier: MIT
# Run in a matching Visual Studio developer environment.
param(
    [Parameter(Mandatory)][ValidateSet('Win32','x64','Arm64')][string]$Platform,
    [Parameter(Mandatory)][ValidateSet('Debug','Release')][string]$Configuration,
    [ValidateSet('openssl','bcrypt')][string]$CryptoBackend = 'openssl',
    [string]$OpenSSLRoot,
    [Parameter(Mandatory)][string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed: $LASTEXITCODE" }
}
$Source = (Resolve-Path "$PSScriptRoot/../..").Path
$Crypto = $null
if ($CryptoBackend -eq 'openssl') {
    if (!$OpenSSLRoot) { throw 'OpenSSLRoot is required for the OpenSSL backend' }
    $Crypto = (Resolve-Path "$OpenSSLRoot/lib/libcrypto.lib").Path
} elseif ($OpenSSLRoot) { throw 'OpenSSLRoot must not be supplied for BCrypt' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$Build = Join-Path $OutputRoot "build-$Configuration-$Platform"
$Stage = Join-Path $OutputRoot "sdk-$Configuration-$Platform"
if ((Test-Path $Build) -or (Test-Path $Stage)) { throw 'Use a fresh output directory; existing files are never deleted.' }
$Runtime = if ($Configuration -eq 'Debug') { 'MultiThreadedDebugDLL' } else { 'MultiThreadedDLL' }
$Options = @('-S',$Source,'-B',$Build,'-G','Ninja',
    "-DCMAKE_BUILD_TYPE=$Configuration","-DCMAKE_MSVC_RUNTIME_LIBRARY=$Runtime",
    '-DBUILD_SHARED_LIBS=OFF','-DROBOTWEAX_SRT_BUILD_TESTS=OFF',
    '-DROBOTWEAX_SRT_BUILD_TOOLS=OFF','-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF',
    '-DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF','-DROBOTWEAX_SRT_INSTALL_LAYOUT=namespaced',
    "-DROBOTWEAX_SRT_CRYPTO_BACKEND=$CryptoBackend")
if ($CryptoBackend -eq 'openssl') {
    $Options += @(
    '-DOPENSSL_USE_STATIC_LIBS=ON',"-DOPENSSL_ROOT_DIR=$OpenSSLRoot",
    "-DOPENSSL_INCLUDE_DIR=$OpenSSLRoot/include",
    "-DOPENSSL_CRYPTO_LIBRARY=$Crypto",
    "-DLIB_EAY_DEBUG:FILEPATH=$Crypto","-DLIB_EAY_RELEASE:FILEPATH=$Crypto")
} else { $Options += '-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON' }
Invoke-Checked cmake $Options
# FindOpenSSL on MSVC derives its imported target from LIB_EAY_DEBUG/RELEASE,
# not just OPENSSL_CRYPTO_LIBRARY. Fail before compilation if that pin changes.
$Cache = Get-Content "$Build/CMakeCache.txt"
foreach ($Name in $(if ($CryptoBackend -eq 'openssl') { @('LIB_EAY_DEBUG', 'LIB_EAY_RELEASE') } else { @() })) {
    $Entry = @($Cache | Where-Object { $_ -like "${Name}:FILEPATH=*" })
    if ($Entry.Count -ne 1) { throw "Missing pinned $Name" }
    $Selected = $Entry[0].Substring($Entry[0].IndexOf('=') + 1)
    if ([IO.Path]::GetFullPath($Selected) -ne [IO.Path]::GetFullPath($Crypto)) {
        throw "Unexpected OpenSSL selection: $Selected"
    }
}
Invoke-Checked cmake @('--build',$Build,'--parallel','2','--target','robotweax_srt')
New-Item -ItemType Directory -Path "$Stage/lib/$Configuration-$Platform" -Force | Out-Null
Copy-Item "$Build/robotweax-srt.lib" "$Stage/lib/$Configuration-$Platform/"
if ($Crypto) { Copy-Item $Crypto "$Stage/lib/$Configuration-$Platform/" }
New-Item -ItemType Directory -Path "$Stage/include/srt" -Force | Out-Null
Copy-Item "$Source/include/srt.h","$Source/include/robotweax_srt.h" "$Stage/include/"
Copy-Item "$Source/include/srt/*.h" "$Stage/include/srt/"
Copy-Item "$Source/LICENSE" "$Stage/LICENSE.txt"
Copy-Item "$Source/THIRD_PARTY.md" "$Stage/THIRD_PARTY.md"
Copy-Item "$PSScriptRoot/srt.props" "$Stage/srt.props"
Copy-Item "$PSScriptRoot/README.md" "$Stage/README.md"
# The producer must supply the exact dependency license next to its build.
if ($Crypto) { Copy-Item "$OpenSSLRoot/LICENSE.txt" "$Stage/OpenSSL-LICENSE.txt" }
$Dependencies = if ($CryptoBackend -eq 'openssl') { 'libcrypto.lib;crypt32.lib;advapi32.lib;user32.lib;bcrypt.lib' } else { 'bcrypt.lib' }
@"
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <RobotweaxSrtCryptoBackend>$CryptoBackend</RobotweaxSrtCryptoBackend>
    <RobotweaxSrtCryptoDependencies>$Dependencies</RobotweaxSrtCryptoDependencies>
  </PropertyGroup>
</Project>
"@ | Set-Content "$Stage/srt-backend.props"
@{platform=$Platform; configuration=$Configuration; runtime=$Runtime;
  source=(git -C $Source rev-parse HEAD); crypto_backend=$CryptoBackend;
  crypto_sha256=$(if ($Crypto) { (Get-FileHash $Crypto).Hash } else { $null })
} | ConvertTo-Json | Set-Content "$Stage/lib/$Configuration-$Platform/build.json"
