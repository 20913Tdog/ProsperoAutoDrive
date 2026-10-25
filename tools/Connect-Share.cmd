@echo off
rem Opens the share of the paired console in the Explorer, with the user
rem and password the probe stored when it paired (Windows Credential
rem Manager). The connection lasts until the next logoff. A stopgap until
rem ProsperoAutoDrive.exe maps the drive itself (phase 3).
rem   Connect-Share.cmd [PS5-IP]
rem Without an argument it uses the IP in tools\ps5.local.ini.
rem Windows built-ins only.
setlocal EnableExtensions
chcp 65001 >nul

for %%R in ("%~dp0..") do set "ROOT=%%~fR"
set "PS5=%~1"
set "INI=%ROOT%\tools\ps5.local.ini"
if not defined PS5 if exist "%INI%" (
  for /f "usebackq tokens=1,* delims==" %%A in ("%INI%") do if /i "%%A"=="ip" set "PS5=%%B"
)
if not defined PS5 (
  echo ERROR: no IP address. Usage: %~nx0 ^<PS5-IP^>
  pause
  exit /b 2
)
set "EXE=%ROOT%\dist\dev\prospero-probe.exe"
if not exist "%EXE%" (
  echo ERROR: %EXE% is missing. Run tools\Get-DevBuild.cmd first.
  pause
  exit /b 1
)

"%EXE%" %PS5% --no-broadcast --connect
if errorlevel 1 (
  echo.
  echo The connection failed. Please send back the output above.
  pause
  exit /b 1
)
start "" explorer.exe "\\%PS5%\prospero"
exit /b 0
