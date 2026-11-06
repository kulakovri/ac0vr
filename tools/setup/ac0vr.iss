; ac0vr.iss -- the player's installer, Inno Setup 6 (setup_1105 package C, 2026-11-05). Built by tools/setup/make_installer.sh, from make_release.sh:
;   ISCC.exe /DVERSION=0.1.0 /DSTAGE=<out/release/0.1.0/stage> /O<out/release/0.1.0> ac0vr.iss
; Per user, no administrator, no UAC. The disc is referenced, never copied; the installer holds nothing from the game (the same scan as
; make_release.sh runs on its file list). The checks are ac0vr.exe's own: --check-disc on the disc page, --set-disc after the copy, the first-run build at
; "Start ac0vr". /DCOCKPIT_URL=... shows the optional component with the public cockpit set, once the owner has uploaded it as a release asset.

#ifndef VERSION
  #define VERSION "0.0.0"
#endif
#ifndef STAGE
  #define STAGE "stage"
#endif

[Setup]
AppId=ac0vr-8f4e2b70
AppName=ac0vr
AppVersion={#VERSION}
AppVerName=ac0vr {#VERSION}
AppPublisher=ac0vr
DefaultDirName={localappdata}\Programs\ac0vr
DefaultGroupName=ac0vr
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=ac0vr-{#VERSION}-setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
UninstallDisplayName=ac0vr {#VERSION}
UninstallDisplayIcon={app}\ac0vr.exe
SetupLogging=yes
UsePreviousAppDir=yes

[Types]
Name: "default"; Description: "The game"; Flags: iscustom

[Components]
Name: "game"; Description: "The game: the launcher, the compiler and the runtime that build it from your disc"; Types: default; Flags: fixed
#ifdef COCKPIT_URL
Name: "cockpits"; Description: "The public cockpit set (under 1 GB, downloaded now)"; Types: default
#endif

[Tasks]
Name: "desktopicon"; Description: "A shortcut on the desktop"; GroupDescription: "Shortcuts:"

[Files]
; the check of the disc page runs before anything is installed: this copy goes to {tmp} only
Source: "{#STAGE}\ac0vr.exe"; Flags: dontcopy
Source: "{#STAGE}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: game
; the asset folder beside the setup, when there is one (cockpits, movies, ground and menu textures, ffmpeg): laid out like build\zero, copied in, left where it is.
; Newer files only, so an install over an install is quick. The uninstaller's second question decides about them, as about the generated code.
Source: "{src}\ac0vr-assets\*"; DestDir: "{app}\build\zero"; Flags: external recursesubdirs createallsubdirs skipifsourcedoesntexist comparetimestamp uninsneveruninstall; Components: game

[Icons]
Name: "{group}\ac0vr"; Filename: "{app}\ac0vr.exe"
Name: "{group}\ac0vr - another disc, a rebuild, updates"; Filename: "{app}\ac0vr.exe"; Parameters: "--dialog"
Name: "{group}\Uninstall ac0vr"; Filename: "{uninstallexe}"
Name: "{userdesktop}\ac0vr"; Filename: "{app}\ac0vr.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\ac0vr.exe"; Description: "Start ac0vr"; Flags: nowait postinstall skipifsilent

[Code]
var
  DiscPage: TInputFileWizardPage;
  DiscFolderButton: TNewButton;
  DiscMessage: TNewStaticText;

{ ac0vr.exe holds the check: serial and executable hash, the US disc only. It runs from the temporary folder before anything is installed. }
function CheckDisc(const Path: String; var Words: String): Boolean;
var
  Rc: Integer;
  OutFile: String;
  Lines: AnsiString;
begin
  Result := False;
  Words := '';
  ExtractTemporaryFile('ac0vr.exe');
  OutFile := ExpandConstant('{tmp}\disc_check.txt');
  DeleteFile(OutFile);
  { (RemoveBackslash: a quoted D:\ would hide its closing quote; ac0vr.exe takes D: as the drive's root) }
  if not Exec(ExpandConstant('{tmp}\ac0vr.exe'), '--check-disc "' + RemoveBackslash(Path) + '" --out "' + OutFile + '"', '', SW_HIDE, ewWaitUntilTerminated, Rc) then
  begin
    Words := 'The check could not run.';
    Exit;
  end;
  if LoadStringFromFile(OutFile, Lines) then
    Words := UTF8Decode(Lines);
  Result := Rc = 0;
end;

{ The default: an .iso next to the setup exe whose check passes ("the image lies beside the installer"). }
function FindDiscBesideSetup: String;
var
  F: TFindRec;
  Dir, Words: String;
begin
  Result := '';
  Dir := ExpandConstant('{src}');
  if FindFirst(Dir + '\*.iso', F) then
  try
    repeat
      if CheckDisc(Dir + '\' + F.Name, Words) then
      begin
        Result := Dir + '\' + F.Name;
        Exit;
      end;
    until not FindNext(F);
  finally
    FindClose(F);
  end;
end;

procedure DiscFolderClick(Sender: TObject);
var
  Dir: String;
begin
  Dir := DiscPage.Values[0];
  if BrowseForFolder('The folder with your disc''s files, or the disc''s drive', Dir, False) then
    DiscPage.Values[0] := Dir;
end;

procedure InitializeWizard;
begin
  DiscPage := CreateInputFilePage(wpSelectDir, 'Your disc',
    'This program holds no game. It builds the game from your own US disc of Ace Combat Zero: The Belkan War (SLUS-21346).',
    'An ISO image of the disc, or a folder with its files. It is read, never copied: keep it where it is.');
  DiscPage.Add('The disc image:', 'Disc image (*.iso)|*.iso|All files|*.*', '.iso');
  DiscFolderButton := TNewButton.Create(DiscPage);
  DiscFolderButton.Parent := DiscPage.Surface;
  DiscFolderButton.Caption := 'A folder...';
  DiscFolderButton.Left := DiscPage.Edits[0].Left;
  DiscFolderButton.Top := DiscPage.Edits[0].Top + DiscPage.Edits[0].Height + ScaleY(8);
  DiscFolderButton.Width := ScaleX(90);
  DiscFolderButton.Height := ScaleY(23);
  DiscFolderButton.OnClick := @DiscFolderClick;
  DiscMessage := TNewStaticText.Create(DiscPage);
  DiscMessage.Parent := DiscPage.Surface;
  DiscMessage.Left := DiscPage.Edits[0].Left;
  DiscMessage.Top := DiscFolderButton.Top + DiscFolderButton.Height + ScaleY(10);
  DiscMessage.Width := DiscPage.SurfaceWidth - DiscMessage.Left;
  DiscMessage.Height := ScaleY(90);
  DiscMessage.AutoSize := False;
  DiscMessage.WordWrap := True;
  DiscMessage.Caption := '';
end;

#ifndef COCKPIT_URL
{ one fixed row is no choice }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := PageID = wpSelectComponents;
end;

#endif
procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = DiscPage.ID) and (DiscPage.Values[0] = '') then
    DiscPage.Values[0] := FindDiscBesideSetup;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Words: String;
  P: Integer;
begin
  Result := True;
  if CurPageID = DiscPage.ID then
  begin
    if DiscPage.Values[0] = '' then
    begin
      DiscMessage.Caption := 'The game cannot be built without your disc.';
      Result := False;
      Exit;
    end;
    Result := CheckDisc(DiscPage.Values[0], Words);
    { a bad disc keeps the page, with the check's own words; the last line is for scripts }
    P := Pos(#10 + 'result:', Words);
    if P > 0 then
      Words := Copy(Words, 1, P - 1);
    DiscMessage.Caption := Words;
  end;
end;

function AssetsBeside: Boolean;
begin
  Result := DirExists(ExpandConstant('{src}\ac0vr-assets'));
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Assets: String;
begin
  if AssetsBeside then
    Assets := 'Assets, copied into the game:' + NewLine + Space + ExpandConstant('{src}\ac0vr-assets')
  else
    Assets := 'No ac0vr-assets folder beside the setup: the game runs without 3D cockpits, upscaled movies and ground textures.';
  Result := MemoDirInfo + NewLine + NewLine + 'Your disc:' + NewLine + Space + DiscPage.Values[0] + NewLine + NewLine + Assets + NewLine + NewLine + MemoTasksInfo;
end;

#ifdef COCKPIT_URL
function CockpitProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure InstallCockpits;
var
  Rc: Integer;
begin
  if WizardIsComponentSelected('cockpits') then
  begin
    try
      DownloadTemporaryFile('{#COCKPIT_URL}', 'cockpits.zip', '{#COCKPIT_SHA256}', @CockpitProgress);
      ForceDirectories(ExpandConstant('{app}\build\zero\cockpits'));
      { tar.exe has come with Windows since 10 1803 and reads zip }
      Exec(ExpandConstant('{sys}\tar.exe'), '-xf "' + ExpandConstant('{tmp}\cockpits.zip') + '" -C "' + ExpandConstant('{app}\build\zero\cockpits') + '"', '', SW_HIDE, ewWaitUntilTerminated, Rc);
    except
      SuppressibleMsgBox('The cockpit set could not be downloaded. The game runs without it, from its own view: ' + GetExceptionMessage, mbInformation, MB_OK, IDOK);
    end;
  end;
end;
#endif

procedure CurStepChanged(CurStep: TSetupStep);
var
  Rc: Integer;
  Ini: String;
begin
  if CurStep = ssPostInstall then
  begin
    { the disc goes into the settings file: [paths] disc =. The disc is referenced, never copied. }
    Exec(ExpandConstant('{app}\ac0vr.exe'), '--set-disc "' + RemoveBackslash(DiscPage.Values[0]) + '"', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Rc);
    { the ground textures are a row that starts at GAME: the sharpest pack when it came with this install; a player's own choice stays }
    Ini := ExpandConstant('{app}\build\zero\ac5_settings.ini');
    if DirExists(ExpandConstant('{app}\build\zero\out\terrain_t5\x4')) and (GetIniString('enhancements', 'ground_textures', '', Ini) = '') then
      SetIniString('enhancements', 'ground_textures', 'x4', Ini);
#ifdef COCKPIT_URL
    InstallCockpits;
#endif
  end;
end;

{ The uninstaller removes what Setup copied, and asks about what the player's PC made: the memory cards and settings, and the generated code. }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
  F: TFindRec;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    App := ExpandConstant('{app}');
    { a silent uninstall asks nothing and keeps what the player's PC made }
    if (not UninstallSilent) and (DirExists(App + '\build\zero\saves') or FileExists(App + '\build\zero\ac5_settings.ini')) then
    begin
      if MsgBox('Delete your saved games and settings too?' + #13#10 + App + '\build\zero\saves', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      begin
        DelTree(App + '\build\zero\saves', True, True, True);
        DeleteFile(App + '\build\zero\ac5_settings.ini');
      end;
    end;
    if (not UninstallSilent) and DirExists(App + '\build') then
    begin
      if MsgBox('Delete the generated code and the assets too (the game built from your disc, the cockpits, movies and textures)? A new install builds it again in a few minutes.', mbConfirmation, MB_YESNO) = IDYES then
      begin
        { everything under build\ but what the first question kept }
        if FindFirst(App + '\build\zero\*', F) then
        try
          repeat
            if (F.Name <> '.') and (F.Name <> '..') and (F.Name <> 'saves') and (F.Name <> 'ac5_settings.ini') then
            begin
              if (F.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
                DelTree(App + '\build\zero\' + F.Name, True, True, True)
              else
                DeleteFile(App + '\build\zero\' + F.Name);
            end;
          until not FindNext(F);
        finally
          FindClose(F);
        end;
        DelTree(App + '\build\obj', True, True, True);
        DelTree(App + '\build\elf', True, True, True);
        DeleteFile(App + '\build\build.ninja');
        DeleteFile(App + '\build\player_build.log');
        DeleteFile(App + '\.ninja_log');
        DeleteFile(App + '\.ninja_deps');
        DeleteFile(App + '\build_state.json');
        RemoveDir(App + '\build\zero');
        RemoveDir(App + '\build');
      end;
    end;
    DeleteFile(App + '\ac0vr.exe.old');
    { files an update brought that Setup never copied }
    DelTree(App + '\bundle', True, True, True);
    DelTree(App + '\tools', True, True, True);
    DelTree(App + '\docs', True, True, True);
    DelTree(App + '\licenses', True, True, True);
    DelTree(App + '\python', True, True, True);
    RemoveDir(App);
  end;
end;
