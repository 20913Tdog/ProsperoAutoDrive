@echo off
rem Spike S2b: where does the time go when Windows reads from the S2 server?
rem For each server profile (a: as in S2, b: 1 MiB socket buffers and
rem TCP_NODELAY, c: as b but SMB 2.1 signing) it copies the 64 MiB file twice
rem and 100 small files once, and prints robocopy's timing next to the
rem server's CPU and read counters. A download from ftpsrv on the console
rem (port 2121 unless given) before and after gives a WLAN reference.
rem Needs a freshly started payload, so that profile a is untouched.
rem Windows built-ins only (net, robocopy, curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Aufruf: %~nx0 ^<PS5-IP^> [FTP-Port]
  exit /b 2
)
set "PS5=%~1"
set "FTPPORT=%~2"
if not defined FTPPORT set "FTPPORT=2121"
set "SHARE=\\%PS5%\s2"
set "CTL=http://%PS5%:4445"
set "OUT=%TEMP%\ProsperoS2b-%RANDOM%%RANDOM%"
set "STATUS=%TEMP%\prospero-s2b-status.txt"

curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-S2 OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo FEHLER: Das S2-Payload antwortet nicht auf Port 4445. Bitte neu starten.
  exit /b 1
)
set "FOLDER="
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"folder=" "%STATUS%"') do set "FOLDER=%%B"
echo Testordner: %FOLDER%
rem tools\Run-Test.cmd passes the password in PROSPERO_PW.
if defined PROSPERO_PW (
  set "PW=%PROSPERO_PW%"
) else (
  set /p "PW=Passwort aus der PS5-Benachrichtigung: "
)
mkdir "%OUT%"
echo Start: %date% %time%
echo Vorhandene Verbindungen zur PS5 (werden getrennt):
net use 2>nul | findstr /i /c:"%PS5%"

call :ftp vorher
for %%P in (a b c) do call :profile %%P
call :ftp nachher

call :disconnect
rmdir /s /q "%OUT%"
echo.
echo Ende: %date% %time%
echo S2-SPEED: FERTIG, Ausgabe bitte komplett zurueckmelden
exit /b 0

:ftp
echo.
echo ===== WLAN-Referenz %1: FTP auf Port %FTPPORT%, 64 MiB =====
curl.exe -sS --connect-timeout 3 --max-time 300 -o NUL -w "Bytes %%{size_download}, Sekunden %%{time_total}, Bytes/s %%{speed_download}\n" "ftp://%PS5%:%FTPPORT%%FOLDER%/pattern-64MiB.bin"
if errorlevel 1 echo ^(FTP-Referenz fehlgeschlagen, siehe curl-Meldung oben^)
goto :eof

rem Windows refuses a second user name for the same server (system error
rem 1219), so every connection to the console goes, including IPC$ and ones
rem the Explorer opened.
:disconnect
for /f "delims=" %%L in ('net use 2^>nul ^| findstr /i /c:"%PS5%"') do (
  for %%T in (%%L) do (
    set "T=%%T"
    if "!T:~0,2!"=="\\" net use "%%T" /delete /y >nul 2>&1
  )
)
goto :eof

:profile
set "P=%1"
echo.
echo ===== Profil %P% =====
call :disconnect
curl.exe -s --max-time 5 "%CTL%/profile/%P%"
rem Give the console a moment to end the previous connection.
ping -n 3 127.0.0.1 >nul
net use "%SHARE%" "%PW%" /user:prospero /persistent:no >nul
if errorlevel 1 (
  echo FEHLER: Verbindung mit Profil %P% fehlgeschlagen. Bestehende Verbindungen:
  net use
  goto :eof
)
call :big 1
call :big 2
call :small
goto :eof

:big
curl.exe -s --max-time 5 "%CTL%/reset" >nul
echo --- 64 MiB, Lauf %1
robocopy "%SHARE%" "%OUT%\%P%-%1" pattern-64MiB.bin /R:0 /W:0 /NP /NJH /NFL /NDL | findstr /c:"Times :" /c:"Speed :"
call :server
goto :eof

:small
curl.exe -s --max-time 5 "%CTL%/reset" >nul
echo --- 100 kleine Dateien
robocopy "%SHARE%\viele" "%OUT%\%P%-klein" "datei-01*.txt" /R:0 /W:0 /NP /NJH /NFL /NDL | findstr /c:"Files :" /c:"Times :"
call :server
goto :eof

:server
curl.exe -s --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"profile=" /c:"dialect=" /c:"since_reset_ms=" /c:"cpu_user_ms=" /c:"cpu_sys_ms=" /c:"reads=" /c:"bytes_read=" /c:"read_window_ms=" /c:"pread_ms=" "%STATUS%"
goto :eof
