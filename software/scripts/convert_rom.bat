@echo off
setlocal
if "%~1"=="" (
    echo Usage: %~nx0 path\to\rom.gb
    exit /b 1
)
set "ROM=%~1"
py software\scripts\gb_rom_to_header.py "%ROM%"
endlocal
