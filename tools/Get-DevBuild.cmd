@echo off
rem Fetches the latest green dev build (pre-release "dev-build") into dist\dev
rem and verifies every file against SHA256SUMS. Needs only Windows built-ins
rem (curl.exe, certutil); git is optional and only used for a commit check.
setlocal EnableExtensions EnableDelayedExpansion

set "BASE=https://github.com/20913Tdog/ProsperoAutoDrive/releases/download/dev-build"
set "ROOT=%~dp0.."
set "OUT=%ROOT%\dist\dev"

if exist "%OUT%" rmdir /s /q "%OUT%" 2>nul
if exist "%OUT%" (
  echo ERROR: %OUT% cannot be emptied, a file in it is still in use.
  echo These programs from the build are still running:
  set "SEEN=0"
  for %%P in (ProsperoAutoDrive.exe prospero-probe.exe s7-discover.exe) do (
    tasklist /fi "imagename eq %%P" /nh 2>nul | findstr /i /b "%%P" >nul && (
      echo   %%P
      set "SEEN=1"
    )
  )
  if "!SEEN!"=="0" echo   none
  echo What helps:
  echo   - ProsperoAutoDrive.exe: right-click the tray icon, Exit.
  echo   - Otherwise end the process in Task Manager, tab Details.
  echo   - Close any Explorer or cmd window that is in dist\dev.
  echo   - A stuck connection to the PS5: net use \\^<PS5-IP^>\prospero /delete /y
  echo Then start this script again.
  exit /b 1
)
mkdir "%OUT%" || exit /b 1

curl.exe -fsSL -o "%OUT%\SHA256SUMS" "%BASE%/SHA256SUMS"
if errorlevel 1 (
  echo ERROR: SHA256SUMS cannot be fetched. Is there a green dev build yet?
  exit /b 1
)

set "FAIL=0"
set "TOTAL=0"
for /f "usebackq" %%H in ("%OUT%\SHA256SUMS") do set /a TOTAL+=1
set "N=0"
for /f "usebackq tokens=1,2" %%H in ("%OUT%\SHA256SUMS") do (
  set /a N+=1
  rem Progress on the console only, so that the list below stays clean.
  >con echo   [!N!/%TOTAL%] %%I
  curl.exe -fsSL -o "%OUT%\%%I" "%BASE%/%%I"
  if errorlevel 1 (
    echo FAIL    %%I  download failed
    set "FAIL=1"
  ) else (
    set "GOT="
    for /f "skip=1 delims=" %%L in ('certutil -hashfile "%OUT%\%%I" SHA256') do (
      if not defined GOT set "GOT=%%L"
    )
    set "GOT=!GOT: =!"
    if /i "!GOT!"=="%%H" (
      echo OK      %%I
    ) else (
      echo FAIL    %%I  checksum does not match
      set "FAIL=1"
    )
  )
)

if "%FAIL%"=="1" (
  echo.
  echo ERROR: At least one file is incomplete or changed. Do not use the build.
  exit /b 1
)

set "BUILD_COMMIT="
for /f "usebackq tokens=1,* delims==" %%A in ("%OUT%\BUILD-INFO.txt") do (
  if "%%A"=="commit" set "BUILD_COMMIT=%%B"
)
echo.
echo Build commit: %BUILD_COMMIT%

where git >nul 2>&1
if not errorlevel 1 (
  set "LOCAL_COMMIT="
  for /f "delims=" %%C in ('git -C "%ROOT%" rev-parse HEAD 2^>nul') do set "LOCAL_COMMIT=%%C"
  if /i not "!LOCAL_COMMIT!"=="%BUILD_COMMIT%" (
    echo Note: your local checkout is !LOCAL_COMMIT!.
    echo       For matching test scripts, run "git pull" on dev first.
  )
)

echo All files verified: %OUT%
exit /b 0
