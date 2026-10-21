@echo off
REM Builds Frame Studio with MSVC (any Visual Studio with the C++ workload). Output: <build dir>\Frame Studio.exe
REM Usage: build_windows.bat [x64 ^| x86 ^| arm64] [build dir]
REM   x64 (default): 64-bit PCs. x86: 32-bit Windows. arm64: Windows on ARM (Snapdragon laptops, Surface Pro X...).
REM   Build dir default: %LOCALAPPDATA%\FrameStudio\build-<arch> (local disk, fast).
REM Building for x86 or arm64 needs Visual Studio's matching C++ tools ("MSVC ... x64/x86" is always there; for
REM ARM64 add "MSVC ... ARM64/ARM64EC build tools" in the Visual Studio Installer).
setlocal
set "SRC=%~dp0"
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
set "BDIR=%~2"
if "%BDIR%"=="" set "BDIR=%LOCALAPPDATA%\FrameStudio\build-%ARCH%"

REM vcvarsall's host_target: built on this PC's own architecture, for the one asked for
set "HOSTARCH=amd64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOSTARCH=arm64"
if /i "%ARCH%"=="x64" set "VCARCH=%HOSTARCH%_amd64"
if /i "%ARCH%"=="x86" set "VCARCH=%HOSTARCH%_x86"
if /i "%ARCH%"=="arm64" set "VCARCH=%HOSTARCH%_arm64"
if "%VCARCH%"=="" ( echo ERROR: unknown architecture "%ARCH%": x64, x86 or arm64 & exit /b 1 )
if "%VCARCH%"=="amd64_amd64" set "VCARCH=amd64"
if "%VCARCH%"=="arm64_arm64" set "VCARCH=arm64"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" ( echo ERROR: Visual Studio with the C++ toolset not found & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul || ( echo ERROR: no MSVC tools for %ARCH% & exit /b 1 )
where cmake >nul 2>nul || set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
where ninja >nul 2>nul || set "PATH=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

cmake -S "%SRC%." -B "%BDIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%BDIR%" --target frame-studio || exit /b 1
echo Built "%BDIR%\Frame Studio.exe" (%ARCH%)
