# Environments and the .fenv format

A **Frame environment** is one `.fenv` file. It holds everything Frame Home needs to show one home: the scene, where
you can walk and sit, your edits, a sound, a thumbnail and the home's own settings. Frame Studio writes it; Frame
Home and the Environments panel load it directly.

## What a .fenv is

A `.fenv` is a ZIP file. You can open it with any ZIP tool to look inside (don't change it that way, though).

There are two kinds:

| Kind | What's inside | Notes |
|---|---|---|
| **Baked** (native) | The home already converted for the Steam Frame: meshes, textures in the headset's GPU format with their smaller levels, animations, the world layer | Opens several times faster and uses less memory. What Studio and `fenv-tool` make today. |
| **Wrapped** (older) | The original Meta `.apk` files, plus edits and a `world.json` | Frame Home converts it every time it loads. Made by older versions, or when a home could not be baked. `fenv-tool upgrade` turns it into a baked one. |

`fenv-tool info` tells you which kind a file is: it prints "baked scene" or "loads from its homes".

## What's inside

| Member | In | What it is |
|---|---|---|
| `fenv.json` | every `.fenv` | The manifest: name, where the home came from (package and version of each `.apk`), and which other members are there. It also holds the environment's own settings. |
| `scene.bin.lz` | baked | The baked scene, packed with LZMA (much smaller than ZIP's own packing for textures). It is unpacked once, into a cache, then loaded from there. |
| `apks/...` | wrapped | The original Meta home files (a room, and maybe its vista) |
| `world.json` | wrapped | The world layer: spawn points, seats, teleport hotspots, walkable surfaces, gravity zones. A baked scene carries this inside `scene.bin`. |
| `edits.json` | when you edited | What you changed in Studio: moved and deleted objects, the added sound and how loud the home's own sound plays |
| a sound file | when you added one | The sound you added in Studio (`.ogg`, `.opus`, `.wav`, `.mp3`, `.flac`), named in `edits.json` |
| `thumb.png` | usually | The picture the Environments panel shows |
| `models/...` | when imported | Models from Blender or Unity, with their texture files (wrapped files only; a baked scene holds their meshes) |

Unpacked copies live in a cache: `~/.cache/frame-home/fenv` on the headset and on Linux and macOS, and
`%LOCALAPPDATA%\FrameStudio\cache` on Windows. They are keyed by the file's path, size and time, so a changed file is
unpacked again. You can delete the cache at any time.

## Typical sizes

Measured over a selector of 48 environments:

| | Size |
|---|---|
| Average | about 70 MB |
| Median | about 48 MB |
| Smallest | 3 MB |
| Largest | 218 MB |

A home with a room and its vista is usually larger than either alone.

## Making a .fenv in Frame Studio

1. Open a home: **File > Open...**.
2. Edit it if you like.
3. **File > Save to My Environments** (Ctrl+S) or **File > Save As...**.

Two options in the **File** menu change how Studio saves:

| Option | What it does |
|---|---|
| Faster Saving | Packs on all CPU cores: about 3 times quicker, about 1% bigger. Off: the smallest file. Nothing is lost either way. |
| Leave Out What Can't Be Seen | Leaves out geometry nobody can see from anywhere they can walk, sit or teleport to (the backs of things, what's behind walls), so the headset draws less. Moved and deleted objects are kept whole. Saving takes up to a minute longer. |

## Converting homes with fenv-tool

`fenv-tool` is a command-line converter. It comes with Frame Home and is installed on the headset as
`~/frame-home/bin/fenv-tool`. Run it over SSH (`ssh frame`, see [headset-setup.md](headset-setup.md)).

It reads Meta home `.apk` files (HSL, Clay `.opa`, glTF `.ovrscene`) and models (`.glb`, `.gltf`, `.fbx`, `.obj`).

First copy the files to the headset, for example:

```sh
scp haven2025.apk calming.apk frame:
ssh frame
```

### convert: one .fenv per home

```sh
~/frame-home/bin/fenv-tool convert ~/frame-home/envs haven2025.apk courtyard.apk
```

Makes `haven2025.fenv` and `courtyard.fenv` in `~/frame-home/envs`. To pair a room with its vista, join them with
`+`:

```sh
~/frame-home/bin/fenv-tool convert ~/frame-home/envs haven2025.apk+calming.apk
```

This makes `haven2025+calming.fenv`.

### pack: one .fenv from chosen inputs

```sh
~/frame-home/bin/fenv-tool pack ~/frame-home/envs/haven-calming.fenv --name "Haven + Calming" haven2025.apk calming.apk
```

| Option | Meaning |
|---|---|
| `--name NAME` | The name the panel shows. Default: made from the package names. |
| `--edits edits.json` | Include edits made in Studio |
| `--thumb thumb.png` | Include a thumbnail |
| `--keep-apks` | Store the original `.apk` files as well |
| `--fast` | Pack in parallel: several times quicker, about 3% bigger |
| `--no-optimize` | Keep geometry that can't be seen from anywhere you can be (also works with `convert` and `upgrade`) |

### upgrade: re-bake with the current version

```sh
~/frame-home/bin/fenv-tool upgrade ~/frame-home/envs/*.fenv
```

Re-bakes each file's scene with this version and keeps everything else (world, edits, sound, picture). A file that
needs nothing is left alone. A wrapped file is baked: the scene replaces its `.apk` files.

### info: what a .fenv holds

```sh
~/frame-home/bin/fenv-tool info ~/frame-home/envs/haven-calming.fenv
```

Prints the name, baked or wrapped, its sources and models, whether it has edits and a thumbnail, and its settings.

### report: what a home costs and what's wrong with it

```sh
~/frame-home/bin/fenv-tool report ~/frame-home/envs/haven-calming.fenv
~/frame-home/bin/fenv-tool report --json haven2025.apk+calming.apk
```

Lists what the home is made of, what it costs, and problems found. It ends with exit code 3 when the home is broken.
Studio shows the same under **Window > Home Report...**.

## Per-world settings

Every home costs the headset a different amount. A `.fenv` can carry its own settings, which win over the headset's
while you are in it. Every field is optional; an unset field uses the headset's setting.

| Field | Values | Meaning |
|---|---|---|
| `resolution` | 0.5 to 2 | A fixed resolution scale (1 = SteamVR's recommended size) |
| `msaa` | 1, 2, 4 | Anti-aliasing samples |
| `anisotropy` | 4, 8, 16 | Texture filtering |
| `foveation` | 1 to 3 (in between too) | Foveation strength: 1 gentle, 2 medium, 3 strong |
| `frameRate` | 0 or 1 | 0 the display's rate, 1 half rate |

Resolution, foveation and frame rate apply live. A different anti-aliasing or filtering makes Frame Home restart
itself when you switch to that home. A `.fenv` keeps these in its `fenv.json`. A Meta `.apk` in `envs/` keeps them in
a file beside it, `<name>.apk.settings.json`.

### Setting them by hand

```sh
~/frame-home/bin/fenv-tool settings my-home.fenv                                  # show
~/frame-home/bin/fenv-tool settings my-home.fenv '{"resolution": 1.2, "foveation": 2}'   # set
~/frame-home/bin/fenv-tool settings my-home.fenv --clear                          # back to the headset's
```

In Studio, the open environment's world settings have the same fields (Resolution, Foveation, MSAA, Frame rate),
with **Use Headset Settings** to clear them.

### Letting the headset work them out: fenv-tool tune

`fenv-tool tune` finds the best-looking settings the headset keeps up with in one home. Run it on the headset:

```sh
~/frame-home/bin/fenv-tool tune ~/frame-home/envs/my-home.fenv
```

How it works:

1. It starts from your headset settings.
2. For each try, it walks through the home for 30 seconds, paced at the display's rate, like a visitor: walking,
   snap turns, teleports and fast head turns. It runs beside the live compositor, as on the headset in use.
3. A try **keeps up** when it holds 120 fps with at most 1% of frames late.
4. If your settings keep up, it tries sharper foveation, half a step at a time, while it still keeps up.
   If not, it tries stronger foveation, then lower resolution (10% at a time), and as a last resort half frame rate.
   Your anti-aliasing stays as it is.
5. Before each try it waits until the headset is below 80 °C, so every try starts alike.
6. It stores the result in the `.fenv` (or `<apk>.settings.json`) with a note of when and how it was tuned, and
   prints it as JSON.

It takes a few minutes. Options:

| Option | Default | Meaning |
|---|---|---|
| `--seconds S` | 30 | Length of each walk (5 to 300) |
| `--late F` | 0.01 | Share of frames allowed to be late (0.01 = 1%) |
| `--hz N` | 120 | Frame rate to keep up with |
| `--cool C` | 80 | Wait until the headset is below C °C before each try |
| `--dry-run` | | Print the result without storing it |
| `--bench PATH` | `frame-home` beside `fenv-tool` | The Frame Home program to walk with |

From Studio, **Calculate Best Settings** in the environment's world settings does the same: it sends the home to the
headset, runs the tune there, and keeps the result in the open environment. Save to keep it in the file.

See [performance.md](performance.md) for the bigger picture.
