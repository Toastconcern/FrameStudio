# Frame Studio guide

Frame Studio opens a home, shows it the way the Steam Frame draws it, and lets you change it: move, hide or delete
its objects, decide where you can walk, add spawn points, seats and sounds, and save it as a `.fenv` environment for
the headset. This guide walks through the window and the everyday tasks. To install Studio, see
[getting-started.md](getting-started.md).

## The window

Frame Studio is laid out the way Blender works: the window is split into **areas**, each showing one editor, with
a header along its top.

```
┌────────────────────────────────────────────────────────────────────────────┐
│ ◎ File Edit Headset Window Help │ Layout  Walking  Headset │    home name    │  top bar
├──────────────────────────────────────────────────────┬─────────────────────┤
│ ▣▾ Arrange▾ View Select Add Object  Global▾   ◈ ◯◯◯▾  │ ☰▾ As in the File▾ 🔍 │
│ ┌──┐ ● home › selection                       (axes) │ ▾ Root               │
│ │✥ │                                            🔍   │   ▾ folder         👁 │
│ │⟳ │              3D VIEWPORT                   ✋   │     object         👁 │  Outliner
│ │⤢ │                                            👤   ├─────────────────────┤
│ │✎ │                                            ⤢   │ 🔧▾ Object           │
│ └──┘                                                 │ ■ │ ▾ Object         │
│  (toolbar: T)                     (sidebar: N) ▸     │ 👣│  Location  …      │  Properties
├──────────────────────────────────────────────────────┤ ▣ │ ▾ Surface        │
│ 🕐▾ Playback▾          ⏮ ▶ ⏭          12.0 / 60.0 s 🔊 │ ▤ │   …              │
│ 0    5    10 ▮12.0  15    20    25    30    …       │ ⌂ │                  │  Timeline
├──────────────────────────────────────────────────────┴─────────────────────┤
│ 🖱 Select  🖱 Rotate View  🖱 Context Menu / Fly        182 objects | 60 fps │  status bar
└────────────────────────────────────────────────────────────────────────────┘
```

| Area | What it's for |
|---|---|
| **3D Viewport** | the home as the headset draws it. Its header has the mode (Arrange or Draw Floor), the **View**, **Select**, **Add** and **Object** menus, the gizmo's orientation (Global or Local), the world layer and the shading (Wireframe, Solid, Material, more in ▾). Inside it: the toolbar (T), the axes and navigation buttons, and the sidebar (N) |
| **Outliner** | everything in the home: its objects (as the file lays them out, or by kind) and its world items, with an eye to hide each |
| **Properties** | settings, in tabs down its left edge: **Object** (what you selected), **World** (where you can walk), **Environment** (name, picture, sound, headset settings), **Bundle**, **Headset** |
| **Timeline** | plays the home's animations and sound; click or drag in it to scrub through the loop |

**Rearranging areas.** Drag the gutter between two areas to resize them. To move an area, drag the **editor button**
at the left of its header (the icon with ▾) and drop it on another area: onto its middle to stack them as tabs, or
onto an edge to split that area. Click the editor button to show a different editor in that area. **Window > Reset
Layout** puts everything back; **Ctrl+Space** fills the window with the 3D Viewport until you press it again.

**Workspaces.** The tabs in the top bar switch between layouts made for a job: **Layout** (everything), **Walking**
(a big view with Properties on the World tab) and **Headset** (Properties on the Headset tab). Studio remembers
your layout between runs.

**Themes.** **Window > Theme** recolours the whole interface, from dark to light, using the palette from the MetaDB
site. The choice is saved and comes back next time.

## Opening a home

- **File > Open...** (Ctrl+O), or drop a file on the window. It opens:
  - a Meta home (`.apk`). A *footprint* (the room) and its *vista* (the world outside) are opened together: pick both
    in the file dialog, or open the footprint and Studio offers the vistas beside it;
  - a Frame environment (`.fenv`);
  - models from Blender or Unity (`.glb`, `.gltf`, `.fbx`, `.obj`).
- **File > My Environments** lists what you have saved; **File > Open Recent** lists what you had open.
- **File > Import > Vista...** gives a footprint its vista; **File > Import > Models...** places models into the home.
- A big home asks after a while whether to keep waiting; **Stop Loading** cancels.

## Moving around in the 3D view

| Do this | With |
|---|---|
| Orbit | middle-drag, Alt + left-drag, or drag the axes gizmo in the top-right corner |
| Snap to a view (top, front, side...) | click an axis ball on the gizmo (or the Numpad: 1/3/7 front/side/top, Ctrl for the opposite) |
| Pan | Shift + middle-drag |
| Zoom | mouse wheel, or Ctrl + middle-drag |
| Fly | hold the right mouse button + W A S D, Q / E down / up (wheel: speed, Shift: 4x) |
| Look from where you'd stand | the person button under the axes, P, or Numpad 0 |
| Frame the selection / everything | F or Numpad . / Home (or double-click a row in the Outliner) |

Press **F1** (Help > Controls) for the full list.

## Selecting and editing objects

1. Click an object in the 3D view or the Outliner. Properties switches to its **Object** tab.
2. Pick a tool in the toolbar on the left of the view (or press its key) and drag the gizmo:
   - **Move** (G), **Rotate** (R), **Scale** (S). Hold Ctrl to snap.
   - **Global** / **Local** in the header switches the gizmo between the world's axes and the object's own.
3. Or type exact values in **Location**, **Rotation** and **Scale** in the Object tab.
4. In the same tab: **Visible**, **Reset** (back to where the home had it), **Only This** (hide everything else,
   I), and **Delete** (X).

