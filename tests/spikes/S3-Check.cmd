@echo off
rem Spike S3 check from Windows: connects the built-in SMB client to the
rem read-write test share of the S3 payload. It copies the test set down,
rem up into a new folder and down again, compares every byte and the
rem timestamps, then creates, changes, renames, moves and deletes files and
rem folders. Everything it writes lies inside the new test folder of this
rem run. Windows built-ins only (net, robocopy, certutil, fc, curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Usage: %~nx0 ^<PS5-IP^> [FTP port]
  exit /b 2
)
set "PS5=%~1"
set "FTPPORT=%~2"
if not defined FTPPORT set "FTPPORT=2121"
set "SHARE=\\%PS5%\s3"
set "CTL=http://%PS5%:4445"
set "EXPECTED=%~dp0s2-expected.sha256"
set "OUT=%TEMP%\ProsperoS3-%RANDOM%%RANDOM%"
set "STATUS=%TEMP%\prospero-s3-status.txt"
set "FAIL=0"
rem The CI (spike S6) runs the server on another port, which the Windows 11
rem 24H2 / Server 2025 client accepts with /TCPPORT. The console uses 445.
set "PORTOPT="
if defined PROSPERO_SMB_PORT set "PORTOPT=/TCPPORT:%PROSPERO_SMB_PORT%"

echo ===== 0. Payload status =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-S3 OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo ERROR: the S3 payload does not answer on port 4445. Is it still running?
  exit /b 1
)
type "%STATUS%"
set "FOLDER="
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"folder=" "%STATUS%"') do set "FOLDER=%%B"
echo.
rem tools\Run-Test.cmd passes the password in PROSPERO_PW and sets
rem PROSPERO_AUTO, which skips the manual Explorer step.
if defined PROSPERO_PW (
  set "PW=%PROSPERO_PW%"
) else (
  set /p "PW=Password from the PS5 notification: "
)
mkdir "%OUT%"

echo.
echo ===== 1. Connect =====
call :disconnect
net use "%SHARE%" "%PW%" /user:prospero /persistent:no %PORTOPT%
if errorlevel 1 (
  echo ERROR: connecting failed. Please include the error text above.
  echo Existing connections:
  net use
  set "FAIL=1"
  goto :report
)

echo.
echo ===== 2. Download the test data =====
robocopy "%SHARE%" "%OUT%\vorlage" /E /R:0 /W:0 /NP /NFL /NDL /NJH | findstr /c:"Files :" /c:"Times :" /c:"Speed :"
call :count "%OUT%\vorlage" vorlage

echo.
echo ===== 3. Upload into the new folder hochgeladen =====
curl.exe -s --max-time 5 "%CTL%/reset" >nul
robocopy "%OUT%\vorlage" "%SHARE%\hochgeladen" /E /R:0 /W:0 /NP /NFL /NDL /NJH > "%OUT%\up.txt"
set "RC=%errorlevel%"
findstr /c:"Files :" /c:"Times :" /c:"Speed :" "%OUT%\up.txt"
if %RC% geq 8 (
  echo FAIL    robocopy reports errors while uploading ^(exit code %RC%^):
  type "%OUT%\up.txt"
  set "FAIL=1"
) else (
  echo OK      robocopy without errors
)
call :server

echo.
echo ===== 4. Sizes and modification times =====
rem A dry run lists every file that differs in size or write time. The
rem console keeps times to the microsecond, Windows to 100 ns, so the check
rem uses robocopy's 2-second tolerance (/FFT); the exact count is shown too.
set "DIFF=0"
for /f %%N in ('robocopy "%OUT%\vorlage" "%SHARE%\hochgeladen" /E /L /FFT /R:0 /W:0 /NP /NJH /NJS /NDL /NC /NS /FP ^| findstr /r /c:"[^ ]" ^| find /c /v ""') do set "DIFF=%%N"
if "%DIFF%"=="0" (
  echo OK      all files have the same size and modification time ^(within 2 s^)
) else (
  echo FAIL    %DIFF% files differ in size or modification time, for example:
  robocopy "%OUT%\vorlage" "%SHARE%\hochgeladen" /E /L /FFT /R:0 /W:0 /NP /NJH /NJS /NDL /TS /FP | findstr /r /c:"[^ ]" | findstr /n "^" | findstr /b /r "[1-5]:"
  set "FAIL=1"
)
set "EXACT=0"
for /f %%N in ('robocopy "%OUT%\vorlage" "%SHARE%\hochgeladen" /E /L /R:0 /W:0 /NP /NJH /NJS /NDL /NC /NS /FP ^| findstr /r /c:"[^ ]" ^| find /c /v ""') do set "EXACT=%%N"
echo INFO    without tolerance %EXACT% of 1507 files differ ^(expected: rounding to microseconds^)

