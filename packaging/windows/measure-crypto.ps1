# SPDX-License-Identifier: MIT
# Manual CI diagnostic. Only logs/CSV are uploaded, never reference binaries.
$ErrorActionPreference = 'Stop'
function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed" }
}
New-Item -ItemType Directory evidence | Out-Null
Start-Transcript -Path evidence/build-and-measurement.log
try {
    # Reuse the pinned, assembly-checked SDK dependency recipe and its consumer
    # checks rather than benchmarking an unknown runner OpenSSL installation.
    & "$PSScriptRoot/build-ci.ps1" -Platform x64 -Configuration Release -CryptoBackend openssl
    if (!$?) { throw 'Pinned dependency build failed' }
    $Crypto = "$PWD/crypto-install/lib/libcrypto.lib"
    Run cmake @('-S','tests/crypto_provider_comparison','-B','measure','-A','x64',
        '-DBUILD_PROVIDER_BENCHMARK=ON','-DOPENSSL_USE_STATIC_LIBS=ON',
        "-DOPENSSL_ROOT_DIR=$PWD/crypto-install","-DOPENSSL_INCLUDE_DIR=$PWD/crypto-install/include",
        "-DLIB_EAY_DEBUG:FILEPATH=$Crypto","-DLIB_EAY_RELEASE:FILEPATH=$Crypto")
    Run cmake @('--build','measure','--config','Release','--parallel','2')
    Run ctest @('--test-dir','measure','-C','Release','--output-on-failure')
    Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors | ConvertTo-Json | Set-Content evidence/cpu.json
    Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber | ConvertTo-Json | Set-Content evidence/os.json
    Run powercfg @('/getactivescheme')
    "sha=$env:GITHUB_SHA; runner=$env:RUNNER_ARCH; configuration=Release; clock=steady_clock; order=alternating; setup=excluded" | Set-Content evidence/metadata.txt
    Get-FileHash $Crypto,measure/Release/provider_benchmark.exe | ConvertTo-Json | Set-Content evidence/binary-hashes.json
    Copy-Item measure/CMakeCache.txt evidence/
    Start-Sleep -Seconds 15
    & ./measure/Release/provider_benchmark.exe | Set-Content evidence/provider.csv
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark failed' }
    & ./measure/Release/provider_benchmark.exe --ctr-sweep | Set-Content evidence/ctr-sweep.csv
    if ($LASTEXITCODE -ne 0) { throw 'CTR sweep failed' }
} finally { Stop-Transcript }
