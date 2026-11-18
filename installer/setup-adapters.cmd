@echo off
rem Sets the WinUSB driver on the RTL8812BU USB Wi-Fi adapters plugged in now, so Play RX can use
rem them as receivers (the adapter stops working as Wi-Fi). Uses libwdi's wdi-simple (the library behind Zadig).
rem   setup-adapters.cmd          from the Start menu: asks for admin rights, shows what it did
rem   setup-adapters.cmd /quiet   from setup (already admin): exit code 0 if an adapter was set up
setlocal
set QUIET=
if /i "%~1"=="/quiet" set QUIET=1

net session >nul 2>&1
if errorlevel 1 (
  if defined QUIET exit /b 2
  echo Asking for administrator rights to install the driver...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b 0
)

set WDI="%~dp0wdi-simple.exe"
set DEST="%TEMP%\play-adapters-driver"
set OK=0
rem RTL8812BU: Realtek's own ID (Cudy AC1300 and most others), TP-Link Archer T4U v3
for %%D in ("0x0bda 0xb812" "0x2357 0x0115") do call :one %%~D
if not defined QUIET (
  echo.
  if %OK%==1 (echo Done: the adapters above now use WinUSB. Unplug and replug them, then start Play RX.) else (echo No RTL8812BU adapter was found. Plug the adapters in, then run this again.)
  echo.
  pause
)
if %OK%==1 exit /b 0
exit /b 1

:one
set VID=%1
set PID=%2
if not defined QUIET echo Setting up %VID%:%PID% ...
if defined QUIET (
  %WDI% --vid %VID% --pid %PID% --type 0 --name "RTL8812BU (Play RX receiver)" --manufacturer "Play RX" --inf rtl8812bu_winusb.inf --dest %DEST% --silent
) else (
  %WDI% --vid %VID% --pid %PID% --type 0 --name "RTL8812BU (Play RX receiver)" --manufacturer "Play RX" --inf rtl8812bu_winusb.inf --dest %DEST% --progressbar
)
if not errorlevel 1 if errorlevel 0 (
  set OK=1
  if not defined QUIET echo   %VID%:%PID%: WinUSB installed
)
exit /b 0
