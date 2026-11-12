; Glyphnotic's Windows installer (Inno Setup 6). Package-Windows.ps1 compiles
; it with the install folder and version as defines:
;
;   ISCC.exe /DProductName=glyphnotic /DDisplayName=Glyphnotic /DProductVersion=1.0.0
;            /DAuthor=... /DWebsite=... /DSourceDir=<release\Config> /DOutputDir=<release>
;            /DOutputBaseFilename=glyphnotic-1.0.0-windows-x64-installer installer-Windows.iss
;
; It installs only to OBS 33's third-party plugin location,
; C:\ProgramData\obs-studio\plugins\glyphnotic\ (docs/INSTALL.md), never to
; a legacy location, and registers an uninstaller under Apps -> Installed apps.

#ifndef ProductName
  #error Define ProductName
#endif

[Setup]
; Fixed: an upgrade or the uninstaller finds an earlier install by this id.
AppId={{6A4D2C1E-3B7F-4E58-9C0A-8D5E1F2B7C93}
AppName={#DisplayName}
AppVersion={#ProductVersion}
AppVerName={#DisplayName} {#ProductVersion}
AppPublisher={#Author}
AppPublisherURL={#Website}
AppSupportURL={#Website}/issues
AppUpdatesURL={#Website}/releases
VersionInfoVersion={#ProductVersion}
DefaultDirName={commonappdata}\obs-studio\plugins\{#ProductName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#DisplayName} (OBS Studio plugin)
; OBS holds the plugin open while it runs: ask to close it.
CloseApplications=yes
RestartApplications=no

[Messages]
WelcomeLabel2=This will install [name/ver], a set of video filters for OBS Studio 33 or later.%n%nClose OBS Studio before continuing.

[Files]
Source: "{#SourceDir}\{#ProductName}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: dirifempty; Name: "{app}"
