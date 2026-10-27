@echo off
REM ------------------------------------------------------------------
REM Build.bat - build POC_CANopen_Master_Zephyr for FRDM-MCXN236
REM
REM Usage (from any folder):
REM   Build.bat            incremental build
REM   Build.bat clean      pristine (full clean) build
REM ------------------------------------------------------------------
setlocal

set "WEST=C:\Users\Amrith M S\.mcuxpressotools\.mcux-venv-3.12\Scripts\west.exe"
set "ZEPHYR_BASE=C:\Amrith\zephyr\zephyr"
set "BOARD=frdm_mcxn236"

REM Project folder = folder this .bat file is in
set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build"

if not exist "%WEST%" (
    echo ERROR: west not found at "%WEST%"
    exit /b 1
)
if not exist "%ZEPHYR_BASE%\VERSION" (
    echo ERROR: Zephyr not found at "%ZEPHYR_BASE%"
    exit /b 1
)

set "PRISTINE=auto"
if /i "%~1"=="clean" set "PRISTINE=always"

echo Building %PROJECT_DIR% for %BOARD% (pristine=%PRISTINE%)
"%WEST%" build -b %BOARD% -s "%PROJECT_DIR%." -d "%BUILD_DIR%" -p %PRISTINE%
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo BUILD OK: %BUILD_DIR%\zephyr\zephyr.elf
endlocal