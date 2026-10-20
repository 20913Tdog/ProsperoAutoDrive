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
  echo Aufruf: %~nx0 ^<PS5-IP^> [FTP-Port]
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

echo ===== 0. Payload-Status =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-S3 OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo FEHLER: Das S3-Payload antwortet nicht auf Port 4445. Laeuft es noch?
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
  set /p "PW=Passwort aus der PS5-Benachrichtigung: "
)
mkdir "%OUT%"

echo.
echo ===== 1. Verbinden =====
call :disconnect
net use "%SHARE%" "%PW%" /user:prospero /persistent:no
if errorlevel 1 (
  echo FEHLER: Verbindung fehlgeschlagen. Fehlertext oben bitte mitschicken.
  echo Bestehende Verbindungen:
  net use
  set "FAIL=1"
  goto :report
)

echo.
echo ===== 2. Testdaten herunterladen =====
robocopy "%SHARE%" "%OUT%\vorlage" /E /R:0 /W:0 /NP /NFL /NDL /NJH | findstr /c:"Files :" /c:"Times :" /c:"Speed :"
call :count "%OUT%\vorlage" vorlage

echo.
echo ===== 3. Hochladen in den neuen Ordner hochgeladen =====
curl.exe -s --max-time 5 "%CTL%/reset" >nul
robocopy "%OUT%\vorlage" "%SHARE%\hochgeladen" /E /R:0 /W:0 /NP /NFL /NDL /NJH > "%OUT%\up.txt"
set "RC=%errorlevel%"
findstr /c:"Files :" /c:"Times :" /c:"Speed :" "%OUT%\up.txt"
if %RC% geq 8 (
  echo FALSCH  robocopy meldet Fehler beim Hochladen ^(Exitcode %RC%^):
  type "%OUT%\up.txt"
  set "FAIL=1"
) else (
  echo OK      robocopy ohne Fehler
)
call :server

echo.
echo ===== 4. Groessen und Aenderungszeiten =====
rem A dry run lists every file that differs in size or write time. The
rem console keeps times to the microsecond, Windows to 100 ns, so the check
rem uses robocopy's 2-second tolerance (/FFT); the exact count is shown too.
set "DIFF=0"
for /f %%N in ('robocopy "%OUT%\vorlage" "%SHARE%\hochgeladen" /E /L /FFT /R:0 /W:0 /NP /NJH /NJS /NDL /NC /NS /FP ^| findstr /r /c:"[^ ]" ^| find /c /v ""') do set "DIFF=%%N"
if "%DIFF%"=="0" (
  echo OK      alle Dateien mit gleicher Groesse und gleicher Aenderungszeit ^(auf 2 s^)
) else (
  echo FALSCH  %DIFF% Dateien weichen in Groesse oder Aenderungszeit ab, zum Beispiel:
  robocopy "%OUT%\vorlage" "%SHARE%\hochgeladen" /E /L /FFT /R:0 /W:0 /NP /NJH /NJS /NDL /TS /FP | findstr /r /c:"[^ ]" | findstr /n "^" | findstr /b /r "[1-5]:"
  set "FAIL=1"
)
set "EXACT=0"
for /f %%N in ('robocopy "%OUT%\vorlage" "%SHARE%\hochgeladen" /E /L /R:0 /W:0 /NP /NJH /NJS /NDL /NC /NS /FP ^| findstr /r /c:"[^ ]" ^| find /c /v ""') do set "EXACT=%%N"
echo INFO    ohne Toleranz weichen %EXACT% von 1507 Dateien ab ^(erwartet: Rundung auf Mikrosekunden^)

echo.
echo ===== 5. Wieder herunterladen und Byte fuer Byte vergleichen =====
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
    if !BAD! leq 5 echo FALSCH  !R!
  )
)
if "%BAD%"=="0" (
  echo OK      alle Dateien identisch
) else (
  echo FALSCH  %BAD% Dateien unterscheiden sich
  set "FAIL=1"
)

