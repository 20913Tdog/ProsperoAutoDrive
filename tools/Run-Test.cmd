@echo off
rem One-click test run for the project owner. It updates the checkout
rem (git pull on dev), fetches and verifies the dev build, restarts the
rem payload on the console, runs the test script without questions, and puts
rem the complete output into the clipboard and into logs\<test>.log.
rem   Run-Test.cmd [test] [PS5-IP]
rem Without arguments it runs DEFAULT_TEST against the IP saved in
rem tools\ps5.local.ini (asked for once). Double-clicking works as well.
rem Windows built-ins plus git.
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

rem The test the owner is asked to run right now.
set "DEFAULT_TEST=P33"

rem git pull may rewrite this very file, and cmd reads a running batch file
rem from disk as it goes. So the pull runs from a copy in %TEMP%, which then
rem calls the updated file with --run.
if /i "%~1"=="--run" goto :main
if /i "%~1"=="--update" goto :update
copy /y "%~f0" "%TEMP%\prospero-run-test.cmd" >nul || exit /b 1
"%TEMP%\prospero-run-test.cmd" --update "%~dp0." "%~1" "%~2"

:update
for %%R in ("%~f2\..") do set "ROOT=%%~fR"
where git >nul 2>&1
if errorlevel 1 (
  echo ERROR: git was not found.
  pause
  exit /b 1
)
set "BRANCH="
for /f "delims=" %%B in ('git -C "%ROOT%" rev-parse --abbrev-ref HEAD') do set "BRANCH=%%B"
if /i not "%BRANCH%"=="dev" (
  echo ERROR: The folder %ROOT% is on branch "%BRANCH%" instead of dev.
  echo Run "git checkout dev" once and start again.
  pause
  exit /b 1
)
echo Fetching the latest state of dev ...
git -C "%ROOT%" pull --ff-only
if errorlevel 1 (
  echo ERROR: git pull failed, see the message above.
  pause
  exit /b 1
)
call "%ROOT%\tools\Run-Test.cmd" --run "%~3" "%~4"
set "RC=%errorlevel%"
echo.
pause
exit /b %RC%

:main
for %%R in ("%~dp0..") do set "ROOT=%%~fR"
set "TEST=%~2"
if not defined TEST set "TEST=%DEFAULT_TEST%"
set "PS5=%~3"
set "INI=%ROOT%\tools\ps5.local.ini"
if not defined PS5 if exist "%INI%" (
  for /f "usebackq tokens=1,* delims==" %%A in ("%INI%") do if /i "%%A"=="ip" set "PS5=%%B"
)
if not defined PS5 (
  set /p "PS5=IP address of the PS5: "
  if not defined PS5 exit /b 2
  > "%INI%" echo ip=!PS5!
)

