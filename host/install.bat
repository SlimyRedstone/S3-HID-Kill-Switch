@echo off
REM Installs KillSwitch for the current user.
REM
REM   install.bat              prerequisites, build, install
REM   install.bat --uninstall  remove the installed copy
REM
REM Everything lands in %APPDATA%\KillSwitch, which needs no administrator
REM rights to install. The build itself still happens in this directory.
REM
REM Note: KillSwitch *runs* elevated -- resources\app.manifest requests
REM administrator, so launching the installed copy raises a UAC prompt. That is
REM required to enable and disable HID device nodes; the install does not need
REM it, only the run does.

setlocal EnableExtensions
cd /d "%~dp0"

set "TARGET=%APPDATA%\KillSwitch"
set "VCPKG=%USERPROFILE%\vcpkg"
set "TRIPLET=x64-mingw-dynamic"
set "EXE=build\KillSwitch.exe"
set "STARTMENU=%APPDATA%\Microsoft\Windows\Start Menu\Programs"

if /i "%~1"=="--uninstall" goto :uninstall
if /i "%~1"=="-h"          goto :usage
if /i "%~1"=="--help"      goto :usage
if not "%~1"==""           goto :badopt


echo === 1/3  Prerequisites ===
if not exist "%VCPKG%\vcpkg.exe" (
    echo vcpkg was not found at %VCPKG%.
    echo Install it once with:
    echo     git clone https://github.com/microsoft/vcpkg "%VCPKG%"
    echo     "%VCPKG%\bootstrap-vcpkg.bat"
    exit /b 1
)

REM Already-installed ports are skipped by vcpkg itself, so this is cheap to
REM repeat. cJSON and Clay are vendored in libs\ and need nothing here.
"%VCPKG%\vcpkg.exe" install libusb:%TRIPLET% raylib:%TRIPLET%
if errorlevel 1 (
    echo vcpkg could not install the prerequisites.
    exit /b 1
)

echo.
echo === 2/3  Build ===
call build.bat clean
if errorlevel 1 exit /b 1
call build.bat build
if errorlevel 1 exit /b 1

if not exist "%EXE%" (
    echo Build did not produce %EXE%.
    exit /b 1
)

echo.
echo === 3/3  Install into %TARGET% ===
if not exist "%TARGET%"           mkdir "%TARGET%"
if not exist "%TARGET%\resources" mkdir "%TARGET%\resources"

copy /y "%EXE%" "%TARGET%\KillSwitch.exe" >nul
if errorlevel 1 (
    echo Could not copy the executable. Is KillSwitch still running?
    exit /b 1
)

REM raylib and libusb are dynamic, so they travel with the executable.
copy /y "build\*.dll" "%TARGET%" >nul 2>&1
copy /y "resources\*.ttf" "%TARGET%\resources" >nul
copy /y "resources\icon.png" "%TARGET%\resources" >nul
copy /y "resources\icon.ico" "%TARGET%\resources" >nul

REM The configuration is the user's; a reinstall must not discard it.
if not exist "%TARGET%\config.json" (
    if exist "config.json" (
        copy /y "config.json" "%TARGET%\config.json" >nul
    )
)

REM A shortcut is the only convenient way to reach an %APPDATA% install, and
REM it gives the Start menu the embedded icon. The shortcut inherits the exe's
REM manifest, so starting it from here still prompts for elevation.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$s = (New-Object -ComObject WScript.Shell).CreateShortcut('%STARTMENU%\KillSwitch.lnk');" ^
  "$s.TargetPath = '%TARGET%\KillSwitch.exe';" ^
  "$s.WorkingDirectory = '%TARGET%';" ^
  "$s.Description = 'NFC kill switch: a card toggles blocking of selected keyboard/mouse';" ^
  "$s.Save()" >nul 2>&1

echo.
echo Installed:
echo   %TARGET%\KillSwitch.exe
echo   %TARGET%\resources\
echo   %STARTMENU%\KillSwitch.lnk
echo.
echo config.json lives in %TARGET% and controls the debug console and
echo the allow_virtual / card / device settings.
exit /b 0


:uninstall
echo Removing %TARGET%
if exist "%TARGET%\KillSwitch.exe" del /q "%TARGET%\KillSwitch.exe"
if exist "%TARGET%\resources"      rmdir /s /q "%TARGET%\resources"
del /q "%TARGET%\*.dll" >nul 2>&1
if exist "%STARTMENU%\KillSwitch.lnk" del /q "%STARTMENU%\KillSwitch.lnk"
echo Kept %TARGET%\config.json
exit /b 0


:badopt
echo Unknown option: %~1
echo.
:usage
echo Usage: install.bat [--uninstall]
echo.
echo   (none)       install prerequisites, build, then install to %%APPDATA%%\KillSwitch
echo   --uninstall  remove the installed copy, keeping config.json
exit /b 0
