@echo off
rem Spike S4: is SMB fast enough? Reads and writes a 256 MiB file over SMB
rem and, as the reference, over ftpsrv on the console, in alternating order
rem (ABBA, so that a drifting WLAN hits both alike). Per direction the median
rem of all rounds counts; gate G1 needs SMB >= 70 % of FTP. Then the same
rem with server profile c (SMB 2.1, HMAC-SHA256 instead of AES-CMAC), the
rem console's CPU time per MiB, and how long a small file takes to read
rem while a large copy runs (the server handles requests one at a time).
rem Writes only schreiben-smb.bin and schreiben-ftp.bin in the new test
rem folder, overwriting them each round. After a failed transfer it stops
rem measuring and only reads the status: no retry.
rem   S4-Check.cmd <PS5-IP> [FTP port] [rounds]
rem Windows built-ins only (net, copy, certutil, fc, sort, curl.exe).
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" (
  echo Usage: %~nx0 ^<PS5-IP^> [FTP port] [rounds]
  exit /b 2
)
set "PS5=%~1"
set "FTPPORT=%~2"
if not defined FTPPORT set "FTPPORT=2121"
set "ROUNDS=%~3"
if not defined ROUNDS set "ROUNDS=7"
set "C_ROUNDS=3"
set "SHARE=\\%PS5%\s4"
set "CTL=http://%PS5%:4445"
set "BIG=pattern-256MiB.bin"
set "BYTES=268435456"
set "BIG_SHA=3cb78104e219bba5bcb358a737572d2b3be5bebfe38b96f60973d2033f0526f7"
set "OUT=%TEMP%\ProsperoS4-%RANDOM%%RANDOM%"
set "RES=%OUT%\werte"
set "STATUS=%TEMP%\prospero-s4-status.txt"
set "SORT=%SystemRoot%\System32\sort.exe"
set "FAIL=0"
set "STOP=0"
set "N_smb_lesen=SMB read      "
set "N_ftp_lesen=FTP read      "
set "N_smb_schreiben=SMB write     "
set "N_ftp_schreiben=FTP write     "
set "N_c_lesen=SMB2.1 read   "
set "N_c_schreiben=SMB2.1 write  "

echo ===== 0. Payload status =====
curl.exe -s --connect-timeout 3 --max-time 5 -o "%STATUS%" "%CTL%/"
findstr /b /c:"PROSPERO-S4 OK" "%STATUS%" >nul 2>&1
if errorlevel 1 (
  echo ERROR: the S4 payload does not answer on port 4445. Is it still running?
  exit /b 1
)
type "%STATUS%"
set "FOLDER="
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"folder=" "%STATUS%"') do set "FOLDER=%%B"
echo.
rem tools\Run-Test.cmd passes the password in PROSPERO_PW.
if defined PROSPERO_PW (
  set "PW=%PROSPERO_PW%"
) else (
  set /p "PW=Password from the PS5 notification: "
)
mkdir "%OUT%" "%RES%"
echo Start: %date% %time%
echo File %BIG% ^(%BYTES% bytes^), %ROUNDS% rounds per direction, FTP port %FTPPORT%

echo.
echo ===== 1. FTP reference reachable? =====
curl.exe -sS --connect-timeout 5 --max-time 30 -o "%OUT%\hello.txt" "ftp://%PS5%:%FTPPORT%%FOLDER%/hello.txt"
if errorlevel 1 (
  echo ERROR: ftpsrv does not answer on port %FTPPORT%. Without the reference G1 cannot be judged.
  set "FAIL=1"
  goto :report
)
echo OK      ftpsrv serves files from the test folder

echo.
echo ===== 2. Connect and fetch the file once =====
call :connect || goto :report
call :now T0
copy /y /b "%SHARE%\%BIG%" "%OUT%\quelle.bin" >nul
if errorlevel 1 (
  echo ERROR: the 256 MiB file could not be read over SMB.
  set "FAIL=1"
  goto :report
)
call :now T1
call :elapsed T0 T1 WARM
call :sha "%OUT%\quelle.bin"
if /i "!GOT!"=="%BIG_SHA%" (
  echo OK      SHA-256 of the file matches
) else (
  echo FAIL    SHA-256 of the file: !GOT!
  set "FAIL=1"
  goto :report
)
set /a "EST=WARM*(4*ROUNDS+2*C_ROUNDS+2)/6000+1"
echo First run: !WARM! cs. Estimated duration: about !EST! minutes.
>con echo   S4 is measuring now, about !EST! minutes. Please leave the PC and the PS5 alone.

