; Goggles Cast installer (Inno Setup 6). Build: .\build.ps1 -Installer
; Installs into the user's profile (no administrator rights needed).
#define AppVer GetEnv("GC_VERSION")
#if AppVer == ""
  #define AppVer "0.2.0"
#endif

[Setup]
AppId={{6E3C2B7A-2F44-4E58-9C11-5A0D9F1E7B21}
AppName=Goggles Cast
AppVersion={#AppVer}
AppPublisher=FPV-Glasses
DefaultDirName={localappdata}\Programs\GogglesCast
DefaultGroupName=Goggles Cast
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=installer
OutputBaseFilename=GogglesCast-Setup-{#AppVer}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\GogglesCast.exe
ShowLanguageDialog=yes

[Languages]
; asked at the start (the Windows display language preselected); the program takes the same one (install_lang)
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[CustomMessages]
en.DesktopIcon=Desktop shortcut
en.Shortcuts=Shortcuts:
en.Uninstall=Uninstall Goggles Cast
en.Launch=Start Goggles Cast
ru.DesktopIcon=Ярлык на рабочем столе
ru.Shortcuts=Ярлыки:
ru.Uninstall=Удалить Goggles Cast
ru.Launch=Запустить Goggles Cast

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; GroupDescription: "{cm:Shortcuts}"

[Files]
Source: "dist\GogglesCast\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; the capture program's folder is replaced as a whole
Type: filesandordirs; Name: "{app}\native"

[Icons]
Name: "{group}\Goggles Cast"; Filename: "{app}\GogglesCast.exe"
Name: "{group}\{cm:Uninstall}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Goggles Cast"; Filename: "{app}\GogglesCast.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\GogglesCast.exe"; Description: "{cm:Launch}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\settings.json"

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
begin
  { the language picked here becomes the program's (goggles_cast/i18n.py reads install_lang once) }
  if CurStep = ssPostInstall then
    SaveStringToFile(ExpandConstant('{app}\install_lang'), ActiveLanguage(), False);
end;
