@echo off
rem P3.2 live test (E3): ProsperoAutoDrive.exe from the dev build keeps the
rem console's share as a drive (P:, or the next free letter) while the
rem payload runs. The times come from the program's log:
rem   1. start: drive within 3 s
rem   2. the payload is sent again; the new instance ends the old one (an
rem      orderly end): drive gone within 1 s, back within 3 s of the restart
rem   3. on request: the console's network cable out for 20 s, then in
rem      again: drive gone within 5 s, back within 3 s
rem The program neither reads nor writes on the console; its state is in
rem %TEMP%\prospero-p32, the credentials are those of the pairing.
rem   P32-Check.cmd <PS5-IP>
rem Windows built-ins only (curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Usage: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "DIST=%~dp0..\..\dist\dev"
set "APP=%DIST%\ProsperoAutoDrive.exe"
set "STATE=%TEMP%\prospero-p32"
set "LOG=%STATE%\log.txt"
if not exist "%APP%" (
  echo ERROR: %APP% is missing. Run tools\Get-DevBuild.cmd first.
  exit /b 1
)
"%APP%" --stop >nul
if exist "%STATE%" rmdir /s /q "%STATE%"

rem The probe pairs this PC if it is not paired yet.
"%DIST%\prospero-probe.exe" %PS5% --no-broadcast >nul
if errorlevel 1 (
  echo ERROR: the test program could not pair, its output:
  "%DIST%\prospero-probe.exe" %PS5% --no-broadcast
  exit /b 1
)

echo ===== 1. Start the program =====
>con echo   Starting the program, the drive should appear shortly
echo %time% Start
start "ProsperoAutoDrive" /min "%APP%" --ip %PS5% --state-dir "%STATE%"
call :wait_for " connected as " 10 || (
  echo ERROR: no drive after 10 s
  goto :end
)

echo.
echo ===== 2. Send the payload again (orderly end and restart) =====
>con echo   Sending the payload again, the drive should vanish briefly and come back
echo %time% Payload sent
call "%~dp0..\..\tools\Send-Payload.cmd" %PS5% "%DIST%\prospero-smbd.elf" >nul
call :wait_count " connected as " 2 20 || echo ERROR: the drive did not come back

echo.
echo ===== 3. Network cable of the PS5 (optional) =====
>con echo.
>con echo   Now unplug the network cable of the PS5 and press Enter.
>con set /p "SKIP=  (To skip this step: n and Enter) " <con
if /i "!SKIP!"=="n" (
  echo skipped
  goto :end
)
echo %time% Cable unplugged
>con echo   Waiting 20 s ...
ping -n 21 127.0.0.1 >nul
>con echo   Now plug the cable in again and press Enter.
>con set /p "X=" <con
echo %time% Cable plugged in
>con echo   Waiting until the drive is back (up to 60 s) ...
call :wait_count " connected as " 3 60 || echo ERROR: the drive did not come back within 60 s

:end
echo.
echo ===== End the program =====
"%APP%" --stop
echo.
echo ===== Log of the program =====
type "%LOG%"
echo.
echo P32-CHECK: COMPLETED, the times are in the log
exit /b 0

rem :wait_for <text> <seconds>: until the log has the text.
:wait_for
call :wait_count %1 1 %2
exit /b %errorlevel%

rem :wait_count <text> <n> <seconds>: until the log has the text n times.
:wait_count
set /a "WAIT_LEFT=%~3 * 4"
:wait_loop
set "N=0"
if exist "%LOG%" for /f %%C in ('findstr /c:%1 "%LOG%" ^| find /c /v ""') do set "N=%%C"
if !N! geq %~2 exit /b 0
set /a WAIT_LEFT-=1
if !WAIT_LEFT! leq 0 exit /b 1
rem a quarter second
ping -n 1 -w 250 192.0.2.1 >nul
goto :wait_loop
