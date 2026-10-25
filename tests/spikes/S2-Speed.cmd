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
  echo Usage: %~nx0 ^<PS5-IP^> [FTP port]
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
  echo ERROR: the S2 payload does not answer on port 4445. Please start it again.
  exit /b 1
)
set "FOLDER="
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"folder=" "%STATUS%"') do set "FOLDER=%%B"
echo Test folder: %FOLDER%
rem tools\Run-Test.cmd passes the password in PROSPERO_PW.
if defined PROSPERO_PW (
  set "PW=%PROSPERO_PW%"
) else (
  set /p "PW=Password from the PS5 notification: "
)
mkdir "%OUT%"
echo Start: %date% %time%
echo Existing connections to the PS5 (they are dropped):
net use 2>nul | findstr /i /c:"%PS5%"

call :ftp before
for %%P in (a b c) do call :profile %%P
call :ftp after

call :disconnect
rmdir /s /q "%OUT%"
echo.
echo End: %date% %time%
echo S2-SPEED: DONE, please send back the complete output
exit /b 0

:ftp
echo.
echo ===== Wi-Fi reference %1: FTP on port %FTPPORT%, 64 MiB =====
curl.exe -sS --connect-timeout 3 --max-time 300 -o NUL -w "bytes %%{size_download}, seconds %%{time_total}, bytes/s %%{speed_download}\n" "ftp://%PS5%:%FTPPORT%%FOLDER%/pattern-64MiB.bin"
if errorlevel 1 echo ^(FTP reference failed, see the curl message above^)
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
echo ===== Profile %P% =====
call :disconnect
curl.exe -s --max-time 5 "%CTL%/profile/%P%"
rem Give the console a moment to end the previous connection.
ping -n 3 127.0.0.1 >nul
net use "%SHARE%" "%PW%" /user:prospero /persistent:no >nul
if errorlevel 1 (
  echo ERROR: connecting with profile %P% failed. Existing connections:
  net use
  goto :eof
)
call :big 1
call :big 2
call :small
goto :eof

:big
curl.exe -s --max-time 5 "%CTL%/reset" >nul
echo --- 64 MiB, run %1
robocopy "%SHARE%" "%OUT%\%P%-%1" pattern-64MiB.bin /R:0 /W:0 /NP /NJH /NFL /NDL | findstr /c:"Times :" /c:"Speed :"
call :server
goto :eof

:small
curl.exe -s --max-time 5 "%CTL%/reset" >nul
echo --- 100 small files
robocopy "%SHARE%\viele" "%OUT%\%P%-small" "datei-01*.txt" /R:0 /W:0 /NP /NJH /NFL /NDL | findstr /c:"Files :" /c:"Times :"
call :server
goto :eof

:server
curl.exe -s --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"profile=" /c:"dialect=" /c:"since_reset_ms=" /c:"cpu_user_ms=" /c:"cpu_sys_ms=" /c:"reads=" /c:"bytes_read=" /c:"read_window_ms=" /c:"pread_ms=" "%STATUS%"
goto :eof
