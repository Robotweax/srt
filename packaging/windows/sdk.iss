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
UsePreviousAppDir=yes
UninstallLogMode=append
PrivilegesRequired=admin
ChangesEnvironment=yes
OutputBaseFilename=robotweax-srt-{#ProductVersion}-windows-sdk-{#CryptoBackend}
Compression=lzma2
SolidCompression=yes
LicenseFile={#SdkRoot}\LICENSE.txt
UninstallDisplayName=Robotweax SRT SDK ({#CryptoBackend})
[Files]
Source: "upgrade-check.ps1"; Flags: dontcopy
Source: "{#SdkRoot}\checksums.json"; DestName: "incoming-checksums.json"; Flags: dontcopy
Source: "{#SdkRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "{#BackendEnvironment}"; ValueData: "{app}"
[Code]
var
  Obsolete: TArrayOfString;
  RetiredCount: Integer;
  InstallCompleted: Boolean;

function BackupName(Index: Integer): String;
begin
  Result := ExpandConstant('{tmp}\obsolete-') + IntToStr(Index);
end;

procedure RestoreObsolete;
var I: Integer; Target: String;
begin
  for I := 0 to RetiredCount - 1 do begin
    Target := AddBackslash(ExpandConstant('{app}')) + Copy(Obsolete[I], 67, MaxInt);
    if not FileExists(Target) then
      if not CopyFile(BackupName(I), Target, True) then
        Log('Could not restore obsolete SDK file: ' + Target);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Key, PreviousDir, PreviousVersion, Params, Detail: String;
  ExitCode, I, J, Count: Integer;
  ErrorText, Plan: TArrayOfString;
begin
  Result := '';
  if CompareText(AddBackslash(ExpandConstant('{app}')),
                 AddBackslash(ExtractFileDrive(ExpandConstant('{app}')))) = 0 then begin
    Result := 'Select an SDK directory rather than a drive or share root.';
    Exit;
  end;
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Robotweax.SRT.SDK.{#CryptoBackend}_is1';
  if RegKeyExists(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\Robotweax.SRT.SDK_is1') then begin
    Result := 'Uninstall the legacy backend-neutral Robotweax SRT SDK first.';
    Exit;
  end;
  PreviousVersion := '';
  if RegKeyExists(HKLM, Key) then begin
    if not RegQueryStringValue(HKLM, Key, 'InstallLocation', PreviousDir)
       or not RegQueryStringValue(HKLM, Key, 'DisplayVersion', PreviousVersion) then begin
      Result := 'The existing SDK registration is incomplete. Uninstall it first.';
      Exit;
    end;
    if CompareText(AddBackslash(ExpandFileName(PreviousDir)),
                   AddBackslash(ExpandFileName(ExpandConstant('{app}')))) <> 0 then begin
      Result := 'Upgrade the SDK in its existing directory, or uninstall it before changing directories.';
      Exit;
    end;
    if not FileExists(ExpandConstant('{app}\unins000.exe')) then begin
      Result := 'The existing SDK uninstaller is missing. Repair its registration before upgrading.';
      Exit;
    end;
    // Only a numeric version may enter the child process argument string.
    if (Length(PreviousVersion) > 32) or (PreviousVersion = '') then begin
      Result := 'The existing SDK version is invalid.';
      Exit;
    end;
    for ExitCode := 1 to Length(PreviousVersion) do
      if Pos(PreviousVersion[ExitCode], '0123456789.') = 0 then begin
        Result := 'The existing SDK version is invalid.';
        Exit;
      end;
  end else if FileExists(ExpandConstant('{app}\unins000.exe')) then begin
    Result := 'This directory belongs to another installation. Select a separate SDK directory.';
    Exit;
  end;
  ExtractTemporaryFile('upgrade-check.ps1');
  ExtractTemporaryFile('incoming-checksums.json');
  Params := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ExpandConstant('{tmp}\upgrade-check.ps1') + '" -Root "' +
    RemoveBackslashUnlessRoot(ExpandConstant('{app}')) + '" -Manifest "' +
    ExpandConstant('{tmp}\incoming-checksums.json') + '" -OldVersion "' +
    PreviousVersion + '" -NewVersion "{#ProductVersion}" -Backend "{#CryptoBackend}" -PlanPath "' +
    ExpandConstant('{tmp}\obsolete.txt') + '" -ErrorPath "' + ExpandConstant('{tmp}\upgrade-error.txt') + '"';
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Params,
              '', SW_HIDE, ewWaitUntilTerminated, ExitCode) or (ExitCode <> 0) then begin
    Detail := 'SDK upgrade validation failed. Use /LOG for details.';
    if LoadStringsFromFile(ExpandConstant('{tmp}\upgrade-error.txt'), ErrorText) then
      if GetArrayLength(ErrorText) > 0 then Detail := ErrorText[0];
    Log(Detail);
    Result := Detail;
    Exit;
  end;
  SetArrayLength(Obsolete, 0);
  if not LoadStringsFromFile(ExpandConstant('{tmp}\obsolete.txt'), Plan) then begin
    Result := 'Could not read the SDK upgrade plan.';
    Exit;
  end;
  // A BOM-only UTF-8 file can load as one empty line. Fresh installs and
  // repairs without obsolete files must not treat it as a file to retire.
  for I := 0 to GetArrayLength(Plan) - 1 do begin
    if Trim(Plan[I]) <> '' then begin
      if (Length(Plan[I]) <= 66) or (Copy(Plan[I], 65, 2) <> '  ') then begin
        Result := 'The SDK upgrade plan contains an invalid entry.';
        Exit;
      end;
      for J := 1 to 64 do
        if Pos(Lowercase(Plan[I][J]), '0123456789abcdef') = 0 then begin
          Result := 'The SDK upgrade plan contains an invalid digest.';
          Exit;
        end;
      Count := GetArrayLength(Obsolete);
      SetArrayLength(Obsolete, Count + 1);
      Obsolete[Count] := Plan[I];
    end;
  end;
  Log('SDK files to retire: ' + IntToStr(GetArrayLength(Obsolete)));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var I: Integer; Target, Digest: String;
begin
  if CurStep = ssInstall then
    for I := 0 to GetArrayLength(Obsolete) - 1 do begin
      Digest := Copy(Obsolete[I], 1, 64);
      Target := AddBackslash(ExpandConstant('{app}')) + Copy(Obsolete[I], 67, MaxInt);
      if CompareText(GetSHA256OfFile(Target), Digest) <> 0 then
        RaiseException('An obsolete SDK file changed after validation: ' + Target);
      if not CopyFile(Target, BackupName(I), True) then
        RaiseException('Could not preserve obsolete SDK file: ' + Target);
      if not DeleteFile(Target) then
        RaiseException('Could not remove obsolete SDK file: ' + Target);
      RetiredCount := I + 1;
    end;
  if CurStep = ssDone then InstallCompleted := True;
end;

procedure DeinitializeSetup;
begin
  if not InstallCompleted then RestoreObsolete;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Current: String;
begin
  if CurUninstallStep = usUninstall then
    if RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', '{#BackendEnvironment}', Current) then
      if CompareText(Current, ExpandConstant('{app}')) = 0 then
        RegDeleteValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', '{#BackendEnvironment}');
end;
