@echo off
REM ------------------------------------------------------------------
REM Flash.bat - flash this project's build to a FRDM-MCXN236
REM
REM Usage (from any folder):
REM   Flash.bat                  flash the board set in PROBE below
REM                              (or the only connected board if PROBE is empty)
REM   Flash.bat list             list connected MCU-Link probes (serial numbers)
REM   Flash.bat <probe-serial>   flash the board with this probe serial
REM
REM Build first with Build.bat. Put this file next to Build.bat in the
REM project root (TRM and master each get their own copy).
REM ------------------------------------------------------------------
setlocal

set "WEST=C:\Users\Amrith M S\.mcuxpressotools\.mcux-venv-3.12\Scripts\west.exe"
set "ZEPHYR_BASE=C:\Amrith\zephyr\zephyr"

REM LinkServer install folder (contains LinkServer.exe)
set "LINKSERVER_DIR=C:\nxp\LinkServer_26.9.130"
set "LINKSERVER_EXE=%LINKSERVER_DIR%\LinkServer.exe"
set "PATH=%LINKSERVER_DIR%;%PATH%"

REM Probe serial of the board this project belongs to (from "Flash.bat list").
REM Fill in once per project copy, then plain "Flash.bat" always flashes the right board.
set "PROBE=RKWZNOQ2RCKPB"

REM Project folder = folder this .bat file is in
set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%build"

if not exist "%WEST%" (
    echo ERROR: west not found at "%WEST%"
    exit /b 1
)
if not exist "%LINKSERVER_EXE%" (
    echo ERROR: LinkServer not found at "%LINKSERVER_EXE%"
    exit /b 1
)

REM --- list probes -------------------------------------------------
if /i "%~1"=="list" (
    "%LINKSERVER_EXE%" probes
    exit /b 0
)

if not exist "%BUILD_DIR%\zephyr\zephyr.elf" (
    echo ERROR: no build found in "%BUILD_DIR%" - run Build.bat first
    exit /b 1
)

REM --- flash -------------------------------------------------------
REM a serial given on the command line overrides the one set above
if not "%~1"=="" set "PROBE=%~1"

if "%PROBE%"=="" (
    echo Flashing %BUILD_DIR% ^(single connected board^)
    "%WEST%" flash -d "%BUILD_DIR%" -r linkserver --linkserver "%LINKSERVER_EXE%"
) else (
    echo Flashing %BUILD_DIR% to probe %PROBE%
    "%WEST%" flash -d "%BUILD_DIR%" -r linkserver --linkserver "%LINKSERVER_EXE%" --probe %PROBE%
)

if errorlevel 1 (
    echo.
    echo FLASH FAILED
    exit /b 1
)

echo.
echo FLASH OK
endlocal