echo.
echo ===== 3. Read a small file, without and with load =====
call :small IDLE
echo Without load:            !IDLE! ms per file ^(20x hello.txt^)
> "%OUT%\last.cmd" (
  echo @copy /y /b "%SHARE%\%BIG%" "%OUT%\last.bin" ^>nul
  echo @echo done^> "%OUT%\last.done"
)
rem cmd /c, because start alone keeps a batch file's cmd open (/K).
start "" /b cmd /c ""%OUT%\last.cmd""
ping -n 4 127.0.0.1 >nul
call :small BUSY
echo During a copy:           !BUSY! ms per file
set "N=0"
:wait_load
if exist "%OUT%\last.done" goto :load_done
set /a N+=1
if %N% geq 600 (
  echo FAIL    the background copy does not end. The measurement stops.
  set "FAIL=1"
  goto :report
)
ping -n 2 127.0.0.1 >nul
goto :wait_load
:load_done
set "TF=%OUT%\last.bin"
call :size_of
if not "!SIZE!"=="%BYTES%" (
  echo FAIL    the background copy is incomplete ^(!SIZE!^).
  set "FAIL=1"
  goto :report
)
del /q "%OUT%\last.bin" 2>nul

echo.
echo ===== 4. %ROUNDS% rounds with profile b ^(SMB 3.1.1, AES-CMAC^) =====
for /l %%R in (1,1,%ROUNDS%) do call :round %%R
if "%STOP%"=="1" goto :evaluate

echo.
echo ===== 5. %C_ROUNDS% rounds with profile c ^(SMB 2.1, HMAC-SHA256^) =====
call :disconnect
curl.exe -s --max-time 5 "%CTL%/profile/c"
ping -n 3 127.0.0.1 >nul
call :connect || goto :evaluate
for /l %%R in (1,1,%C_ROUNDS%) do (
  call :xfer c_lesen %%R
  call :xfer c_schreiben %%R
)

:evaluate
echo.
echo ===== 6. Evaluation ^(median^) =====
for %%K in (smb_lesen ftp_lesen smb_schreiben ftp_schreiben c_lesen c_schreiben) do (
  call :median %%K
  call :median cpu_%%K
)
set "G1=1"
call :verdict lesen Read
call :verdict schreiben Write
call :mbs "!MED_c_lesen!" CL
call :mbs "!MED_c_schreiben!" CW
echo SMB 2.1 with HMAC-SHA256: read !CL! MB/s, write !CW! MB/s ^(!CNT_c_lesen! and !CNT_c_schreiben! runs^)
echo CPU time of the PS5 per MiB in ms, measured with getrusage:
for %%K in (smb_lesen smb_schreiben c_lesen c_schreiben) do (
  set "M=!MED_cpu_%%K!"
  if defined M (
    set /a "W=M*10/256, WI=W/10, WF=W%%10"
    echo   !N_%%K!  !WI!.!WF!
  )
)
echo Small file read: without load !IDLE! ms, during a large copy !BUSY! ms

:report
echo.
echo ===== Payload status after =====
curl.exe -s --connect-timeout 3 --max-time 5 "%CTL%/"

call :disconnect
if exist "%OUT%" rmdir /s /q "%OUT%"

echo.
echo End: %date% %time%
if not "%FAIL%"=="0" (
  echo S4-CHECK: FAILED, please send back the complete output
  exit /b 1
)
if "%G1%"=="1" (
  echo S4-CHECK: G1 PASSED ^(read !PCT_lesen! %%, write !PCT_schreiben! %% of ftpsrv^)
  exit /b 0
)
echo S4-CHECK: G1 NOT REACHED ^(read !PCT_lesen! %%, write !PCT_schreiben! %% of ftpsrv, goal 70 %%^)
exit /b 1


