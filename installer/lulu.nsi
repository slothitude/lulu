; Lulu NSIS Installer
; Build: makensis installer/lulu.nsi

!define PRODUCT_NAME "Lulu"
!define PRODUCT_VERSION "4.1.0"
!define PRODUCT_PUBLISHER "Slothitude"
!define PRODUCT_WEB_SITE "https://github.com/slothitude/lulu"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_REG_KEY "Software\Lulu"

SetCompressor lzma

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "dist\lulu-${PRODUCT_VERSION}-setup.exe"
InstallDir "$PROGRAMFILES\${PRODUCT_NAME}"
RequestExecutionLevel admin

; Modern UI
!include "MUI2.nsh"
!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

; ========================= Sections =========================

Section "!Core Files (required)" SEC_CORE
    SectionIn RO
    SetOutPath $INSTDIR

    ; Binaries
    File "build\agent.exe"
    File "build\libryu_shared.dll"
    File "build\updater.exe"

    ; Config templates
    File "agent.json"
    FileOpen $0 "$INSTDIR\config.json" w
    FileWrite $0 '{"model":"glm-5.1","endpoint":"https://api.z.ai/api/coding/paas/v4/chat/completions","apikey":"","max_tokens":4096,"temperature":0.7}$\n'
    FileClose $0

    ; Create directories
    CreateDirectory "$INSTDIR\state"
    CreateDirectory "$INSTDIR\workspace"
    CreateDirectory "$INSTDIR\tg_data"
    CreateDirectory "$INSTDIR\tools"

    ; Write install.json
    FileOpen $0 "$INSTDIR\install.json" w
    FileWrite $0 '{"version":"${PRODUCT_VERSION}"}$\n'
    FileClose $0

    ; Registry
    WriteRegStr HKLM "${PRODUCT_REG_KEY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninstall.exe"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "URLInfoAbout" "${PRODUCT_WEB_SITE}"

    ; Uninstaller
    WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Tool Plugins" SEC_TOOLS
    SetOutPath "$INSTDIR\tools"
    File /nonfatal "tools\*.dll"
SectionEnd

Section "SDL3 Runtime (optional)" SEC_SDL3
    SetOutPath $INSTDIR
    File /nonfatal "SDL3.dll"
    File /nonfatal "SDL3_image.dll"
SectionEnd

Section "Add to PATH" SEC_PATH
    EnVar::AddValue "PATH" "$INSTDIR"
SectionEnd

Section "Start Menu Shortcuts" SEC_SHORTCUTS
    CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
    CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\Lulu Agent.lnk" "$INSTDIR\agent.exe"
    CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\Check for Updates.lnk" "$INSTDIR\updater.exe" "--check"
    CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

; ========================= Descriptions =========================

LangString DESC_CORE ${LANG_ENGLISH} "Core agent files (agent.exe, updater.exe, config template)"
LangString DESC_TOOLS ${LANG_ENGLISH} "Tool plugin DLLs (sdl3_render.dll, etc.)"
LangString DESC_SDL3 ${LANG_ENGLISH} "SDL3 runtime libraries (for UI toolkit)"
LangString DESC_PATH ${LANG_ENGLISH} "Add install directory to system PATH"
LangString DESC_SHORTCUTS ${LANG_ENGLISH} "Create Start Menu shortcuts"

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CORE} $(DESC_CORE)
    !insertmacro MUI_DESCRIPTION_TEXT ${SEC_TOOLS} $(DESC_TOOLS)
    !insertmacro MUI_DESCRIPTION_TEXT ${SEC_SDL3} $(DESC_SDL3)
    !insertmacro MUI_DESCRIPTION_TEXT ${SEC_PATH} $(DESC_PATH)
    !insertmacro MUI_DESCRIPTION_TEXT ${SEC_SHORTCUTS} $(DESC_SHORTCUTS)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ========================= Uninstaller =========================

Section "un.Core Files" UNSEC_CORE
    SectionIn RO
    Delete "$INSTDIR\agent.exe"
    Delete "$INSTDIR\libryu_shared.dll"
    Delete "$INSTDIR\updater.exe"
    Delete "$INSTDIR\agent.json"
    Delete "$INSTDIR\install.json"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd

Section /o "un.User Data" UNSEC_DATA
    MessageBox MB_YESNO "Delete user data (config.json, state, workspace, tg_data)?" IDNO skip
    Delete "$INSTDIR\config.json"
    RMDir /r "$INSTDIR\state"
    RMDir /r "$INSTDIR\workspace"
    RMDir /r "$INSTDIR\tg_data"
    skip:
SectionEnd

Section /o "un.Tool Plugins" UNSEC_TOOLS
    RMDir /r "$INSTDIR\tools"
SectionEnd

Section /o "un.Start Menu Shortcuts" UNSEC_SHORTCUTS
    RMDir /r "$SMPROGRAMS\${PRODUCT_NAME}"
SectionEnd

Section /o "un.Registry + PATH" UNSEC_REG
    DeleteRegKey HKLM "${PRODUCT_REG_KEY}"
    DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"
    EnVar::DeleteValue "PATH" "$INSTDIR"
SectionEnd

LangString DESC_UNCORE ${LANG_ENGLISH} "Remove core binaries"
LangString DESC_UNDATA ${LANG_ENGLISH} "Remove user data (config, state, workspace)"
LangString DESC_UNTOOLS ${LANG_ENGLISH} "Remove tool plugins"
LangString DESC_UNSHORTCUTS ${LANG_ENGLISH} "Remove Start Menu shortcuts"
LangString DESC_UNREG ${LANG_ENGLISH} "Remove registry keys and PATH entry"

!insertmacro MUI_UNFUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${UNSEC_CORE} $(DESC_UNCORE)
    !insertmacro MUI_DESCRIPTION_TEXT ${UNSEC_DATA} $(DESC_UNDATA)
    !insertmacro MUI_DESCRIPTION_TEXT ${UNSEC_TOOLS} $(DESC_UNTOOLS)
    !insertmacro MUI_DESCRIPTION_TEXT ${UNSEC_SHORTCUTS} $(DESC_UNSHORTCUTS)
    !insertmacro MUI_DESCRIPTION_TEXT ${UNSEC_REG} $(DESC_UNREG)
!insertmacro MUI_UNFUNCTION_DESCRIPTION_END
