@echo off
rem Spike S2b: where does the time go when Windows reads from the S2 server?
rem For each server profile (a: as in S2, b: 1 MiB socket buffers and
rem TCP_NODELAY, c: as b but SMB 2.1 signing) it copies the 64 MiB file twice
rem and 100 small files once, and prints robocopy's timing next to the
rem server's CPU and read counters. A download from the console's FTP server
rem on port 1337 (if there is one) before and after gives a WLAN reference.
rem Needs a freshly started payload, so that profile a is untouched.
rem Windows built-ins only (net, robocopy, curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Aufruf: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
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
set /p "PW=Passwort aus der PS5-Benachrichtigung: "
mkdir "%OUT%"
echo Start: %date% %time%

call :ftp vorher
for %%P in (a b c) do call :profile %%P
call :ftp nachher

net use "%SHARE%" /delete /y >nul 2>&1
rmdir /s /q "%OUT%"
echo.
echo Ende: %date% %time%
echo S2-SPEED: FERTIG, Ausgabe bitte komplett zurueckmelden
exit /b 0

:ftp
echo.
echo ===== WLAN-Referenz %1: FTP auf Port 1337, 64 MiB =====
curl.exe -s --connect-timeout 3 --max-time 300 -o NUL -w "Bytes %%{size_download}, Sekunden %%{time_total}, Bytes/s %%{speed_download}\n" "ftp://%PS5%:1337%FOLDER%/pattern-64MiB.bin"
if errorlevel 1 echo ^(kein FTP-Server auf Port 1337 erreichbar, Referenz entfaellt^)
goto :eof

:profile
set "P=%1"
echo.
echo ===== Profil %P% =====
net use "%SHARE%" /delete /y >nul 2>&1
curl.exe -s --max-time 5 "%CTL%/profile/%P%"
rem Give the console a moment to end the previous connection.
ping -n 3 127.0.0.1 >nul
net use "%SHARE%" "%PW%" /user:prospero /persistent:no >nul
if errorlevel 1 (
  echo FEHLER: Verbindung mit Profil %P% fehlgeschlagen.
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
