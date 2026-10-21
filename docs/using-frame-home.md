# Using Frame Home on the headset

Once Frame Home is installed (see [install-frame-home.md](install-frame-home.md)), your home is the Steam Frame's
environment. It shows when no game is running, behind the Steam menu, and while games load.

## The Environments panel

Frame Home adds a panel called **Environments** to the Steam menu. It starts with SteamVR on its own.

1. Open the Steam menu.
2. Choose **Environments** in the menu's bar.
3. Point at the panel with a controller and click.

The panel has three pages: **Environments**, **Settings** and **Debug**. Its top right shows whether Frame Home is
running, with a button:

| The panel says | The button | What it does |
|---|---|---|
| Frame Home is running | Restart Frame Home | Reloads the home from its file |
| Frame Home isn't running | Start Frame Home | Starts it now |
| Off: it failed to start twice | Turn Frame Home On | Clears the off switch and starts it |

### Choosing an environment

The **Environments** page shows a card for every environment on the headset, with its name, its kind and a
thumbnail. The one you are in says "Showing now".

- **Click a card** to switch to it. Frame Home changes the environment live; you don't need to restart anything.
- **Delete** a card with its delete button. The panel asks first ("Delete ...?"). Deleting removes the file from the
  headset, its thumbnail and its unpacked copy.

The kinds you may see:

| Kind | Meaning |
|---|---|
| Frame environment | A `.fenv` file |
| Frame environment, footprint + vista | A `.fenv` holding a room and the view outside it |
| Meta home | A Meta Quest home `.apk` |
| Meta home, footprint only / vista only | Half of a Meta home pair |
| Footprint + Vista | Two `.apk` files listed as a pair (see below) |

### From a terminal

Over SSH (`ssh frame`) you can do the same with the panel's program:

```sh
~/frame-home/bin/frame-switcher --list          # the environments; the current one is marked with *
~/frame-home/bin/frame-switcher --set 3         # switch, by number from --list or by file name
~/frame-home/bin/frame-switcher --delete NAME   # delete one, like the panel's delete button
```

## Adding environments

Every environment lives in `~/frame-home/envs` on the headset. Ways to put one there:

- From Frame Studio: **Headset > Send to Frame** (the open home), or **Headset > Install on Steam Frame...** (any
  `.fenv` files).
- With the stand-alone installer: drop `.fenv` files on `Install Frame Home.bat`, or list them after
  `install-frame-home.sh`. See [install-frame-home.md](install-frame-home.md).
- By hand: copy the file into `~/frame-home/envs`, for example `scp my-home.fenv frame:frame-home/envs/`.

The panel notices new files by itself. Then, in the background:

- **Each new `.fenv` is unpacked ahead of time.** A big one takes about ten seconds. After that, switching to it is
  as quick as switching back to one you have used before.
- **Thumbnails are made while nobody is looking at Frame Home**, for example while a game is running. Making one
  loads and draws the whole home, which would make the home you are standing in stutter. So a new environment can
  show without a picture for a while.

Meta home `.apk` files work too, but they load more slowly than a `.fenv`. Studio or `fenv-tool` converts them (see
[environments-and-fenv.md](environments-and-fenv.md)). To show a room `.apk` and its vista `.apk` together, add a
line with both file names to `~/frame-home/pairs.txt`:

```
haven2025.apk calming.apk
```

## Moving around

Frame Home moves you the way Meta's homes do. You start at the home's spawn point, its floor on your real floor.

| Control | What it does |
|---|---|
| Left stick | Walk in the direction you push it, relative to where you look. A light push strolls. Walls stop you and you slide along them; ramps and steps are followed. |
| Right stick left / right | Turn: snap turns (30, 45 or 90 degrees) or smooth turning, as set in Settings |
| Right stick forward | Aim a teleport arc from your right hand. A marker shows where you'll land. Let go to go there. |
| Point at a seat, pull the trigger | Sit in it. Seats near you show a faint marker that lights up when you point at it. |
| Walk, teleport or step away while seated | Stand up again |

More details:

- The teleport arc stops at the first thing it meets. A floor or a seat is a landing; a wall, a ceiling or the side
  of furniture is not, unless it is low, then you land at its foot.
- Landing near a teleport hotspot snaps you onto it.
- Seated, you can still turn, but you stay put.
- Falling is real. If you fall further than the home's limit, you are put back at the spawn point.
- Some homes have gravity zones (walls you can walk up, small planets).
- **With the Steam menu open, the controllers belong to the menu.** Nothing moves you until you close it.

## Settings

The **Settings** page has three groups. Frame Studio shows the same settings under **Edit > Settings...**, in the
"Steam Frame (Frame Home)" part, so you can change them from your computer too.

