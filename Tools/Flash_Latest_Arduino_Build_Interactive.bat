@echo off
setlocal EnableExtensions EnableDelayedExpansion
title ESP32 Surveyor - Flash Latest Arduino Build

rem ============================================================================
rem ESP32 Surveyor Flash Utility
rem
rem Features:
rem   - Finds installed ESP32 Arduino core automatically
rem   - Finds installed Espressif esptool automatically
rem   - Enumerates Windows COM ports with friendly names
rem   - Verifies the selected COM port exists and is not already in use
rem   - Finds Arduino IDE 1.8.x arduino_build_* folders
rem   - Shows sketch name, build folder, and timestamp for each build
rem   - Lets the user select the build to flash
rem   - Automatically finds application, bootloader, partitions, and boot_app0
rem   - Flashes the ESP32
rem   - Opens an interactive two-way serial terminal after flashing
rem
rem Serial terminal:
rem   Type normally and press Enter to send commands to the ESP32.
rem   Press Ctrl+] to exit the terminal.
rem ============================================================================

set "FLASH_BAUD=921600"
set "SERIAL_BAUD=115200"

set "BUILD_ROOT=%LOCALAPPDATA%\Temp"
set "ESP32_PACKAGE_ROOT=%LOCALAPPDATA%\Arduino15\packages\esp32"

echo.
echo ============================================================
echo  ESP32 Surveyor - Arduino Build Flasher
echo ============================================================
echo Flash baud:  %FLASH_BAUD%
echo Serial baud: %SERIAL_BAUD%
echo.

rem ============================================================================
rem Discover installed ESP32 Arduino core
rem ============================================================================

set "ESP32_CORE_DIR="
set "ESP32_CORE_VER="

for /f "usebackq tokens=1,* delims=|" %%A in (`powershell -NoProfile -Command ^
  "$root='%ESP32_PACKAGE_ROOT%\hardware\esp32';" ^
  "if (Test-Path -LiteralPath $root) {" ^
  "  $dirs=Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue;" ^
  "  $best=$dirs | Sort-Object { try {[version]$_.Name} catch {[version]'0.0.0'} } -Descending | Select-Object -First 1;" ^
  "  if ($best) { '{0}|{1}' -f $best.Name,$best.FullName }" ^
  "}"`) do (
    set "ESP32_CORE_VER=%%A"
    set "ESP32_CORE_DIR=%%B"
)

if not defined ESP32_CORE_DIR (
    echo ERROR: No installed ESP32 Arduino core was found under:
    echo   %ESP32_PACKAGE_ROOT%\hardware\esp32
    echo.
    echo Install "esp32 by Espressif Systems" in Arduino IDE Board Manager.
    pause
    exit /b 1
)

echo ESP32 Arduino core: %ESP32_CORE_VER%
echo   %ESP32_CORE_DIR%
echo.

rem ============================================================================
rem Discover installed esptool
rem ============================================================================

set "ESPTOOL="
set "ESPTOOL_VER="

for /f "usebackq tokens=1,* delims=|" %%A in (`powershell -NoProfile -Command ^
  "$root='%ESP32_PACKAGE_ROOT%\tools\esptool_py';" ^
  "if (Test-Path -LiteralPath $root) {" ^
  "  $dirs=Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue;" ^
  "  $best=$dirs | Sort-Object { try {[version]$_.Name} catch {[version]'0.0.0'} } -Descending | Select-Object -First 1;" ^
  "  if ($best) {" ^
  "    $exe=Join-Path $best.FullName 'esptool.exe';" ^
  "    if (Test-Path -LiteralPath $exe) { '{0}|{1}' -f $best.Name,$exe }" ^
  "  }" ^
  "}"`) do (
    set "ESPTOOL_VER=%%A"
    set "ESPTOOL=%%B"
)

if not defined ESPTOOL (
    echo ERROR: No installed esptool.exe was found under:
    echo   %ESP32_PACKAGE_ROOT%\tools\esptool_py
    echo.
    echo Reinstall or update the ESP32 Arduino core through Board Manager.
    pause
    exit /b 1
)