rem :round <n>: odd rounds start with SMB, even ones with FTP.
:round
set /a "ODD=%1 %% 2"
if "%ODD%"=="1" (
  call :xfer smb_lesen %1
  call :xfer ftp_lesen %1
  call :xfer ftp_schreiben %1
  call :xfer smb_schreiben %1
) else (
  call :xfer ftp_lesen %1
  call :xfer smb_lesen %1
  call :xfer smb_schreiben %1
  call :xfer ftp_schreiben %1
)
goto :eof

rem :xfer <kind> <round>: one timed transfer. The result goes into
rem %RES%\<kind>.txt in kB/s, the console's CPU time into cpu_<kind>.txt.
:xfer
if "%STOP%"=="1" goto :eof
curl.exe -s --max-time 5 "%CTL%/reset" >nul
call :now T0
call :do_%1
set "XRC=!errorlevel!"
call :now T1
call :elapsed T0 T1 CS
call :size_%1
if not "!XRC!"=="0" set "SIZE=exit code !XRC!"
if not "!SIZE!"=="%BYTES%" (
  echo FAIL    round %2 !N_%1!: failed ^(!SIZE!^). The measurement ends here, without a retry.
  set "FAIL=1"
  set "STOP=1"
  goto :eof
)
set /a "KBS=BYTES/CS*100/1000, SI=CS/100, SF=CS%%100"
if !SF! lss 10 set "SF=0!SF!"
>>"%RES%\%1.txt" echo !KBS!
call :mbs !KBS! TXT
set "CPUTXT="
if not "%1"=="ftp_lesen" if not "%1"=="ftp_schreiben" (
  call :cpu
  >>"%RES%\cpu_%1.txt" echo !CPU!
  set "CPUTXT=  CPU !CPU! ms"
)
echo round %2  !N_%1!  !SI!.!SF! s  !TXT! MB/s!CPUTXT!
rem Round 1 also compares the bytes; outside the timed part.
if "%2"=="1" if "%1"=="smb_lesen" call :same "%OUT%\smb-lesen.bin"
if "%2"=="1" if "%1"=="ftp_lesen" call :same "%OUT%\ftp-lesen.bin"
goto :eof

:do_smb_lesen
copy /y /b "%SHARE%\%BIG%" "%OUT%\smb-lesen.bin" >nul
goto :eof
:do_ftp_lesen
curl.exe -sS --connect-timeout 5 --max-time 1800 -o "%OUT%\ftp-lesen.bin" "ftp://%PS5%:%FTPPORT%%FOLDER%/%BIG%"
goto :eof
:do_smb_schreiben
copy /y /b "%OUT%\quelle.bin" "%SHARE%\schreiben-smb.bin" >nul
goto :eof
:do_ftp_schreiben
curl.exe -sS --connect-timeout 5 --max-time 1800 -T "%OUT%\quelle.bin" "ftp://%PS5%:%FTPPORT%%FOLDER%/schreiben-ftp.bin"
goto :eof
:do_c_lesen
goto :do_smb_lesen
:do_c_schreiben
goto :do_smb_schreiben

rem :size_<kind> sets SIZE to the size of what the transfer produced. The
rem FTP upload is checked over SMB, as both reach the same folder.
:size_smb_lesen
:size_c_lesen
set "TF=%OUT%\smb-lesen.bin"
goto :size_of
:size_ftp_lesen
set "TF=%OUT%\ftp-lesen.bin"
goto :size_of
:size_smb_schreiben
:size_c_schreiben
set "TF=%SHARE%\schreiben-smb.bin"
goto :size_of
:size_ftp_schreiben
set "TF=%SHARE%\schreiben-ftp.bin"
:size_of
set "SIZE=missing"
if exist "%TF%" for %%F in ("%TF%") do set "SIZE=%%~zF"
goto :eof

rem :same <file>: byte comparison with the first SMB copy.
:same
fc /b "%~1" "%OUT%\quelle.bin" >nul 2>&1
if errorlevel 1 (
  echo FAIL    %~nx1 differs from the first copy
  set "FAIL=1"
  set "STOP=1"
) else (
  echo OK      %~nx1 identical byte by byte
)
goto :eof

