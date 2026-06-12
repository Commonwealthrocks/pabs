; installer.iss
; last updated: 12/06/2026
; mk

[Setup]
AppName=PABS
AppVersion=0.3a
AppVerName=PABS - PYROFOREVER's actual burning software v0.3a
AppPublisher=Commonwealthrocks
AppCopyright=Copyright (C) 2026 Commonwealthrocks and Mike
AppContact=common@gmailbutnotreally.xyz
DefaultDirName={autopf}\PABS
DisableDirPage=no
OutputBaseFilename=setup_pabs_0.2a_win32
Compression=lzma2/ultra64
DefaultGroupName=PABS
SolidCompression=yes
SetupIconFile=assets\imgs\s_icons\pabs.ico
UninstallDisplayIcon={app}\assets\imgs\s_icons\unins.ico
InfoBeforeFile=..\license.txt
WizardStyle=modern

[Files]
Source: "pabs.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "assets\imgs\*"; DestDir: "{app}\assets\imgs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "assets\sfx\*"; DestDir: "{app}\assets\sfx"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "assets\txt_data\*"; DestDir: "{app}\assets\txt_data"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\PABS"; Filename: "{app}\pabs.exe"; IconFilename: "{app}\assets\imgs\s_icons\pabs.ico"; Tasks: startmenuicon
Name: "{autodesktop}\PABS"; Filename: "{app}\pabs.exe"; IconFilename: "{app}\assets\imgs\s_icons\pabs.ico"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked
Name: "startmenuicon"; Description: "Create a &Start Menu shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    if MsgBox('Do you want to completely remove your PABS settings (appdata)?', mbConfirmation, MB_YESNO) = IDYES then
    begin
      DelTree(ExpandConstant('{userappdata}\PABS'), True, True, True);
    end;
  end;
end;