echo esptool: %ESPTOOL_VER%
echo   %ESPTOOL%
echo.

rem ============================================================================
rem Enumerate COM ports
rem ============================================================================

set "PORTLIST=%TEMP%\esp32_ports_%RANDOM%.txt"

powershell -NoProfile -Command ^
  "$seen=@{};" ^
  "$out=@();" ^
  "Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | ForEach-Object {" ^
  "  if ($_.DeviceID) { $seen[$_.DeviceID]=$true; $out += [pscustomobject]@{Port=$_.DeviceID;Name=$_.Name} }" ^
  "};" ^
  "[System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object | ForEach-Object {" ^
  "  if (-not $seen.ContainsKey($_)) { $out += [pscustomobject]@{Port=$_;Name='Serial Port'} }" ^
  "};" ^
  "$out | Sort-Object {[int]($_.Port -replace '\D','')} | ForEach-Object { '{0}|{1}' -f $_.Port,$_.Name }" ^
  > "%PORTLIST%"

set /a PORTCOUNT=0
for /f "usebackq tokens=1,* delims=|" %%A in ("%PORTLIST%") do (
    set /a PORTCOUNT+=1
    set "PORT_!PORTCOUNT!=%%A"
    set "PORTNAME_!PORTCOUNT!=%%B"
)
del "%PORTLIST%" >nul 2>&1

if %PORTCOUNT% EQU 0 (
    echo ERROR: No Windows COM ports were detected.
    echo.
    echo Connect the ESP32 and confirm that Windows Device Manager shows a
    echo serial/COM device, then run this utility again.
    pause
    exit /b 1
)

:SELECT_PORT
echo Detected COM ports:
echo.
for /L %%N in (1,1,%PORTCOUNT%) do (
    echo   %%N^) !PORT_%%N! - !PORTNAME_%%N!
)
echo.

if %PORTCOUNT% EQU 1 (
    echo Only one COM port was detected; selecting it automatically.
    set "PORTCHOICE=1"
) else (
    set /p "PORTCHOICE=Select COM port [1-%PORTCOUNT%]: "
)

set "PORT="
for /L %%N in (1,1,%PORTCOUNT%) do (
    if "%PORTCHOICE%"=="%%N" set "PORT=!PORT_%%N!"
)

if not defined PORT (
    echo.
    echo ERROR: Invalid COM port selection.
    echo.
    goto SELECT_PORT
)

echo.
echo Selected port: %PORT%
echo.

rem ============================================================================
rem Preflight COM-port availability
rem ============================================================================

:CHECK_PORT
powershell -NoProfile -Command ^
  "$p=$null;" ^
  "try {" ^
  "  $p=New-Object System.IO.Ports.SerialPort '%PORT%',%SERIAL_BAUD%,'None',8,'One';" ^
  "  $p.DtrEnable=$false; $p.RtsEnable=$false;" ^
  "  $p.Open();" ^
  "  $p.Close();" ^
  "  exit 0" ^
  "} catch {" ^
  "  Write-Host $_.Exception.Message;" ^
  "  exit 1" ^
  "} finally {" ^
  "  if ($p) { if ($p.IsOpen) {$p.Close()}; $p.Dispose() }" ^
  "}"

if errorlevel 1 (
    echo.
    echo ============================================================
    echo  COM PORT NOT AVAILABLE
    echo ============================================================
    echo.
    echo %PORT% exists, but Windows could not open it.
    echo.
    echo Common causes:
    echo   - Arduino Serial Monitor is still open
    echo   - Another terminal/program is using the port
    echo   - The USB cable/device was disconnected
    echo   - The USB serial driver stopped responding
    echo.
    choice /C RSE /N /M "[R]etry this port, [S]elect another port, or [E]xit? "
    if errorlevel 3 exit /b 1
    if errorlevel 2 (
        echo.
        goto SELECT_PORT
    )
    echo.
    goto CHECK_PORT
)

echo Port availability check: PASS
echo.

