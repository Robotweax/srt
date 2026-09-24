# SPDX-License-Identifier: MIT
# Check SDK library lookup only; custom Visual C++ toolset configurations remain consumer-owned.
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ tools are required' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
if (!(Test-Path $msbuild)) { throw "MSBuild not found: $msbuild" }

$root = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $root | Out-Null
try {
    Copy-Item (Join-Path $PSScriptRoot 'srt.props') (Join-Path $root 'srt.props')
    '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003" />' |
        Set-Content (Join-Path $root 'srt-backend.props')
    $project = Join-Path $root 'test.proj'
    $projectXml = @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="srt.props" />
  <Target Name="VerifySdkLibDir">
    <Error Condition="'$(RobotweaxSrtLibDir)' != '$(MSBuildProjectDirectory)\lib\$(Configuration)-$(ExpectedPlatform)'" Text="Wrong SDK library path: $(RobotweaxSrtLibDir)" />
    <Error Condition="!Exists('$(RobotweaxSrtLibDir)\robotweax-srt.lib')" Text="Missing mapped SDK library: $(RobotweaxSrtLibDir)" />
  </Target>
</Project>
'@
    Set-Content -Path $project -Value $projectXml

    $platforms = @(
        @{ Name = 'Win32'; Packaged = 'Win32' },
        @{ Name = 'x86'; Packaged = 'Win32' },
        @{ Name = 'x64'; Packaged = 'x64' },
        @{ Name = 'Win64'; Packaged = 'x64' },
        @{ Name = 'Arm64'; Packaged = 'Arm64' }
    )
    foreach ($configuration in 'Debug', 'Release') {
        foreach ($packaged in 'Win32', 'x64', 'Arm64') {
            $directory = Join-Path $root "lib\$configuration-$packaged"
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
            'fixture' | Set-Content (Join-Path $directory 'robotweax-srt.lib')
        }
        foreach ($platform in $platforms) {
            & $msbuild $project -nologo -verbosity:quiet -target:VerifySdkLibDir `
                "-property:Configuration=$configuration" `
                "-property:Platform=$($platform.Name)" `
                "-property:ExpectedPlatform=$($platform.Packaged)"
            if ($LASTEXITCODE -ne 0) {
                throw "SDK path resolution failed for $configuration-$($platform.Name)"
            }
        }
    }
    Write-Output 'SDK platform alias path tests passed'
} finally {
    Remove-Item -Recurse -Force $root
}
