# SPDX-License-Identifier: MIT
# Manual CI diagnostic. Only logs/CSV are uploaded, never reference binaries.
param([ValidateSet('x64','Arm64')][string]$Platform = 'x64')
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
    & "$PSScriptRoot/build-ci.ps1" -Platform $Platform -Configuration Release -CryptoBackend openssl
    if (!$?) { throw 'Pinned dependency build failed' }
    $Crypto = "$PWD/crypto-install/lib/libcrypto.lib"
    Run cmake @('-S','tests/crypto_provider_comparison','-B','measure','-A',$Platform,
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
    & ./measure/Release/provider_benchmark.exe --ecb | Set-Content evidence/ecb.csv
    if ($LASTEXITCODE -ne 0) { throw 'ECB benchmark failed' }
    & ./measure/Release/provider_benchmark.exe --ctr-components | Set-Content evidence/ctr-components.csv
    if ($LASTEXITCODE -ne 0) { throw 'CTR component benchmark failed' }
    & ./measure/Release/provider_benchmark.exe --ctr-candidate | Set-Content evidence/ctr-candidate.csv
    if ($LASTEXITCODE -ne 0) { throw 'CTR candidate benchmark failed' }
    foreach ($backend in 'bcrypt','openssl') {
        $options = @('-S','tests/crypto_end_to_end','-B',"e2e-$backend",'-A',$Platform,
            "-DROBOTWEAX_SRT_CRYPTO_BACKEND=$backend",'-DBUILD_SHARED_LIBS=OFF')
        if ($backend -eq 'openssl') {
            $options += @('-DOPENSSL_USE_STATIC_LIBS=ON',"-DOPENSSL_ROOT_DIR=$PWD/crypto-install",
                "-DOPENSSL_INCLUDE_DIR=$PWD/crypto-install/include",
                "-DLIB_EAY_DEBUG:FILEPATH=$Crypto","-DLIB_EAY_RELEASE:FILEPATH=$Crypto")
        } else { $options += '-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON' }
        Run cmake $options
        Run cmake @('--build',"e2e-$backend",'--config','Release','--parallel','2','--target','crypto_end_to_end')
        Copy-Item "e2e-$backend/CMakeCache.txt" "evidence/e2e-$backend-cache.txt"
        Get-FileHash "e2e-$backend/Release/crypto_end_to_end.exe" | ConvertTo-Json | Set-Content "evidence/e2e-$backend-hash.json"
    }
    Start-Sleep -Seconds 15
    for ($round = 0; $round -lt 3; $round++) {
        $backends = if ($round % 2) { @('openssl','bcrypt') } else { @('bcrypt','openssl') }
        foreach ($backend in $backends) {
            foreach ($mode in 0,1,2) {
                $result = & "./e2e-$backend/Release/crypto_end_to_end.exe" $mode 2> "evidence/e2e-$round-$backend-$mode-error.log"
                if ($LASTEXITCODE -ne 0) { throw "End-to-end failure: $round $backend $mode" }
                $record = $result | ConvertFrom-Json
                if (!$record.integrity) { throw 'End-to-end integrity failed' }
                $record | Add-Member -NotePropertyName backend -NotePropertyValue $backend
                $record | Add-Member -NotePropertyName round -NotePropertyValue $round
                $record | ConvertTo-Json -Compress | Add-Content evidence/end-to-end.jsonl
            }
        }
    }
} finally { Stop-Transcript }