echo.
echo ===== 5. Download again and compare byte by byte =====
robocopy "%SHARE%\hochgeladen" "%OUT%\zurueck" /E /R:0 /W:0 /NP /NFL /NDL /NJH | findstr /c:"Files :" /c:"Times :" /c:"Speed :"
call :count "%OUT%\zurueck" zurueck
for /f "usebackq tokens=1,*" %%H in ("%EXPECTED%") do call :check "%OUT%\zurueck" %%H "%%I"
set "BAD=0"
for /f "delims=" %%F in ('dir /s /b /a-d "%OUT%\vorlage"') do (
  set "F=%%F"
  set "R=!F:%OUT%\vorlage\=!"
  fc /b "%%F" "%OUT%\zurueck\!R!" >nul 2>&1
  if errorlevel 1 (
    set /a BAD+=1
    if !BAD! leq 5 echo FAIL    !R!
  )
)
if "%BAD%"=="0" (
  echo OK      all files identical
) else (
  echo FAIL    %BAD% files differ
  set "FAIL=1"
)

echo.
echo ===== 6. Work with files and folders =====
rem Each step sets V to 1 when its result is right; :verdict reports it.
set "OPS=%SHARE%\ops"
mkdir "%OPS%" 2>nul
set "V=0" & if exist "%OPS%\" set "V=1"
call :verdict "create a folder"
set "V=0" & mkdir "%SHARE%\OPS" 2>nul || set "V=1"
call :verdict "folder OPS next to ops refused"
(echo one) > "%OPS%\a.txt"
call :lines "%OPS%\a.txt" 1
call :verdict "create a file"
(echo two) >> "%OPS%\a.txt"
call :lines "%OPS%\a.txt" 2
call :verdict "append"
(echo three) > "%OPS%\a.txt"
call :lines "%OPS%\a.txt" 1
call :verdict "overwriting shortens the file"
copy /y "%SHARE%\hello.txt" "%OPS%\kopie.txt" >nul
set "V=0" & fc /b "%SHARE%\hello.txt" "%OPS%\kopie.txt" >nul 2>&1 && set "V=1"
call :verdict "copy within the share"
ren "%OPS%\a.txt" b.txt
set "V=0" & if exist "%OPS%\b.txt" if not exist "%OPS%\a.txt" set "V=1"
call :verdict "rename"
ren "%OPS%\b.txt" B.TXT
set "NAME="
for /f "delims=" %%N in ('dir /b "%OPS%\b.txt" 2^>nul') do set "NAME=%%N"
set "V=0" & if "!NAME!"=="B.TXT" set "V=1"
call :verdict "change upper/lower case, name now !NAME!"
mkdir "%OPS%\unter"
move "%OPS%\B.TXT" "%OPS%\unter\" >nul
set "V=0" & if exist "%OPS%\unter\B.TXT" if not exist "%OPS%\B.TXT" set "V=1"
call :verdict "move"
ren "%OPS%\unter" unter2
set "V=0" & if exist "%OPS%\unter2\B.TXT" if not exist "%OPS%\unter\" set "V=1"
call :verdict "rename a folder"
rd "%OPS%\unter2" 2>nul
set "V=0" & if exist "%OPS%\unter2\B.TXT" set "V=1"
call :verdict "deleting a non-empty folder refused"
del /q "%OPS%\unter2\B.TXT"
rd "%OPS%\unter2" 2>nul
set "V=0" & if not exist "%OPS%\unter2\" set "V=1"
call :verdict "delete a file and an empty folder"
rem Recursive delete of a small tree created just before, inside ops.
mkdir "%OPS%\baum\a\b"
(echo x) > "%OPS%\baum\a\b\y.txt"
(echo x) > "%OPS%\baum\z.txt"
rd /s /q "%OPS%\baum"
set "V=0" & if not exist "%OPS%\baum\" set "V=1"
call :verdict "delete a folder tree"

