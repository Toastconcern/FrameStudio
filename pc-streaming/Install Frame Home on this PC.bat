@echo off
rem Makes Frame Home SteamVR's environment on this PC (your homes from Frame Studio's My Environments, switched
rem in the Steam menu's Environments panel). Run it again after updating Frame Studio.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-frame-home-pc.ps1"
if errorlevel 1 (echo. ^& echo Frame Home was NOT installed: see the messages above.) else (echo. ^& echo Done.)
pause
