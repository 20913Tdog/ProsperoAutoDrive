@echo off
rem P33 live test (E3): prospero-smbd.elf as a process of its own, started
rem by the Payload Manager from its file in /data/prospero-smbd/ instead of
rem being sent over the network. The project owner does the steps on the
rem console by hand; the script asks for each one on this console, writes a
rem time stamp into the log, waits for the program's log line and records
rem the server's status (instance, pid, ppid, uptime_s) after it. First it
rem starts the program of the build against whichever server runs, so that
rem the drive is there for copying the new file:
rem   P33.1 the owner copies the new file onto the drive, over the old one,
rem         and starts it from the dashboard: it replaces the old server,
rem         the drive is there within 3 s
rem   P33.4 what "Active Processes" shows (asked), pid and ppid of the server
rem   P33.2 stopped in "Active Processes": drive gone within 5 s on cable,
rem         the Payload Manager keeps answering
rem   P33.3 started again: drive back within 3 s, without a new pairing
rem   P33.6 the owner copies the file once more, over the one that runs now;
rem         the later starts run the copy
rem   P33.5 last in "Autoload", PS5 restarted: the drive comes by itself
rem   P33.7 started a second time while it runs, via websend and then from
rem         the dashboard: each new instance replaces the old one
rem The script itself neither reads nor writes files on the console; the
rem copying is the owner's. Its state is in %TEMP%\prospero-p33, the
rem credentials are those of the pairing.
rem   P33-Check.cmd <PS5-IP>
rem Windows built-ins only (curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Usage: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "CTL=http://%PS5%:4445"
for %%D in ("%~dp0..\..\dist\dev") do set "DIST=%%~fD"
set "APP=%DIST%\ProsperoAutoDrive.exe"
set "STATE=%TEMP%\prospero-p33"
set "LOG=%STATE%\log.txt"
set "ST=%TEMP%\prospero-p33-status.txt"
set "SUMMARY=%TEMP%\prospero-p33-summary.txt"
if not exist "%APP%" (
  echo ERROR: %APP% is missing. Run tools\Get-DevBuild.cmd first.
  exit /b 1
)
"%APP%" --stop >nul
if exist "%STATE%" rmdir /s /q "%STATE%"
del /q "%SUMMARY%" 2>nul

echo ===== Before the test: the server that runs now =====
call :status "before the test"
if not defined S_INSTANCE (
  echo ERROR: no server answers on port 4445.
  >con echo ERROR: no server answers. Start prospero-smbd from the dashboard first.
  exit /b 1
)

rem The probe pairs this PC if it is not paired yet.
"%DIST%\prospero-probe.exe" %PS5% --no-broadcast >nul
if errorlevel 1 (
  echo ERROR: the test program could not pair, its output:
  "%DIST%\prospero-probe.exe" %PS5% --no-broadcast
  exit /b 1
)

echo.
echo ===== Program start against the running server =====
>con echo   Starting the program, the drive should appear shortly
echo %time% Program started
start "ProsperoAutoDrive" /min "%APP%" --ip %PS5% --state-dir "%STATE%"
call :wait_count " connected as " 1 10 || (
  echo ERROR: no drive after 10 s
  goto :end
)
call :letter
echo %time% The drive is !DRIVE!

echo.
echo ===== P33.1 Copy the new file onto the drive and start it from the dashboard =====
set "BEFORE=!S_INSTANCE!"
call :mark
>con echo.
>con echo   P33.1: copy %DIST%\prospero-smbd.elf
>con echo   onto the drive into !DRIVE!\data\prospero-smbd\ and replace the file
>con echo   there. The folder with the new file opens for you. If copying fails,
>con echo   copy it another way, for example by FTP.
start "" explorer.exe /select,"%DIST%\prospero-smbd.elf"
set "OK="
>con set /p "OK=  Did copying via the drive work? y or n, then Enter: " <con
echo %time% Owner: copying the new file over the old one via the drive worked: "!OK!"
>>"%SUMMARY%" echo P33.1 copying the new file over the old one via the drive worked: !OK!
>con echo.
>con echo   If "Manage Payloads" in the Payload Manager does not list it yet,
>con echo   register /data/prospero-smbd/prospero-smbd.elf there. Then start it
>con echo   from the dashboard; it replaces the server that runs now.
>con set /p "X=  Press Enter right after starting it: " <con
echo %time% Owner: started from the dashboard
call :switched "P33.1 from the dashboard" 120 || goto :end
if not defined S_PID (
  echo ERROR: the server that started has no pid= in its status, so it is an older file
  >con echo ERROR: an older prospero-smbd.elf was started. Check the copy and the
  >con echo        path under "Manage Payloads", then start the test again.
  goto :end
)
call :ask_processes "P33.1/P33.4"

echo.
echo ===== P33.2 Stop the server in "Active Processes" =====
call :mark
>con echo.
>con echo   P33.2: stop prospero-smbd in the Payload Manager under "Active
>con echo   Processes". If it is not listed there, use the tray menu of the
>con echo   program instead: "Shut down PS5...".
set "HOW="
>con set /p "HOW=  Right after stopping it: a (Active Processes) or t (tray), then Enter: " <con
echo %time% Owner: server stopped via "!HOW!" (a = Active Processes, t = tray)
>con echo   Waiting until the drive is gone (up to 30 s) ...
call :wait_count " disconnected in " !NEXT_DOWN! 30 || echo ERROR: the drive did not go within 30 s
call :status "P33.2 after stopping"
call :pldmgr

echo.
echo ===== P33.3 Start it again from the dashboard =====
call :mark
>con echo.
>con echo   P33.3: start prospero-smbd again from the dashboard.
>con set /p "X=  Press Enter right after starting it: " <con
echo %time% Owner: started again from the dashboard
>con echo   Waiting until the drive is back (up to 60 s) ...
call :wait_count " connected as " !NEXT_UP! 60 || echo ERROR: the drive did not come back within 60 s
call :status "P33.3 started again"
findstr /c:"Paired with server" "%LOG%" >nul 2>&1 && (
  echo ERROR: the program paired again
) || (
  echo %time% No new pairing: the program used the stored credentials
)

echo.
echo ===== P33.6 Copy the file over the one that runs =====
call :letter
>con echo.
>con echo   P33.6: copy %DIST%\prospero-smbd.elf
>con echo   once more onto the drive, into !DRIVE!\data\prospero-smbd\, and replace
>con echo   the file there while the server runs from it. The folder with the file
>con echo   opens for you.
start "" explorer.exe /select,"%DIST%\prospero-smbd.elf"
set "OK="
>con set /p "OK=  Did copying work? y or n, then Enter: " <con
echo %time% Owner: copying over the running file worked: "!OK!"
>>"%SUMMARY%" echo P33.6 copying over the running file worked: !OK!
call :status "P33.6 after copying, still running"

echo.
echo ===== P33.5 Autoload after a restart of the PS5 =====
call :mark
>con echo.
>con echo   P33.5: in the Payload Manager, put prospero-smbd last in "Autoload".
>con echo   Then restart the PS5 and start the jailbreak and the Payload
>con echo   Manager as usual.
set "SKIP="
>con set /p "SKIP=  Press Enter when you restart the PS5 (to skip this step: n and Enter): " <con
if /i "!SKIP!"=="n" (
  echo P33.5 skipped
  goto :p337
)
echo %time% Owner: the PS5 restarts
>con echo   Waiting until the drive is gone (up to 120 s) ...
call :wait_count " disconnected in " !NEXT_DOWN! 120 || echo ERROR: the drive did not go within 120 s
>con echo.
>con echo   Press Enter as soon as the PS5 shows "Prospero AutoDrive running".
>con set /p "X=" <con
echo %time% Owner: the PS5 shows "running"
>con echo   Waiting until the drive is back (up to 120 s) ...
call :wait_count " connected as " !NEXT_UP! 120 || (
  echo ERROR: the drive did not come back within 120 s
  >con echo   The drive did not come back. If the PS5 has a new IP address now:
  >con echo   the program does not search by itself, its tray menu has "Search
  >con echo   the network for PS5 ^(30 s^)". This test keeps using %PS5%.
)
call :status "P33.5 from Autoload"

:p337
echo.
echo ===== P33.7 Started a second time while it runs: websend, then the dashboard =====
call :status "P33.7 before"
set "BEFORE=!S_INSTANCE!"
call :mark
>con echo   P33.7: sending the payload over the network (websend) while the server runs ...
echo %time% Payload sent via websend
call "%~dp0..\..\tools\Send-Payload.cmd" %PS5% "%DIST%\prospero-smbd.elf"
if errorlevel 1 (
  echo websend skipped: no ELF loader answers on port 9021
) else (
  call :switched "P33.7 via websend" 30
  set "BEFORE=!S_INSTANCE!"
)
call :mark
>con echo.
>con echo   P33.7: start prospero-smbd from the dashboard once more, while it runs.
>con set /p "X=  Press Enter right after starting it: " <con
echo %time% Owner: started from the dashboard while it runs
call :switched "P33.7 from the dashboard while it ran" 60
call :ask_processes "P33.7"

:end
echo.
echo ===== End the program =====
"%APP%" --stop
echo.
echo ===== Status after each step =====
if exist "%SUMMARY%" type "%SUMMARY%"
echo.
echo ===== Log of the program =====
type "%LOG%"
echo.
echo P33-CHECK: COMPLETED, the times are in the log
exit /b 0

rem :fetch: the server's status in %ST%, its instance and pid in S_INSTANCE
rem and S_PID, both empty without an answer.
:fetch
set "S_PID="
set "S_INSTANCE="
del /q "%ST%" 2>nul
curl.exe -s --connect-timeout 3 --max-time 5 -o "%ST%" "%CTL%/"
if exist "%ST%" for /f "usebackq tokens=1,* delims==" %%A in ("%ST%") do (
  if "%%A"=="pid" set "S_PID=%%B"
  if "%%A"=="instance" set "S_INSTANCE=%%B"
)
goto :eof

rem :status <label>: version, instance, process and uptime of the server,
rem into the log and into the summary at the end.
:status
call :fetch
set "LINE="
if exist "%ST%" for /f "usebackq tokens=1,* delims==" %%A in ("%ST%") do (
  for %%K in (version instance pid ppid uptime_s pairing) do if "%%A"=="%%K" set "LINE=!LINE! %%A=%%B"
)
if not defined LINE set "LINE= no answer on port 4445"
echo %time% Status %~1:!LINE!
>>"%SUMMARY%" echo %~1:!LINE!
goto :eof

rem :switched <label> <seconds>: after a start while a server runs (BEFORE is
rem its instance). Waits for the new instance, then for the drive: it either
rem went and came back, or stayed because the new server took over quickly
rem enough. Exit code 1 if no new instance answers in time.
:switched
>con echo   Waiting for the new server (up to %~2 s) ...
call :wait_new %~2 || (
  echo ERROR: no new instance within %~2 s
  call :status "%~1"
  exit /b 1
)
echo %time% A new instance answers
rem The program notices the end of the old server at once; a moment for
rem its log lines.
ping -n 4 127.0.0.1 >nul
>con echo   Waiting for the drive (up to 60 s) ...
call :wait_up 60 || echo ERROR: no drive 60 s after the new server answered
call :count " connected as "
if !CNT! geq !NEXT_UP! (
  echo %time% The drive went and came back
) else (
  echo %time% The drive stayed while the server was replaced
)
call :status "%~1"
exit /b 0

rem :wait_new <seconds>: until an instance other than BEFORE answers.
:wait_new
set /a "WAIT_LEFT=%~1 / 2"
:wait_new_loop
call :fetch
if defined S_INSTANCE if not "!S_INSTANCE!"=="!BEFORE!" exit /b 0
set /a WAIT_LEFT-=1
if !WAIT_LEFT! leq 0 exit /b 1
ping -n 3 127.0.0.1 >nul
goto :wait_new_loop

rem :wait_up <seconds>: until the drive is there, that is the log has more
rem "connected as" than "disconnected in" lines.
:wait_up
set /a "WAIT_LEFT=%~1 * 4"
:wait_up_loop
call :count " connected as "
set "UP=!CNT!"
call :count " disconnected in "
if !UP! gtr !CNT! exit /b 0
set /a WAIT_LEFT-=1
if !WAIT_LEFT! leq 0 exit /b 1
rem a quarter second
ping -n 1 -w 250 192.0.2.1 >nul
goto :wait_up_loop

rem :letter: the drive letter of the last connection, from the log, in DRIVE.
:letter
set "DRIVE=P:"
if exist "%LOG%" for /f "delims=" %%L in ('findstr /c:" connected as " "%LOG%"') do (
  for %%T in (%%L) do set "DRIVE=%%T"
)
goto :eof

rem :ask_processes <step>: what the Payload Manager lists, typed by the owner.
:ask_processes
>con echo.
>con echo   %~1: open the Payload Manager, "Active Processes". Which entries do
>con echo   you see, with their PID? For example: pldmgr.elf 77, prospero-smbd 81
>con echo   (the server itself reports pid=!S_PID!)
set "SEEN="
>con set /p "SEEN=  Type what you see, then Enter: " <con
echo %time% Owner, Active Processes at %~1: !SEEN!
>>"%SUMMARY%" echo %~1 Active Processes: !SEEN!
goto :eof

rem :pldmgr: does the Payload Manager still answer on its web port?
:pldmgr
set "CODE="
for /f %%C in ('curl.exe -s -o nul --connect-timeout 3 --max-time 5 -w "%%{http_code}" "http://%PS5%:8084/"') do set "CODE=%%C"
if "!CODE!"=="000" set "CODE="
if defined CODE (
  echo %time% Payload Manager on port 8084: answers ^(HTTP !CODE!^)
) else (
  echo %time% Payload Manager on port 8084: no answer
)
>>"%SUMMARY%" echo P33.2 Payload Manager after stopping: HTTP !CODE!
goto :eof

rem :mark: counts the drive events in the program's log so far; the next
rem step waits for one more (NEXT_UP, NEXT_DOWN).
:mark
call :count " connected as "
set /a "NEXT_UP=CNT+1"
call :count " disconnected in "
set /a "NEXT_DOWN=CNT+1"
goto :eof

rem :count <text>: how often the log has the text, in CNT.
:count
set "CNT=0"
if exist "%LOG%" for /f %%C in ('findstr /c:%1 "%LOG%" ^| find /c /v ""') do set "CNT=%%C"
goto :eof

rem :wait_count <text> <n> <seconds>: until the log has the text n times.
:wait_count
set /a "WAIT_LEFT=%~3 * 4"
:wait_loop
call :count %1
if !CNT! geq %~2 exit /b 0
set /a WAIT_LEFT-=1
if !WAIT_LEFT! leq 0 exit /b 1
rem a quarter second
ping -n 1 -w 250 192.0.2.1 >nul
goto :wait_loop