set "ELF="
set "DURATION=a few minutes"
if /i "%TEST%"=="S2" (
  set "ELF=prospero-s2-smbserver.elf"
  set "SCRIPT=S2-Check.cmd"
  set "TAG=PROSPERO-S2"
)
if /i "%TEST%"=="S2b" (
  set "ELF=prospero-s2-smbserver.elf"
  set "SCRIPT=S2-Speed.cmd"
  set "TAG=PROSPERO-S2"
)
if /i "%TEST%"=="S3" (
  set "ELF=prospero-s3-smbserver.elf"
  set "SCRIPT=S3-Check.cmd"
  set "TAG=PROSPERO-S3"
)
if /i "%TEST%"=="S4" (
  set "ELF=prospero-s4-smbserver.elf"
  set "SCRIPT=S4-Check.cmd"
  set "TAG=PROSPERO-S4"
  set "DURATION=about 30 to 60 minutes"
)
if /i "%TEST%"=="S7" (
  set "ELF=prospero-s7-smbserver.elf"
  set "SCRIPT=S7-Check.cmd"
  set "TAG=PROSPERO-S7"
  set "DURATION=about 1 minute, with a question at the end"
)
rem Product server: it replaces a running instance by itself and refuses a
rem quit from the network, it has no folder but an instance ID per start,
rem and the probe pairs instead of a password over the control port. It
rem keeps running after the test.
set "PRODUCT="
set "KEY=folder"
if /i "%TEST%"=="P22" (
  set "ELF=prospero-smbd.elf"
  set "SCRIPT=..\smbd\P22-Check.cmd"
  set "TAG=PROSPERO-SMBD"
  set "PRODUCT=1"
  set "KEY=instance"
  set "DURATION=about 1 minute"
)
if /i "%TEST%"=="P24" (
  set "ELF=prospero-smbd.elf"
  set "SCRIPT=..\smbd\P24-Check.cmd"
  set "TAG=PROSPERO-SMBD"
  set "PRODUCT=1"
  set "KEY=instance"
  set "DURATION=about 10 minutes"
)
if /i "%TEST%"=="P32" (
  set "ELF=prospero-smbd.elf"
  set "SCRIPT=..\smbd\P32-Check.cmd"
  set "TAG=PROSPERO-SMBD"
  set "PRODUCT=1"
  set "KEY=instance"
  set "DURATION=about 2 minutes, with questions on the way"
)
if /i "%TEST%"=="P30" (
  set "ELF=prospero-smbd.elf"
  set "SCRIPT=..\smbd\P30-Check.cmd"
  set "TAG=PROSPERO-SMBD"
  set "PRODUCT=1"
  set "KEY=instance"
  set "DURATION=about 3 minutes"
)
if /i "%TEST%"=="P34" (
  set "ELF=prospero-smbd.elf"
  set "SCRIPT=..\smbd\P34-Check.cmd"
  set "TAG=PROSPERO-SMBD"
  set "PRODUCT=1"
  set "KEY=instance"
  set "DURATION=about 10 minutes"
)
rem P33: the Payload Manager starts the server from its file on the console,
rem so nothing is sent over the network here (NOSEND); the owner copies the
rem new file there and starts it from the dashboard.
set "NOSEND="
if /i "%TEST%"=="P33" (
  set "ELF=prospero-smbd.elf"
  set "SCRIPT=..\smbd\P33-Check.cmd"
  set "TAG=PROSPERO-SMBD"
  set "PRODUCT=1"
  set "KEY=instance"
  set "NOSEND=1"
  set "DURATION=about 15 minutes, with steps by hand on the PS5"
)
if not defined ELF (
  echo ERROR: Unknown test "%TEST%". Known tests: S2, S2b, S3, S4, S7, P22, P24, P30, P32, P33 and P34.
  exit /b 2
)

set "CTL=http://%PS5%:4445"
set "STATUS=%TEMP%\prospero-run-test-status.txt"
set "LOG=%ROOT%\logs\%TEST%.log"
if not exist "%ROOT%\logs" mkdir "%ROOT%\logs"

echo.
echo Test %TEST% with the PS5 at %PS5%. Please close Explorer windows showing the PS5.
echo The complete output is shown at the end.
echo.
call :steps > "%LOG%" 2>&1
set "RC=%errorlevel%"
echo.
type "%LOG%"
clip < "%LOG%"
echo.
echo ================================================================
echo The complete output is in the clipboard and in
echo %LOG%
echo Paste it into the chat with Ctrl+V.
echo ================================================================
exit /b %RC%


:steps
echo ===== Test run %TEST%, PS5 %PS5% =====
echo Start: %date% %time%
ver
git -C "%ROOT%" log -1 --oneline

