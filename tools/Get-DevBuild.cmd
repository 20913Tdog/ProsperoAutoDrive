@echo off
rem Fetches the latest green dev build (pre-release "dev-build") into dist\dev
rem and verifies every file against SHA256SUMS. Needs only Windows built-ins
rem (curl.exe, certutil); git is optional and only used for a commit check.
setlocal EnableExtensions EnableDelayedExpansion

set "BASE=https://github.com/20913Tdog/ProsperoAutoDrive/releases/download/dev-build"
set "ROOT=%~dp0.."
set "OUT=%ROOT%\dist\dev"

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || exit /b 1

curl.exe -fsSL -o "%OUT%\SHA256SUMS" "%BASE%/SHA256SUMS"
if errorlevel 1 (
  echo FEHLER: SHA256SUMS nicht abrufbar. Gibt es schon einen gruenen dev-Build?
  exit /b 1
)

set "FAIL=0"
for /f "usebackq tokens=1,2" %%H in ("%OUT%\SHA256SUMS") do (
  curl.exe -fsSL -o "%OUT%\%%I" "%BASE%/%%I"
  if errorlevel 1 (
    echo FEHLER  %%I  Download fehlgeschlagen
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
      echo FALSCH  %%I  Pruefsumme stimmt nicht
      set "FAIL=1"
    )
  )
)

if "%FAIL%"=="1" (
  echo.
  echo FEHLER: Mindestens eine Datei ist unvollstaendig oder veraendert. Nicht verwenden.
  exit /b 1
)

set "BUILD_COMMIT="
for /f "usebackq tokens=1,* delims==" %%A in ("%OUT%\BUILD-INFO.txt") do (
  if "%%A"=="commit" set "BUILD_COMMIT=%%B"
)
echo.
echo Build-Commit: %BUILD_COMMIT%

where git >nul 2>&1
if not errorlevel 1 (
  set "LOCAL_COMMIT="
  for /f "delims=" %%C in ('git -C "%ROOT%" rev-parse HEAD 2^>nul') do set "LOCAL_COMMIT=%%C"
  if /i not "!LOCAL_COMMIT!"=="%BUILD_COMMIT%" (
    echo Hinweis: Dein lokaler Stand ist !LOCAL_COMMIT!.
    echo          Fuer passende Testskripte vorher "git pull" auf dev ausfuehren.
  )
)

echo Alle Dateien geprueft: %OUT%
exit /b 0
