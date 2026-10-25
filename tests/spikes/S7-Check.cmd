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
  echo Usage: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "CTL=http://%PS5%:4445"
set "EXE=%~dp0..\..\dist\dev\prospero-s7-discover.exe"
set "STATUS=%TEMP%\prospero-s7-status.txt"
set "FAIL=0"

echo ===== 0. Payload status and PC =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-S7 OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo ERROR: the S7 payload does not answer on port 4445. Is it still running?
  exit /b 1
)
findstr /b /c:"folder=" /c:"now_utc=" /c:"discovery_" "%STATUS%"
if not exist "%EXE%" (
  echo ERROR: %EXE% is missing. Run tools\Get-DevBuild.cmd first.
  exit /b 1
)
rem Standard user or administrator: an elevated token would hide a prompt.
"%SystemRoot%\System32\whoami.exe" /groups | findstr /c:"S-1-16-12288" >nul && (
  echo NOTE: this console runs with administrator rights. For S7, start it as a standard user.
) || (
  echo OK      console without administrator rights
)
echo Active firewall profile:
netsh advfirewall show currentprofile 2>nul | findstr /i /r /c:"profil" /c:"profile" /c:"state" /c:"status" /c:"inbound" /c:"eingehend"

echo.
echo ===== 1. Discovery over UDP =====
>con echo.
>con echo   A test program now searches for the PS5. If a Windows Firewall
>con echo   window appears: click "Cancel" and answer y at the end.
>con echo.
ping -n 3 127.0.0.1 >nul
"%EXE%" %PS5%
set "RC=%errorlevel%"
echo Exit code %RC%

echo.
echo ===== 2. What the payload measured =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"now_utc=" /c:"discovery_" "%STATUS%"
echo Reference UTC ^(github.com^):
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
echo ===== 3. Firewall window? =====
rem The question goes to the console, the answer into the output.
ping -n 4 127.0.0.1 >nul
set "FW="
if defined PROSPERO_AUTO (
  >con echo.
  >con set /p "FW=  Did a Windows Firewall window appear? (y/n): " <con
) else (
  set /p "FW=Did a Windows Firewall window appear? (y/n): "
)
echo Answer: "!FW!"

echo.
set "UNI=FAIL"
set "BRO=FAIL"
set /a "U=RC & 1, B=RC & 2"
if %RC% geq 3 set "FAIL=1"
if "%U%"=="0" (set "UNI=OK") else (set "FAIL=1")
if "%B%"=="0" (set "BRO=OK") else (set "FAIL=1")
echo %UNI%  unicast answer
echo %BRO%  broadcast answer
if /i "!FW!"=="n" (
  echo OK  no firewall window
) else if /i "!FW!"=="y" (
  echo FAIL    a firewall window appeared
  set "FAIL=1"
) else (
  echo FAIL    no clear answer about the firewall window
  set "FAIL=1"
)
echo.
if "%FAIL%"=="0" (
  echo S7-CHECK: PASSED
) else (
  echo S7-CHECK: FAILED, please send back the complete output
)
exit /b %FAIL%
