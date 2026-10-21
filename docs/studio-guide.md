# Frame Studio guide

Frame Studio opens a home, shows it the way the Steam Frame draws it, and lets you change it: move, hide or delete
its objects, decide where you can walk, add spawn points, seats and sounds, and save it as a `.fenv` environment for
the headset. This guide walks through the window and the everyday tasks. To install Studio, see
[getting-started.md](getting-started.md).

## The window

```
┌───────────────────────────────────────────────────────────────────────────┐
│ File  Edit  View  World  Bundle  Headset  Debug  Help         menu bar     │
├────┬──────────────┬──────────────────────────────┬────────────────────────┤
│ ✥  │ EXPLORER     │                              │ Selected World Env… … │
│ ⟳  │ Filter…      │                              │ ───────────────────── │
│ ⤢  │ ▾ Root       │          3D VIEW             │      INSPECTOR         │
│ ✎  │   ▾ folder   │                              │  (the chosen tab's     │
│ ── │     object   │                              │   settings)            │
│ ◍  │ ▾ World      │                              │                        │
│ ☺  │   Spawns …   │                              │                        │
│ ⌗  │              │                              │                        │
├────┴──────────────┴──────────────────────────────┴────────────────────────┤
│ ❚❚ Pause  ───────────●─────────────────  12.0 / 60.0 s          Sound on   │
│ Home name · format · selection · fps                      Frame: sent …    │
└───────────────────────────────────────────────────────────────────────────┘
```

