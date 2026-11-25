; Inno Setup script. Built by CI with:
;   iscc /DAppVersion=1.0.1 /DSourceDir=<staged slideshow-caption folder> /DOutputDir=<out> installer\slideshow-caption.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6E1B6B7A-3C57-4B0B-9C0A-5E7D2A9D4F21}
AppName=Slideshow Captions
AppVersion={#AppVersion}
AppPublisher=Bluefroguk
AppPublisherURL=https://github.com/bluefroguk1/Slideshow-Captions
DefaultDirName={commonappdata}\obs-studio\plugins\slideshow-caption
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=slideshow-captions-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
UninstallDisplayName=Slideshow Captions

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  if MsgBox('Please close OBS Studio before continuing. Continue with the installation?',
            mbConfirmation, MB_YESNO) = IDNO then
    Result := False;
end;
