# Performance

The Steam Frame shows 120 frames a second. That leaves about 8.3 ms per frame. A late frame is a visible stutter.
This page explains how Frame Home keeps up, how to check it, and what to do when a home stutters. The last section
covers Frame Studio on your computer.

## How Frame Home reaches 120 fps

| What | How it helps |
|---|---|
| **Baked environments** | A baked `.fenv` holds the home already converted for the headset: textures in its GPU format with all their smaller levels, animations stored as data, geometry that can't be seen left out. Loading is quick, memory use is lower and the GPU does less. See [environments-and-fenv.md](environments-and-fenv.md). |
| **Eye-tracked foveation** | Full detail where you look, coarser toward the edges. The Steam Frame's eye tracking moves the sharp area with your gaze, so you don't see the coarse parts. The time saved goes to resolution. |
| **Foveation smoothing in the compositor** | The coarse edge areas are smoothed as SteamVR's compositor reads Frame Home's picture, instead of in an extra pass of their own. That saves GPU time on every frame. |
| **Auto resolution** | With Resolution set to Auto, Frame Home moves the resolution between 70% and 150% to keep each frame inside its time: sharper whenever the home is light enough. |
| **Per-world settings** | Each `.fenv` can carry settings tuned for that home (resolution, foveation strength, frame rate). `fenv-tool tune` and Studio's **Calculate Best Settings** find them by walking the home on the headset. |
| **Thermal governor** | A hot Steam Frame slows its GPU (to about a third in the mid-90s °C), and every frame then takes up to three times as long. Above the **Keep cool** temperature (86 °C by default), Frame Home makes the edges of view a little coarser at a time until the headset cools. Where you look stays sharp. |
| **Half frame rate** (optional) | Draws the environment every other display frame and turns it with your head in between, as Meta's home does. Each frame gets twice the GPU time. A last resort for very heavy homes. |
| **Behind the Steam menu** (optional) | Half rate there leaves more GPU to the menu and its apps. The default is full rate. |

## Checking performance

### In the headset

Open the Steam menu, then **Environments > Settings**. The **Graphics** heading shows the GPU time a frame takes
out of the time it has, the size drawn per eye, and whether eye tracking is in use. When it starts with **"Too slow,
lower Resolution"**, frames are late.

The **Debug** page helps find what is slow:

- **View > GPU cost** tints each mesh: green is cheap, red is costly.
- **Costliest meshes** lists the most expensive ones.
- **Leave out** See-through surfaces, Cut-out surfaces or Sky shows whether those are what's slow.

See [using-frame-home.md](using-frame-home.md#the-debug-page).

### From your computer

- In Studio, **Edit > Settings...** shows "Now on the headset: ..." under the Steam Frame settings, read from the
  headset. **Read from Headset** refreshes it.
- **Debug > Headset Debug...** shows and changes the headset's Debug page.
- **Debug > Home Report...** lists what the open home costs and what's wrong with it.
- Studio's own **Debug** views (Normal, Wireframe, Meshes, GPU Cost, Overdraw) and **Debug > Leave Out** work like the
  headset's, but they measure your computer's GPU, not the headset's.

On the headset, Frame Home writes its numbers to `~/frame-home/stats.json` every 2 seconds (GPU and CPU time per
frame, frame rate, late frames, resolution, temperature). `ssh frame cat frame-home/stats.json` shows them.

### Benchmark (advanced)

`frame-home --bench` draws a home the way the headset does, without SteamVR showing it, and prints what its frames
cost. Run it on the headset over SSH:

```sh
~/frame-home/bin/frame-home --bench ~/frame-home/envs/my-home.fenv --seconds 30
```

| Option | Meaning |
|---|---|
| `--seconds N` | How long to run |
| `--hz N` | The rate to pace at (default 120) |
| `--unpaced` | Run as fast as possible instead of at the display's rate |

The live environment shares the GPU, so the numbers include its work. `fenv-tool tune` uses this benchmark to walk
through a home.

## When a home stutters

Try these in order:

1. **Tune it.** On the headset: `~/frame-home/bin/fenv-tool tune ~/frame-home/envs/<home>.fenv`, or in Studio open
   the home and use **Calculate Best Settings**, then save and send it again. See
   [environments-and-fenv.md](environments-and-fenv.md#letting-the-headset-work-them-out-fenv-tool-tune).
2. **Re-bake it** if it is an older, wrapped `.fenv` or a Meta `.apk`: `fenv-tool upgrade <file>.fenv`, or open and
   save it in a current Studio. Baked homes are much lighter.
3. **Lower the settings** in **Environments > Settings**:
   - Resolution to Auto, or a lower fixed value.
   - Foveated rendering to Eye tracked, and Foveation strength up (Medium or Strong).
   - Anti-aliasing to 2x.
   - As a last resort, Frame rate to Half.
4. **Let it cool.** A hot headset is slower. Keep **Keep cool** on (86° is the default).
5. **Find the costly part** with the Debug page (GPU cost view, Costliest meshes, Leave out). In Studio you can then
   hide or delete that object, or turn on **File > Leave Out What Can't Be Seen** before saving.

Other programs running on the headset share its GPU too.

## Frame Studio's own frame pacing

Studio tries not to use your computer more than it needs to:

- It draws at your display's refresh rate while its window has focus, and at 30 fps when it doesn't.
- When nothing changes (no input for a second and nothing moving by itself), it stops drawing and waits for input.
  Playing animations, loading and saving keep it drawing.
- **Edit > Settings...** under "This PC (Frame Studio)" has Viewport resolution, Anti-aliasing, Texture limit and
  Texture filtering for big homes on slower computers.

Two environment variables help when measuring:

| Variable | Effect |
|---|---|
| `FS_FPS=n` | Draw at `n` frames a second instead of the display's rate. `0` means unlimited. |
| `FS_STATS=1` | Every 5 seconds, write the frame rate and where a frame's time goes (3D view, GPU, UI, present) to `studio.log` |

For example, on Linux: `FS_STATS=1 ./"Frame Studio"`. On Windows (Command Prompt): `set FS_STATS=1` then
`"Frame Studio.exe"`.
