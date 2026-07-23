; DendroLog — Inno Setup installer script.
;
; Собирается в CI (см. .github/workflows/release.yml):
;   iscc /DAppVersion=1.2.3 /DStagingDir=<папка с exe и Qt DLL> dendrolog.iss
; Локально: сначала windeployqt в папку stage, затем iscc с теми же define.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
; VERSIONINFO установщика требует строго числовую X.Y.Z; AppVersion у
; dev-сборок CI содержит суффикс (dev-57), поэтому числовая версия задаётся
; отдельным define — релизный workflow передаёт настоящую.
#ifndef NumericVersion
  #define NumericVersion "0.0.0"
#endif
#ifndef StagingDir
  #define StagingDir "..\..\build\stage"
#endif

[Setup]
AppId={{8F4C1D7A-2B3E-4A5D-9C6F-D1E8A7B4C302}
AppName=DendroLog
AppVersion={#AppVersion}
AppVerName=DendroLog {#AppVersion}
VersionInfoVersion={#NumericVersion}
AppPublisher=Anton Petrov
AppPublisherURL=https://github.com/scaremongr/dendrolog
AppSupportURL=https://github.com/scaremongr/dendrolog/issues
AppUpdatesURL=https://github.com/scaremongr/dendrolog/releases
DefaultDirName={autopf}\DendroLog
DefaultGroupName=DendroLog
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputBaseFilename=DendroLog-{#AppVersion}-setup
SetupIconFile=..\..\resources\windows\dendrolog.ico
UninstallDisplayIcon={app}\DendroLog.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; По умолчанию ставимся per-user (без UAC); диалог позволяет выбрать per-machine.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "associatelog"; Description: "Associate .log files with DendroLog"

[Files]
Source: "{#StagingDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\DendroLog"; Filename: "{app}\DendroLog.exe"
Name: "{autodesktop}\DendroLog"; Filename: "{app}\DendroLog.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.log\OpenWithProgids"; ValueType: string; ValueName: "DendroLog.log"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associatelog
Root: HKA; Subkey: "Software\Classes\DendroLog.log"; ValueType: string; ValueName: ""; ValueData: "Log file"; Flags: uninsdeletekey; Tasks: associatelog
; Индекс 1 — иконка документа (ресурс IDI_LOGFILE в dendrolog.rc.in), а не
; иконка приложения: иначе .log-файлы в проводнике неотличимы от ярлыка exe.
Root: HKA; Subkey: "Software\Classes\DendroLog.log\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\DendroLog.exe,1"; Tasks: associatelog
Root: HKA; Subkey: "Software\Classes\DendroLog.log\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\DendroLog.exe"" ""%1"""; Tasks: associatelog

[Run]
Filename: "{app}\DendroLog.exe"; Description: "{cm:LaunchProgram,DendroLog}"; Flags: nowait postinstall skipifsilent
