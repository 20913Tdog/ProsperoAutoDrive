@echo off
rem Spike S2 check from Windows: connects the built-in SMB client to the
rem read-only test share of the S2 payload, copies everything, verifies the
rem SHA-256 values and that nothing can be written. Windows built-ins only
rem (net, robocopy, certutil, curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Aufruf: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"
set "SHARE=\\%PS5%\s2"
set "EXPECTED=%~dp0s2-expected.sha256"
set "OUT=%TEMP%\ProsperoS2-%RANDOM%%RANDOM%"
set "STATUS=%TEMP%\prospero-s2-status.txt"
set "FAIL=0"

echo ===== 0. Payload-Status =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "http://%PS5%:4445/"
findstr /b /c:"PROSPERO-S2 OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo FEHLER: Das S2-Payload antwortet nicht auf Port 4445. Laeuft es noch?
  exit /b 1
)
type "%STATUS%"
echo.
set /p "PW=Passwort aus der PS5-Benachrichtigung: "

echo.
echo ===== 1. Verbinden =====
net use "%SHARE%" "%PW%" /user:prospero /persistent:no
if errorlevel 1 (
  echo FEHLER: Verbindung fehlgeschlagen. Fehlertext oben bitte mitschicken.
  set "FAIL=1"
  goto :report
)
net use "%SHARE%"

echo.
echo ===== 2. Ordnerinhalt =====
dir "%SHARE%"

echo.
echo ===== 3. Alles kopieren =====
robocopy "%SHARE%" "%OUT%" /E /R:0 /W:0 /NP /NFL /NDL /NJH
if errorlevel 8 (
  echo FEHLER: robocopy meldet Fehler.
  set "FAIL=1"
)

echo.
echo ===== 4. Pruefsummen =====
for /f "usebackq tokens=1,*" %%H in ("%EXPECTED%") do call :check %%H "%%I"
set "COUNT=0"
for /f %%N in ('dir /s /b /a-d "%OUT%" 2^>nul ^| find /c /v ""') do set "COUNT=%%N"
if "%COUNT%"=="1507" (
  echo OK      1507 Dateien kopiert
) else (
  echo FALSCH  %COUNT% statt 1507 Dateien kopiert
  set "FAIL=1"
)

echo.
echo ===== 5. Gross-/Kleinschreibung =====
set "LINE="
for /f "usebackq delims=" %%L in ("%SHARE%\mixedcase.txt") do set "LINE=%%L"
if "%LINE%"=="Gross und klein" (
  echo OK      mixedcase.txt findet MixedCase.txt
) else (
  echo FALSCH  mixedcase.txt nicht lesbar
  set "FAIL=1"
)

echo.
echo ===== 6. Schreibschutz =====
(echo test> "%SHARE%\neu.txt") 2>nul
if exist "%SHARE%\neu.txt" (
  echo FALSCH  neu.txt konnte angelegt werden
  set "FAIL=1"
) else (
  echo OK      Datei anlegen abgelehnt
)
mkdir "%SHARE%\neu" 2>nul
if exist "%SHARE%\neu\" (
  echo FALSCH  Ordner neu konnte angelegt werden
  set "FAIL=1"
) else (
  echo OK      Ordner anlegen abgelehnt
)
del /q "%SHARE%\hello.txt" >nul 2>&1
ren "%SHARE%\MixedCase.txt" umbenannt.txt >nul 2>&1
if exist "%SHARE%\hello.txt" if exist "%SHARE%\MixedCase.txt" (
  echo OK      Loeschen und Umbenennen abgelehnt
  goto :explorer
)
echo FALSCH  Loeschen oder Umbenennen war moeglich
set "FAIL=1"

:explorer
echo.
echo ===== 7. Explorer =====
echo Jetzt %SHARE% im Explorer oeffnen und wie in tests\spikes\README.md
echo beschrieben testen. Danach hier eine Taste druecken.
pause >nul

:report
echo.
echo ===== 8. Verbindungsdetails =====
powershell.exe -NoProfile -Command "Get-SmbConnection -ServerName '%PS5%' -ErrorAction Stop | Format-List ShareName,Dialect,Signed,Encrypted" 2>nul
if errorlevel 1 echo ^(Get-SmbConnection nicht verfuegbar, z. B. ohne Administratorrechte^)

echo.
echo ===== 9. Payload-Status danach =====
curl.exe -s --connect-timeout 3 --max-time 5 "http://%PS5%:4445/"

net use "%SHARE%" /delete /y >nul 2>&1
if exist "%OUT%" rmdir /s /q "%OUT%"

echo.
if "%FAIL%"=="0" (
  echo S2-CHECK: ALLES OK
) else (
  echo S2-CHECK: FEHLER, Ausgabe bitte komplett zurueckmelden
)
exit /b %FAIL%

:check
set "FILE=%~2"
set "FILE=!FILE:/=\!"
if not exist "%OUT%\!FILE!" (
  echo FEHLT   !FILE!
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
  echo FALSCH  !FILE!
  set "FAIL=1"
)
goto :eof
