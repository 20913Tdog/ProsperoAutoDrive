@echo off
rem Many files in one folder (E3, 25.10.2026: deleting 100,000 files left
rem the service channel without answers for minutes). The probe creates
rem COUNT empty files in a new folder /data/_ProsperoAutoDrive_Test_<time>-<random>,
rem looks some up by name, lists the folder and deletes them again, one by
rem one, then the folder. Meanwhile it sends a PING every 200 ms. Goal: every
rem PING answered within 1 s. Only files this run created are deleted, by
rem name; the first failure stops it and leaves the rest.
rem   P34-Check.cmd <PS5-IP> [COUNT, default 20000]
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Usage: %~nx0 ^<PS5-IP^> [count]
  exit /b 2
)
set "PS5=%~1"
set "COUNT=%~2"
if not defined COUNT set "COUNT=20000"
set "EXE=%~dp0..\..\dist\dev\prospero-probe.exe"

if not exist "%EXE%" (
  echo ERROR: %EXE% is missing. Run tools\Get-DevBuild.cmd first.
  exit /b 1
)
echo ===== Many files in one folder: %COUNT% =====
>con echo   Create, look up, list and delete %COUNT% files. Progress below.
"%EXE%" %PS5% --no-broadcast --many-files-test data %COUNT%
set "RC=%errorlevel%"
echo Exit code %RC%

echo.
if "%RC%"=="0" (
  echo P34-CHECK: PASSED
) else (
  echo P34-CHECK: FAILED, please send back the complete output
)
exit /b %RC%