rem ============================================================================
rem Find Arduino build folders and identify actual sketch names
rem
rem Build-list format:
rem   FullPath|SketchName|Timestamp
rem ============================================================================

set "BUILDLIST=%TEMP%\esp32_arduino_builds_%RANDOM%.txt"

powershell -NoProfile -Command ^
  "$root='%BUILD_ROOT%';" ^
  "$dirs=Get-ChildItem -LiteralPath $root -Directory -Filter 'arduino_build_*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending;" ^
  "foreach ($d in $dirs) {" ^
  "  $app=Get-ChildItem -LiteralPath $d.FullName -File -Filter '*.ino.bin' -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch '\.(bootloader|partitions)\.bin$' } | Select-Object -First 1;" ^
  "  if ($app) {" ^
  "    $sketch=$app.Name -replace '\.ino\.bin$','';" ^
  "    '{0}|{1}|{2}' -f $d.FullName,$sketch,$d.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')" ^
  "  }" ^
  "}" ^
  > "%BUILDLIST%"

set /a COUNT=0
for /f "usebackq tokens=1,2,3 delims=|" %%A in ("%BUILDLIST%") do (
    set /a COUNT+=1
    set "BUILD_!COUNT!=%%A"
    set "SKETCH_!COUNT!=%%B"
    set "BUILDTIME_!COUNT!=%%C"
)
del "%BUILDLIST%" >nul 2>&1

if %COUNT% EQU 0 (
    echo ERROR: No usable Arduino build folders were found under:
    echo   %BUILD_ROOT%
    echo.
    echo Compile the sketch in Arduino IDE first, then run this utility again.
    pause
    exit /b 1
)

echo Available Arduino builds, newest first:
echo.
for /L %%N in (1,1,%COUNT%) do (
    for %%D in ("!BUILD_%%N!") do (
        echo   %%N^) !SKETCH_%%N!
        echo      Built:  !BUILDTIME_%%N!
        echo      Folder: %%~nxD
        echo.
    )
)

if %COUNT% EQU 1 (
    echo Only one usable build was found; selecting it automatically.
    set "CHOICE=1"
) else (
    set /p "CHOICE=Select build [1-%COUNT%, Enter=1]: "
    if not defined CHOICE set "CHOICE=1"
)

set "SELECTED="
set "SELECTED_SKETCH="
for /L %%N in (1,1,%COUNT%) do (
    if "%CHOICE%"=="%%N" (
        set "SELECTED=!BUILD_%%N!"
        set "SELECTED_SKETCH=!SKETCH_%%N!"
    )
)

if not defined SELECTED (
    echo ERROR: Invalid build selection.
    pause
    exit /b 1
)

echo.
echo Selected sketch:
echo   %SELECTED_SKETCH%
echo Selected build:
echo   %SELECTED%
echo.

rem ============================================================================
rem Locate binary artifacts
rem ============================================================================

set "APPBIN="
for /f "delims=" %%F in ('dir /b /a-d "%SELECTED%\*.ino.bin" 2^>nul') do (
    echo %%F | findstr /i /v "\.bootloader\.bin \.partitions\.bin" >nul
    if not errorlevel 1 if not defined APPBIN set "APPBIN=%SELECTED%\%%F"
)

set "BOOTLOADER="
for /f "delims=" %%F in ('dir /b /a-d "%SELECTED%\*.ino.bootloader.bin" 2^>nul') do (
    if not defined BOOTLOADER set "BOOTLOADER=%SELECTED%\%%F"
)

set "PARTITIONS="
for /f "delims=" %%F in ('dir /b /a-d "%SELECTED%\*.ino.partitions.bin" 2^>nul') do (
    if not defined PARTITIONS set "PARTITIONS=%SELECTED%\%%F"
)

set "BOOTAPP="
for /r "%ESP32_CORE_DIR%" %%F in (boot_app0.bin) do (
    if not defined BOOTAPP set "BOOTAPP=%%F"
)

