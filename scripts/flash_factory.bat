@echo off
REM ============================================================
REM  Flash the factory binary to an ESP32
REM  Usage: flash_factory.bat [COM_PORT]
REM  Example: flash_factory.bat COM3
REM ============================================================

set SCRIPT_DIR=%~dp0
set BINARY=%SCRIPT_DIR%\factory_modulo_wifi.bin
set ESPTOOL=%SCRIPT_DIR%esptool\esptool.exe
set BAUD=921600

REM Use provided COM port or default to COM3
if "%~1"=="" (
    set PORT=COM3
) else (
    set PORT=%~1
)

echo.
echo ========================================
echo  Factory Flash Tool
echo  Port: %PORT%
echo  Baud: %BAUD%
echo  Binary: %BINARY%
echo ========================================
echo.

REM Check that esptool.exe exists locally
if not exist "%ESPTOOL%" (
    echo ERROR: esptool.exe not found at %ESPTOOL%
    echo        Download the standalone esptool from:
    echo        https://github.com/espressif/esptool/releases
    echo        Extract it into: %SCRIPT_DIR%esptool\
    goto :fail
)

REM Check that the merged binary exists
if not exist "%BINARY%" (
    echo ERROR: factory_modulo_wifi.bin not found.
    goto :fail
)

REM Flash the device
"%ESPTOOL%" --chip esp32 --port %PORT% --baud %BAUD% ^
    write-flash --flash-mode dio --flash-size 4MB --flash-freq 40m ^
    0x0 "%BINARY%"

if %ERRORLEVEL% neq 0 goto :fail

echo.
echo ========================================
echo  ===== PASS - Flash successful =====
echo ========================================
echo.
goto :end

:fail
echo.
echo ========================================
echo  ===== FAIL - See errors above =====
echo ========================================
echo.
pause
exit /b 1

:end
pause
exit /b 0
