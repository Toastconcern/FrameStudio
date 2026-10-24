# Getting started

This guide explains what Frame Studio and Frame Home are, what you need, and how to go from a download to a home
on your Steam Frame in a few minutes.

> Screens and panel positions are described in [studio-guide.md](studio-guide.md). This guide keeps to menu names,
> because the window layout is being redesigned.

## What they are

**Frame Home** runs on the Steam Frame. It replaces SteamVR's own environment (the space you stand in when no game
is running) with a home of your choice. It shows at boot, behind the Steam menu and while games load, like the
built-in environment does. It is not a game and has no entry in your library.

**Frame Studio** is the desktop editor. It runs on Windows, Linux and macOS. It opens:

- Meta Quest homes (`.apk` files: HSL, Clay `.opa` or glTF homes),
- Frame environments (`.fenv` files),
- models from Blender or Unity (`.glb`, `.gltf`, `.fbx`, `.obj`).

It shows them with the same renderer the headset uses. You can move, hide and delete objects, shape where you can
walk, add seats, sounds and a picture, save the result as a `.fenv` environment, and send it to the headset.

## What you need

| | Requirement |
|---|---|
| Graphics | A GPU with Vulkan 1.2. Any recent NVIDIA, AMD or Intel driver on Windows and Linux. |
| Windows | Windows 10 or 11, 64-bit (x64) or ARM64. |
| Linux | 64-bit (x64) or ARM64, with an X11 desktop or XWayland. |
| macOS | macOS 11 or later on Apple Silicon. Vulkan runs on Metal through MoltenVK, which is inside the app. |
| Headset | A Steam Frame with developer mode and SSH turned on, on the same network as your computer. See [headset-setup.md](headset-setup.md). |

32-bit Windows and Linux and Intel Macs have no ready-made download. You can still build Studio for them yourself:
see [building.md](building.md). A 32-bit build can use at most 4 GB of memory, so the very largest homes may not fit.

You do not need Studio to use Frame Home. If you only want to install Frame Home and copy ready-made `.fenv` files,
see [install-frame-home.md](install-frame-home.md).

## Download

1. Open the project's GitHub page and go to **Releases**.
2. Pick a release:
   - **Latest build** is rebuilt from the newest code every time the main branch changes. It is marked as a
     pre-release.
   - A **version release** (for example `v1.0`) is a fixed version.
3. Download the file for your computer:

| Computer | File |
|---|---|
| Windows, 64-bit PCs | `FrameStudio-windows-x64.zip` |
| Windows on ARM | `FrameStudio-windows-arm64.zip` |
| Linux, 64-bit PCs | `FrameStudio-linux-x64.tar.gz` |
| Linux on ARM | `FrameStudio-linux-arm64.tar.gz` |
| Mac with Apple Silicon (M1 and later) | `FrameStudio-macos-arm64.zip` |
| Frame Home only (no Studio) | `FrameHome-installer.zip` |

Builds of other branches are not on the Releases page. You find them under **Actions**: open the run, then scroll
down to **Artifacts**. You must be signed in to GitHub to download artifacts.

## Unpack and first launch

Every Studio download holds the program and, beside it, a `frame` folder. That folder is the headset side of
Frame Home, which Studio installs for you. Keep the two together.

### Windows

1. Right-click the zip and choose **Extract All**. Pick a folder you can write to, for example your Documents.
2. Open the extracted folder and double-click **Frame Studio.exe**.
3. Windows may show "Windows protected your PC" (SmartScreen), because the program is not signed.
   Click **More info**, then **Run anyway**.

### macOS

1. Double-click the zip. You get **Frame Studio.app**.
2. Move it to your Applications folder if you like.
3. The app is not signed, so a double-click is refused the first time. Instead, right-click (or Control-click) the
   app and choose **Open**, then **Open** again in the dialog. After that, a normal double-click works.

### Linux

1. Unpack the archive:

   ```sh
   tar -xzf FrameStudio-linux-x64.tar.gz
   cd FrameStudio-linux-x64
   ```

2. Make sure the program can run, then start it:

   ```sh
   chmod +x "Frame Studio"
   ./"Frame Studio"
   ```

   You can also double-click it in your file manager. Studio writes its messages to `studio.log` in its data
   folder (see below), not to the terminal.
3. Open and save dialogs need `zenity` or `kdialog`. Install one with your package manager if dialogs do not appear.

## Where Studio keeps its files

| What | Windows | Linux | macOS |
|---|---|---|---|
| Studio's own files (settings, edits, recent homes, auto-saves, `studio.log`) | `%APPDATA%\FrameStudio` | `~/.local/share/FrameStudio` | `~/Library/Application Support/FrameStudio` |
| My Environments (what you save) | `Documents\Frame Studio\Environments` | `Frame Studio/Environments` in your Documents folder (`~/Documents` on most systems) | `~/Documents/Frame Studio/Environments` |

**File > My Environments > Show the Folder** opens the My Environments folder.

## A first walkthrough (about 5 minutes)

Before you start, set up the headset once: [headset-setup.md](headset-setup.md). Then install Frame Home on it with
**Headset > Install on Steam Frame...** (see [install-frame-home.md](install-frame-home.md)).

### 1. Open a home

1. Choose **File > Open...** (Ctrl+O), or drop files onto the Studio window.
2. Pick a Meta home (`.apk`), a `.fenv` environment, or a model.
   - To open a room together with its view outside (a "footprint" and a "vista"), select both files in the dialog.
     You can also add a vista later with **File > Import > Vista...**.
3. Wait for the home to load. Studio shows its progress.

### 2. Look around

| To | Do this |
|---|---|
| Orbit | Middle mouse drag, or Alt + left drag |
| Pan | Shift + middle mouse drag |
| Zoom | Mouse wheel |
| Fly | Hold the right mouse button and use W A S D, Q and E for down and up |
| See it from where you will stand | **View > Player View** (Numpad 0 or P) |
| Select an object | Left click it |

**Help > Controls** (F1) lists every shortcut.

To change something, select an object, then pick **Move** (G), **Rotate** (R) or **Scale** (S) in the toolbar on the
left of the view and drag the gizmo, or use **Object > Show/Hide** (H) or **Object > Delete** (X). The selected
object's details show in Properties, on the right. Shift+A adds things where you look; F3 finds any command.
**Edit > Undo** (Ctrl+Z) takes any step back.

### 3. Save

1. Choose **File > Save to My Environments** (Ctrl+S).
2. Studio bakes the home into one `.fenv` file and puts it in My Environments under its name.

After the first save, the menu item is called **Save** and writes to the same file. **File > Save As...** saves a
copy somewhere else.

### 4. Send it to the headset

1. Put the headset on, or at least make sure it is on and connected to your network.
2. Choose **Headset > Send to Frame**.
3. Studio saves the home as one `.fenv`, copies it to the headset and makes it the current environment. Its
   progress shows in the Studio window.

In the headset you now stand in your home. Open the Steam menu to switch environments or change settings: see
[using-frame-home.md](using-frame-home.md).

## Next steps

- [headset-setup.md](headset-setup.md): developer mode, SSH and keys.
- [install-frame-home.md](install-frame-home.md): all ways to install, update and remove Frame Home.
- [using-frame-home.md](using-frame-home.md): the Environments panel and every setting.
- [environments-and-fenv.md](environments-and-fenv.md): what a `.fenv` is and how to convert homes.
- [troubleshooting.md](troubleshooting.md): when something does not work.