if not defined APPBIN (
    echo ERROR: No application *.ino.bin image found.
    pause
    exit /b 1
)
if not defined BOOTLOADER (
    echo ERROR: No *.ino.bootloader.bin found.
    pause
    exit /b 1
)
if not defined PARTITIONS (
    echo ERROR: No *.ino.partitions.bin found.
    pause
    exit /b 1
)
if not defined BOOTAPP (
    echo ERROR: boot_app0.bin not found under:
    echo   %ESP32_CORE_DIR%
    pause
    exit /b 1
)

echo Application:
echo   %APPBIN%
echo Bootloader:
echo   %BOOTLOADER%
echo Partitions:
echo   %PARTITIONS%
echo boot_app0:
echo   %BOOTAPP%
echo.
echo Port:
echo   %PORT%
echo.
echo Toolchain:
echo   ESP32 Arduino core %ESP32_CORE_VER%
echo   esptool %ESPTOOL_VER%
echo.
echo If connection fails, hold BOOT while the flasher says "Connecting..."
echo and release BOOT once writing begins.
echo.
pause

rem Re-check immediately before flashing
echo Checking %PORT% again before flashing...
powershell -NoProfile -Command ^
  "$p=$null;" ^
  "try {" ^
  "  $p=New-Object System.IO.Ports.SerialPort '%PORT%',%SERIAL_BAUD%,'None',8,'One';" ^
  "  $p.DtrEnable=$false; $p.RtsEnable=$false;" ^
  "  $p.Open(); $p.Close(); exit 0" ^
  "} catch { Write-Host $_.Exception.Message; exit 1 }" ^
  "finally { if ($p) { if ($p.IsOpen) {$p.Close()}; $p.Dispose() } }"

if errorlevel 1 (
    echo.
    echo ERROR: %PORT% became unavailable before flashing.
    echo Close any program using the port and run the script again.
    pause
    exit /b 1
)

rem ============================================================================
rem Flash
rem ============================================================================

echo.
echo Flashing %SELECTED_SKETCH% to %PORT%...
echo.

"%ESPTOOL%" --chip esp32 --port %PORT% --baud %FLASH_BAUD% ^
  --before default-reset --after hard-reset write-flash -z ^
  --flash-mode keep --flash-freq keep --flash-size keep ^
  0x1000 "%BOOTLOADER%" ^
  0x8000 "%PARTITIONS%" ^
  0xe000 "%BOOTAPP%" ^
  0x10000 "%APPBIN%"

