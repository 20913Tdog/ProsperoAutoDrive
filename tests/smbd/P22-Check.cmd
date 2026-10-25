@echo off
rem P2.2 and P2.3 live test (E3): the product server prospero-smbd on the console and
rem prospero-probe.exe from the dev build in the role of the later program.
rem The probe finds the console by unicast and broadcast, pairs once (the
rem credential goes into the Windows Credential Manager, as the program will
rem do it), sends PING with and without mac, and writes one file into a new
rem folder /data/_ProsperoAutoDrive_Test_<time>-<random>. Nothing else on
rem the console is changed, nothing is deleted.
rem   P22-Check.cmd <PS5-IP>
rem Windows built-ins only (curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Usage: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "CTL=http://%PS5%:4445"
set "EXE=%~dp0..\..\dist\dev\prospero-probe.exe"
set "STATUS=%TEMP%\prospero-p22-status.txt"

echo ===== 0. Server status before =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-SMBD OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo ERROR: prospero-smbd does not answer on port 4445. Is it running?
  exit /b 1
)
findstr /b /c:"version=" /c:"server_id=" /c:"share=" /c:"paired_pcs=" /c:"pairing=" /c:"skew_" /c:"clock_skew_s=" /c:"now_utc=" /c:"restarts=" "%STATUS%"
if not exist "%EXE%" (
  echo ERROR: %EXE% is missing. Run tools\Get-DevBuild.cmd first.
  exit /b 1
)

echo.
echo ===== 1. Test program =====
"%EXE%" %PS5% --write-test data
set "RC=%errorlevel%"
echo Exit code %RC%

echo.
echo ===== 2. Server status after =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"paired_pcs=" /c:"pairing=" /c:"skew_" /c:"clock_skew_s=" /c:"discovery_" /c:"connections=" /c:"files_created=" /c:"dirs_created=" /c:"special_refused=" "%STATUS%"
echo Reference UTC ^(github.com^):
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
if "%RC%"=="0" (
  echo P22-CHECK: PASSED
) else (
  echo P22-CHECK: FAILED, please send back the complete output
)
exit /b %RC%
