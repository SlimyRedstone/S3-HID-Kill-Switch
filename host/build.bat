@echo off
REM Build the C client on Windows. libusb comes from vcpkg, so the port must be
REM installed for the triplet used below:
REM     vcpkg install libusb:x64-mingw-dynamic
REM
REM Ubuntu counterpart: build.sh
setlocal EnableExtensions

REM Work from this script's directory so it can be called from anywhere.
cd /d "%~dp0"

set "IN_CMD=%~1"
set "EXE=build\KillSwitch.exe"

if "%IN_CMD%"==""        goto :default
if /i "%IN_CMD%"=="run"   goto :run
if /i "%IN_CMD%"=="clear" goto :clear
if /i "%IN_CMD%"=="clean" goto :clean
if /i "%IN_CMD%"=="build" goto :buildonly
if /i "%IN_CMD%"=="help"  goto :usage
if /i "%IN_CMD%"=="-h"    goto :usage
if /i "%IN_CMD%"=="--help" goto :usage
if "%IN_CMD%"=="/?"       goto :usage

echo Unknown option: %IN_CMD%
echo.
goto :usage


REM Compile without launching, which is what install.bat wants.
:buildonly
echo.
if not exist "build\CMakeCache.txt" (
    call :do_setup
    if errorlevel 1 exit /b 1
)
call :do_compile
if errorlevel 1 exit /b 1
exit /b 0


REM Configure only when there is no cache, then build and run.
:default
echo.
if not exist "build\CMakeCache.txt" (
    call :do_setup
    if errorlevel 1 exit /b 1
)
call :do_compile
if errorlevel 1 exit /b 1
goto :run


:clear
cls
call :do_compile
if errorlevel 1 exit /b 1
goto :run


:clean
echo.
call :do_setup
if errorlevel 1 exit /b 1
echo Configured. Run build.bat to compile.
exit /b 0


:run
if not exist "%EXE%" (
    echo Failed to compile !
    exit /b 1
)
color 07
rem The executable is now a GUI application, and cmd does not wait for one
rem of those. start /wait keeps "build.bat" behaving as it did before.
start /wait "" "%EXE%"
exit /b %errorlevel%


:usage
echo Usage: build.bat [command]
echo.
echo   (none)   configure if needed, compile, then run
echo   clear    clear the console, compile, then run
echo   clean    wipe the build directory and reconfigure
echo   build    compile without running
echo   run      run without recompiling
echo   help     show this text
exit /b 0


:do_setup
if exist "build" (
    echo Removing old build directory
    rmdir /s /q "build"
)
echo Configuring CMake...
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:/ProgramData/mingw64/mingw64/bin/gcc.exe -DCMAKE_TOOLCHAIN_FILE=%USERPROFILE%/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic
if errorlevel 1 (
    echo CMake configuration failed !
    exit /b 1
)
exit /b 0


:do_compile
echo Compiling KillSwitch.exe...
cmake --build build
if errorlevel 1 (
    echo Compilation failed !
    exit /b 1
)
echo Compiling done !
echo.
exit /b 0
