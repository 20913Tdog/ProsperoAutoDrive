@echo off
rem Sends an ELF payload to the PS5 ELF loader (elfldr, default port 9021).
rem Optional helper; any loader you already use works just as well.
setlocal EnableExtensions

if "%~2"=="" (
  echo Usage: %~nx0 ^<PS5-IP^> ^<file.elf^> [port]
  exit /b 2
)
if not exist "%~2" (
  echo ERROR: file not found: %~2
  exit /b 2
)

set "PAYLOAD_HOST=%~1"
set "PAYLOAD_FILE=%~f2"
set "PAYLOAD_PORT=%~3"
if not defined PAYLOAD_PORT set "PAYLOAD_PORT=9021"

powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$b=[IO.File]::ReadAllBytes($env:PAYLOAD_FILE);" ^
  "$c=New-Object Net.Sockets.TcpClient;" ^
  "$c.Connect($env:PAYLOAD_HOST,[int]$env:PAYLOAD_PORT);" ^
  "$s=$c.GetStream(); $s.Write($b,0,$b.Length); $s.Flush();" ^
  "$c.Client.Shutdown([Net.Sockets.SocketShutdown]::Send);" ^
  "Start-Sleep -Milliseconds 500; $c.Close();" ^
  "Write-Output ('Sent {0} bytes to {1}:{2}' -f $b.Length,$env:PAYLOAD_HOST,$env:PAYLOAD_PORT)"
if errorlevel 1 (
  echo ERROR: sending failed. Is an ELF loader running on port %PAYLOAD_PORT%?
  exit /b 1
)
exit /b 0