The Outliner's eye buttons hide objects or whole folders. **Select > Reveal Hidden** (Alt+H) brings them back. Every
change can be undone with Ctrl+Z and redone with Ctrl+Shift+Z.

## Where you can walk

Frame Home works out where you can walk from the home's shapes: floors you can reach from the start, under enough
headroom, and nothing in the way. The 3D view shows it: green is walkable, blue are small gaps it bridges, orange are
edges, grey are floors you can't reach. **W** shows or hides it.

- The **World** tab shows the area's size and its settings (step height, headroom, body radius). The presets
  **Open**, **Standard** and **Careful** set them for you.
- **Object > Auto-generate Walkable Surfaces** (in the 3D Viewport's header) gives every mesh a surface: walkable where it's flat, blocking where
  it's steep. A summary shows what it did; **Undo** in it takes back just that run.
- Per object, the **Surface** buttons in the Object tab decide what it is for the player:
  **Auto** (decided by its shape), **Surface** (walk on its flat parts, bump into its steep ones),
  **Collider** (you can't walk through it), **Walk through** (ignored). Right-clicking an object offers the same.
- **Draw Floor** (B, the brush in the toolbar, or the **Draw Floor** mode in the header): select an object, then drag over it to paint walkable floor
  onto it, even on furniture. Shift+drag erases; [ and ] change the brush size. **Fill Flat Faces** paints every face
  of it that's flat enough at once.

## World items

**Add** (in the 3D Viewport's header, or Shift+A over the view) places items where the camera looks:

| Item | What it does on the headset |
|---|---|
| **Spawn Point** | where you start, facing the way it faces |
| **Seat** | a place to sit: point at it and pull the trigger; you sit facing the way its arrow points; its "stand up here" ring is where you get up |
| **Teleport Hotspot** | a spot the teleport arc snaps to |
| **Surface** | a box you can scale into a floor, a step or a wall |
| **Invisible Wall** | stops you; never drawn on the headset. Scale and turn it to fit; where it stands is no longer walkable |
| **Gravity Zone** | inside it, "down" is the zone's own: a direction, or any surface you stand on |

Select an item to move it with the gizmo or type its values in the Object tab. **Object > Clear World Layer**
removes them all.

Meta's own homes have invisible walls too, round their play space (Cascadia, Space Station, Desert Terrace, Haven).
Frame Studio and Frame Home read them as invisible walls, so you can't walk out past them. Select one in the Outliner
to delete it, or set its **Role** to Surface, if you want to go further in that home.

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
**File > Bundle** or the **Bundle** tab in Properties: **New Bundle**, add environments (the open one, from My Environments, or any
`.fenv`), **Save Bundle...**, and **Install on Steam Frame...**. Opening a `.fbundle` lists what's in it; double-click
one to open it, or **Unpack into My Environments**. See [bundles.md](bundles.md).

## Sending it to the headset

The **Headset** tab (or the **Headset** menu):

- **Send to Frame** makes the open environment the headset's current one, saved or not.
- **Install Frame Home** puts Frame Home itself on the headset, with the environments and bundles you list under
  **Environments to install**. **Restore SteamVR's Environment** takes it off again.
- **This PC** (Linux): **Install Frame Home on This PC** makes your environments SteamVR's environment on the
  computer Studio runs on.
- The **Headset** field is the headset's address: an SSH alias such as `frame`, or `steamos@<address>`.
  See [headset-setup.md](headset-setup.md) and [install-frame-home.md](install-frame-home.md).

## Settings and tools

- **Edit > Preferences...**: Studio's own (3D view resolution, anti-aliasing, texture size and filtering) and the
  headset's (read from and written to the Frame).
- **Viewport shading** (the buttons and ▾ at the right of the 3D Viewport's header): Material (as the headset draws
  it), Solid, Wireframe, and in ▾ **GPU Cost**, **Overdraw** and **Leave Out** (hide whole kinds of objects).
- **Window > Home Report...**: what the home is made of and what may be slow.
- **Headset > Headset Debug...**: the same views, on the headset.
- **View** (in the 3D Viewport's header): the toolbar and sidebar, framing, the numpad viewpoints, **Local View**
  (show only the selection) and **Show Everything**.
- **Edit > Menu Search...** (F3): find any command by typing part of its name.

## Updates

Studio checks GitHub for a newer **Latest build** a few seconds after it starts and every 6 hours after that. When
there is one, an orange **Update Available** button shows in the top bar. Click it, then **Download and Install**:
Studio downloads the build for your computer and puts it in place of the copy you're running. **Restart Now** opens
the new one (it asks about unsaved changes first). **Help > Check for Updates...** checks right away.

If Studio's folder can't be written to (Program Files, for example), nothing is changed and the window offers the
download page instead. A copy you built yourself doesn't update itself.

## Keyboard shortcuts

| Key | Does | Key | Does |
|---|---|---|---|
| Ctrl+O | open | Ctrl+S | save |
| Ctrl+Shift+S | save as | Ctrl+Z / Ctrl+Shift+Z | undo / redo |
| G / R / S | move / rotate / scale tool | B | draw floor |
| Shift+A | add menu at the mouse | X | delete (asks first); Delete: right away |
| H / Alt+H | hide / reveal hidden | Alt+A, Esc | select nothing |
| Numpad . / Home | frame selected / all | Numpad 0, P | player view |
| Numpad 1 / 3 / 7 | front / right / top (Ctrl: the other side) | Numpad /, I | show only selected |
| W | world layer | Space | play / pause |
| T / N | toolbar / sidebar | Ctrl+Space | maximize the 3D Viewport |
| F3 | menu search: every command by name | F1 | controls |
