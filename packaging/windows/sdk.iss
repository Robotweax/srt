; SPDX-License-Identifier: MIT
#ifndef SdkRoot
  #error SdkRoot is required
#endif
#ifndef ProductVersion
  #error ProductVersion is required
#endif
#ifndef CryptoBackend
  #error CryptoBackend is required
#endif
#if CryptoBackend == "openssl"
  #define BackendEnvironment "ROBOTWEAX_SRT_OPENSSL"
#elif CryptoBackend == "bcrypt"
  #define BackendEnvironment "ROBOTWEAX_SRT_BCRYPT"
#else
  #error Invalid CryptoBackend
#endif
[Setup]
AppId=Robotweax.SRT.SDK.{#CryptoBackend}
AppName=Robotweax SRT SDK ({#CryptoBackend})
AppVersion={#ProductVersion}
AppPublisher=Robotweax GmbH
DefaultDirName={autopf}\Robotweax-SRT-{#CryptoBackend}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ChangesEnvironment=yes
OutputBaseFilename=robotweax-srt-{#ProductVersion}-windows-sdk-{#CryptoBackend}
Compression=lzma2
SolidCompression=yes
LicenseFile={#SdkRoot}\LICENSE.txt
UninstallDisplayName=Robotweax SRT SDK ({#CryptoBackend})
[Files]
Source: "{#SdkRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "{#BackendEnvironment}"; ValueData: "{app}"
[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if RegKeyExists(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Robotweax.SRT.SDK_is1')
     or RegKeyExists(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Robotweax.SRT.SDK.{#CryptoBackend}_is1')
     or FileExists(ExpandConstant('{app}\unins000.exe')) then
    Result := 'Uninstall the previous Robotweax SRT SDK before installing this candidate.';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Current: String;
begin
  if CurUninstallStep = usUninstall then
    if RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', '{#BackendEnvironment}', Current) then
      if CompareText(Current, ExpandConstant('{app}')) = 0 then
        RegDeleteValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', '{#BackendEnvironment}');
end;
