; Installer for Play League or Play RX (NSIS 3, from MSYS2: mingw-w64-ucrt-x86_64-nsis).
; Built by scripts/package.sh, which stages the files and passes:
;   PRODUCT  "Play League" | "Play RX"     EXE      PlayLeague.exe | PlayRX.exe
;   KEY      registry / uninstall key      VERSION  x.y.z.build (VERSION file)
;   STAGE    the staged files (exe, DLLs, web\, licenses\, NOTICE.txt, driver\)
;   OUTFILE  the setup .exe to write
; Installs for all users into Program Files (64-bit), adds Start menu and desktop shortcuts and an uninstaller.
; The apps keep their data in %APPDATA%\<product>; uninstalling leaves it.

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!ifndef PRODUCT | EXE | KEY | VERSION | STAGE | OUTFILE
  !error "run through scripts/package.sh"
!endif
!define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${KEY}"
!define WEBVIEW2_ID "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

Name "${PRODUCT}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${PRODUCT}"
InstallDirRegKey HKLM "Software\${PRODUCT}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${PRODUCT} ${VERSION}"
VIProductVersion "${VERSION}"  ; VERSION is x.y.z.build
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${PRODUCT} setup"
VIAddVersionKey "LegalCopyright" "GPL-2.0"

!define MUI_ABORTWARNING
!ifdef DRIVER
  !define DRIVER_TEXT "They need the WinUSB driver, which setup can install for the adapters plugged in now; you can run it again later from the Start menu (${PRODUCT} - Set up adapters)."
!else
  !define DRIVER_TEXT "Each needs the WinUSB driver, set once per adapter with Zadig (zadig.akeo.ie): Options > List All Devices, pick the adapter, WinUSB, Replace Driver."
!endif
!define MUI_WELCOMEPAGE_TEXT "This installs ${PRODUCT} ${VERSION}.$\r$\n$\r$\nUSB adapters: RTL8812BU Wi-Fi adapters (for example TP-Link Archer T4U v3, Cudy AC1300). ${DRIVER_TEXT}$\r$\n$\r$\nClose ${PRODUCT} if it is running, then click Next."
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${PRODUCT}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\NOTICE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "${PRODUCT}" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${STAGE}\${EXE}"
  File "${STAGE}\*.dll"
  File "${STAGE}\NOTICE.txt"
  SetOutPath "$INSTDIR\web"
  File "${STAGE}\web\*.*"
  SetOutPath "$INSTDIR\licenses"
  File "${STAGE}\licenses\*.*"
  !ifdef DRIVER
    SetOutPath "$INSTDIR\driver"
    File "${STAGE}\driver\*.*"
  !endif
  SetOutPath "$INSTDIR"

  WriteUninstaller "$INSTDIR\Uninstall ${PRODUCT}.exe"
  CreateShortcut "$SMPROGRAMS\${PRODUCT}.lnk" "$INSTDIR\${EXE}"
  CreateShortcut "$DESKTOP\${PRODUCT}.lnk" "$INSTDIR\${EXE}"
  !ifdef DRIVER
    CreateShortcut "$SMPROGRAMS\${PRODUCT} - Set up adapters.lnk" "$INSTDIR\driver\setup-adapters.cmd" "" "$INSTDIR\${EXE}"
  !endif

  WriteRegStr HKLM "Software\${PRODUCT}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNKEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNKEY}" "Publisher" "${PRODUCT}"
  WriteRegStr HKLM "${UNKEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKLM "${UNKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNKEY}" "UninstallString" '"$INSTDIR\Uninstall ${PRODUCT}.exe"'
  WriteRegDWORD HKLM "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNKEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNKEY}" "EstimatedSize" 12000

  ; the window is Microsoft Edge WebView2, part of Windows 11 (and most Windows 10 PCs)
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_ID}" "pv"
  ${If} $0 == ""
    ReadRegStr $0 HKCU "Software\Microsoft\EdgeUpdate\Clients\${WEBVIEW2_ID}" "pv"
  ${EndIf}
  ${If} $0 == ""
  ${OrIf} $0 == "0.0.0.0"
    MessageBox MB_YESNO|MB_ICONINFORMATION "${PRODUCT} needs the Microsoft Edge WebView2 Runtime, which this PC does not seem to have.$\r$\n$\r$\nOpen Microsoft's download page now?" /SD IDNO IDNO +2
      ExecShell "open" "https://developer.microsoft.com/microsoft-edge/webview2/"
  ${EndIf}
SectionEnd

!ifdef DRIVER
Section "Set up the USB adapters now (WinUSB driver)" SecDriver
  ; libwdi's wdi-simple: WinUSB for every RTL8812BU adapter plugged in now (needs admin: setup has it)
  DetailPrint "Installing the WinUSB driver for the adapters plugged in..."
  nsExec::ExecToLog '"$SYSDIR\cmd.exe" /c ""$INSTDIR\driver\setup-adapters.cmd" /quiet"'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONINFORMATION "No adapter was set up (none plugged in?). Plug them in and run Start menu > ${PRODUCT} - Set up adapters." /SD IDOK
  ${EndIf}
SectionEnd
!endif

; (after the sections: .onInit refers to them by name)
Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT} needs 64-bit Windows 10 or 11." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  !ifdef DRIVER
    ; an update (already installed): the adapters already have their driver, so the driver step starts unticked
    ; (tick it to set up a new adapter; or Start menu > ${PRODUCT} - Set up adapters at any time)
    ReadRegStr $0 HKLM "Software\${PRODUCT}" "InstallDir"
    ${If} $0 != ""
      SectionSetFlags ${SecDriver} 0
    ${EndIf}
  !endif
FunctionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all
  ; the virtual cameras (registered for the user by the app at every start): unregister before the DLL goes
  ExecWait '"$SYSDIR\regsvr32.exe" /s /u /n /i "$INSTDIR\PlayLeagueCam.dll"'
  Delete "$INSTDIR\${EXE}"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\NOTICE.txt"
  RMDir /r "$INSTDIR\web"
  RMDir /r "$INSTDIR\licenses"
  RMDir /r "$INSTDIR\driver"
  Delete "$INSTDIR\Uninstall ${PRODUCT}.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${PRODUCT}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT} - Set up adapters.lnk"
  Delete "$DESKTOP\${PRODUCT}.lnk"
  DeleteRegKey HKLM "${UNKEY}"
  DeleteRegKey HKLM "Software\${PRODUCT}"
SectionEnd