echo.
call :say 1/6 Fetch and verify the build
rem A running ProsperoAutoDrive.exe from dist\dev keeps the folder from being
rem replaced, and the tests run their own instance anyway. --stop ends the one
rem of this logon, wherever it was started from; an installed one is started
rem again at the end.
set "WAS_RUNNING="
tasklist /fi "imagename eq ProsperoAutoDrive.exe" /nh 2>nul | findstr /i /b "ProsperoAutoDrive.exe" >nul && set "WAS_RUNNING=1"
if defined WAS_RUNNING (
  echo ProsperoAutoDrive.exe is running; it is ended for the test.
  >con echo   Ending the running ProsperoAutoDrive.exe for the test ...
  call :installed
  if exist "%ROOT%\dist\dev\ProsperoAutoDrive.exe" (
    "%ROOT%\dist\dev\ProsperoAutoDrive.exe" --stop
  ) else if defined INSTALLED_EXE (
    "!INSTALLED_EXE!" --stop
  )
  ping -n 2 127.0.0.1 >nul
)
call "%ROOT%\tools\Get-DevBuild.cmd"
if errorlevel 1 (
  call :fail "The build cannot be fetched or is damaged."
  exit /b 1
)
set "BUILD_COMMIT="
for /f "usebackq tokens=1,* delims==" %%A in ("%ROOT%\dist\dev\BUILD-INFO.txt") do (
  if "%%A"=="commit" set "BUILD_COMMIT=%%B"
)
set "HEAD_COMMIT="
for /f "delims=" %%C in ('git -C "%ROOT%" rev-parse HEAD') do set "HEAD_COMMIT=%%C"
if /i not "%BUILD_COMMIT%"=="%HEAD_COMMIT%" (
  call :fail "The latest green build is %BUILD_COMMIT:~0,7%, your checkout is %HEAD_COMMIT:~0,7%. The CI is probably still running. Please start again in a few minutes."
  exit /b 1
)

echo.
call :say 2/6 Drop old connections and end the old payload
call :disconnect
set "OLD_FOLDER="
del /q "%STATUS%" 2>nul
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
rem Without sending, the server that runs now is the one to start with.
if defined NOSEND del /q "%STATUS%" 2>nul
if exist "%STATUS%" (
  for /f "usebackq tokens=1,* delims==" %%A in ("%STATUS%") do (
    if "%%A"=="%KEY%" set "OLD_FOLDER=%%B"
  )
)
if defined PRODUCT (
  echo The new payload replaces a running one by itself.
  goto :gone
)
curl.exe -s --connect-timeout 3 --max-time 5 "%CTL%/quit"
set "N=0"
:wait_gone
set /a N+=1
curl.exe -s --connect-timeout 2 --max-time 3 -o nul "%CTL%/"
if errorlevel 1 goto :gone
if %N% geq 10 (
  call :fail "The old payload does not end."
  exit /b 1
)
ping -n 2 127.0.0.1 >nul
goto :wait_gone
:gone
echo Port 4445 is free.

echo.
call :say 3/6 Send the payload
set "MAXN=36"
rem Without sending, any server that answers will do; the test then asks for
rem the new file.
if defined NOSEND (
  call :from_file
) else (
  call "%ROOT%\tools\Send-Payload.cmd" %PS5% "%ROOT%\dist\dev\%ELF%"
  if errorlevel 1 call :manual_load
)

echo.
call :say 4/6 Wait until the payload is ready
set "N=0"
:wait_ready
set /a N+=1
del /q "%STATUS%" 2>nul
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
set "FOLDER="
findstr /b /c:"%TAG% OK" "%STATUS%" >nul 2>&1
if not errorlevel 1 (
  for /f "usebackq tokens=1,* delims==" %%A in ("%STATUS%") do (
    if "%%A"=="%KEY%" set "FOLDER=%%B"
  )
)
if defined FOLDER if not "%FOLDER%"=="%OLD_FOLDER%" goto :ready
if %N% geq %MAXN% (
  rem "refused" means the payload has ended, a timeout that it hangs.
  echo Last try in detail:
  curl.exe -sS --connect-timeout 3 --max-time 5 -o nul "%CTL%/"
  call :fail "The payload does not answer on port 4445."
  exit /b 1
)
ping -n 3 127.0.0.1 >nul
goto :wait_ready
:ready
echo Ready after %N% request^(s^): %FOLDER%
rem Clock check for S3: the console's UTC, the PC's local time and the Date
rem header of github.com as a reference, taken within about a second.
echo Clock comparison:
findstr /b /c:"now_utc=" "%STATUS%"
echo pc_local=%date% %time%
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
call :say 5/6 Get the password
if defined PRODUCT (
  echo Not needed: the test program pairs by itself.
  goto :have_pw
)
del /q "%STATUS%" 2>nul
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/password"
set "PROSPERO_PW="
if exist "%STATUS%" (
  for /f "usebackq tokens=1,* delims==" %%A in ("%STATUS%") do (
    if "%%A"=="password" set "PROSPERO_PW=%%B"
  )
)
if not defined PROSPERO_PW (
  if exist "%STATUS%" type "%STATUS%"
  call :fail "The payload did not return a password."
  exit /b 1
)
echo Password received.
:have_pw

