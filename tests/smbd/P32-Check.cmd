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
  echo Aufruf: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "DIST=%~dp0..\..\dist\dev"
set "APP=%DIST%\ProsperoAutoDrive.exe"
set "STATE=%TEMP%\prospero-p32"
set "LOG=%STATE%\log.txt"
if not exist "%APP%" (
  echo FEHLER: %APP% fehlt. Bitte tools\Get-DevBuild.cmd ausfuehren.
  exit /b 1
)
"%APP%" --stop >nul
if exist "%STATE%" rmdir /s /q "%STATE%"

rem The probe pairs this PC if it is not paired yet.
"%DIST%\prospero-probe.exe" %PS5% --no-broadcast >nul
if errorlevel 1 (
  echo FEHLER: Das Testprogramm konnte nicht koppeln, Ausgabe:
  "%DIST%\prospero-probe.exe" %PS5% --no-broadcast
  exit /b 1
)

echo ===== 1. Programm starten =====
>con echo   Programm starten, das Laufwerk sollte gleich erscheinen
echo %time% Start
start "ProsperoAutoDrive" /min "%APP%" --ip %PS5% --state-dir "%STATE%"
call :wait_for "MELDUNG: Server verbunden" 10 || (
  echo FEHLER: kein Laufwerk nach 10 s
  goto :end
)

echo.
echo ===== 2. Payload neu senden (ordentliches Ende und Neustart) =====
>con echo   Payload neu senden, das Laufwerk sollte kurz verschwinden und wiederkommen
echo %time% Payload senden
call "%~dp0..\..\tools\Send-Payload.cmd" %PS5% "%DIST%\prospero-smbd.elf" >nul
call :wait_count "MELDUNG: Server verbunden" 2 20 || echo FEHLER: das Laufwerk kam nicht wieder

echo.
echo ===== 3. Netzwerkkabel der PS5 (auf Wunsch) =====
>con echo.
>con echo   Jetzt bitte das Netzwerkkabel an der PS5 ziehen und Enter druecken.
>con set /p "SKIP=  (Wer das nicht moechte: n und Enter) " <con
if /i "!SKIP!"=="n" (
  echo uebersprungen
  goto :end
)
echo %time% Kabel gezogen
>con echo   Warte 20 s ...
ping -n 21 127.0.0.1 >nul
>con echo   Jetzt das Kabel wieder einstecken und Enter druecken.
>con set /p "X=" <con
echo %time% Kabel eingesteckt
>con echo   Warte, bis das Laufwerk wieder da ist (bis 60 s) ...
call :wait_count "MELDUNG: Server verbunden" 3 60 || echo FEHLER: das Laufwerk kam nach 60 s nicht wieder

:end
echo.
echo ===== Programm beenden =====
"%APP%" --stop
echo.
echo ===== Log des Programms =====
type "%LOG%"
echo.
echo P32-CHECK: DURCHGELAUFEN, die Zeiten stehen im Log
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
