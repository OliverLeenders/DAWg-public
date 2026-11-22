; The Windows installer (Inno Setup 6). Build it with tools/build_installer.py, which passes:
;   AppVersion  the project version from CMakeLists.txt
;   AppDir      the Release app folder (DAWg.exe with onnxruntime.dll, ui/, faust/, midi-files/, models/ and licenses/ next to it)
;   RedistDir   the MSVC runtime DLLs (VC\Redist\MSVC\<version>\x64\Microsoft.VC143.CRT), installed next to the app
;   OutputDir   where the setup program goes
; The WebView2 runtime that the UI needs ships with Windows 11 and current Windows 10.

#ifndef AppVersion
  #error Run tools/build_installer.py, which defines AppVersion, AppDir, RedistDir and OutputDir.
#endif

[Setup]
AppId={{6F0C8B8E-3A7D-4C55-9C2B-5D2E7A1F4D10}
AppName=DAWg
AppVersion={#AppVersion}
AppPublisher=DAWg
AppPublisherURL=https://github.com/OliverLeenders/DAWg
DefaultDirName={autopf}\DAWg
DefaultGroupName=DAWg
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=DAWg-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\DAWg.exe
SetupIconFile=..\branding\dawg.ico
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#AppDir}\DAWg.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppDir}\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AppDir}\faust\*"; DestDir: "{app}\faust"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AppDir}\midi-files\*"; DestDir: "{app}\midi-files"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AppDir}\models\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AppDir}\onnxruntime.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AppDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RedistDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RedistDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RedistDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
Type: filesandordirs; Name: "{app}\ui"

[Icons]
Name: "{group}\DAWg"; Filename: "{app}\DAWg.exe"
Name: "{group}\{cm:UninstallProgram,DAWg}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\DAWg"; Filename: "{app}\DAWg.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\DAWg.exe"; Description: "{cm:LaunchProgram,DAWg}"; Flags: nowait postinstall skipifsilent
