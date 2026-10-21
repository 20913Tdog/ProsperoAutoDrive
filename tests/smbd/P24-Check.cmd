@echo off
rem P2.4 live measurement (E3): where does a reply wait under load? The
rem probe from the dev build writes a 256 MiB file into a new folder
rem /data/_ProsperoAutoDrive_Test_<time>-<random> and reads it back, twice.
rem Meanwhile it times a small file every 2 s and fetches the server's queue
rem counters over the service channel. Only measures, changes no settings.
rem Nothing outside the new test folder is written, nothing is deleted.
rem   P24-Check.cmd <PS5-IP>
rem Windows built-ins only (curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Aufruf: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "CTL=http://%PS5%:4445"
set "EXE=%~dp0..\..\dist\dev\prospero-probe.exe"
set "STATUS=%TEMP%\prospero-p24-status.txt"

echo ===== 0. Server-Status =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-SMBD OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo FEHLER: prospero-smbd antwortet nicht auf Port 4445. Laeuft es?
  exit /b 1
)
findstr /b /c:"version=" /c:"paired_pcs=" /c:"clock_skew_s=" /c:"profile=" /c:"diag_sndbuf=" "%STATUS%"
if not exist "%EXE%" (
  echo FEHLER: %EXE% fehlt. Bitte tools\Get-DevBuild.cmd ausfuehren.
  exit /b 1
)

echo.
echo ===== 1. Messung =====
"%EXE%" %PS5% --no-broadcast --load-test data 256 --rounds 2
set "RC=%errorlevel%"
echo Exitcode %RC%

echo.
echo ===== 2. Server-Status nachher =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"diag_" /c:"cpu_" /c:"since_reset_ms=" /c:"bytes_read=" /c:"bytes_written=" /c:"lost_replies=" "%STATUS%"

echo.
if "%RC%"=="0" (
  echo P24-CHECK: DURCHGELAUFEN
) else (
  echo P24-CHECK: FEHLER, Ausgabe bitte komplett zurueckmelden
)
exit /b %RC%
