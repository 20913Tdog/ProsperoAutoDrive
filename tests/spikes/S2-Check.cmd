@echo off
rem Spike S2 check from Windows: connects the built-in SMB client to the
rem read-only test share of the S2 payload, copies everything, verifies the
rem SHA-256 values and that nothing can be written. Windows built-ins only
rem (net, robocopy, certutil, curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Usage: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "SHARE=\\%PS5%\s2"
set "EXPECTED=%~dp0s2-expected.sha256"
set "OUT=%TEMP%\ProsperoS2-%RANDOM%%RANDOM%"
set "STATUS=%TEMP%\prospero-s2-status.txt"
set "FAIL=0"

echo ===== 0. Payload status =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "http://%PS5%:4445/"
findstr /b /c:"PROSPERO-S2 OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo ERROR: the S2 payload does not answer on port 4445. Is it still running?
  exit /b 1
)
type "%STATUS%"
echo.
rem tools\Run-Test.cmd passes the password in PROSPERO_PW and sets
rem PROSPERO_AUTO, which skips the manual Explorer step.
if defined PROSPERO_PW (
  set "PW=%PROSPERO_PW%"
) else (
  set /p "PW=Password from the PS5 notification: "
)

echo.
echo ===== 1. Connect =====
call :disconnect
net use "%SHARE%" "%PW%" /user:prospero /persistent:no
if errorlevel 1 (
  echo ERROR: connecting failed. Please include the error text above.
  echo Existing connections:
  net use
  set "FAIL=1"
  goto :report
)
net use "%SHARE%"

echo.
echo ===== 2. Folder contents =====
dir "%SHARE%"

echo.
echo ===== 3. Copy everything =====
robocopy "%SHARE%" "%OUT%" /E /R:0 /W:0 /NP /NFL /NDL /NJH
if errorlevel 8 (
  echo ERROR: robocopy reports errors.
  set "FAIL=1"
)

echo.
echo ===== 4. Checksums =====
for /f "usebackq tokens=1,*" %%H in ("%EXPECTED%") do call :check %%H "%%I"
set "COUNT=0"
for /f %%N in ('dir /s /b /a-d "%OUT%" 2^>nul ^| find /c /v ""') do set "COUNT=%%N"
if "%COUNT%"=="1507" (
  echo OK      1507 files copied
) else (
  echo FAIL    %COUNT% files copied instead of 1507
  set "FAIL=1"
)

echo.
echo ===== 5. Upper and lower case =====
set "LINE="
for /f "usebackq delims=" %%L in ("%SHARE%\mixedcase.txt") do set "LINE=%%L"
if "%LINE%"=="Gross und klein" (
  echo OK      mixedcase.txt finds MixedCase.txt
) else (
  echo FAIL    mixedcase.txt not readable
  set "FAIL=1"
)

echo.
echo ===== 6. Write protection =====
(echo test> "%SHARE%\neu.txt") 2>nul
if exist "%SHARE%\neu.txt" (
  echo FAIL    neu.txt could be created
  set "FAIL=1"
) else (
  echo OK      creating a file refused
)
mkdir "%SHARE%\neu" 2>nul
if exist "%SHARE%\neu\" (
  echo FAIL    folder neu could be created
  set "FAIL=1"
) else (
  echo OK      creating a folder refused
)
del /q "%SHARE%\hello.txt" >nul 2>&1
ren "%SHARE%\MixedCase.txt" umbenannt.txt >nul 2>&1
if exist "%SHARE%\hello.txt" if exist "%SHARE%\MixedCase.txt" (
  echo OK      deleting and renaming refused
  goto :explorer
)
echo FAIL    deleting or renaming was possible
set "FAIL=1"

:explorer
echo.
echo ===== 7. Explorer =====
if defined PROSPERO_AUTO (
  echo skipped ^(automatic run^)
  goto :report
)
echo Now open %SHARE% in the Explorer and test it as described in
echo tests\spikes\README.md. Then press a key here.
pause >nul

:report
echo.
echo ===== 8. Connection details =====
powershell.exe -NoProfile -Command "Get-SmbConnection -ServerName '%PS5%' -ErrorAction Stop | Format-List ShareName,Dialect,Signed,Encrypted" 2>nul
if errorlevel 1 echo ^(Get-SmbConnection not available, for example without administrator rights^)

echo.
echo ===== 9. Payload status after =====
curl.exe -s --connect-timeout 3 --max-time 5 "http://%PS5%:4445/"

call :disconnect
if exist "%OUT%" rmdir /s /q "%OUT%"

echo.
if "%FAIL%"=="0" (
  echo S2-CHECK: ALL OK
) else (
  echo S2-CHECK: FAILED, please send back the complete output
)
exit /b %FAIL%

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

:check
set "FILE=%~2"
set "FILE=!FILE:/=\!"
if not exist "%OUT%\!FILE!" (
  echo MISSING !FILE!
  set "FAIL=1"
  goto :eof
)
rem certutil cannot hash empty files; their SHA-256 is fixed.
for %%S in ("%OUT%\!FILE!") do set "SIZE=%%~zS"
if "!SIZE!"=="0" if /i "%~1"=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" (
  echo OK      !FILE!
  goto :eof
)
set "GOT="
for /f "skip=1 delims=" %%L in ('certutil -hashfile "%OUT%\!FILE!" SHA256') do (
  if not defined GOT set "GOT=%%L"
)
set "GOT=!GOT: =!"
if /i "!GOT!"=="%~1" (
  echo OK      !FILE!
) else (
  echo FAIL    !FILE!
  set "FAIL=1"
)
goto :eof
