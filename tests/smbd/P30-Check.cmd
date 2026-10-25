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
  echo Usage: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "CTL=http://%PS5%:4445"
set "EXE=%~dp0..\..\dist\dev\prospero-probe.exe"
set "SECONDS=120"

echo ===== 1. Network of the PS5 =====
>con echo   Asking the PS5 for its network
curl.exe -sS --connect-timeout 3 --max-time 10 "%CTL%/net"
if errorlevel 1 (
  echo ERROR: no answer to /net. Is the payload still running?
  curl.exe -sS --connect-timeout 3 --max-time 5 -o nul "%CTL%/"
  if errorlevel 1 echo The payload does not answer at all any more, it probably crashed during the request.
  exit /b 1
)

echo.
echo ===== 2. Idle: CPU time =====
if not exist "%EXE%" (
  echo ERROR: %EXE% is missing. Run tools\Get-DevBuild.cmd first.
  exit /b 1
)
>con echo   Measuring idle load, about 2.5 minutes, progress below. Please do not use the PC or the PS5.
"%EXE%" %PS5% --no-broadcast --idle-test %SECONDS%
set "RC=%errorlevel%"
echo Exit code %RC%

echo.
if "%RC%"=="0" (
  echo P30-CHECK: PASSED
) else (
  echo P30-CHECK: FAILED, please send back the complete output
)
exit /b %RC%
