Frame Home for the Steam Frame
==============================

Frame Home replaces SteamVR's own environment on the Steam Frame with your homes (Meta Quest homes converted by
Frame Studio, or any .fenv environment). This folder installs it without Frame Studio.

What you need
-------------
- The Steam Frame with developer mode and SSH turned on, on the same network as your computer.
- SSH access to it: the headset's developer key in your SSH settings, or its password.
  The headset's address is "frame" when an entry of that name is in your ~/.ssh/config (or the network knows it by
  that name); otherwise use its IP address, e.g. steamos@192.168.1.50.

Install from a Windows PC
-------------------------
Double-click "Install Frame Home.bat" and type the headset's address (Enter keeps the one shown).
To copy environments to the headset at the same time, drop .fenv files (or .fbundle bundles) onto
"Install Frame Home.bat".
(Needs Windows' OpenSSH Client, under Settings > Apps > Optional features, or Git for Windows.)

Install from Linux or macOS
---------------------------
    sh install-frame-home.sh --host steamos@frame [environment.fenv | bundle.fbundle ...]

The first .fenv given is the one shown.

Install on the headset itself
-----------------------------
Copy this folder to the headset (Desktop Mode), open a terminal (Konsole) in it and run:

    sh payload/install.sh

Put .fenv environments in ~/frame-home/envs; the Environments panel in the Steam menu lists them.

Bundles
-------
A .fbundle holds several environments in one file, to share or install together (Frame Studio or
"fenv-tool bundle" makes them). Copied into ~/frame-home/envs (the installers do that), it is unpacked there by the
Environments panel into its .fenv environments, each listed on its own, and then deleted. An environment already
there is kept: the same one is skipped, a different one of the same name gets the bundle's as "name 2". A bundle
never changes which environment is shown.

Uninstall
---------
    sh install-frame-home.sh --uninstall              (Linux / macOS)
    powershell -ExecutionPolicy Bypass -File install-frame-home.ps1 -Uninstall   (Windows)
    sh payload/install.sh uninstall                   (on the headset)

SteamVR's own environment comes back; your environments in ~/frame-home stay.
If anything goes wrong, creating the file ~/frame-home/layer/off on the headset turns Frame Home off at once
(SteamVR's environment shows after its next start).
