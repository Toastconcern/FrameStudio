@echo off
rem Installs Frame Home on the Steam Frame (no Frame Studio needed). Double-click it, or drop .fenv environments on
rem it to copy them to the headset too. Asks for the headset's address the first time (default: frame).
setlocal
set "HOSTFILE=%APPDATA%\FrameStudio\frame-host.txt"
set "FRAMEHOST="
if exist "%HOSTFILE%" set /p FRAMEHOST=<"%HOSTFILE%"
if "%FRAMEHOST%"=="" set "FRAMEHOST=frame"
set /p ENTERED=Steam Frame address [%FRAMEHOST%]:
if not "%ENTERED%"=="" set "FRAMEHOST=%ENTERED%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-frame-home.ps1" -HostName "%FRAMEHOST%" %*
if errorlevel 1 (echo. & echo Frame Home was NOT installed: see the messages above.) else (echo. & echo Frame Home is installed.)
pause
