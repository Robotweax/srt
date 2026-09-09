# SPDX-License-Identifier: MIT
param([string]$Platform, [string]$Configuration)
$ErrorActionPreference = 'Stop'
function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed: $LASTEXITCODE" }
}
$Root = (Get-Location).Path
$Target = @{Win32='VC-WIN32';x64='VC-WIN64A';Arm64='VC-WIN64-ARM'}[$Platform]
if (!$Target) { throw 'Invalid platform' }
$Prefix = "$Root/crypto-install"
New-Item -ItemType Directory crypto-build | Out-Null
Push-Location crypto-build
try {
    $Options = @("$Root/openssl-source/Configure",$Target,'no-shared','no-tests','no-asm',"--prefix=$Prefix",'--libdir=lib')
    if ($Configuration -eq 'Debug') { $Options += '--debug' }
    Run perl $Options
    Run nmake @()
    Run nmake @('install_dev')
} finally { Pop-Location }
Copy-Item "$Root/openssl-source/LICENSE.txt" "$Prefix/LICENSE.txt"
& "$PSScriptRoot/build-sdk.ps1" -Platform $Platform -Configuration $Configuration -OpenSSLRoot $Prefix -OutputRoot "$Root/output"
if (!$?) { throw 'SDK build failed' }
$Sdk = "$Root/output/sdk-$Configuration-$Platform"
$Runtime = if ($Configuration -eq 'Debug') { '/MDd' } else { '/MD' }
# Link every architecture; execute only architectures supported by this runner.
Run cl @('/nologo','/std:c11',$Runtime,"/I$Sdk/include",'/c',"$Root/tests/package_consumer/c_consumer.c",'/Foconsumer.obj')
Run link @('/nologo','consumer.obj',"/LIBPATH:$Sdk/lib/$Configuration-$Platform",'robotweax-srt.lib','libcrypto.lib','ws2_32.lib','crypt32.lib','advapi32.lib','user32.lib','bcrypt.lib','/OUT:consumer.exe')
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
