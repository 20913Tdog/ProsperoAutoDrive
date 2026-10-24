@echo off
rem P2.4 live measurement (E3): response time under load and throughput,
rem for several settings of the server, plus ftpsrv as the reference.
rem The probe from the dev build sets send buffer and credit limit on the
rem server (TUNE) and per setting copies a 1 GiB file to the share and
rem back, while it times a small file every 2 s and fetches the server's
rem queue counters. Seven rounds of 1 GiB (G2: at least 7 runs per
rem direction, alternating, median); after each SMB round the probe calls
rem this script again with "--round <n>", and ftpsrv reads the file SMB just
rem wrote and writes it back as ftp-<n>.bin.
rem Everything on the console goes into one new folder
rem /data/_ProsperoAutoDrive_Test_<time>-<random>; nothing is deleted there.
rem The local copies in %TEMP% are deleted.
rem   P24-Check.cmd <PS5-IP>
rem Windows built-ins only (curl.exe).
if "%~1"=="--round" goto ftp_round
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
set "LOCAL=%TEMP%\prospero-p24-ftp.bin"
if not defined FTPPORT set "FTPPORT=2121"
set "MIB=1024"
set "ROUNDS=7"
rem No TUNE: changing the credit limit within a connection made Windows
rem reset it (twice) or hang (once). Only the server defaults are measured.
set "VARIANTS="

echo ===== 0. Server-Status =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-SMBD OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo FEHLER: prospero-smbd antwortet nicht auf Port 4445. Laeuft es?
  exit /b 1
)
findstr /b /c:"version=" /c:"paired_pcs=" /c:"clock_skew_s=" /c:"profile=" /c:"tune_" /c:"signing_" "%STATUS%"
if not exist "%EXE%" (
  echo FEHLER: %EXE% fehlt. Bitte tools\Get-DevBuild.cmd ausfuehren.
  exit /b 1
)
rem Folder name from the console's own clock, as the payloads name theirs.
set "U="
for /f "usebackq tokens=1,* delims==" %%A in ("%STATUS%") do if "%%A"=="now_utc" set "U=%%B"
set "NAME=_ProsperoAutoDrive_Test_!U:~0,4!!U:~5,2!!U:~8,2!-!U:~11,2!!U:~14,2!!U:~17,2!-p24!RANDOM!"
echo Testordner: /data/!NAME!
echo Einstellungen: Standard des Servers, %ROUNDS% Runden je %MIB% MiB, im Wechsel SMB und ftpsrv (Port %FTPPORT%)
echo Platz auf der PS5: etwa %ROUNDS% GiB SMB und %ROUNDS% GiB FTP im Testordner

echo.
echo ===== 1. SMB und ftpsrv im Wechsel =====
rem For the "--round" calls from the probe.
set "P24_FTP=ftp://%PS5%:%FTPPORT%/data/!NAME!"
set "P24_LOCAL=%LOCAL%"
del /q "%LOCAL%" 2>nul
>con echo   %ROUNDS% Runden je %MIB% MiB, Fortschritt unten
"%EXE%" %PS5% --no-broadcast --load-test data %MIB% --rounds %ROUNDS% --variants "%VARIANTS%" --test-name !NAME! --round-cmd "%~f0"
set "RC=%errorlevel%"
del /q "%LOCAL%" 2>nul
echo Exitcode %RC%

echo.
echo ===== 2. Server-Status nachher =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"tune_" /c:"diag_" /c:"cpu_" /c:"since_reset_ms=" /c:"lost_replies=" "%STATUS%"

echo.
if "%RC%"=="0" (
  echo P24-CHECK: DURCHGELAUFEN
) else (
  echo P24-CHECK: FEHLER, Ausgabe bitte komplett zurueckmelden
)
exit /b %RC%

rem Called by the probe after SMB round %2: ftpsrv reads the file of that
rem round and writes it back as ftp-<n>.bin. The progress bar of curl goes
rem to the console (CON), the -w line with the result into the log.
:ftp_round
setlocal EnableExtensions
set "R=%~2"
echo.
echo   --- ftpsrv, Runde %R%
>con echo   FTP lesen, Runde %R%
del /q "%P24_LOCAL%" 2>nul
curl.exe -S --progress-bar --stderr CON --connect-timeout 5 --max-time 1800 -o "%P24_LOCAL%" -w "  FTP lesen     %R%: %%{size_download} Bytes, %%{speed_download} Bytes/s\n" "%P24_FTP%/big-%R%-1.bin"
if not exist "%P24_LOCAL%" (
  echo   FTP lesen %R% fehlgeschlagen, ohne Referenz weiter.
  exit /b 0
)
>con echo   FTP schreiben, Runde %R%
curl.exe -S --progress-bar --stderr CON --connect-timeout 5 --max-time 1800 -T "%P24_LOCAL%" -w "  FTP schreiben %R%: %%{size_upload} Bytes, %%{speed_upload} Bytes/s\n" "%P24_FTP%/ftp-%R%.bin"
exit /b 0
