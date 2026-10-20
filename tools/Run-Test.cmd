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
set "DEFAULT_TEST=S3"

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
  echo FEHLER: git wurde nicht gefunden.
  pause
  exit /b 1
)
set "BRANCH="
for /f "delims=" %%B in ('git -C "%ROOT%" rev-parse --abbrev-ref HEAD') do set "BRANCH=%%B"
if /i not "%BRANCH%"=="dev" (
  echo FEHLER: Der Ordner %ROOT% steht auf "%BRANCH%" statt auf dev.
  echo Bitte einmal "git checkout dev" ausfuehren und neu starten.
  pause
  exit /b 1
)
echo Hole den neuesten Stand von dev ...
git -C "%ROOT%" pull --ff-only
if errorlevel 1 (
  echo FEHLER: git pull ist fehlgeschlagen, Meldung siehe oben.
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
  set /p "PS5=IP-Adresse der PS5: "
  if not defined PS5 exit /b 2
  > "%INI%" echo ip=!PS5!
)

set "ELF="
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
if not defined ELF (
  echo FEHLER: Unbekannter Test "%TEST%". Bekannt sind S2, S2b und S3.
  exit /b 2
)

set "CTL=http://%PS5%:4445"
set "STATUS=%TEMP%\prospero-run-test-status.txt"
set "LOG=%ROOT%\logs\%TEST%.log"
if not exist "%ROOT%\logs" mkdir "%ROOT%\logs"

echo.
echo Test %TEST% mit der PS5 %PS5%. Explorer-Fenster zur PS5 bitte schliessen.
echo Die Ausgabe erscheint am Ende gesammelt.
echo.
call :steps > "%LOG%" 2>&1
set "RC=%errorlevel%"
echo.
type "%LOG%"
clip < "%LOG%"
echo.
echo ================================================================
echo Die komplette Ausgabe steht in der Zwischenablage und in
echo %LOG%
echo Bitte im Chat einfach mit Strg+V einfuegen.
echo ================================================================
exit /b %RC%


:steps
echo ===== Testlauf %TEST%, PS5 %PS5% =====
echo Start: %date% %time%
ver
git -C "%ROOT%" log -1 --oneline

echo.
call :say 1/6 Build holen und pruefen
call "%ROOT%\tools\Get-DevBuild.cmd"
if errorlevel 1 (
  call :fail "Der Build ist nicht abrufbar oder fehlerhaft."
  exit /b 1
)
set "BUILD_COMMIT="
for /f "usebackq tokens=1,* delims==" %%A in ("%ROOT%\dist\dev\BUILD-INFO.txt") do (
  if "%%A"=="commit" set "BUILD_COMMIT=%%B"
)
set "HEAD_COMMIT="
for /f "delims=" %%C in ('git -C "%ROOT%" rev-parse HEAD') do set "HEAD_COMMIT=%%C"
if /i not "%BUILD_COMMIT%"=="%HEAD_COMMIT%" (
  call :fail "Der neueste gruene Build ist %BUILD_COMMIT:~0,7%, dein Stand ist %HEAD_COMMIT:~0,7%. Die CI laeuft vermutlich noch. Bitte in ein paar Minuten noch einmal starten."
  exit /b 1
)

echo.
call :say 2/6 Alte Verbindungen trennen und altes Payload beenden
call :disconnect
set "OLD_FOLDER="
del /q "%STATUS%" 2>nul
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
if exist "%STATUS%" (
  for /f "usebackq tokens=1,* delims==" %%A in ("%STATUS%") do (
    if "%%A"=="folder" set "OLD_FOLDER=%%B"
  )
)
curl.exe -s --connect-timeout 3 --max-time 5 "%CTL%/quit"
set "N=0"
:wait_gone
set /a N+=1
curl.exe -s --connect-timeout 2 --max-time 3 -o nul "%CTL%/"
if errorlevel 1 goto :gone
if %N% geq 10 (
  call :fail "Das alte Payload beendet sich nicht."
  exit /b 1
)
ping -n 2 127.0.0.1 >nul
goto :wait_gone
:gone
echo Port 4445 ist frei.

