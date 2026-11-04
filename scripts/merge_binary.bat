@echo off
REM ============================================================
REM  Merge all ESP32 flash binaries into a single factory image
REM  Run this after "idf.py build" from the project root
REM ============================================================

set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR%..
set BUILD_DIR=%PROJECT_DIR%\build
set OUTPUT=%BUILD_DIR%\factory_modulo_wifi.bin
set ESPTOOL=%SCRIPT_DIR%esptool\esptool.exe

echo.
echo ========================================
echo  Merging factory binary...
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

REM Check that build files exist
if not exist "%BUILD_DIR%\bootloader\bootloader.bin" (
    echo ERROR: bootloader.bin not found. Run "idf.py build" first.
    goto :fail
)
if not exist "%BUILD_DIR%\partition_table\partition-table.bin" (
    echo ERROR: partition-table.bin not found. Run "idf.py build" first.
    goto :fail
)
if not exist "%BUILD_DIR%\ota_data_initial.bin" (
    echo ERROR: ota_data_initial.bin not found. Run "idf.py build" first.
    goto :fail
)
if not exist "%BUILD_DIR%\modulo_wifi.bin" (
    echo ERROR: modulo_wifi.bin not found. Run "idf.py build" first.
    goto :fail
)

REM Merge into a single binary
"%ESPTOOL%" --chip esp32 merge-bin -o "%OUTPUT%" ^
    --flash-mode dio --flash-size 4MB --flash-freq 40m ^
    0x1000  "%BUILD_DIR%\bootloader\bootloader.bin" ^
    0x8000  "%BUILD_DIR%\partition_table\partition-table.bin" ^
    0xe000  "%BUILD_DIR%\ota_data_initial.bin" ^
    0x10000 "%BUILD_DIR%\modulo_wifi.bin"

if %ERRORLEVEL% neq 0 goto :fail

echo.
echo ========================================
echo  SUCCESS: %OUTPUT%
echo ========================================
echo.
goto :end

:fail
echo.
echo ========================================
echo  FAILED - see errors above
echo ========================================
echo.
pause
exit /b 1

:end
pause
exit /b 0