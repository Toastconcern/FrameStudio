@echo off
rem Gives SteamVR its own environment back on this PC (your environments are kept).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-frame-home-pc.ps1" -Uninstall
pause