echo.
call :say 3/6 Payload senden
set "MAXN=36"
call "%ROOT%\tools\Send-Payload.cmd" %PS5% "%ROOT%\dist\dev\%ELF%"
if errorlevel 1 call :manual_load

echo.
call :say 4/6 Warten, bis das Payload bereit ist
set "N=0"
:wait_ready
set /a N+=1
del /q "%STATUS%" 2>nul
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
set "FOLDER="
findstr /b /c:"%TAG% OK" "%STATUS%" >nul 2>&1
if not errorlevel 1 (
  for /f "usebackq tokens=1,* delims==" %%A in ("%STATUS%") do (
    if "%%A"=="folder" set "FOLDER=%%B"
  )
)
if defined FOLDER if not "%FOLDER%"=="%OLD_FOLDER%" goto :ready
if %N% geq %MAXN% (
  rem "refused" means the payload has ended, a timeout that it hangs.
  echo Letzter Versuch im Detail:
  curl.exe -sS --connect-timeout 3 --max-time 5 -o nul "%CTL%/"
  call :fail "Das Payload meldet sich nicht auf Port 4445."
  exit /b 1
)
ping -n 3 127.0.0.1 >nul
goto :wait_ready
:ready
echo Bereit nach %N% Abfrage^(n^): %FOLDER%
rem Clock check for S3: the console's UTC, the PC's local time and the Date
rem header of github.com as a reference, taken within about a second.
echo Uhrvergleich:
findstr /b /c:"now_utc=" "%STATUS%"
echo pc_local=%date% %time%
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
call :say 5/6 Passwort holen
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
  call :fail "Das Payload hat kein Passwort geliefert."
  exit /b 1
)
echo Passwort erhalten.

echo.
call :say 6/6 Test %TEST% laeuft, das dauert einige Minuten
set "PROSPERO_AUTO=1"
call "%ROOT%\tests\spikes\%SCRIPT%" %PS5%
set "TEST_RC=%errorlevel%"
set "PROSPERO_PW="

echo.
echo ===== Uhr der PS5 =====
rem After the test on purpose: if this request fails, the results stand.
curl.exe -sS --connect-timeout 3 --max-time 5 "%CTL%/clock"
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
echo ===== Aufraeumen =====
call :disconnect
curl.exe -s --connect-timeout 3 --max-time 5 "%CTL%/quit"
echo.
echo Ende: %date% %time%
echo RUN-TEST: %TEST% beendet, Exitcode %TEST_RC%
exit /b %TEST_RC%


rem Without an ELF loader on 9021 the owner starts the payload by hand, for
rem example with a payload manager in the browser. The script opens the
rem folder with the file and the usual manager address, then waits longer.
:manual_load
echo Kein ELF-Loader auf Port 9021, das Payload muss von Hand gestartet werden.
>con echo.
>con echo Kein ELF-Loader auf Port 9021. Bitte jetzt von Hand starten:
>con echo   %ROOT%\dist\dev\%ELF%
>con echo zum Beispiel ueber den Payload-Manager http://%PS5%:8084
>con echo hochladen und starten. Der Ordner mit der Datei und der Browser
>con echo werden geoeffnet. Das Skript wartet bis zu 10 Minuten.
>con echo.
start "" explorer.exe /select,"%ROOT%\dist\dev\%ELF%"
start "" "http://%PS5%:8084"
rem A closed port fails at once, so a round takes about 2 s.
set "MAXN=300"
goto :eof

rem Progress for the console window; the log gets the same line.
:say
echo ===== %* =====
>con echo %*
goto :eof

:fail
echo.
echo FEHLER: %~1
>con echo FEHLER: %~1
echo RUN-TEST: %TEST% ABGEBROCHEN
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
