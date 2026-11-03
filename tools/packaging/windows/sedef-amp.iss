; SPDX-License-Identifier: AGPL-3.0-or-later
;
; Inno Setup script for Sedef-Amp-Windows-Setup.exe (x64). build-installer.sh runs it as
;   iscc /DAppVersion=<x.y.z> /DArtefactsDir=<JUCE artefacts folder> /O<output folder> sedef-amp.iss
; Needs Inno Setup 6.
;
; The in-app updater (docs/updates.md) starts this setup elevated with
;   /SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /RESTARTEXITCODE=3010 /WAITPID=<pid>
; and, from the standalone app, /RELAUNCH. /WAITPID makes Setup wait until that process (the DAW
; that has the plugin loaded, and locked) has exited before it installs anything; /RELAUNCH opens
; the standalone app again afterwards. Silent mode keeps the formats chosen at the previous install
; (UsePreviousSetupType). Setups from before 0.3.1 ignore both switches.

#ifndef AppVersion
  #error Define AppVersion, e.g. iscc /DAppVersion=0.2.0
#endif
#ifndef ArtefactsDir
  #define ArtefactsDir SourcePath + "..\..\..\build\release\plugin\SedefAmp_artefacts\Release"
#endif
#define RepoDir SourcePath + "..\..\.."
#define AssetsDir SourcePath + "..\assets"
#define AppName "Sedef Amp"
#define AppExe "Sedef Amp.exe"

[Setup]
; AppId identifies the installation for upgrades and the uninstaller; never change it.
AppId={{8F234EBD-87BC-4821-9789-4BF2E8D8AB79}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Sedef
AppPublisherURL=https://github.com/mr-kaynak/sedef-amp
AppSupportURL=https://github.com/mr-kaynak/sedef-amp/issues
AppUpdatesURL=https://github.com/mr-kaynak/sedef-amp/releases
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
LicenseFile={#RepoDir}\LICENSE
SetupIconFile={#AssetsDir}\icon.ico
UninstallDisplayIcon={app}\sedef-amp.ico
UninstallDisplayName={#AppName}
OutputBaseFilename=Sedef-Amp-Windows-Setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
; A DAW with the plugin loaded keeps its binary locked (the in-app updater starts Setup from
; inside one). Restart Manager finds such programs, including by the plugin extensions added to
; the default filter, and the Preparing to Install page offers to close them. If the user keeps
; them open, restartreplace swaps the files in at the next restart and Setup says so.
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.dll,*.chm,*.vst3,*.clap
RestartApplications=no

[Messages]
ApplicationsFound=These programs are using Sedef Amp, for example a DAW with the plugin loaded. Save your work in them first.%n%nSetup can close them now. If you keep them open, the update is completed when you restart your computer.
FinishedLabel=Sedef Amp is installed.%n%nRescan plugins in your DAW and look for Sedef Amp by Sedef. The standalone app is in the Start menu.

[Types]
Name: "full"; Description: "Everything"
Name: "custom"; Description: "Choose formats"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (Common Files\VST3)"; Types: full custom
Name: "clap"; Description: "CLAP plugin (Common Files\CLAP)"; Types: full custom
Name: "standalone"; Description: "Standalone app with a Start menu shortcut"; Types: full custom

[InstallDelete]
; Clear an older VST3 bundle, including one copied there by hand, so no stale files remain in it.
Type: filesandordirs; Name: "{commoncf64}\VST3\Sedef Amp.vst3"; Components: vst3

[Files]
Source: "{#ArtefactsDir}\VST3\Sedef Amp.vst3\*"; DestDir: "{commoncf64}\VST3\Sedef Amp.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace uninsrestartdelete
Source: "{#ArtefactsDir}\CLAP\Sedef Amp.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#ArtefactsDir}\Standalone\{#AppExe}"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; DestName: "THIRD_PARTY_NOTICES.txt"; Flags: ignoreversion
Source: "{#AssetsDir}\icon.ico"; DestDir: "{app}"; DestName: "sedef-amp.ico"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Components: standalone

[Run]
Filename: "{app}\{#AppExe}"; Description: "Open {#AppName} now"; Components: standalone; Flags: nowait postinstall skipifsilent unchecked runasoriginaluser
; /RELAUNCH: the updater restarts the standalone app. Sedef starts Setup elevated, and then
; runasoriginaluser has no effect (Setup never saw the user's own credentials), so Explorer opens
; the app: it runs at the desktop's normal privileges, as when you start it yourself.
Filename: "{win}\explorer.exe"; Parameters: """{app}\{#AppExe}"""; Components: standalone; Flags: nowait runasoriginaluser; Check: ShouldRelaunch

[Code]
const
  SYNCHRONIZE = $00100000;
  WAIT_TIMEOUT = $00000102;

function OpenProcess(DesiredAccess: Cardinal; InheritHandle: Cardinal; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Cardinal;
  external 'CloseHandle@kernel32.dll stdcall';

{ Whether the command line holds /Name (any case). }
function HasSwitch(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
      Result := True;
end;

function ShouldRelaunch: Boolean;
begin
  Result := HasSwitch('/RELAUNCH');
end;

{ /WAITPID=<pid>: wait until that process has exited, so the files it holds are free. A process
  that is already gone (or never existed) means no wait. }
function InitializeSetup: Boolean;
var
  Pid: Integer;
  Process: THandle;
begin
  Result := True;
  Pid := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if Pid <= 0 then
    Exit;
  Process := OpenProcess(SYNCHRONIZE, 0, Pid);
  if Process = 0 then
    Exit;
  Log(Format('Waiting for process %d to exit', [Pid]));
  while WaitForSingleObject(Process, 250) = WAIT_TIMEOUT do
  begin
  end;
  CloseHandle(Process);
  Log(Format('Process %d has exited', [Pid]));
end;
