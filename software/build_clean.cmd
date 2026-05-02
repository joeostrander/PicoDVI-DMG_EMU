@echo off
setlocal EnableDelayedExpansion


REM Resolution modes:
REM 0 = 640x480, horizontally scaled x4, vertically x3 --> 640x480 Full screen
REM 1 = 800x600, horizontally scaled x4, vertically x4 --> 640x576 window
REM 2 = 640x480, horizontally scaled x2, vertically x2 --> 320x288 window


set "PLATFORM=rp2350"
set "BOARD="
set "RESOLUTION_MODE=2"
set "CONTROLLER_MODE=bluetooth"
set "PICO_COPY_TO_RAM=0"
set "DRIVE_LETTER="
set "BOARD_OVERRIDE="

if /i "%~1"=="/h" goto :usage
if /i "%~1"=="-h" goto :usage
if /i "%~1"=="/?" goto :usage

if not "%~1"=="" set "PLATFORM=%~1"
if not "%~2"=="" set "RESOLUTION_MODE=%~2"
if not "%~3"=="" set "CONTROLLER_MODE=%~3"
if not "%~4"=="" set "PICO_COPY_TO_RAM=%~4"
if not "%~5"=="" set "DRIVE_LETTER=%~5"
if not "%~6"=="" set "BOARD_OVERRIDE=%~6"

if /i "%PLATFORM%"=="rp2040" goto :platform_ok
if /i "%PLATFORM%"=="rp2350" goto :platform_ok
    echo Invalid platform: %PLATFORM%
    goto :usage

:platform_ok

if "%RESOLUTION_MODE%"=="0" goto :resolution_ok
if "%RESOLUTION_MODE%"=="1" goto :resolution_ok
if "%RESOLUTION_MODE%"=="2" goto :resolution_ok
    echo Invalid resolution mode: %RESOLUTION_MODE%
    goto :usage

:resolution_ok

if "%PICO_COPY_TO_RAM%"=="0" goto :copy_to_ram_ok
if "%PICO_COPY_TO_RAM%"=="1" goto :copy_to_ram_ok
    echo Invalid copy-to-RAM value: %PICO_COPY_TO_RAM%
    goto :usage

:copy_to_ram_ok

set "USE_BLUETOOTH_CONTROLLER=0"
set "USE_NES_CLASSIC_CONTROLLER=0"
set "CONTROLLER_SUFFIX=nes"

if /i "%CONTROLLER_MODE%"=="bluetooth" goto :controller_bluetooth
if /i "%CONTROLLER_MODE%"=="wii" goto :controller_wii
if /i "%CONTROLLER_MODE%"=="classic" goto :controller_wii
if /i "%CONTROLLER_MODE%"=="nes" goto :controller_nes
if /i "%CONTROLLER_MODE%"=="shift" goto :controller_nes

echo Invalid controller mode: %CONTROLLER_MODE%
goto :usage

:controller_bluetooth
set "USE_BLUETOOTH_CONTROLLER=1"
set "USE_NES_CLASSIC_CONTROLLER=0"
set "CONTROLLER_SUFFIX=bt"
goto :controller_done

:controller_wii
set "USE_BLUETOOTH_CONTROLLER=0"
set "USE_NES_CLASSIC_CONTROLLER=1"
set "CONTROLLER_SUFFIX=wii"
goto :controller_done

:controller_nes
set "USE_BLUETOOTH_CONTROLLER=0"
set "USE_NES_CLASSIC_CONTROLLER=0"
set "CONTROLLER_SUFFIX=nes"

:controller_done
if not "%BOARD_OVERRIDE%"=="" (
    set "BOARD=%BOARD_OVERRIDE%"
) else (
    if "%USE_BLUETOOTH_CONTROLLER%"=="1" (
        if /i "%PLATFORM%"=="rp2040" set "BOARD=pico_w"
        if /i "%PLATFORM%"=="rp2350" set "BOARD=pico2_w"
    ) else (
        if /i "%PLATFORM%"=="rp2040" set "BOARD=pico"
        if /i "%PLATFORM%"=="rp2350" set "BOARD=pico2"
    )
)

if "%USE_BLUETOOTH_CONTROLLER%"=="1" (
    if /i "%BOARD%"=="pico" (
        echo Bluetooth mode requires a wireless board. Use pico_w or a compatible custom board.
        goto :usage
    )
    if /i "%BOARD%"=="pico2" (
        echo Bluetooth mode requires a wireless board. Use pico2_w or a compatible custom board.
        goto :usage
    )
)

