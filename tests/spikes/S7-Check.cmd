@echo off
rem Spike S7: can a Windows program find the console by UDP, as a standard
rem user and without a firewall prompt? Runs prospero-s7-discover.exe from
rem the dev build: first unicast to the console's address, then broadcast
rem on every network. Then shows what the payload measured, including the
rem clock skew from the PC's time in the requests. At the end it asks
rem whether a firewall window appeared. Nothing is written to the console.
rem   S7-Check.cmd <PS5-IP>
rem Windows built-ins only (curl.exe, netsh, whoami).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Aufruf: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "CTL=http://%PS5%:4445"
set "EXE=%~dp0..\..\dist\dev\prospero-s7-discover.exe"
set "STATUS=%TEMP%\prospero-s7-status.txt"
set "FAIL=0"

echo ===== 0. Payload-Status und PC =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-S7 OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo FEHLER: Das S7-Payload antwortet nicht auf Port 4445. Laeuft es noch?
  exit /b 1
)
findstr /b /c:"folder=" /c:"now_utc=" /c:"discovery_" "%STATUS%"
if not exist "%EXE%" (
  echo FEHLER: %EXE% fehlt. Bitte tools\Get-DevBuild.cmd ausfuehren.
  exit /b 1
)
rem Standard user or administrator: an elevated token would hide a prompt.
"%SystemRoot%\System32\whoami.exe" /groups | findstr /c:"S-1-16-12288" >nul && (
  echo HINWEIS: Die Konsole laeuft mit Administratorrechten. Fuer S7 bitte als normaler Benutzer starten.
) || (
  echo OK      Konsole ohne Administratorrechte
)
echo Aktives Firewall-Profil:
netsh advfirewall show currentprofile 2>nul | findstr /i /r /c:"profil" /c:"profile" /c:"state" /c:"status" /c:"inbound" /c:"eingehend"

echo.
echo ===== 1. Suchen per UDP =====
>con echo.
>con echo   Gleich sucht ein Testprogramm die PS5. Falls ein Fenster der
>con echo   Windows-Firewall erscheint: auf "Abbrechen" klicken und am Ende
>con echo   mit j antworten.
>con echo.
ping -n 3 127.0.0.1 >nul
"%EXE%" %PS5%
set "RC=%errorlevel%"
echo Exitcode %RC%

echo.
echo ===== 2. Was das Payload gemessen hat =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"now_utc=" /c:"discovery_" "%STATUS%"
echo Referenz UTC ^(github.com^):
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
echo ===== 3. Firewall-Fenster? =====
rem The question goes to the console, the answer into the output.
ping -n 4 127.0.0.1 >nul
set "FW="
if defined PROSPERO_AUTO (
  >con echo.
  >con set /p "FW=  Ist ein Fenster der Windows-Firewall erschienen? (j/n): " <con
) else (
  set /p "FW=Ist ein Fenster der Windows-Firewall erschienen? (j/n): "
)
echo Antwort: "!FW!"

echo.
set "UNI=FALSCH"
set "BRO=FALSCH"
set /a "U=RC & 1, B=RC & 2"
if %RC% geq 3 set "FAIL=1"
if "%U%"=="0" (set "UNI=OK") else (set "FAIL=1")
if "%B%"=="0" (set "BRO=OK") else (set "FAIL=1")
echo %UNI%  Unicast-Antwort
echo %BRO%  Broadcast-Antwort
if /i "!FW!"=="n" (
  echo OK  kein Firewall-Fenster
) else if /i "!FW!"=="j" (
  echo FALSCH  Firewall-Fenster erschienen
  set "FAIL=1"
) else (
  echo FALSCH  keine eindeutige Antwort zum Firewall-Fenster
  set "FAIL=1"
)
echo.
if "%FAIL%"=="0" (
  echo S7-CHECK: BESTANDEN
) else (
  echo S7-CHECK: NICHT BESTANDEN, Ausgabe bitte komplett zurueckmelden
)
exit /b %FAIL%