echo.
echo ===== 7. Timestamps of new files =====
rem Written without an explicit time, so the console's clock sets it.
(echo zeit) > "%OPS%\zeit.txt"
echo PC now, local time:        %date% %time%
for %%F in ("%OPS%\zeit.txt") do echo zeit.txt in the Explorer:  %%~tF
echo zeit.txt in UTC ^(robocopy^):
robocopy "%OPS%" "%OUT%\leer" zeit.txt /L /TS /NJH /NJS /NDL /NC /NS /FP | findstr /r /c:"[^ ]"
echo Reference UTC ^(github.com^):
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
echo ===== 8. Write speed, 64 MiB =====
curl.exe -s --max-time 5 "%CTL%/reset" >nul
robocopy "%OUT%\vorlage" "%SHARE%\tempo" pattern-64MiB.bin /R:0 /W:0 /NP /NJH /NFL /NDL | findstr /c:"Times :" /c:"Speed :"
call :server
echo --- Wi-Fi reference: FTP upload on port %FTPPORT% into the test folder
curl.exe -sS --connect-timeout 3 --max-time 300 -T "%OUT%\vorlage\pattern-64MiB.bin" -w "bytes %%{size_upload}, seconds %%{time_total}, bytes/s %%{speed_upload}\n" "ftp://%PS5%:%FTPPORT%%FOLDER%/ftp-upload.bin"
if errorlevel 1 echo ^(FTP reference failed, see the curl message above^)

if defined PROSPERO_AUTO goto :report
echo.
echo ===== 9. Explorer =====
echo Now open %SHARE% in the Explorer and test it as described in
echo tests\spikes\README.md. Then press a key here.
pause >nul

:report
echo.
echo ===== Payload status after =====
curl.exe -s --connect-timeout 3 --max-time 5 "%CTL%/"

call :disconnect
if exist "%OUT%" rmdir /s /q "%OUT%"

echo.
if "%FAIL%"=="0" (
  echo S3-CHECK: ALL OK
) else (
  echo S3-CHECK: FAILED, please send back the complete output
)
exit /b %FAIL%


:server
curl.exe -s --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"since_reset_ms=" /c:"cpu_user_ms=" /c:"writes=" /c:"bytes_written=" /c:"pwrite_ms=" /c:"fsyncs=" /c:"fsync_ms=" "%STATUS%"
goto :eof

:count
set "COUNT=0"
for /f %%N in ('dir /s /b /a-d "%~1" 2^>nul ^| find /c /v ""') do set "COUNT=%%N"
if "%COUNT%"=="1507" (
  echo OK      %~2: 1507 files
) else (
  echo FAIL    %~2: %COUNT% files instead of 1507
  set "FAIL=1"
)
goto :eof

rem :lines <file> <n> sets V to 1 if the file has exactly n lines.
:lines
set "LINES=0"
for /f %%N in ('type "%~1" 2^>nul ^| find /c /v ""') do set "LINES=%%N"
set "V=0"
if "%LINES%"=="%~2" set "V=1"
goto :eof

:verdict
if "%V%"=="1" (
  echo OK      %~1
) else (
  echo FAIL    %~1
  set "FAIL=1"
)
goto :eof

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
set "FILE=%~3"
set "FILE=!FILE:/=\!"
if not exist "%~1\!FILE!" (
  echo MISSING !FILE!
  set "FAIL=1"
  goto :eof
)
rem certutil cannot hash empty files; their SHA-256 is fixed.
for %%S in ("%~1\!FILE!") do set "SIZE=%%~zS"
if "!SIZE!"=="0" if /i "%~2"=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" (
  echo OK      !FILE!
  goto :eof
)
set "GOT="
for /f "skip=1 delims=" %%L in ('certutil -hashfile "%~1\!FILE!" SHA256') do (
  if not defined GOT set "GOT=%%L"
)
set "GOT=!GOT: =!"
if /i "!GOT!"=="%~2" (
  echo OK      !FILE!
) else (
  echo FAIL    !FILE!
  set "FAIL=1"
)
goto :eof