set "OUTPUT_NAME=dmg_emu_%PLATFORM%_res%RESOLUTION_MODE%_%CONTROLLER_SUFFIX%"

echo.
echo Configuration:
echo   PLATFORM=%PLATFORM%
echo   BOARD=%BOARD%
echo   RESOLUTION_MODE=%RESOLUTION_MODE%
echo   CONTROLLER_MODE=%CONTROLLER_MODE%
echo   USE_BLUETOOTH_CONTROLLER=%USE_BLUETOOTH_CONTROLLER%
echo   USE_NES_CLASSIC_CONTROLLER=%USE_NES_CLASSIC_CONTROLLER%
echo   PICO_COPY_TO_RAM=%PICO_COPY_TO_RAM%
if not "%DRIVE_LETTER%"=="" (
    echo   DRIVE_LETTER=%DRIVE_LETTER%
) else (
    echo   DRIVE_LETTER=auto-detect RPI-RP2
)
if not "%BOARD_OVERRIDE%"=="" echo   BOARD_OVERRIDE=%BOARD_OVERRIDE%

cd /d "%~dp0"
rmdir /s /q build 2>nul
mkdir build
cd build

echo.
echo ===== Running CMake Configuration =====
cmake -G "MinGW Makefiles" -DPICO_COPY_TO_RAM=%PICO_COPY_TO_RAM% -DPICO_PLATFORM=%PLATFORM% -DPICO_BOARD=%BOARD% -DRESOLUTION_MODE=%RESOLUTION_MODE% -DUSE_BLUETOOTH_CONTROLLER=%USE_BLUETOOTH_CONTROLLER% -DUSE_NES_CLASSIC_CONTROLLER=%USE_NES_CLASSIC_CONTROLLER% ..
if %errorlevel% neq 0 (
    echo.
    echo *** CMAKE CONFIGURATION FAILED ***
    exit /b %errorlevel%
)

echo.
echo ===== Building Project =====
cmd /c make -j4
if %errorlevel% neq 0 (
    echo.
    echo *** BUILD FAILED ***
    exit /b %errorlevel%
)

echo.
echo ===== Build Successful =====
echo Binary size:
dir apps\dmg_emu\dmg_emu.elf | find "dmg_emu.elf"

if "%DRIVE_LETTER%"=="" call :detect_rpi_rp2_drive

if not "%DRIVE_LETTER%"=="" if exist %DRIVE_LETTER%\ (
    echo.
    echo ===== Copying UF2 to %DRIVE_LETTER%\ =====
    copy /y apps\dmg_emu\dmg_emu.uf2 %DRIVE_LETTER%\ >nul
    if %errorlevel% neq 0 (
        echo *** COPY FAILED ***
        exit /b %errorlevel%
    )
    echo Copy successful!
) else (
    echo RPI-RP2 drive not found, skipping copy
)

copy /y apps\dmg_emu\dmg_emu.uf2 ..\%OUTPUT_NAME%.uf2 >nul

echo.
echo Output: %OUTPUT_NAME%.uf2
echo ===== ALL DONE =====
cd /d "%~dp0"
exit /b 0

:detect_rpi_rp2_drive
for /f "usebackq delims=" %%D in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$v=Get-Volume -FileSystemLabel 'RPI-RP2' -ErrorAction SilentlyContinue ^| Select-Object -First 1 -ExpandProperty DriveLetter; Write-Output $v"`) do (
    if not "%%D"=="" set "DRIVE_LETTER=%%D:"
)
exit /b 0

:usage
echo.
echo Usage: %~n0 [rp2040^|rp2350] [0^|1^|2] [nes^|wii^|bluetooth] [0^|1] [DRIVE] [BOARD]
echo.
echo   platform      : rp2040 or rp2350
echo   resolution    : 0, 1, or 2
echo   controller    : nes/shift, wii/classic, or bluetooth
echo   copy_to_ram   : 0 or 1
echo   drive         : optional UF2 drive override, e.g. E:
echo   board         : optional PICO_BOARD override, e.g. pico2 or adafruit_feather_rp2040
echo.
echo Examples:
echo   %~n0
echo   %~n0 rp2350 2 bluetooth 0
echo   %~n0 rp2350 0 wii 0 F:
echo   %~n0 rp2040 2 nes 1
echo   %~n0 rp2350 2 wii 0 "" pico2
exit /b 1
