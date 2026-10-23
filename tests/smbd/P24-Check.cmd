@echo off
rem P2.4 live measurement (E3): response time under load and throughput,
rem for several settings of the server, plus ftpsrv as the reference.
rem The probe from the dev build sets send buffer and credit limit on the
rem server (TUNE) and per setting copies a 1 GiB file to the share and
rem back, while it times a small file every 2 s and fetches the server's
rem queue counters. Three rounds, every other one in reverse order (1 GiB,
rem so that a run on the cable lasts long enough to measure). Afterwards
rem ftpsrv reads and writes the same file three times.
rem Everything on the console goes into one new folder
rem /data/_ProsperoAutoDrive_Test_<time>-<random>; nothing is deleted there.
rem The local copies in %TEMP% are deleted.
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
set "LOCAL=%TEMP%\prospero-p24-ftp.bin"
if not defined FTPPORT set "FTPPORT=2121"
set "MIB=1024"
set "ROUNDS=3"
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
echo Einstellungen: Standard des Servers, %ROUNDS% Runden je %MIB% MiB

echo.
echo ===== 1. SMB: Varianten =====
>con echo   SMB: %ROUNDS% Runden je %MIB% MiB schreiben und lesen, Fortschritt unten
"%EXE%" %PS5% --no-broadcast --load-test data %MIB% --rounds %ROUNDS% --variants "%VARIANTS%" --test-name !NAME!
set "RC=%errorlevel%"
echo Exitcode %RC%

echo.
echo ===== 2. Referenz: ftpsrv auf Port %FTPPORT% =====
set "FTP=ftp://%PS5%:%FTPPORT%/data/!NAME!"
del /q "%LOCAL%" 2>nul
rem The progress bar of curl goes to the console (CON), not into the log of
rem Run-Test.cmd; the -w line with the result goes into the log.
for %%R in (1 2 3) do (
  >con echo   FTP lesen %%R von 3
  curl.exe -S --progress-bar --stderr CON --connect-timeout 5 --max-time 1800 -o "%LOCAL%" -w "FTP lesen     %%R: %%{size_download} Bytes, %%{speed_download} Bytes/s\n" "!FTP!/big-1-1.bin"
  if exist "%LOCAL%" (
    >con echo   FTP schreiben %%R von 3
    curl.exe -S --progress-bar --stderr CON --connect-timeout 5 --max-time 1800 -T "%LOCAL%" -w "FTP schreiben %%R: %%{size_upload} Bytes, %%{speed_upload} Bytes/s\n" "!FTP!/ftp-%%R.bin"
  ) else (
    echo FTP lesen %%R fehlgeschlagen, ohne Referenz weiter.
  )
)
del /q "%LOCAL%" 2>nul

echo.
echo ===== 3. Server-Status nachher =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"tune_" /c:"diag_" /c:"cpu_" /c:"since_reset_ms=" /c:"lost_replies=" "%STATUS%"

echo.
if "%RC%"=="0" (
  echo P24-CHECK: DURCHGELAUFEN
) else (
  echo P24-CHECK: FEHLER, Ausgabe bitte komplett zurueckmelden
)
exit /b %RC%
