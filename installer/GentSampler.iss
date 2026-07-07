; GentSampler.iss — Inno Setup 6 script for the GentSampler VST3 installer.
;
; This script is NOT compiled as part of any build. It is invoked by hand
; (or via installer\make_installer.bat) on the authoring machine only.
; CMakeLists.txt and build.bat are untouched by this file and never invoke it.
;
; AppVer MUST be supplied on the ISCC command line via /DAppVer=x.y.z
; (make_installer.bat parses this from CMakeLists.txt's `project(... VERSION x.y.z)`
; line so the version is never hand-copied or allowed to go stale). A bare
; ISCC compile without /DAppVer fails loudly by design — see the #error below.
#ifndef AppVer
  #error "AppVer must be defined on the command line, e.g. ISCC /DAppVer=1.1.0 GentSampler.iss -- do not hardcode a version here."
#endif

[Setup]
AppId={{34A283D6-272B-491E-84BD-AFD079B239F7}
AppName=GentSampler
AppVersion={#AppVer}
AppPublisher=GentSampler
DefaultDirName={commoncf}\VST3\GentSampler.vst3
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableWelcomePage=no
UsePreviousAppDir=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=GentSamplerSetup-{#AppVer}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=GentSampler {#AppVer}
; No LicenseFile directive: no EULA exists yet (deliberate — INSTALLER_SPEC.md
; correction 1). License texts ship as installed files under Contents\Resources
; instead of a wizard page.

[Files]
; The 3 keeper files from the staged Release artefact (payload proven by
; INSTALLER_SPEC.md T1: exactly these 3, no CUDA DLLs, no models, no Standalone).
Source: "..\build\GentSampler_artefacts\Release\VST3\GentSampler.vst3\Contents\x86_64-win\GentSampler.vst3"; DestDir: "{app}\Contents\x86_64-win"; Flags: ignoreversion
Source: "..\build\GentSampler_artefacts\Release\VST3\GentSampler.vst3\Contents\x86_64-win\onnxruntime.dll"; DestDir: "{app}\Contents\x86_64-win"; Flags: ignoreversion
Source: "..\build\GentSampler_artefacts\Release\VST3\GentSampler.vst3\Contents\x86_64-win\onnxruntime_providers_shared.dll"; DestDir: "{app}\Contents\x86_64-win"; Flags: ignoreversion
; License texts + README, installed into the bundle's Resources folder.
Source: "..\THIRD_PARTY_LICENSES\*"; DestDir: "{app}\Contents\Resources\THIRD_PARTY_LICENSES"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}\Contents\Resources"; Flags: ignoreversion

; T4 finding (2026-07-07): the uninstaller (unins000.exe, living in {app})
; self-deletes AFTER Inno's empty-dir sweep, so the bundle's empty skeleton
; survived the first uninstall test. The dirifempty entries below clean that
; up — they act ONLY inside the bundle (the one path this installer owns),
; staying within the spec's nothing-outside-the-bundle rule, and dirifempty
; never deletes a directory that still has content.
[UninstallDelete]
Type: dirifempty; Name: "{app}\Contents\x86_64-win"
Type: dirifempty; Name: "{app}\Contents\Resources"
Type: dirifempty; Name: "{app}\Contents"
Type: dirifempty; Name: "{app}"

; No OTHER [UninstallDelete] entries, and no [Registry]/[Run] sections:
; uninstall removes only the files listed above plus then-empty directories
; under {app}. Nothing outside {commoncf}\VST3\GentSampler.vst3 is ever
; touched, and Documents\GentSampler is never referenced (models resolve
; independently of install location — see INSTALLER_SPEC.md T1).
;
; In-use files (e.g. FL Studio has the plugin open): Inno's default
; abort/retry dialog is used as-is. No restart-manager force-close, no
; /FORCECLOSEAPPLICATIONS — the project's never-terminate-a-user-application
; rule applies to the installer as much as to any agent.
