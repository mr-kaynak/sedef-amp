; SPDX-License-Identifier: AGPL-3.0-or-later
;
; Inno Setup script for Sedef-Amp-Windows-Setup.exe (x64): Sedef Amp and Sedef MIDI.
; build-installer.sh runs it as
;   iscc /DAppVersion=<x.y.z> /DArtefactsDir=<JUCE artefacts folder>
;        /DMidiArtefactsDir=<Sedef MIDI's JUCE artefacts folder> /O<output folder> sedef-amp.iss
; Needs Inno Setup 6.
;
; The in-app updater (docs/updates.md) starts this setup elevated with
;   /SILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /RESTARTEXITCODE=3010
;   /COMPONENTS=<the formats installed now> /WAITPID=<pid> /WAITPIDTIME=<its creation time>
; and, from the standalone app, /RELAUNCH. /COMPONENTS names the formats found on disk plus the
; one the updater runs as, and "midi" when any of Sedef MIDI's files is installed (updaters up to
; 0.5.0 never name it, so updating from them leaves Sedef MIDI out). Never rename a component.
; /WAITPID makes Setup wait until that process (the DAW that has the
; plugin loaded, and locked) has exited before it installs anything, if it is still the process
; created at /WAITPIDTIME (a FILETIME) and not a later one that got the same id; the wait has no
; limit, as the updater says ("It installs when you close your DAW"). With /WAITPID, Setup also
; holds its own file open so nothing can change it until Setup has finished: the updater holds it
; until it exits, and Setup takes over before then. /RELAUNCH opens the standalone app again
; afterwards. Setups up to 0.4.0 ignore /WAITPID, /WAITPIDTIME and /RELAUNCH.

#ifndef AppVersion
  #error Define AppVersion, e.g. iscc /DAppVersion=0.2.0
#endif
#ifndef ArtefactsDir
  #define ArtefactsDir SourcePath + "..\..\..\build\release\plugin\SedefAmp_artefacts\Release"
#endif
#define RepoDir SourcePath + "..\..\.."
#define AssetsDir SourcePath + "..\assets"
#ifndef MidiArtefactsDir
  #define MidiArtefactsDir ArtefactsDir + "\..\..\SedefMidi_artefacts\Release"
#endif
#define AppName "Sedef Amp"
#define AppExe "Sedef Amp.exe"
#define MidiName "Sedef MIDI"
#define MidiExe "Sedef MIDI.exe"

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
FinishedLabel=Sedef Amp is installed.%n%nRescan plugins in your DAW and look for Sedef Amp by Sedef, and Sedef MIDI if you chose it. The standalone apps are in the Start menu.

[Types]
Name: "full"; Description: "Everything"
Name: "custom"; Description: "Choose formats"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (Common Files\VST3)"; Types: full custom
Name: "clap"; Description: "CLAP plugin (Common Files\CLAP)"; Types: full custom
Name: "standalone"; Description: "Standalone app with a Start menu shortcut"; Types: full custom
Name: "midi"; Description: "Sedef MIDI: play MIDI instruments from your guitar (VST3, CLAP, and an app with a Start menu shortcut)"; Types: full custom

[InstallDelete]
; Clear an older VST3 bundle, including one copied there by hand, so no stale files remain in it.
Type: filesandordirs; Name: "{commoncf64}\VST3\Sedef Amp.vst3"; Components: vst3
Type: filesandordirs; Name: "{commoncf64}\VST3\Sedef MIDI.vst3"; Components: midi

[Files]
Source: "{#ArtefactsDir}\VST3\Sedef Amp.vst3\*"; DestDir: "{commoncf64}\VST3\Sedef Amp.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace uninsrestartdelete
Source: "{#ArtefactsDir}\CLAP\Sedef Amp.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#ArtefactsDir}\Standalone\{#AppExe}"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#MidiArtefactsDir}\VST3\Sedef MIDI.vst3\*"; DestDir: "{commoncf64}\VST3\Sedef MIDI.vst3"; Components: midi; Flags: ignoreversion recursesubdirs createallsubdirs restartreplace uninsrestartdelete
Source: "{#MidiArtefactsDir}\CLAP\Sedef MIDI.clap"; DestDir: "{commoncf64}\CLAP"; Components: midi; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#MidiArtefactsDir}\Standalone\{#MidiExe}"; DestDir: "{app}"; Components: midi; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#RepoDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; DestName: "THIRD_PARTY_NOTICES.txt"; Flags: ignoreversion
Source: "{#AssetsDir}\icon.ico"; DestDir: "{app}"; DestName: "sedef-amp.ico"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Components: standalone
Name: "{autoprograms}\{#MidiName}"; Filename: "{app}\{#MidiExe}"; Components: midi

