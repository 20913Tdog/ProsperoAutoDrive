@echo off
rem Spike S1 check from Windows: what answers on TCP 445 and 4445 of the PS5?
rem Run once BEFORE starting the probe payload (expect: nothing on 445)
rem and once AFTER (expect: "PROSPERO-S1 OK" on both ports).
setlocal EnableExtensions

if "%~1"=="" (
  echo Aufruf: %~nx0 ^<PS5-IP^>
  exit /b 2
)
set "PS5=%~1"

for %%P in (445 4445) do call :probe %%P
exit /b 0

:probe
set "PORT=%1"
set "BODY=%TEMP%\prospero-s1-%PORT%.txt"
curl.exe -s --connect-timeout 3 --max-time 5 -o "%BODY%" "http://%PS5%:%PORT%/"
set "RC=%errorlevel%"

echo.
echo ===== Port %PORT% =====
if "%RC%"=="0" (
  findstr /b /c:"PROSPERO-S1 OK" "%BODY%" >nul
  if errorlevel 1 (
    echo Ein FREMDER Dienst antwortet auf Port %PORT%.
  ) else (
    type "%BODY%"
  )
  goto :eof
)
if "%RC%"=="7" (
  echo Nichts lauscht auf Port %PORT% ^(Verbindung abgelehnt^).
  goto :eof
)
if "%RC%"=="28" (
  echo Keine Antwort auf Port %PORT% ^(Timeout^): PS5 nicht erreichbar oder Port gefiltert.
  goto :eof
)
if "%RC%"=="52" (
  echo Ein FREMDER Dienst nimmt Verbindungen an, antwortet aber nicht per HTTP.
  goto :eof
)
if "%RC%"=="56" (
  echo Ein FREMDER Dienst hat die Verbindung sofort getrennt.
  goto :eof
)
echo Unerwartetes Ergebnis ^(curl-Code %RC%^).
goto :eof