if errorlevel 1 (
    echo.
    echo ============================================================
    echo  FLASH FAILED
    echo ============================================================
    echo.
    echo Verify the USB connection and try again.
    echo If necessary, hold BOOT during the initial connection.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo  FLASH COMPLETE
echo ============================================================
echo.
echo Sketch: %SELECTED_SKETCH%
echo Port:   %PORT%
echo.
echo Waiting for the ESP32 to reset...
timeout /t 2 /nobreak >nul

rem ============================================================================
rem Interactive serial terminal
rem ============================================================================

set "PSSCRIPT=%TEMP%\esp32_serial_terminal_%RANDOM%.ps1"

> "%PSSCRIPT%" echo param([string]$PortName,[int]$BaudRate)
>>"%PSSCRIPT%" echo $ErrorActionPreference = 'Stop'
>>"%PSSCRIPT%" echo $port = New-Object System.IO.Ports.SerialPort $PortName,$BaudRate,'None',8,'One'
>>"%PSSCRIPT%" echo $port.NewLine = "`r`n"
>>"%PSSCRIPT%" echo $port.ReadTimeout = 25
>>"%PSSCRIPT%" echo $port.WriteTimeout = 1000
>>"%PSSCRIPT%" echo $port.DtrEnable = $false
>>"%PSSCRIPT%" echo $port.RtsEnable = $false
>>"%PSSCRIPT%" echo try {
>>"%PSSCRIPT%" echo ^    $port.Open()
>>"%PSSCRIPT%" echo ^    Write-Host ""
>>"%PSSCRIPT%" echo ^    Write-Host "============================================================"
>>"%PSSCRIPT%" echo ^    Write-Host " Interactive Serial Terminal - $PortName @ $BaudRate baud"
>>"%PSSCRIPT%" echo ^    Write-Host " Type commands normally; Enter sends CR/LF."
>>"%PSSCRIPT%" echo ^    Write-Host " Press Ctrl+] to exit."
>>"%PSSCRIPT%" echo ^    Write-Host "============================================================"
>>"%PSSCRIPT%" echo ^    Write-Host ""
>>"%PSSCRIPT%" echo ^    $line = New-Object System.Text.StringBuilder
>>"%PSSCRIPT%" echo ^    while ($true) {
>>"%PSSCRIPT%" echo ^        if ($port.BytesToRead -gt 0) {
>>"%PSSCRIPT%" echo ^            $incoming = $port.ReadExisting()
>>"%PSSCRIPT%" echo ^            if ($incoming.Length -gt 0) { [Console]::Write($incoming) }
>>"%PSSCRIPT%" echo ^        }
>>"%PSSCRIPT%" echo ^        while ([Console]::KeyAvailable) {
>>"%PSSCRIPT%" echo ^            $key = [Console]::ReadKey($true)
>>"%PSSCRIPT%" echo ^            $c = [int][char]$key.KeyChar
>>"%PSSCRIPT%" echo ^            if ($c -eq 29) { throw [System.OperationCanceledException]::new('User exit') }
>>"%PSSCRIPT%" echo ^            elseif ($key.Key -eq [ConsoleKey]::Enter) {
>>"%PSSCRIPT%" echo ^                $text = $line.ToString()
>>"%PSSCRIPT%" echo ^                $port.Write($text + "`r`n")
>>"%PSSCRIPT%" echo ^                [Console]::WriteLine()
>>"%PSSCRIPT%" echo ^                $null = $line.Clear()
>>"%PSSCRIPT%" echo ^            }
>>"%PSSCRIPT%" echo ^            elseif ($key.Key -eq [ConsoleKey]::Backspace) {
>>"%PSSCRIPT%" echo ^                if ($line.Length -gt 0) {
>>"%PSSCRIPT%" echo ^                    $line.Length = $line.Length - 1
>>"%PSSCRIPT%" echo ^                    [Console]::Write("`b `b")
>>"%PSSCRIPT%" echo ^                }
>>"%PSSCRIPT%" echo ^            }
>>"%PSSCRIPT%" echo ^            elseif (-not [char]::IsControl($key.KeyChar)) {
>>"%PSSCRIPT%" echo ^                $null = $line.Append($key.KeyChar)
>>"%PSSCRIPT%" echo ^                [Console]::Write($key.KeyChar)
>>"%PSSCRIPT%" echo ^            }
>>"%PSSCRIPT%" echo ^        }
>>"%PSSCRIPT%" echo ^        Start-Sleep -Milliseconds 10
>>"%PSSCRIPT%" echo ^    }
>>"%PSSCRIPT%" echo }
>>"%PSSCRIPT%" echo catch [System.OperationCanceledException] {
>>"%PSSCRIPT%" echo ^    Write-Host ""
>>"%PSSCRIPT%" echo ^    Write-Host "Serial terminal closed."
>>"%PSSCRIPT%" echo }
>>"%PSSCRIPT%" echo catch {
>>"%PSSCRIPT%" echo ^    Write-Host ""
>>"%PSSCRIPT%" echo ^    Write-Host "Serial terminal error: $($_.Exception.Message)" -ForegroundColor Red
>>"%PSSCRIPT%" echo }
>>"%PSSCRIPT%" echo finally {
>>"%PSSCRIPT%" echo ^    if ($port -and $port.IsOpen) { $port.Close() }
>>"%PSSCRIPT%" echo ^    if ($port) { $port.Dispose() }
>>"%PSSCRIPT%" echo }

powershell -NoProfile -ExecutionPolicy Bypass -File "%PSSCRIPT%" -PortName "%PORT%" -BaudRate %SERIAL_BAUD%
set "SERIAL_RC=%ERRORLEVEL%"
del "%PSSCRIPT%" >nul 2>&1

echo.
echo Done.
exit /b %SERIAL_RC%
