; Screen+ Setup: per-user installer (NSIS 3, Modern UI 2). No administrator rights needed.
;
; Built by CMake when makensis is available, or by hand:
;   makensis /DVERSION=1.2.3 /DSTAGE=<folder with ScreenPlus.exe> /DOUTFILE=<setup.exe> installer\screenplus.nsi
; STAGE is the build output: ScreenPlus.exe, ScreenPlus.com (terminal), the FFmpeg DLLs, licenses\ and samples\.
;
; Silent install: Setup.exe /S [/DEFAULT | /CLEAN] [/D=<install folder>]   (/D must be last)
; An existing installation is found automatically and updated in place, keeping the library.
; Installation types (applied by Screen+ on its next launch via HKCU\Software\ScreenPlus\SetupMode):
;   default - Aurora sample + automatic import of Lively Wallpaper's library
;   clean   - empty library, no sample, no automatic Lively import
;   keep    - update of an existing installation: nothing is changed (SetupMode is not written)

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "WordFunc.nsh"

!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!ifndef STAGE
  !define STAGE "..\build"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\build\ScreenPlus-Setup-${VERSION}.exe"
!endif

!define APP_NAME     "Screen+"
!define APP_EXE      "ScreenPlus.exe"
!define APP_KEY      "Software\ScreenPlus"
!define UNINST_KEY   "Software\Microsoft\Windows\CurrentVersion\Uninstall\ScreenPlus"
!define RUN_KEY      "Software\Microsoft\Windows\CurrentVersion\Run"
!define APPROVED_KEY "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"

Name "${APP_NAME}"
Caption "${APP_NAME} ${VERSION} Setup"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\ScreenPlus"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel user
BrandingText "${APP_NAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 Screen+ contributors"

; ------------------------------------------------------------------ look
!define MUI_ICON "..\resources\app.ico"
!define MUI_UNICON "..\resources\app.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "wizard.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "wizard.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "header.bmp"
!define MUI_ABORTWARNING

; ------------------------------------------------------------------ state
Var InstMode        ; "keep" (update in place) | "default" | "clean"
Var OptStartup      ; 1 = start with Windows
Var OptDesktop      ; 1 = desktop shortcut
Var Existing        ; folder of an installed Screen+, "" when none was found
Var ExistingVer
Var WelcomeText
Var hKeep
Var hDefault
Var hClean
Var hStartup
Var hDesktop

; ------------------------------------------------------------------ pages
!define MUI_WELCOMEPAGE_TEXT "$WelcomeText"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\LICENSE"
Page custom InstallTypePage InstallTypeLeave
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipWhenUpdating
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Waits (up to 10 s) until no Screen+ instance holds its single-instance mutex.
!macro WaitForExit
  StrCpy $1 0
  ${Do}
    System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "Local\ScreenPlus.Instance.v1") p.r2'
    ${If} $2 = 0
      ${Break}
    ${EndIf}
    System::Call 'kernel32::CloseHandle(p r2)'
    Sleep 250
    IntOp $1 $1 + 1
  ${LoopUntil} $1 >= 40
!macroend

; Asks a running Screen+ (and its renderer processes) to exit, then waits until its files can be replaced.
!macro StopScreenPlus
  ${If} ${FileExists} "$INSTDIR\${APP_EXE}"
    DetailPrint "Closing ${APP_NAME}..."
    nsExec::Exec '"$INSTDIR\${APP_EXE}" --exit'
    Pop $0
    !insertmacro WaitForExit
    StrCpy $1 0
    ${Do}
      ClearErrors
      FileOpen $2 "$INSTDIR\${APP_EXE}" a
      ${IfNot} ${Errors}
        FileClose $2
        ${Break}
      ${EndIf}
      Sleep 250
      IntOp $1 $1 + 1
    ${LoopUntil} $1 >= 40
  ${EndIf}
!macroend