| Part | Where | What it's for |
|---|---|---|
| **Tool strip** | left edge | the transform tools, Draw Floor, and how the gizmo and the camera behave |
| **Explorer** | left | everything in the home: its objects (in the file's own folders, or grouped by kind) and its world items |
| **3D view** | middle | the home as the headset draws it, with the gizmo and the walkable area on top |
| **Inspector** | right | settings, in tabs: **Selected**, **World**, **Environment**, **Bundle**, **Headset** |
| **Timeline** | bottom | plays the home's animations and sound; drag to scrub through the loop |
| **Status bar** | very bottom | the home's name and format, what's selected, the frame rate, and whether the headset has it |

Drag the thin lines between the Explorer, the 3D view and the Inspector to make any of them wider.

## Opening a home

- **File > Open Home...** (Ctrl+O), or drop a file on the window. It opens:
  - a Meta home (`.apk`). A *footprint* (the room) and its *vista* (the world outside) are opened together: pick both
    in the file dialog, or open the footprint and Studio offers the vistas beside it;
  - a Frame environment (`.fenv`);
  - models from Blender or Unity (`.glb`, `.gltf`, `.fbx`, `.obj`).
- **File > My Environments** lists what you have saved; **File > Open Recent** lists what you had open.
- **File > Add Vista...** gives a footprint its vista; **File > Add Models...** places models into the home.
- A big home asks after a while whether to keep waiting; **Stop Loading** cancels.

## Moving around in the 3D view

| Do this | With |
|---|---|
| Orbit | middle-drag, or Alt + left-drag |
| Pan | Shift + middle-drag |
| Zoom | mouse wheel, or Ctrl + middle-drag |
| Fly | hold the right mouse button + W A S D, Q / E down / up (wheel: speed, Shift: 4x) |
| Look from where you'd stand | **Player view** in the tool strip, P, or Numpad 0 |
| Frame the selection / everything | F / Home (or double-click a row in the Explorer) |

Press **F1** (Help > Controls) for the full list.

## Selecting and editing objects

1. Click an object in the 3D view or the Explorer. The Inspector switches to **Selected**.
2. Pick a tool in the tool strip (or press its key) and drag the gizmo:
   - **Move** (G), **Rotate** (R), **Scale** (S). Hold Ctrl to snap.
   - The globe / cube button switches the gizmo between the world's axes and the object's own.
3. Or type exact values in **Location**, **Rotation** and **Scale** in the Selected tab.
4. In the same tab: **Visible**, **Reset** (back to where the home had it), **Only This** (hide everything else,
   I), and **Delete** (X).

The Explorer's eye buttons hide objects or whole folders. **Edit > Unhide All** (Alt+H) brings them back. Every
change can be undone with Ctrl+Z and redone with Ctrl+Shift+Z.

## Where you can walk

Frame Home works out where you can walk from the home's shapes: floors you can reach from the start, under enough
headroom, and nothing in the way. The 3D view shows it: green is walkable, blue are small gaps it bridges, orange are
edges, grey are floors you can't reach. **W** shows or hides it.

- The **World** tab shows the area's size and its settings (step height, headroom, body radius). The presets
  **Open**, **Standard** and **Careful** set them for you.
- **World > Auto-generate Walkable Surfaces** gives every mesh a surface: walkable where it's flat, blocking where
  it's steep. A summary shows what it did; **Undo** in it takes back just that run.
- Per object, the **Surface** buttons in the Selected tab decide what it is for the player:
  **Auto** (decided by its shape), **Surface** (walk on its flat parts, bump into its steep ones),
  **Collider** (you can't walk through it), **Walk through** (ignored). Right-clicking an object offers the same.
- **Draw Floor** (B, or the brush in the tool strip): select an object, then drag over it to paint walkable floor
  onto it, even on furniture. Shift+drag erases; [ and ] change the brush size. **Fill Flat Faces** paints every face
  of it that's flat enough at once.

## World items

**World > Add at the View's Centre** places items where the camera looks:

| Item | What it does on the headset |
|---|---|
| **Spawn Point** | where you start, facing the way it faces |
| **Seat** | a place to sit: point at it and pull the trigger; its "stand up here" ring is where you get up |
| **Teleport Hotspot** | a spot the teleport arc snaps to |
| **Surface** | a box you can scale into a floor, a step or a wall |
| **Wall Spot** | where SteamVR's panels dock |
| **Gravity Zone** | inside it, "down" is the zone's own: a direction, any surface you stand on, or a planet's pull |
| **Planet** | a sphere you can walk all the way round |

Select an item to move it with the gizmo or type its values in the Selected tab. **World > Clear World Layer**
removes them all.

## The environment: name, picture, sound

The **Environment** tab, top to bottom:

- **Environment**: the name it shows on the headset, its picture (**Choose...**, or a view taken when you save), and
  what it's made of.
- **Sound**: the home's own ambience and a background sound of your own (MP3, Ogg, Opus, WAV or FLAC), with their
  volumes. Drop a sound file on the window to add it. **Save Its Sound...** keeps the home's ambience as an Ogg file.
- **World settings**: its own headset settings: resolution, foveation, MSAA and frame rate.
  **Calculate Best Settings** walks it on the headset and keeps the sharpest settings that hold 120 fps.

## Saving

| Command | Does |
|---|---|
| **File > Save** (Ctrl+S) | saves into **My Environments** (`Documents/Frame Studio/Environments`); a saved `.fenv` saves into itself |
| **File > Save As...** (Ctrl+Shift+S) | saves a copy anywhere; Save goes there from then on |
| **File > Auto-saved Versions** | every 5 minutes with unsaved changes, a version is kept (the last 10) |
| **File > Faster Saving** | packs on all CPU cores: about 3x quicker, about 1% bigger |
| **File > Leave Out What Can't Be Seen** | drops geometry nobody can see from anywhere they can stand, so the headset draws less |
| **File > Revert to Original** | the home as it shipped, without your changes |

Saved environments are *baked*: everything the headset needs, already in its own format. See
[environments-and-fenv.md](environments-and-fenv.md).

## Bundles

A bundle (`.fbundle`) packs several saved environments into one file, to share or install together. Use the
**Bundle** menu or the **Bundle** tab: **New Bundle**, add environments (the open one, from My Environments, or any
`.fenv`), **Save Bundle...**, and **Install on Steam Frame...**. Opening a `.fbundle` lists what's in it; double-click
one to open it, or **Unpack into My Environments**. See [bundles.md](bundles.md).

## Sending it to the headset

The **Headset** tab (or the **Headset** menu):

- **Send to Frame** makes the open environment the headset's current one, saved or not.
- **Install Frame Home** puts Frame Home itself on the headset, with the environments and bundles you list under
  **Environments to install**. **Restore SteamVR's Environment** takes it off again.
- The **Headset** field is the headset's address: an SSH alias such as `frame`, or `steamos@<address>`.
  See [headset-setup.md](headset-setup.md) and [install-frame-home.md](install-frame-home.md).

## Settings and tools

- **Edit > Settings...**: Studio's own (3D view resolution, anti-aliasing, texture size and filtering) and the
  headset's (read from and written to the Frame).
- **Debug**: views that show how the home is built and what it costs (Wireframe, Meshes, GPU Cost, Overdraw),
  **Leave Out** to hide whole kinds of objects, **Home Report...** (what the home is made of and what may be slow)
  and **Headset Debug...** (the same views on the headset).
- **View**: **Show Only Selected** (I), **The Home's Meshes**, **World Layer** (W) and **Show Everything**.

## Keyboard shortcuts

| Key | Does | Key | Does |
|---|---|---|---|
| Ctrl+O | open | Ctrl+S | save |
| Ctrl+Shift+S | save as | Ctrl+Z / Ctrl+Shift+Z | undo / redo |
| G / R / S | move / rotate / scale | B | draw floor |
| H / Alt+H | hide / unhide all | X, Delete | delete |
| F / Home | frame selected / all | P, Numpad 0 | player view |
| I | show only selected | W | world layer |
| Esc | deselect | F1 | controls |