Settings are stored in `~/frame-home/settings.json` on the headset. Frame Home reads it again whenever it changes.

### Movement (applies right away)

| Setting | Choices | Default | What it does |
|---|---|---|---|
| Walking | Off, Smooth | Smooth | Left stick walks you smoothly. Off: teleport only. |
| Teleport | Off, Instant, Slide | Instant | Right stick forward, aim, let go. Instant blinks you there; Slide glides there. |
| Turning | Snap, Smooth | Snap | What the right stick does left and right |
| Snap turn angle | 30°, 45°, 90° | 30° | How far each snap turn goes |
| Walking speed | Slow, Normal, Fast | Normal | How fast smooth walking moves you (1.4, 2 and 3 m/s) |
| Comfort vignette | Off, Gentle, Strong | Off | Darkens the edges of view while the stick moves or turns you. Helps if smooth walking makes you queasy. |
| Seat markers | Shown, Hidden | Shown | Markers on the seats near you. Hidden, pointing at a seat still finds it. |

### Environment (applies right away)

| Setting | Choices | Default | What it does |
|---|---|---|---|
| Volume | Off, 25%, 50%, 75%, 100% | 100% | All of the environment's sound |
| Animations | Play, Hold still | Play | Hold still stops the environment's moving parts; animated environments then cost less. |

### Graphics

The group's heading shows how Frame Home is doing right now: the GPU time a frame takes out of the time it has, the
size drawn per eye and its resolution, and whether eye tracking is used. For example: "6.1 of 8.3 ms a frame, ... per
eye (140%), eye tracking on". If it starts with "Too slow, lower Resolution", frames are late and the home stutters.

| Setting | Choices | Default | What it does |
|---|---|---|---|
| Resolution | Auto, 80%, 100%, 120%, 140% | Auto | Share of SteamVR's recommended size per eye. Auto moves between 70% and 150% to keep every frame on time, sharper whenever the home is light enough. |
| Frame rate | Full, Half | Full | Half draws the environment every other frame and turns it with your head in between, as Meta's home does: each frame gets twice the GPU time. |
| Anti-aliasing | Off, 2x, 4x | 4x | Smooths jagged edges. 4x looks best; lower is faster. |
| Texture filtering | 4x, 8x, 16x | 8x | Keeps floors and walls sharp at an angle. Higher looks better. |
| Foveated rendering | Off, Fixed, Eye tracked | Eye tracked | Full detail where you look, coarser toward the edges. The time this frees goes to resolution (with Auto). Fixed keeps the sharp area at the lens centre; Eye tracked follows your gaze. |
| Foveation strength | Gentle, Medium, Strong | Medium | How quickly detail falls off away from where you look |
| Keep cool | Off, 80°, 86°, 90° | 86° | Above this temperature (°C), the edges of view give up a little detail at a time until the headset cools. Where you look stays sharp. A hot headset slows its GPU. |
| Behind the Steam menu | Half rate, Full rate | Full rate | With the menu open, the environment is only a backdrop. Half rate leaves more GPU to the menu and its apps. |

Anti-aliasing, texture filtering and turning foveation on or off restart Frame Home. That takes a few seconds.
The others apply at once.

With eye-tracked foveation, the heading says "eye tracking waiting for your eyes" until the headset sees your gaze.

**Per-environment settings.** An environment can carry its own resolution, foveation strength, anti-aliasing and
frame rate, tuned for that home. While you are in it, those win over the settings above. See
[environments-and-fenv.md](environments-and-fenv.md#per-world-settings) and [performance.md](performance.md).

## The Debug page

The Debug page is for finding out why a home looks or runs wrong. Its settings are not kept: they switch off when
Frame Home restarts.

| Item | What it does |
|---|---|
| View | Normal, Wireframe (every triangle edge), Meshes (each in its own colour), GPU cost (green is cheap, red is costly), Overdraw (bright where surfaces pile up) |
| World layer | Shows where you can walk (green), surfaces and colliders, spawns, seats, wall spots and gravity zones |
| See-through surfaces | Leave out glass, water, glow and fog, to see whether they're what's slow |
| Cut-out surfaces | Leave out leaves, grass and fences |
| Sky | Leave out the sky |
| Costliest meshes | The meshes that cost the GPU most, with their time |
| This environment | The environment's report: problems found, if any (also in `~/frame-home/logs/report.txt`) |
| Save a Picture | Saves what you see to `~/frame-home/logs` |
| Turn Debug Views Off | Back to normal |

The GPU cost view times every draw, which makes the GPU a little slower while it is on.

Studio can show and change the same page from your computer: **Debug > Headset Debug...**.