; Looks for an installed copy: the uninstall entry (also written by the 1.0.0 setup), the
; "Start with Windows" entry, then the default folder. Copies without an uninstaller
; (portable or development builds) are never treated as installations.
Function FindExisting
  StrCpy $Existing ""
  ReadRegStr $0 HKCU "${UNINST_KEY}" "InstallLocation"
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\${APP_EXE}"
    StrCpy $Existing $0
  ${EndIf}
  ${If} $Existing == ""
    ReadRegStr $0 HKCU "${RUN_KEY}" "ScreenPlus"          ; "<folder>\ScreenPlus.exe" --tray
    ${WordReplace} $0 '"' "" "+" $0
    ${WordReplace} $0 " --tray" "" "+" $0
    ${GetParent} $0 $0
    ${If} $0 != ""
    ${AndIf} ${FileExists} "$0\uninstall.exe"
      StrCpy $Existing $0
    ${EndIf}
  ${EndIf}
  ${If} $Existing == ""
  ${AndIf} ${FileExists} "$LOCALAPPDATA\Programs\ScreenPlus\uninstall.exe"
    StrCpy $Existing "$LOCALAPPDATA\Programs\ScreenPlus"
  ${EndIf}
  ${If} $Existing != ""
    ReadRegStr $ExistingVer HKCU "${UNINST_KEY}" "DisplayVersion"
    ${If} $ExistingVer == ""
      ${GetFileVersion} "$Existing\${APP_EXE}" $ExistingVer
    ${EndIf}
  ${EndIf}
FunctionEnd

Function .onInit
  ${IfNot} ${RunningX64}
  ${OrIfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} requires 64-bit Windows 10 or Windows 11." /SD IDOK
    Abort
  ${EndIf}
  StrCpy $InstMode "default"
  StrCpy $OptStartup 1
  StrCpy $OptDesktop 1
  StrCpy $WelcomeText "Setup will guide you through the installation of ${APP_NAME} ${VERSION}, a lightweight live wallpaper engine for Windows.$\r$\n$\r$\n${APP_NAME} is installed for your account only, so administrator rights are not needed.$\r$\n$\r$\nClick Next to continue."

  Call FindExisting
  ${If} $Existing != ""
    ; Update in place: same folder, keep the library and the current startup / shortcut choices.
    StrCpy $INSTDIR $Existing
    StrCpy $InstMode "keep"
    ReadRegStr $0 HKCU "${RUN_KEY}" "ScreenPlus"
    ${If} $0 == ""
      StrCpy $OptStartup 0
    ${EndIf}
    ${IfNot} ${FileExists} "$DESKTOP\${APP_NAME}.lnk"
      StrCpy $OptDesktop 0
    ${EndIf}
    ${VersionCompare} "${VERSION}" "$ExistingVer" $0
    ${If} $0 == 2
      ${If} ${Cmd} `MessageBox MB_YESNO|MB_ICONEXCLAMATION "A newer version of ${APP_NAME} ($ExistingVer) is installed in:$\r$\n$Existing$\r$\n$\r$\nReplace it with the older version ${VERSION}?" /SD IDYES IDNO`
        Abort
      ${EndIf}
      StrCpy $WelcomeText "Setup will replace ${APP_NAME} $ExistingVer in$\r$\n$Existing$\r$\nwith version ${VERSION}.$\r$\n$\r$\nYour wallpapers, library and settings are kept.$\r$\n$\r$\nClick Next to continue."
    ${ElseIf} $0 == 0
      StrCpy $WelcomeText "${APP_NAME} ${VERSION} is already installed in$\r$\n$Existing$\r$\n$\r$\nSetup will reinstall it. Your wallpapers, library and settings are kept.$\r$\n$\r$\nClick Next to continue."
    ${Else}
      StrCpy $WelcomeText "Setup found ${APP_NAME} $ExistingVer in$\r$\n$Existing$\r$\n$\r$\nIt will be updated to version ${VERSION}. Your wallpapers, library and settings are kept; ${APP_NAME} is closed during the update.$\r$\n$\r$\nClick Next to continue."
    ${EndIf}
  ${EndIf}

  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/CLEAN" $R1
  ${IfNot} ${Errors}
    StrCpy $InstMode "clean"
  ${EndIf}
  ClearErrors
  ${GetOptions} $R0 "/DEFAULT" $R1
  ${IfNot} ${Errors}
    StrCpy $InstMode "default"
  ${EndIf}
