# SPDX-License-Identifier: MIT
param([string]$Platform, [string]$Configuration,
    [ValidateSet('openssl','bcrypt')][string]$CryptoBackend = 'openssl')
$ErrorActionPreference = 'Stop'
function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed: $LASTEXITCODE" }
}
$Root = (Get-Location).Path
if ($CryptoBackend -eq 'openssl') {
$Target = @{Win32='VC-WIN32';x64='VC-WIN64A';Arm64='VC-WIN64-CLANGASM-ARM'}[$Platform]
if (!$Target) { throw 'Invalid platform' }
$Assembler = if ($Platform -eq 'Arm64') { 'clang-cl.exe' } else { 'nasm.exe' }
$Candidates = if ($Platform -eq 'Arm64') {
    @("${env:VSINSTALLDIR}VC\Tools\Llvm\bin", "${env:ProgramFiles}\LLVM\bin")
} else {
    @("${env:ProgramFiles}\NASM", "${env:ProgramFiles(x86)}\NASM")
}
if (!(Get-Command $Assembler -ErrorAction SilentlyContinue)) {
    foreach ($Directory in $Candidates) {
        if (Test-Path (Join-Path $Directory $Assembler)) {
            $env:PATH = "$Directory;$env:PATH"
            break
        }
    }
}
if (!(Get-Command $Assembler -ErrorAction SilentlyContinue)) { throw "Required assembler missing: $Assembler" }
Run $Assembler @('--version')
$Prefix = "$Root/crypto-install"
New-Item -ItemType Directory crypto-build | Out-Null
Push-Location crypto-build
try {
    $Options = @("$Root/openssl-source/Configure",$Target,'no-shared','no-tests',"--prefix=$Prefix",'--libdir=lib')
    if ($Configuration -eq 'Debug') { $Options += '--debug' }
    Run perl $Options
    # Fail closed if Configure disables assembly; retain its full configuration.
    Run perl @('-I.','-Mconfigdata','-e','die "OpenSSL assembly disabled" if exists $configdata::disabled{asm}; die "Missing assembly architecture" unless $configdata::target{asm_arch};')
    Run perl @('configdata.pm','--dump')
    Run nmake @()
    Run nmake @('install_dev')
} finally { Pop-Location }
Copy-Item "$Root/openssl-source/LICENSE.txt" "$Prefix/LICENSE.txt"
}
$SdkOptions = @{Platform=$Platform; Configuration=$Configuration; CryptoBackend=$CryptoBackend; OutputRoot="$Root/output"}
if ($CryptoBackend -eq 'openssl') { $SdkOptions.OpenSSLRoot = $Prefix }
& "$PSScriptRoot/build-sdk.ps1" @SdkOptions
if (!$?) { throw 'SDK build failed' }
$Sdk = "$Root/output/sdk-$Configuration-$Platform"
$Runtime = if ($Configuration -eq 'Debug') { '/MDd' } else { '/MD' }
# Link every architecture; execute only architectures supported by this runner.
Run cl @('/nologo','/std:c11',$Runtime,"/I$Sdk/include",'/c',"$Root/tests/package_consumer/c_consumer.c",'/Foconsumer.obj')
$Libraries = @('robotweax-srt.lib','ws2_32.lib','bcrypt.lib')
if ($CryptoBackend -eq 'openssl') { $Libraries += @('libcrypto.lib','crypt32.lib','advapi32.lib','user32.lib') }
Run link (@('/nologo','consumer.obj',"/LIBPATH:$Sdk/lib/$Configuration-$Platform",'/OUT:consumer.exe') + $Libraries)
if ($Platform -ne 'Arm64') { Run "$Root/consumer.exe" @() }

# Exercise the shipped props rather than duplicating its linker settings.
$Project = "$PSScriptRoot/consumer/consumer.vcxproj"
$Properties = @("/p:Configuration=$Configuration", "/p:Platform=$Platform", "/p:ROBOTWEAX_SRT=$Sdk")
Run msbuild (@($Project,'/nologo','/t:Rebuild') + $Properties)
if ($Platform -ne 'Arm64') {
    Run "$PSScriptRoot/consumer/out/$Configuration-$Platform/sdk-consumer.exe" @()
}
$WrongRuntime = if ($Configuration -eq 'Debug') { 'MultiThreadedDebug' } else { 'MultiThreaded' }
$FailureOutput = & msbuild $Project /nologo /t:Rebuild @Properties "/p:SdkTestRuntime=$WrongRuntime" 2>&1
if ($LASTEXITCODE -eq 0 -or ($FailureOutput -join "`n") -notmatch 'SDK requires /MD') {
    throw 'MSBuild props did not reject the incompatible CRT profile as expected'
}