echo.
echo ===== 6. Dateien und Ordner bearbeiten =====
rem Each step sets V to 1 when its result is right; :verdict reports it.
set "OPS=%SHARE%\ops"
mkdir "%OPS%" 2>nul
set "V=0" & if exist "%OPS%\" set "V=1"
call :verdict "Ordner anlegen"
set "V=0" & mkdir "%SHARE%\OPS" 2>nul || set "V=1"
call :verdict "Ordner OPS neben ops abgelehnt"
(echo eins) > "%OPS%\a.txt"
call :lines "%OPS%\a.txt" 1
call :verdict "Datei anlegen"
(echo zwei) >> "%OPS%\a.txt"
call :lines "%OPS%\a.txt" 2
call :verdict "Anhaengen"
(echo drei) > "%OPS%\a.txt"
call :lines "%OPS%\a.txt" 1
call :verdict "Ueberschreiben kuerzt die Datei"
copy /y "%SHARE%\hello.txt" "%OPS%\kopie.txt" >nul
set "V=0" & fc /b "%SHARE%\hello.txt" "%OPS%\kopie.txt" >nul 2>&1 && set "V=1"
call :verdict "Kopieren innerhalb der Freigabe"
ren "%OPS%\a.txt" b.txt
set "V=0" & if exist "%OPS%\b.txt" if not exist "%OPS%\a.txt" set "V=1"
call :verdict "Umbenennen"
ren "%OPS%\b.txt" B.TXT
set "NAME="
for /f "delims=" %%N in ('dir /b "%OPS%\b.txt" 2^>nul') do set "NAME=%%N"
set "V=0" & if "!NAME!"=="B.TXT" set "V=1"
call :verdict "Gross-/Kleinschreibung aendern, Name jetzt !NAME!"
mkdir "%OPS%\unter"
move "%OPS%\B.TXT" "%OPS%\unter\" >nul
set "V=0" & if exist "%OPS%\unter\B.TXT" if not exist "%OPS%\B.TXT" set "V=1"
call :verdict "Verschieben"
ren "%OPS%\unter" unter2
set "V=0" & if exist "%OPS%\unter2\B.TXT" if not exist "%OPS%\unter\" set "V=1"
call :verdict "Ordner umbenennen"
rd "%OPS%\unter2" 2>nul
set "V=0" & if exist "%OPS%\unter2\B.TXT" set "V=1"
call :verdict "Nicht leeren Ordner loeschen abgelehnt"
del /q "%OPS%\unter2\B.TXT"
rd "%OPS%\unter2" 2>nul
set "V=0" & if not exist "%OPS%\unter2\" set "V=1"
call :verdict "Datei und leeren Ordner loeschen"
rem Recursive delete of a small tree created just before, inside ops.
mkdir "%OPS%\baum\a\b"
(echo x) > "%OPS%\baum\a\b\y.txt"
(echo x) > "%OPS%\baum\z.txt"
rd /s /q "%OPS%\baum"
set "V=0" & if not exist "%OPS%\baum\" set "V=1"
call :verdict "Ordnerbaum loeschen"

echo.
echo ===== 7. Zeitstempel neuer Dateien =====
rem Written without an explicit time, so the console's clock sets it.
(echo zeit) > "%OPS%\zeit.txt"
echo PC jetzt, Ortszeit:        %date% %time%
for %%F in ("%OPS%\zeit.txt") do echo zeit.txt laut Explorer:    %%~tF
echo zeit.txt in UTC ^(robocopy^):
robocopy "%OPS%" "%OUT%\leer" zeit.txt /L /TS /NJH /NJS /NDL /NC /NS /FP | findstr /r /c:"[^ ]"
echo Referenz UTC ^(github.com^):
curl.exe -sSI --max-time 5 https://github.com 2>&1 | findstr /b /i /c:"date:"

echo.
echo ===== 8. Schreibgeschwindigkeit, 64 MiB =====
curl.exe -s --max-time 5 "%CTL%/reset" >nul
robocopy "%OUT%\vorlage" "%SHARE%\tempo" pattern-64MiB.bin /R:0 /W:0 /NP /NJH /NFL /NDL | findstr /c:"Times :" /c:"Speed :"
call :server
echo --- WLAN-Referenz: FTP-Upload auf Port %FTPPORT% in den Testordner
curl.exe -sS --connect-timeout 3 --max-time 300 -T "%OUT%\vorlage\pattern-64MiB.bin" -w "Bytes %%{size_upload}, Sekunden %%{time_total}, Bytes/s %%{speed_upload}\n" "ftp://%PS5%:%FTPPORT%%FOLDER%/ftp-upload.bin"
if errorlevel 1 echo ^(FTP-Referenz fehlgeschlagen, siehe curl-Meldung oben^)

if defined PROSPERO_AUTO goto :report
echo.
echo ===== 9. Explorer =====
echo Jetzt %SHARE% im Explorer oeffnen und wie in tests\spikes\README.md
echo beschrieben testen. Danach hier eine Taste druecken.
pause >nul

:report
echo.
echo ===== Payload-Status danach =====
curl.exe -s --connect-timeout 3 --max-time 5 "%CTL%/"

call :disconnect
if exist "%OUT%" rmdir /s /q "%OUT%"

echo.
if "%FAIL%"=="0" (
  echo S3-CHECK: ALLES OK
) else (
  echo S3-CHECK: FEHLER, Ausgabe bitte komplett zurueckmelden
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
  echo OK      %~2: 1507 Dateien
) else (
  echo FALSCH  %~2: %COUNT% statt 1507 Dateien
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
  echo FALSCH  %~1
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
  echo FEHLT   !FILE!
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
  echo FALSCH  !FILE!
  set "FAIL=1"
)
goto :eof