FunctionEnd

Function SkipWhenUpdating
  ${If} $Existing != ""
    Abort                       ; updates stay in the existing folder
  ${EndIf}
FunctionEnd

Function InstallTypePage
  !insertmacro MUI_HEADER_TEXT "Installation Type" "Choose how ${APP_NAME} sets up your wallpaper library."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  StrCpy $hKeep ""
  StrCpy $1 0                   ; top of the next option, in dialog units
  ${If} $Existing != ""
    ${NSD_CreateRadioButton} 0 0 100% 12u "&Update and keep my library (recommended)"
    Pop $hKeep
    ${NSD_AddStyle} $hKeep ${WS_GROUP}
    ${NSD_CreateLabel} 13u 12u -13u 16u "Updates ${APP_NAME} in place. Your wallpapers, library and settings stay exactly as they are."
    Pop $0
    StrCpy $1 30
  ${EndIf}

  ${NSD_CreateRadioButton} 0 $1u 100% 12u "&Default installation"
  Pop $hDefault
  ${If} $hKeep == ""
    ${NSD_AddStyle} $hDefault ${WS_GROUP}
  ${EndIf}
  IntOp $2 $1 + 12
  ${NSD_CreateLabel} 13u $2u -13u 16u "Includes the Aurora sample wallpaper and automatically adds the wallpapers from Lively Wallpaper when it is installed."
  Pop $0

  IntOp $2 $1 + 30
  ${NSD_CreateRadioButton} 0 $2u 100% 12u "&Clean installation"
  Pop $hClean
  IntOp $2 $1 + 42
  ${NSD_CreateLabel} 13u $2u -13u 16u "Starts with an empty library: no sample wallpaper and no automatic Lively import. You can import wallpapers yourself at any time."
  Pop $0

  IntOp $2 $1 + 62
  ${NSD_CreateGroupBox} 0 $2u 100% 46u "Additional tasks"
  Pop $0
  IntOp $2 $1 + 74
  ${NSD_CreateCheckbox} 10u $2u -20u 12u "Start ${APP_NAME} when I sign in to Windows"
  Pop $hStartup
  ${NSD_AddStyle} $hStartup ${WS_GROUP}
  IntOp $2 $1 + 90
  ${NSD_CreateCheckbox} 10u $2u -20u 12u "Create a desktop &shortcut"
  Pop $hDesktop

  ${If} $InstMode == "keep"
    ${NSD_Check} $hKeep
  ${ElseIf} $InstMode == "clean"
    ${NSD_Check} $hClean
  ${Else}
    ${NSD_Check} $hDefault
  ${EndIf}
  ${If} $OptStartup == 1
    ${NSD_Check} $hStartup
  ${EndIf}
  ${If} $OptDesktop == 1
    ${NSD_Check} $hDesktop
  ${EndIf}
  nsDialogs::Show
FunctionEnd

Function InstallTypeLeave
  StrCpy $InstMode "default"
  ${NSD_GetState} $hClean $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $InstMode "clean"
  ${EndIf}
  ${If} $hKeep != ""
    ${NSD_GetState} $hKeep $0
    ${If} $0 == ${BST_CHECKED}
      StrCpy $InstMode "keep"
    ${EndIf}
  ${EndIf}
  ${NSD_GetState} $hStartup $OptStartup
  ${NSD_GetState} $hDesktop $OptDesktop
FunctionEnd

