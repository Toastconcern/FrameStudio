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
| **Properties** | settings, in tabs down its left edge (see below): **Tool**, **Scene**, **Navmesh**, **Object**, **Physics**, **Data**, **Output** |
| **Timeline** | plays the home's animations and sound; click or drag in it to scrub through the loop |

**Rearranging areas.** Drag the gutter between two areas to resize them. To move an area, drag the **editor button**
at the left of its header (the icon with ▾) and drop it on another area: onto its middle to stack them as tabs, or
onto an edge to split that area. Click the editor button to show a different editor in that area. **Window > Reset
Layout** puts everything back; **Ctrl+Space** fills the window with the 3D Viewport until you press it again.

**Properties.** As in Blender, its tabs run down its left edge as icons (hover one for its name), grouped by what
they're about, and the open tab's panels fill the rest under a line naming what they show ("Home › Chair"):

| Tab | Its panels |
|---|---|
| **Tool** | the active tool's settings: Move, Rotate and Scale (orientation, snapping); Draw (Floor, Wall or Seats, the floor's shape and brush, the wall's height); **Draw Navmesh** (its corners, Back, Close, Stop); **Edit Mesh** while editing corners (All, Linked, Delete, Fill, Extrude, Merge) |
| **Scene** | **Environment** (name, picture, sky), **Contents** (what it's made from), **Sound**, **Rendering** (the headset's resolution, foveation, MSAA, frame rate) and **What Nobody Can See** (trimming) |
| **Navmesh** | **Navmesh** (where it comes from, its size, Draw, Edit, Add Square, Auto-generate, Use Meta's, Delete), **Pieces**, **Viewport Display** and **Settings** |
| **Object** | what you selected: its name, what it alone has (a seat's stand-up spot, a spawn's facing), Visible, Reset, Duplicate, Only This, Delete, and **Transform** |
| **Physics** | an object's **Collision** (Auto, Surface, Collider, Walk through) and floor drawn on it; a surface's role and size; a gravity zone's kind, size and priority. Shown when the selection has any |
| **Data** | a surface's mesh: its shape, triangles, **Edit Mesh** or **Convert to Mesh**. Shown when a surface is selected |
| **Output** | **Save** (Save, Export, Export for Blender), **Headset** (send it; Install Frame Home and This PC inside it) and **Bundle** |

Click a panel's header to fold or unfold it; rarely used panels start folded, and Studio keeps them as you leave them
while it runs. The **Search** field in Properties' header shows only the panels with a setting of that name, unfolded,
the matches lit. Selecting something shows the Object tab, unless the open tab (Physics, Data) has something for it.
The 3D Viewport's sidebar (N) shows the same Object, Physics and Data panels in its **Item** tab and the Tool tab's in
its **Tool** tab.

**Workspaces.** The tabs in the top bar switch between layouts made for a job: **Layout** (everything), **Walking**
(a big view with Properties on the Navmesh tab) and **Headset** (Properties on the Output tab's Headset panel).
Studio remembers your layout between runs.

**Themes.** **Window > Theme** recolours the whole interface, from dark to light, using the palette from the MetaDB
site; **Blender** uses Blender's own dark greys and blue. The choice is saved and comes back next time.

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
| Walk around as on the headset | V: W A S D to walk on the navmesh (Shift runs), drag to look; V or Esc back |
| Frame the selection / everything | F or Numpad . / Home (or double-click a row in the Outliner) |

Press **F1** (Help > Controls) for the full list.

## Selecting and editing objects

1. Click an object in the 3D view or the Outliner. Properties shows its **Object** tab. Ctrl+click or
   Shift+click selects more than one.
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
edges, grey are floors you can't reach. Pink marks every step and ledge you walk up (a stair, a kerb, a raised
platform's edge); right-click the World layer button and untick **Steps & ledges** to hide them. **W** shows or hides
it all.

- A home whose makers set up its collision (Meta's older homes: Vista, Del Mar) is walked on that collision. Anything
  they drew without collision (a vase, leaves, a lamp) is walked through, and Auto-generate adds nothing to it.
  Untick **Use Meta's Floor** (Navmesh tab, **Settings**) to remove their floor and walls (their invisible walls too): the home
  is then walked on its meshes like any other. It's saved with the home, so Frame Home does the same.
- A home without collision is walked on its meshes. Meshes its makers named as ground, terrain or floor are always
  floor, even with see-through edges. Grass, ferns, flowers and bushes are never floor: you walk through them up to
  1 m tall, and taller ones block. Grass is also found by its look, whatever it's named: low, upright, cut-out
  pieces with a leafy green texture (or many small ones, like a bed of flowers) count as plants and are listed under
  **Plants**. A lawn named grass that lies flat is floor.

- The **Navmesh** tab shows where the area comes from and its size. Its **Settings** panel has the step height,
  headroom, body radius and bridged gaps; the presets **Open**, **Standard** and **Careful** set them for you.
  **Viewport Display** picks what of it the view shows (the navmesh, steps & ledges, floors out of reach).
- **Object > Auto-generate Navmesh** (in the 3D Viewport's header) first labels everything that's grass **Walk through**
  (its Collision in the Physics tab, as you'd set it by hand), then gives every other mesh a surface: walkable where it's flat, blocking where
  it's steep. A summary shows what it did; **Undo** in it takes back just that run.
### The navmesh

The navmesh is where you can walk. It's the **Navmesh** group of the
Outliner's **World** list, in cyan in the view.

- Until you edit it, the group holds one row, **Navmesh (worked out from the home)**, with its size: Frame Home works
  out the same from the home's floors when it loads it. Click it for its settings, use its eye to show or hide it.
- A home that ships a navmesh of Meta's (its own navmesh mesh, else its collision: Meta's floor) uses that first; the row
  says **Navmesh (Meta's)**. **Use Meta's Navmesh** (**Use Meta's** in the Navmesh tab, the World menu, the row's right-click) goes back to it
  from yours at any time.
- Press **Tab** on it, **Edit** in the Navmesh tab or right-click > **Edit** to edit its corners straight away:
  it becomes pieces you edit, one for each area you can walk apart, listed in the group. From then on the home is walked on the pieces alone: exactly on their faces, at
  their heights, and you stop at their edges. The settings, Meta's floor and the objects' Collision settings don't
  change them.
- **World > Add Navmesh** adds a new 2 m square piece on the floor where you're looking.
- **Delete** it: X or Delete on its row, right-click > **Delete Navmesh** on the group, **World > Delete Navmesh**, or
  **Delete** in the Navmesh tab. Its pieces, what Auto-generate made, the painted floor and the saved area go,
  and nothing is walkable. It isn't made again by itself: **Auto-generate** or **Add Navmesh** makes one. Deleting the
  last piece leaves no navmesh too. Walls, seats, spawns and floors you placed or drew yourself stay. Ctrl+Z undoes it.
- Any mesh surface can be made a piece of it: set its **Role** to **Navmesh** in the Physics tab.
- Select a piece and press **Tab** (or **Edit Mesh** in the Data tab; double-click it in **Pieces**) to edit its
  corners. The same works for every mesh surface, Meta's collision meshes too; a box, ball or capsule gets
  **Convert to Mesh** in the Data tab first. While editing, the Tool tab has its operations as buttons.

In edit mode:

| Do | How |
|---|---|
| Select a corner | click it; Shift+click adds or takes away |
| Select several | drag a box round them; A selects all (or none), Alt+A none |
| Select a whole piece | L selects everything joined to what's selected |
| Move, turn, scale | the gizmo (G, R, S); Ctrl snaps |
| Delete | X or Delete: the selected corners and every face on one of them |
| Make a face | F, with three or more corners selected |
| Extrude | E: new faces out of the selected corners' open edges, ready to move |
| Merge | M: the selected corners become one, at their middle |
| Look round | Alt+drag orbits, as always |
| Done | Tab or Esc |

Corners at the same place (a mesh split at its seams) are one corner and move together. Every change is one undo step.

### The World list

Every world item is listed (spawns, seats, hotspots, every surface and collision piece, gravity zones, the navmesh's
pieces). Ctrl+click or Shift+click selects more than one: the gizmo moves, turns or scales them together, and Delete
deletes them all. Right-click a group for **Select All** and **Delete All**.
- Per object, the **Collision** choice in the Physics tab decides what it is for the player:
  **Auto** (decided by its shape), **Surface** (walk on its flat parts, bump into its steep ones),
  **Collider** (you can't walk through it), **Walk through** (ignored). Right-clicking an object offers the same.
- **Draw Floor** (B, the brush in the toolbar, or the **Draw Floor** mode in the header): paint walkable floor onto any
  surface, even furniture, or erase it with Shift. Pick a shape under the brush size (Q cycles them):
  - **Round** and **Square**: brushes you drag; [ and ] change the size.
  - **Rectangle**: drag from one corner to the opposite one.
  - **Polygon**: click each corner, then click the first one again (or press Enter, or double-click) to close it.
    Backspace takes the last corner back and Esc drops it.

  The square and the rectangle line up with your view, and with the home's walls when you're looking nearly along
  them. Grass, bushes and other plants don't get in the way: the brush reaches the ground under them, and you walk
  through any plant standing on painted floor (unless you set it to **Collider** or **Surface**). With an object
  selected, **Fill Flat Faces** paints every face of it that's flat enough at once.
- **Draw Wall** (B again): drag a line on the floor and an invisible wall stands along it. Nearly straight lines
  straighten, and each end reaches up to half a metre to whatever you drew it up to, so there's no gap to slip through.
- **Draw Seats** (B a third time): press where the seat goes and drag the way it should face (an arrow follows the
  mouse); a plain click faces you. Ctrl+drag (Cmd+drag on a Mac) a line for a row of seats along it, one every 60 cm. Each seat's
  stand-up spot is on floor you can walk on. Turn or move a seat afterwards like anything else.
- Trees, bushes and grass: grass and low plants are walked through and taller plants block, unless you say
  otherwise. Select one and set it to **Collider** to block or **Walk through** to pass through it. Plants can be
  hidden, deleted and resized like any other object.

## World items

**Add** (in the 3D Viewport's header, or Shift+A over the view) places items where the camera looks:

| Item | What it does on the headset |
|---|---|
| **Spawn Point** | where you start, facing the way it faces (drawn as a standing figure with what it sees first). A home with none gets one on open, level floor facing the widest view; **Find a Good Spot** and **Face the Best View** do that for any spawn |
| **Seat** | a place to sit: point at it and pull the trigger; you sit facing the way its arrow points; its "stand up here" ring is where you get up, always on floor you can walk on |
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

The **Scene** tab, top to bottom:

- **Environment**: the name it shows on the headset, its picture and its sky. **From View** (F12) shows the picture's
  16:9 frame over the 3D view: fly or walk (V) to line it up, then **Take** (Enter). Or **Choose...** a picture file;
  with neither, a view from the spawn point is taken when you save.
- **Contents** (folded): the home it's made from and what's in it.
- **Sound**: the home's own ambience and a background sound of your own (MP3, Ogg, Opus, WAV or FLAC), with their
  volumes. Drop a sound file on the window to add it. **Save Home's Sound...** keeps the home's ambience as an Ogg file.
- **Rendering** (folded): its own headset settings: resolution, foveation, MSAA and frame rate. **Calculate Best**
  walks it on the headset and keeps the sharpest settings that hold 120 fps; **Headset's** goes back to the headset's.
- **What Nobody Can See** (folded): **Trim What Can't Be Seen**, **Put Back**, **Show Trimmed** and **Trim When Saving**.

## Saving

| Command | Does |
|---|---|
| **File > Save** (Ctrl+S), or **Save** in the Output tab | saves all of it into **My Environments** (`Documents/Frame Studio/Environments`): one already there saves into itself, one opened from anywhere else becomes a new one there. The file is named after the environment's **Name**: rename it and Save renames the file |
| **File > Export...** (Ctrl+E) | writes a copy of all of it as a `.fenv` anywhere; Save still goes to My Environments |
| **File > Auto-saved Versions** | every 5 minutes with unsaved changes, a version is kept (the last 10) |
| **File > Faster Saving** | packs on all CPU cores: about 3x quicker, about 1% bigger |
| **File > Leave Out What Can't Be Seen** | drops geometry nobody can see from anywhere they can stand, so the headset draws less |
| **File > Revert to Original** | the home as it shipped, without your changes |

Saved environments are *baked*: everything the headset needs, already in its own format. See
[environments-and-fenv.md](environments-and-fenv.md).

## Bundles

A bundle (`.fbundle`) packs several saved environments into one file, to share or install together. Use the
**File > Bundle** or the **Bundle** panel in Properties' Output tab: **New Bundle**, add environments (**Add Open**,
**Add Mine** from My Environments, or any **.fenv**), **Save**, and **Install...** (it goes on the Headset panel's
install list). Opening a `.fbundle` lists what's in it; double-click one to open it, or **Unpack** it into My
Environments. See [bundles.md](bundles.md).

## Sending it to the headset

The **Headset** panel in Properties' Output tab (or the **Headset** menu):

- **Send to Frame** makes the open environment the headset's current one, saved or not.
- **Install Frame Home** (inside it, folded): **Install** puts Frame Home itself on the headset, with the open home
  and the environments and bundles you **Add**. **Restore** takes it off again.
- **This PC** (Linux): **Install** makes your environments SteamVR's environment on the computer Studio runs on.
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
  (show only the selection) and **Show Everything**. **View > Copy Camera Position** copies where the view is, to
  paste with a screenshot when reporting a spot that looks wrong.
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