rem :small <var>: average ms to open and read hello.txt, 20 times. Without
rem leases Windows has to ask the server each time.
:small
call :now L0
for /l %%i in (1,1,20) do type "%SHARE%\hello.txt" >nul 2>&1
call :now L1
call :elapsed L0 L1 LCS
set /a "%1=LCS*10/20"
goto :eof

rem :now <var>: centiseconds since midnight. %time% looks like 9:05:07,12 or
rem 09:05:07.12 depending on the locale; "1" prefixes avoid octal numbers.
:now
set "NOWT=%time: =0%"
for /f "tokens=1-4 delims=:.," %%a in ("%NOWT%") do set /a "%1=(((1%%a-100)*60+(1%%b-100))*60+(1%%c-100))*100+(1%%d-100)"
goto :eof

rem :elapsed <from> <to> <var>: centiseconds, across midnight, at least 1.
:elapsed
set /a "%3=!%2!-!%1!"
if !%3! lss 0 set /a "%3+=8640000"
if !%3! lss 1 set "%3=1"
goto :eof

rem :mbs <kB/s> <var>: as MB/s with two decimals; "-" without a value.
:mbs
set "V=%~1"
if not defined V (
  set "%2=-"
  goto :eof
)
set /a "MI=V/1000, MF=(V%%1000)/10"
if !MF! lss 10 set "MF=0!MF!"
set "%2=!MI!.!MF!"
goto :eof

:cpu
set "CU=0"
set "CSY=0"
curl.exe -s --max-time 5 -o "%STATUS%" "%CTL%/"
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"cpu_user_ms=" /c:"cpu_sys_ms=" "%STATUS%"') do (
  if "%%A"=="cpu_user_ms" set "CU=%%B"
  if "%%A"=="cpu_sys_ms" set "CSY=%%B"
)
set /a "CPU=CU+CSY"
goto :eof

rem :median <kind> sets MED_<kind> and CNT_<kind>; for an even count the
rem lower of the two middle values.
:median
set "MED_%1="
set "CNT_%1=0"
if not exist "%RES%\%1.txt" goto :eof
del /q "%RES%\sort.txt" 2>nul
set "CNT=0"
for /f "usebackq" %%V in ("%RES%\%1.txt") do (
  set "P=0000000000%%V"
  >>"%RES%\sort.txt" echo !P:~-10!
  set /a CNT+=1
)
set /a "MID=(CNT+1)/2"
set "I=0"
for /f %%V in ('%SORT% "%RES%\sort.txt"') do (
  set /a I+=1
  if !I! equ !MID! for /f "tokens=* delims=0" %%Z in ("%%V") do set "MED_%1=%%Z"
)
set "CNT_%1=%CNT%"
goto :eof

rem :verdict <direction> <label>: SMB median against FTP median, G1 >= 70 %.
:verdict
set "SV=!MED_smb_%1!"
set "FV=!MED_ftp_%1!"
set "PCT_%1=-"
call :mbs "!SV!" ST
call :mbs "!FV!" FT
if not defined SV set "G1=0"
if not defined FV set "G1=0"
if !CNT_smb_%1! lss %ROUNDS% set "G1=0"
if !CNT_ftp_%1! lss %ROUNDS% set "G1=0"
if defined SV if defined FV (
  set /a "PCT_%1=SV*100/FV"
  if !PCT_%1! lss 70 set "G1=0"
)
echo %2: SMB !ST! MB/s, FTP !FT! MB/s, SMB = !PCT_%1! %% of FTP ^(goal 70 %%, !CNT_smb_%1!/!CNT_ftp_%1! runs^)
goto :eof

:sha
set "GOT="
for /f "skip=1 delims=" %%L in ('certutil -hashfile "%~1" SHA256') do (
  if not defined GOT set "GOT=%%L"
)
set "GOT=!GOT: =!"
goto :eof

:connect
net use "%SHARE%" "%PW%" /user:prospero /persistent:no >nul
if errorlevel 1 (
  echo ERROR: connecting failed. Existing connections:
  net use
  set "FAIL=1"
  exit /b 1
)
exit /b 0

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