; ------------------------------------------------------------------ install
Section "Install"
  SetShellVarContext current
  !insertmacro StopScreenPlus

  SetOutPath "$INSTDIR"
  File "${STAGE}\${APP_EXE}"
  File "${STAGE}\ScreenPlus.com"                       ; terminal companion: `screenplus <command>`
  File "${STAGE}\avcodec-*.dll"
  File "${STAGE}\avformat-*.dll"
  File "${STAGE}\avutil-*.dll"
  File "${STAGE}\swresample-*.dll"
  File "${STAGE}\swscale-*.dll"
  SetOutPath "$INSTDIR\licenses"
  File "${STAGE}\licenses\*.*"

  ${If} $InstMode == "clean"
    Delete "$INSTDIR\samples\Aurora\index.html"
    Delete "$INSTDIR\samples\Aurora\preview.jpg"
    RMDir "$INSTDIR\samples\Aurora"
    RMDir "$INSTDIR\samples"
  ${ElseIf} $InstMode == "keep"
  ${AndIfNot} ${FileExists} "$INSTDIR\samples\Aurora\index.html"
    ; updating an earlier clean installation: still no sample
  ${Else}
    SetOutPath "$INSTDIR\samples\Aurora"
    File "${STAGE}\samples\Aurora\*.*"
  ${EndIf}

  SetOutPath "$INSTDIR"
  nsExec::Exec '"$INSTDIR\ScreenPlus.com" path add --quiet'   ; `screenplus` in new terminals
  Pop $0
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  ${If} $OptDesktop == 1
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  ${Else}
    Delete "$DESKTOP\${APP_NAME}.lnk"
  ${EndIf}

  ; Same Run value the app's own "Start with Windows" setting manages.
  DeleteRegValue HKCU "${RUN_KEY}" "Screen+"          ; written by the 1.0.0 setup
  ${If} $OptStartup == 1
    WriteRegStr HKCU "${RUN_KEY}" "ScreenPlus" '"$INSTDIR\${APP_EXE}" --tray'
    DeleteRegValue HKCU "${APPROVED_KEY}" "ScreenPlus"   ; re-enable if turned off in Task Manager
  ${Else}
    DeleteRegValue HKCU "${RUN_KEY}" "ScreenPlus"
  ${EndIf}

  ${If} $InstMode == "keep"
    DeleteRegValue HKCU "${APP_KEY}" "SetupMode"        ; nothing for the app to change
  ${Else}
    WriteRegStr HKCU "${APP_KEY}" "SetupMode" $InstMode
  ${EndIf}

  ; A copy still running from another folder (e.g. a portable build) would stop the new version
  ; from starting; the new executable forwards --exit to whichever instance is running.
  nsExec::Exec '"$INSTDIR\${APP_EXE}" --exit'
  Pop $0
  !insertmacro WaitForExit

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "Screen+ contributors"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

; ------------------------------------------------------------------ uninstall
Section "Uninstall"
  SetShellVarContext current
  !insertmacro StopScreenPlus

  nsExec::Exec '"$INSTDIR\ScreenPlus.com" path remove --quiet'
  Pop $0
  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\ScreenPlus.com"
  Delete "$INSTDIR\avcodec-*.dll"
  Delete "$INSTDIR\avformat-*.dll"
  Delete "$INSTDIR\avutil-*.dll"
  Delete "$INSTDIR\swresample-*.dll"
  Delete "$INSTDIR\swscale-*.dll"
  Delete "$INSTDIR\licenses\THIRD_PARTY_NOTICES.md"
  Delete "$INSTDIR\licenses\LICENSE"
  Delete "$INSTDIR\licenses\COPYING.LGPLv2.1"
  Delete "$INSTDIR\licenses\BUILDINFO.txt"
  RMDir "$INSTDIR\licenses"
  Delete "$INSTDIR\samples\Aurora\index.html"
  Delete "$INSTDIR\samples\Aurora\preview.jpg"
  RMDir "$INSTDIR\samples\Aurora"
  RMDir "$INSTDIR\samples"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  DeleteRegValue HKCU "${RUN_KEY}" "ScreenPlus"
  DeleteRegValue HKCU "${RUN_KEY}" "Screen+"
  DeleteRegValue HKCU "${APPROVED_KEY}" "ScreenPlus"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"

  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also remove your ${APP_NAME} settings, library list and logs?$\r$\n$\r$\nYour wallpaper files are never deleted." /SD IDNO IDNO keep
    RMDir /r "$LOCALAPPDATA\ScreenPlus"
  keep:
SectionEnd
