@echo off
rem Builds Screen+ (Release) with MSVC + CMake + Ninja.
rem Usage: build.cmd [Release|Debug] [version]   e.g. build.cmd Release 1.2.0
rem The installer (build\ScreenPlus-Setup-<version>.exe) is built when NSIS 3 is installed.
rem Prerequisites: Visual Studio 2022 (Build Tools) with the C++ workload and Windows SDK,
rem and the FFmpeg build from scripts\build-ffmpeg.sh (third_party\ffmpeg).
setlocal
set "ROOT=%~dp0"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
set "VSDIR="
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%i"
if not defined VSDIR for %%e in (BuildTools Community Professional Enterprise) do (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\%%e"
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\%%e"
)
if not defined VSDIR (
    echo Visual Studio C++ build tools were not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%third_party\ffmpeg\include\libavcodec\avcodec.h" (
    echo FFmpeg is missing. Build it first from an MSYS2 UCRT64 shell: scripts/build-ffmpeg.sh
    exit /b 1
)
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"
set "VERARG="
if not "%~2"=="" set "VERARG=-DSCREENPLUS_VERSION=%~2"
cmake -S "%ROOT%." -B "%ROOT%build" -G Ninja -DCMAKE_BUILD_TYPE=%CFG% %VERARG% || exit /b 1
cmake --build "%ROOT%build" || exit /b 1
echo.
echo Built %ROOT%build\ScreenPlus.exe
for %%f in ("%ROOT%build\ScreenPlus-Setup-*.exe") do echo Built %%~ff