echo.
call :say 6/6 Test %TEST% running, this takes %DURATION%
set "PROSPERO_AUTO=1"
call "%ROOT%\tests\spikes\%SCRIPT%" %PS5%
set "TEST_RC=%errorlevel%"
set "PROSPERO_PW="

echo.
echo ===== Clock of the PS5 =====
rem After the test on purpose: if this request fails, the results stand.
curl.exe -sS --connect-timeout 3 --max-time 5 "%CTL%/clock"
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
echo ===== Clean-up =====
call :disconnect
if defined PRODUCT (
  echo The payload keeps running.
) else (
  curl.exe -s --connect-timeout 3 --max-time 5 "%CTL%/quit"
)
call :restore
echo.
echo End: %date% %time%
echo RUN-TEST: %TEST% finished, exit code %TEST_RC%
exit /b %TEST_RC%


rem Without an ELF loader on 9021 the owner starts the payload by hand, for
rem example with a payload manager in the browser. The script opens the
rem folder with the file and the usual manager address, then waits longer.
:manual_load
echo No ELF loader on port 9021, the payload must be started by hand.
>con echo.
>con echo No ELF loader on port 9021. Please start it by hand now:
>con echo   %ROOT%\dist\dev\%ELF%
>con echo for example: upload and start it in the payload manager at
>con echo http://%PS5%:8084 . The folder with the file and the browser
>con echo are opened for you. The script waits for up to 10 minutes.
>con echo.
start "" explorer.exe /select,"%ROOT%\dist\dev\%ELF%"
start "" "http://%PS5%:8084"
rem A closed port fails at once, so a round takes about 2 s.
set "MAXN=300"
goto :eof

rem The server is started from its file in the Payload Manager (P33). Any
rem server that runs now will do: the test starts its own program with the
rem drive and asks for the new file then. The script writes nothing on the
rem console.
:from_file
echo Not sent: the server is started from its file on the PS5.
curl.exe -s --connect-timeout 3 --max-time 5 -o nul "%CTL%/"
if not errorlevel 1 (
  echo A server runs; the test asks for the new file later.
  goto :eof
)
>con echo.
>con echo No server answers. Please start it from the dashboard of the Payload
>con echo Manager now, whichever version is registered. If none is registered
>con echo yet: copy %ROOT%\dist\dev\%ELF%
>con echo by FTP to /data/prospero-smbd/ on the PS5, register it under "Manage
>con echo Payloads" and start it. The script waits for up to 10 minutes.
>con echo.
rem A closed port fails at once, so a round takes about 2 s.
set "MAXN=300"
goto :eof

rem :restore: starts the installed program again if step 1 ended it, also
rem after an abort.
:restore
if not defined WAS_RUNNING goto :eof
set "WAS_RUNNING="
call :installed
if defined INSTALLED_EXE (
  echo Starting the installed program again: !INSTALLED_EXE!
  start "" "!INSTALLED_EXE!" >nul 2>&1 <nul
) else (
  echo ProsperoAutoDrive.exe was running before the test and is not installed; start it again yourself.
  >con echo   ProsperoAutoDrive.exe was ended for the test; start it again yourself.
)
goto :eof

rem :installed: INSTALLED_EXE, the installed ProsperoAutoDrive.exe, if any.
:installed
set "INSTALLED_EXE="
for %%K in (HKCU HKLM) do if not defined INSTALLED_EXE (
  for /f "tokens=2,*" %%A in ('reg query "%%K\Software\Microsoft\Windows\CurrentVersion\Uninstall\ProsperoAutoDrive" /v InstallLocation 2^>nul ^| findstr /i "InstallLocation"') do set "INSTALLED_EXE=%%B\ProsperoAutoDrive.exe"
)
if defined INSTALLED_EXE if not exist "!INSTALLED_EXE!" set "INSTALLED_EXE="
goto :eof

rem Progress for the console window; the log gets the same line.
:say
echo ===== %* =====
>con echo %*
goto :eof

:fail
echo.
echo ERROR: %~1
>con echo ERROR: %~1
echo RUN-TEST: %TEST% ABORTED
call :restore
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
