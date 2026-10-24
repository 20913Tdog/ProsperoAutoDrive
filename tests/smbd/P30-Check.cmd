@echo off
rem Before phase 3 (E3): what the heartbeat costs, and whether the console
rem can tell cable from Wi-Fi (decision of 24.10.2026).
rem   1. GET /net: the console's network interfaces and the link type of the
rem      interface this PC reaches it through.
rem   2. The probe connects the share, reads and writes nothing, and measures
rem      the CPU time of server and probe: 30 s without, 120 s with a PING
rem      every second. Goal: under 0.1 % of a core on both sides.
rem Nothing is written on the console.
rem   P30-Check.cmd <PS5-IP>
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
set "SECONDS=120"

echo ===== 1. Netzwerk der PS5 =====
>con echo   Netzwerk der PS5 abfragen
curl.exe -sS --connect-timeout 3 --max-time 10 "%CTL%/net"
if errorlevel 1 (
  echo FEHLER: keine Antwort auf /net. Laeuft das Payload noch?
  curl.exe -sS --connect-timeout 3 --max-time 5 -o nul "%CTL%/"
  if errorlevel 1 echo Das Payload antwortet gar nicht mehr, vermutlich beim Abfragen abgestuerzt.
  exit /b 1
)

echo.
echo ===== 2. Leerlauf: Rechenzeit =====
if not exist "%EXE%" (
  echo FEHLER: %EXE% fehlt. Bitte tools\Get-DevBuild.cmd ausfuehren.
  exit /b 1
)
>con echo   Leerlauf messen, etwa 2,5 Minuten, Fortschritt unten. PC und PS5 bitte nicht benutzen.
"%EXE%" %PS5% --no-broadcast --idle-test %SECONDS%
set "RC=%errorlevel%"
echo Exitcode %RC%

echo.
if "%RC%"=="0" (
  echo P30-CHECK: BESTANDEN
) else (
  echo P30-CHECK: FEHLER, Ausgabe bitte komplett zurueckmelden
)
exit /b %RC%
