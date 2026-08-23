; ORMBandPass Windows 安装器 (Inno Setup 6)
; 用法: 用 Inno Setup Compiler 打开本文件 -> Compile, 或命令行:
;   ISCC.exe installer\ORMBandPass.iss
; 注意: 版本号需与 plugins/BandPass/config.h (PLUG_VERSION_STR) 同步。

#define MyAppName "ORMBandPass"
#define MyAppVersion "0.4.0"
#define MyAppExe "ORMBandPass-app.exe"

[Setup]
AppId={{8F3B9A1C-6D4E-4B2F-9A5C-3E7D1F0A2B64}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=OpenRM
AppPublisherURL=https://github.com/OpenRM
DefaultDirName={autopf}\{#MyAppName}
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\build\installer
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-win-x64-Setup
Compression=lzma2
SolidCompression=yes
UninstallDisplayIcon={app}\{#MyAppExe}
SetupIconFile=..\plugins\BandPass\resources\ORMBandPass.ico
; VST3 写入 Program Files\Common Files\VST3 需要管理员权限
PrivilegesRequired=admin
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"

[Files]
; 独立 App
Source: "..\build\out\{#MyAppExe}"; DestDir: "{app}"; Flags: ignoreversion
; VST3 (系统公共目录, 全用户可用)
Source: "..\build\out\{#MyAppName}.vst3\*"; DestDir: "{commoncf64}\VST3\{#MyAppName}.vst3"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "立即运行"; Flags: nowait postinstall skipifsilent