[Run]
Filename: "{app}\{#AppExe}"; Description: "Open {#AppName} now"; Components: standalone; Flags: nowait postinstall skipifsilent unchecked runasoriginaluser
; /RELAUNCH: the updater restarts the standalone app. Sedef starts Setup elevated, and then
; runasoriginaluser has no effect (Setup never saw the user's own credentials), so Explorer opens
; the app: it runs at the desktop's normal privileges, as when you start it yourself.
Filename: "{win}\explorer.exe"; Parameters: """{app}\{#AppExe}"""; Components: standalone; Flags: nowait runasoriginaluser; Check: ShouldRelaunch

[Code]
const
  SYNCHRONIZE = $00100000;
  PROCESS_QUERY_LIMITED_INFORMATION = $00001000;
  WAIT_TIMEOUT = $00000102;
  GENERIC_READ = $80000000;
  FILE_SHARE_READ = $00000001;
  OPEN_EXISTING = 3;
  FILE_TYPE_DISK = 1;

var
  HeldSetup: THandle;
  Holding: Boolean;

function OpenProcess(DesiredAccess: Cardinal; InheritHandle: Cardinal; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function GetProcessTimes(Process: THandle; var Creation, ExitTime, KernelTime, UserTime: Int64): Cardinal;
  external 'GetProcessTimes@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Cardinal;
  external 'CloseHandle@kernel32.dll stdcall';
function CreateFileW(FileName: String; DesiredAccess, ShareMode: Cardinal; Security: THandle;
  Disposition, Flags: Cardinal; Template: THandle): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function GetFileType(Handle: THandle): Cardinal;
  external 'GetFileType@kernel32.dll stdcall';

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

{ /WAITPID=<pid>: hold this setup file, so nothing can change it until Setup has finished (the
  updater holds it until it exits, so it is the file it checked), then wait until that process has
  exited, so the files it holds are free. A process that is already gone, or that was created at
  another time than /WAITPIDTIME says (its id was reused), or no /WAITPIDTIME, means no wait. }
function InitializeSetup: Boolean;
var
  Pid: Integer;
  Expected, Created, ExitTime, KernelTime, UserTime: Int64;
  Process: THandle;
begin
  Result := True;
  Pid := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if Pid <= 0 then
    Exit;
  HeldSetup := CreateFileW(ExpandConstant('{srcexe}'), GENERIC_READ, FILE_SHARE_READ, 0,
    OPEN_EXISTING, 0, 0);
  if GetFileType(HeldSetup) <> FILE_TYPE_DISK then
  begin
    Log('Cannot hold ' + ExpandConstant('{srcexe}') + '; not installing');
    Result := False;
    Exit;
  end;
  Holding := True;
  Log('Holding ' + ExpandConstant('{srcexe}'));
  { Without its creation time, a process can't be told from a later one that got its id. The
    updater always passes both. }
  Expected := StrToInt64Def(ExpandConstant('{param:WAITPIDTIME|0}'), 0);
  if Expected = 0 then
  begin
    Log(Format('No /WAITPIDTIME for process %d: not waiting for it', [Pid]));
    Exit;
  end;
  Process := OpenProcess(SYNCHRONIZE or PROCESS_QUERY_LIMITED_INFORMATION, 0, Pid);
  if Process = 0 then
    Exit;
  if (GetProcessTimes(Process, Created, ExitTime, KernelTime, UserTime) = 0) or
     (Created <> Expected) then
  begin
    Log(Format('Process %d was created at another time: not waiting for it', [Pid]));
    CloseHandle(Process);
    Exit;
  end;
  Log(Format('Waiting for process %d to exit', [Pid]));
  while WaitForSingleObject(Process, 250) = WAIT_TIMEOUT do
  begin
  end;
  CloseHandle(Process);
  Log(Format('Process %d has exited', [Pid]));
end;

procedure DeinitializeSetup;
begin
  if Holding then
    CloseHandle(HeldSetup);
end;
