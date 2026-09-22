# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory)][string]$ObsSource,
    [Parameter(Mandatory)][string]$WorkDirectory
)

$ErrorActionPreference = 'Stop'

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed: $LASTEXITCODE" }
}

$Repository = (Resolve-Path "$PSScriptRoot/../..").Path
$ObsSource = (Resolve-Path $ObsSource).Path
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
if (Test-Path $WorkDirectory) {
    throw 'Use a fresh Windows OBS work directory; existing files are never deleted.'
}
if ((git -C $ObsSource rev-parse HEAD) -cne 'ba2f32bdf791005443988a4955e963663e16b1ed') {
    throw 'OBS source revision does not match the qualified 32.2.2 commit.'
}
if (git -C $ObsSource status --porcelain) {
    throw 'Use an otherwise unmodified OBS checkout.'
}

New-Item -ItemType Directory -Path $WorkDirectory | Out-Null
$Evidence = New-Item -ItemType Directory -Path "$WorkDirectory/evidence"
$SrtBuild = "$WorkDirectory/srt-build"
$SrtPrefix = "$WorkDirectory/robotweax-srt"
$ObsBuild = "$WorkDirectory/obs-build"
$ObsPrefix = "$WorkDirectory/obs"
$Reference = New-Item -ItemType Directory -Path "$WorkDirectory/reference"

Invoke-Checked python @("$Repository/tests/obs/prepare_source.py", $ObsSource, '--profile', 'headless')
Invoke-Checked python @("$Repository/tests/obs/prepare_windows_source.py", $ObsSource)

Invoke-Checked cmake @(
    '-S', $Repository, '-B', $SrtBuild, '-A', 'x64',
    '-DBUILD_SHARED_LIBS=ON',
    '-DROBOTWEAX_SRT_INSTALL_LAYOUT=legacy',
    '-DROBOTWEAX_SRT_CRYPTO_BACKEND=bcrypt',
    '-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON',
    '-DROBOTWEAX_SRT_BUILD_TESTS=OFF',
    '-DROBOTWEAX_SRT_BUILD_TOOLS=OFF',
    '-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF',
    '-DROBOTWEAX_SRT_BUILD_EXAMPLES=OFF',
    "-DCMAKE_INSTALL_PREFIX=$SrtPrefix"
)
Invoke-Checked cmake @('--build', $SrtBuild, '--config', 'Release', '--parallel', '2', '--target', 'robotweax_srt')
Invoke-Checked cmake @('--install', $SrtBuild, '--config', 'Release')

$SrtLibrary = (Resolve-Path "$SrtPrefix/lib/srt.lib").Path
$SrtInclude = (Resolve-Path "$SrtPrefix/include").Path
Invoke-Checked cmake @(
    '-S', $ObsSource, '-B', $ObsBuild, '-A', 'x64',
    '-DOBS_VERSION_OVERRIDE=32.2.2-robotweax-windows-qualification',
    '-DENABLE_FRONTEND=OFF', '-DENABLE_BROWSER=OFF', '-DENABLE_SCRIPTING=OFF',
    '-DENABLE_VIRTUALCAM=OFF', '-DENABLE_WEBSOCKET=OFF', '-DENABLE_AJA=OFF',
    '-DENABLE_VLC=OFF', '-DENABLE_DECKLINK=OFF', '-DENABLE_NEW_MPEGTS_OUTPUT=ON',
    "-DLibsrt_LIBRARY:FILEPATH=$SrtLibrary",
    "-DLibsrt_INCLUDE_DIR:PATH=$SrtInclude",
    "-DCMAKE_INSTALL_PREFIX=$ObsPrefix"
)
Invoke-Checked cmake @(
    '--build', $ObsBuild, '--config', 'Release', '--parallel', '2'
)
Invoke-Checked cmake @('--install', $ObsBuild, '--config', 'Release')
Invoke-Checked cmake @('--install', $ObsBuild, '--config', 'Release', '--component', 'Development')

$DependencyPrefix = (Resolve-Path "$ObsSource/.deps/obs-deps-2026-07-15-x64").Path
$RobotweaxDll = (Resolve-Path "$SrtPrefix/bin/srt.dll").Path
$RuntimeDirectory = New-Item -ItemType Directory -Force -Path "$ObsPrefix/bin/64bit"
Get-ChildItem "$DependencyPrefix/bin/*.dll" |
    Where-Object { $_.Name -cne 'srt.dll' } |
    Copy-Item -Destination $RuntimeDirectory
Copy-Item $RobotweaxDll "$RuntimeDirectory/srt.dll"
$BundledDll = (Resolve-Path "$RuntimeDirectory/srt.dll").Path
if ((Get-FileHash $RobotweaxDll).Hash -cne (Get-FileHash $BundledDll).Hash) {
    throw 'OBS did not bundle the selected Robotweax compatibility DLL.'
}
$ReferenceDll = (Resolve-Path "$DependencyPrefix/bin/srt.dll").Path
if ((Get-FileHash $ReferenceDll).Hash -ceq (Get-FileHash $BundledDll).Hash) {
    throw 'Reference and OBS providers must remain separate binaries.'
}

$ObsObject = "$WorkDirectory/obs-peer.obj"
$ObsPeer = "$ObsPrefix/bin/64bit/windows-obs-peer.exe"
Invoke-Checked cl @(
    '/nologo', '/std:c11', '/W4', '/WX', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$ObsPrefix/include", '/c', "$Repository/tests/obs/windows_obs_peer.c",
    "/Fo$ObsObject"
)
Invoke-Checked link @(
    '/nologo', $ObsObject, "/LIBPATH:$ObsPrefix/lib", 'obs.lib', 'Psapi.lib',
    "/OUT:$ObsPeer"
)

$ReferenceObject = "$WorkDirectory/reference-peer.obj"
$ReferencePeer = "$Reference/windows-reference-peer.exe"
Invoke-Checked cl @(
    '/nologo', '/std:c11', '/W4', '/WX', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$DependencyPrefix/include", '/c',
    "$Repository/tests/obs/windows_reference_peer.c", "/Fo$ReferenceObject"
)
Invoke-Checked link @(
    '/nologo', $ReferenceObject, "/LIBPATH:$DependencyPrefix/lib", 'srt.lib',
    'ws2_32.lib', "/OUT:$ReferencePeer"
)
Copy-Item $ReferenceDll "$Reference/srt.dll"

python "$Repository/tests/obs/run_windows_smoke.py" `
    --obs-prefix $ObsPrefix `
    --obs-peer $ObsPeer `
    --reference-peer $ReferencePeer `
    --reference-srt "$Reference/srt.dll" `
    --artifacts "$Evidence/runtime" |
    Tee-Object -FilePath "$Evidence/results.txt"
if ($LASTEXITCODE -ne 0) { throw "Windows OBS qualification failed: $LASTEXITCODE" }
