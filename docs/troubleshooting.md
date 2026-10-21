# Troubleshooting

Find your symptom in the table, then read the matching section below for details. Logs are described at the end.

| Symptom | Likely cause | Fix |
|---|---|---|
| Studio won't start, or says "Couldn't start Vulkan on this GPU" | No Vulkan 1.2 driver | Update your graphics driver. See [Studio won't start](#studio-wont-start). |
| Studio won't start on a Mac | MoltenVK missing, or the app is blocked | Right-click > Open. Use the release build, which has MoltenVK inside. |
| Windows says "Windows protected your PC" | The program is not signed | **More info > Run anyway** |
| The 3D view is blank or black | Nothing open, still loading, or everything hidden | See [Blank or black view](#blank-or-black-view) |
| Open and save dialogs don't appear (Linux) | No dialog helper installed | Install `zenity` or `kdialog` |
| "Can't reach the headset" / "Can't reach the Frame" | SSH can't connect or needs a password | See [Can't reach the headset](#cant-reach-the-headset) |
| Install failed | Something on the headset | Read the install output. See [Install failed](#install-failed). |
| SteamVR shows its own environment again | Frame Home stopped, or turned off | See [SteamVR's environment is back](#steamvrs-environment-is-back) |
| The home stutters in the headset | Too heavy for the settings | Tune it. See [performance.md](performance.md#when-a-home-stutters). |
| A new environment has no picture in the panel | Thumbnails are made only while you're not looking at Frame Home | Wait, or run a game for a while |
| A large home fails to open in a 32-bit build | 32-bit programs have at most 4 GB | Use a 64-bit build |

## Studio won't start

Studio needs a GPU with Vulkan 1.2.

- **Windows and Linux:** install the current driver from NVIDIA, AMD or Intel (on Linux, your distribution's Mesa
  Vulkan drivers, often `mesa-vulkan-drivers`, or NVIDIA's driver). Old or very basic GPUs may not have Vulkan 1.2.
- **macOS:** Vulkan runs on Metal through MoltenVK. The release builds carry it inside the app. If you built the app
  yourself without MoltenVK, see [building.md](building.md#macos-prerequisites). The first time, right-click the app
  and choose **Open** (the app is not signed).
- **Linux:** Studio needs an X11 desktop or XWayland. Start it from a terminal to see whether it starts at all, then
  look at `studio.log`.

If Studio starts but says it couldn't start Vulkan, the message names the `studio.log` file. Its last lines say why.

## Blank or black view

- With nothing open, Studio shows its start screen. Open a home with **File > Open...**.
- A big home takes a while to load; textures arrive after the shapes.
- Check that things aren't hidden: **View > Show Everything**, and **Select > Reveal Hidden** (Alt+H). **Local
  View** (Numpad / or I) shows only the selection; press it again or Esc to show everything.
- Make sure the view's shading is **Material** (the sphere buttons at the right of the 3D Viewport's header) and
  nothing is ticked under **▾ > Leave Out**.
- Use **View > Frame All** (Home key) to bring the whole home into view.

## Dialogs don't appear on Linux

Studio's open and save dialogs use `zenity` or `kdialog`. Install one:

```sh
sudo apt install zenity      # Debian, Ubuntu
sudo dnf install zenity      # Fedora
```

Meanwhile you can drop files onto the Studio window to open them.

## Can't reach the headset

Studio and the installers use SSH. Test it yourself in a terminal:

```sh
ssh frame
```

(or `ssh steamos@<address>` if you don't use the name `frame`).

| What you see | Fix |
|---|---|
| "Could not resolve hostname frame" | The `frame` entry is missing from your SSH config, or you didn't use it. See [headset-setup.md](headset-setup.md#5-add-a-frame-entry-to-your-ssh-config). Or use the IP address. |
| "Connection timed out" or "No route to host" | Wrong address, the headset is off or asleep, or on another network. Check the address in the headset's network settings. |
| "Connection refused" | SSH is not on in the headset's developer settings |
| It asks for a password | Fine for the stand-alone installers. **Studio needs a key**: it never types a password. Set one up: [headset-setup.md](headset-setup.md#4-set-up-an-ssh-key). |
| "Permission denied (publickey...)" | The key isn't on the headset, or the `IdentityFile` line points at the wrong file |
| "Host key verification failed" | The headset's key changed (for example after a reset). Remove the old entry: `ssh-keygen -R <address>` (and `ssh-keygen -R frame`), then connect again and answer `yes`. |
| Windows: "ssh is not recognized", or Studio says "install OpenSSH or Git for Windows" | Turn on **OpenSSH Client** in **Settings > Apps > Optional features**, or install Git for Windows |
| "... isn't a Steam Frame" | The address points at another computer |

In Studio, the address is the **Headset** field in **Headset > Install on Steam Frame...**. Studio remembers it.

## Install failed

The installers print each step. The last lines say what went wrong. In Studio, the message shows in the Install
window.

| Message | Meaning |
|---|---|
| "the headset files are missing" | The `frame` folder (Studio) or `payload` folder (installer) is not beside the program. Unpack the whole download again. |
| "Frame Home can't run on this headset: ..." | The programs don't fit this headset's system. Nothing was changed. Use a newer release. |
| "SteamVR refused the Environments panel" | SteamVR would not register the panel. Restart SteamVR and install again. |
| "couldn't register with SteamVR" | SteamVR's config files could not be written or read. The message names the file. |
| "Copying ... failed" | The network dropped, or the headset is full. Check free space with `ssh frame df -h ~`. |

If it still fails, look at the logs (below) and try installing on the headset itself
([install-frame-home.md](install-frame-home.md#way-3-directly-on-the-headset)).

## SteamVR's environment is back

Frame Home steps aside and SteamVR's own environment shows whenever Frame Home isn't sending pictures. Check, in this
order:

1. **Open the Environments panel** in the Steam menu. The top right says whether Frame Home is running:
   - "Frame Home isn't running": press **Start Frame Home**.
   - "Off: it failed to start twice": the layer turned itself off after two failed starts. Press **Turn Frame Home
     On**.
   - The panel isn't in the Steam menu at all: install again (the panel starts Frame Home).
2. **Check the off switch.** If `~/frame-home/layer/off` exists, Frame Home is off on purpose:

   ```sh
   ssh frame ls ~/frame-home/layer/
   ```

   Delete `off` (`ssh frame rm ~/frame-home/layer/off`), or use the panel's button.
3. **Look at the logs** (below). `layer.log` says whether the layer loaded and why it stepped aside;
   `frame-home.log` says whether Frame Home started and loaded the home.
4. **Install again.** It replaces the programs and the layer, clears the off switch and restarts what's needed.

If the headset's environment breaks after an update, create the off file to get SteamVR's environment back at once:
`ssh frame touch ~/frame-home/layer/off`. See [install-frame-home.md](install-frame-home.md#the-emergency-off-switch).

## Stutter

See [performance.md](performance.md#when-a-home-stutters). In short: tune the home (`fenv-tool tune` or **Calculate
Best Settings**), make sure it's a baked `.fenv`, and lower Resolution or raise Foveation strength.

## Where the logs are

| Log | Where | What's in it |
|---|---|---|
| `studio.log` | Studio's data folder: `%APPDATA%\FrameStudio` (Windows), `~/.local/share/FrameStudio` (Linux), `~/Library/Application Support/FrameStudio` (macOS) | Everything Studio reports. Started fresh each time Studio starts. |
| `frame-home.log` | `~/frame-home/logs/` on the headset | Frame Home: start, loading, settings, problems |
| `report.txt` | `~/frame-home/logs/` on the headset | The current environment's report (what it costs, what's wrong) |
| `layer.log` | `~/frame-home/layer/` on the headset | The compositor layer: loaded, showing, or stepping aside and why |

To read a headset log from your computer:

```sh
ssh frame tail -50 frame-home/logs/frame-home.log
ssh frame tail -50 frame-home/layer/layer.log
```

When you ask for help, include the end of the relevant log. Logs contain file names and paths from your computer or
headset; look through them before posting them publicly.

## Self-test (for diagnosing)

Studio has a self-test. It opens a home, drives the real interface (selecting, moving, rotating, scaling, making
surfaces, undo, saving) and checks the results. It is meant for finding problems, not for everyday use. It doesn't
change your home's edits or My Environments.

Run it with a Meta home or a `.fenv`:

| System | Command |
|---|---|
| Windows | `"Frame Studio.exe" --selftest C:\path\to\home.apk` |
| Linux | `./"Frame Studio" --selftest /path/to/home.apk` |
| macOS | `"Frame Studio.app/Contents/MacOS/Frame Studio" --selftest /path/to/home.apk` |

Studio runs the test and quits. The report is `selftest/report.txt` in Studio's data folder, with screenshots beside
it.
