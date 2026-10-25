# Changelog

Newest first. When this file changed since the last release, the next release's notes start with the top section,
and the notes post to the Discord's #releases channel: add a section at the top in the push you want announced.
The version: VERSION (major.minor) and the commits since VERSION changed, so the commit that bumps VERSION is .0.

## v0.2.0

### Frame Studio
- Gaussian splats: open or add 3DGS captures (.ply, .splat, .spz from Polycam, Luma, Scaniverse, Postshot...) as splat objects: listed in the Outliner, picked in the view, moved, rotated and scaled with the gizmo (undoable), hidden or deleted, with a headset budget in Properties (over 400,000 may need Half frame rate; over a million is asked about on Save and Send). Saved into the .fenv as .spz files.
- Newer .spz splat files (version 4, the kind Niantic's current spz tools save) open too.
- A saved environment records which Frame Studio made it, shown in Properties > Environment, on the start screen and by fenv-tool info.
- The version is shown at the bottom left (and in the Help menu), the same number as the release.
- Properties tabs are labelled: each shows its icon and name across the top, the open one underlined.
- The open tab of an area (Properties, Outliner) and the open workspace are underlined, so you can tell which is which.
- Buttons in the World, Environment, Bundle and Headset tabs sit in rows at their own size, as in the Object tab,
  instead of stretching across the panel.
- Moving things: click to select, then drag the selected item to move it. Clicking something no longer nudges it
  (or a large mesh behind it) when the mouse moves a little.
- Outliner folders have colours, one for each level of nesting, instead of plain grey.
- Removed "Stop at Meta's invisible walls" from the World tab.
- Help › Join the Discord, and a Discord button on the start screen: https://discord.gg/yJDn59ErgK
- New app icon (a roof over a headset) on Windows, macOS and Linux.
- Opens STL (3D printing, CAD) and PLY (scans, photogrammetry) models, alongside glTF, FBX and OBJ.
- PLY meshes show their own vertex colours (with see-through alpha when the file has it), in Studio and on the
  headset. glTF models' vertex colours show too. Models without colours look the same as before.
- Environment tab › Sky: pick a 360° panorama (.hdr, .jpg or .png, twice as wide as it is tall) to use as the sky
  instead of the home's own. Remove puts the home's sky back. It's saved in the .fenv, so Frame Home shows it as it is.
- Pictures can also be TGA, GIF (its first frame) and PSD (the flattened image).
- Meta homes look right in more places (Studio and Frame Home):
  - Textures stored uncompressed (8-bit or half-float) load properly. They came out as streaky noise or pixelated
    (Focused) or black (Horror's lanterns and bats).
  - A vista's meshes whose shaders ship in its footprint draw when the footprint is in the same folder (Horror's
    front garden, a Calming boulder).
  - Meshes with no material in the scene use the one they were exported with (Horror's sky and candle flame,
    Oceanarium's light rays and surface waves).
  - Vista (Cascadia)'s floors are matte again instead of mirror-like: its reflection map keeps the blurred levels Meta
    made for rough surfaces. Convert an environment made from it before this again to get the fix.
  - Things placed inside another placed group sit where they belong (Haven2025's teleport spots by each seat were
    all in the middle of the room).
- Environments can say which newer features they need; a Frame Studio or Frame Home too old for one says so instead of
  showing it wrong.
- Fixed a crash in the Bundle tab: taking an environment out of a bundle (✕) could close Frame Studio, because its
  picture was freed while it was still being drawn.
- Opening an environment that already has a saved walkable area no longer marks it as changed: the area is shown
  exactly as saved until you edit something, so just opening and closing it doesn't ask you to save.
- Dragging the gizmo can no longer leave an object, a splat or a world item broken (gone from view) when the gizmo
  gives a bad result for a moment, such as a rotate ring dragged edge-on.
- Placed surfaces far from the middle of the world join up as reliably as ones near it.
- Updates are checked before they're installed: a download that was cut off, damaged or not the real file is refused
  and nothing is changed. The files an update sets aside are cleaned up even when the old copy was still running.
- An environment named "." or ".." (typed, or in a .fenv someone sent) could make the 5-minute auto-save delete
  environments in My Environments. Names like that, and Windows device names (CON, NUL...), are now changed into
  safe file names.
- Saving an environment opened from inside a bundle saves it into My Environments; before, Save wrote into a scratch
  copy that was emptied the next time a bundle member was opened.
- Object edits are found again when the environment's name, or your Windows user name, has letters like é or ü
  (they were silently missing, and saving then dropped them), and the edits file is written whole or not at all.
- An environment whose splat placement or a carried-over setting became "infinity" or "not a number" could never be
  opened again after saving: those values are now saved as safe ones. Typing such a number into a Location, Rotation
  or Scale field leaves the value as it was.
- Saving a bundle over an existing one no longer loses both when the old file can't be replaced, and a bundle can't
  be saved over one of its own environments.
- A bundle with many environments no longer leaves the 3D view blank after a resize.
- Running out of memory opening a huge model, or saving a huge home, shows a message instead of closing Frame Studio.
- An OBJ model whose .mtl file is missing opens (without its materials) instead of not at all.
- Damaged or hostile files are handled safely: .spz files that unpack to gigabytes, PLY files with impossible
  counts or indices, glTF files with a broken index count, oversized pictures in a .fenv or bundle.
- No more freeze after an edit: the walkable area is worked out in the background after you move, delete or undo
  something. Big homes used to stop the window for up to 1.5 seconds each time (Hand Tutorial 4).
- The 3D view follows the mouse a frame sooner: orbiting, panning, flying and gizmo drags show in the same frame,
  instead of one refresh behind the rest of the window.
- Frames come at an even pace. On Windows they're timed to the display's refreshes, which cuts the wait between
  your input and the screen.
- Selecting a big mesh no longer holds up a frame for as long (its outline builds about twice as fast).
- The Bundle tab costs a third as much time per frame.
- Two Frame Studio windows can no longer install an update at the same time (the second says another is installing
  it), and an update cut off halfway, by a crash or a power cut, is finished or undone the next time Frame Studio
  starts instead of leaving a mix of old and new files.
- Closing an environment in the same moment the 3D view is resized no longer shows a frame of garbage in the view.
- An update stopped at the worst moment (a crash or power cut while Frame Studio swaps in the new program) can no
  longer leave no program to start: the old program is replaced by the new one in a single step.
- On Windows, homes, models and environments in folders over 260 characters deep open, import and save (where
  Windows' long paths setting is on).
- With letters like é or ü in your Windows user name, compiled shaders are now kept between runs instead of being
  compiled again every time Frame Studio starts.
- glTF models whose index data doesn't start on an even byte are read safely.
- Copy and paste: Ctrl+C, Ctrl+X (cut), Ctrl+V and Ctrl+D (duplicate), also in the Edit menu and the right-click
  menus, for objects, splat objects, spawns, seats, teleport hotspots, surfaces, invisible walls and gravity zones.
  Paste puts the copy where the view is looking (at the same height over the floor), or right beside the original if
  you're looking at it; Duplicate always puts it beside. It works across environments opened in the same Frame Studio,
  and world items also paste into another Frame Studio window. One Ctrl+Z undoes it. Shortcuts do nothing while
  you're typing in a text box.
- A new seat's "stand up here" spot is now in front of it (the way its arrow points) instead of behind it.

### Frame Home
- Sitting down turns you to face the way the seat faces (its arrow in Frame Studio), wherever you were looking when
  you pointed at it or teleported onto it. Before, you kept facing the way you were, so you could end up sitting
  sideways or backwards.
- Gaussian splats: environments with splat captures draw them (sorted back to front as you move, both eyes in one pass); a .fenv that has splats needs this Frame Home (older ones refuse it rather than show it without them). The Environments panel shows which Frame Studio made each environment.
- The Debug page scrolls (mouse wheel and drag).
- Settings shows Frame Home's version at the bottom.
- The dashboard tab has the new icon.
- An environment made by a newer Frame Studio says "Needs a newer Frame Home" on its card.
- Smoother movement at any frame rate: walking and turning step by exactly one display frame's time per frame, so the
  speed no longer wobbles with the frame timing.
- Half frame rate: frames are timed to the display (every other refresh, not a loose sleep), the frame shown twice is
  turned on with your smooth turn instead of standing still, and while you walk, turn, dash or blink every frame is
  drawn (when the home is light enough for that).
- A frame that comes late, or is shown twice, follows your smooth turning and (approximately) your walking, instead of
  lagging behind for a refresh.
- Blink teleports fade to black and back in the system environment (they were an unfaded freeze, then a jump).
- Teleporting near chairs and stools goes where you aim: the arc takes a seat only when it comes down onto it, not when
  it passes a stool on the way or lands beside a desk (Haven2025 kept sitting you in the desk chair). Every spot of
  Haven2025's floor can be teleported to.
- Sitting down no longer throws you straight back out: the stick has to come back to the middle before pushing it
  stands you up.
- While a game is on screen the home stops completely (no drawing, no sound, nothing read from the controllers), so
  the GPU and CPU are all the game's. It keeps going while a game loads, and is back the moment the game closes.
- The Hand Tutorial homes draw faster: their hundreds of look-alike plants (see-through ones too) and other matching
  parts are drawn together instead of one at a time. They look exactly the same. Hand Tutorial 7 takes about 20% less
  GPU time a frame, the other Hand Tutorials 4 to 9% less.
- Updates are checked before they're installed: the downloaded installer must match the SHA-256 the release lists, so
  a download cut short or damaged (or one the release lists no SHA-256 for) is never installed, and a failed update no longer says
  "Frame Home is updated" the next time the panel starts. The installer is removed from the headset once unpacked.
- Installing never leaves new Frame Home programs beside an old compositor layer: everything is copied first, and a
  full disk changes nothing. Installing from a computer reports an environment that didn't copy, and doesn't make a
  missing one the environment shown.
- A damaged environment file can no longer hang or crash Frame Home while loading: walls and floors with absurd or
  broken coordinates, shaders with types that contain themselves, texture data shorter than it claims and settings
  that aren't numbers are caught, and a loader that runs out of memory fails that home instead of closing Frame Home.
- The system environment: no more single-frame flashes of Aurora (or of the home over passthrough) from a clock race
  in the compositor layer, a frame carried on during a fast dash no longer swings sideways, and the layer refuses a
  damaged hand-over instead of crashing the compositor.
- A tracking glitch that reports a broken (not-a-number) head pose is skipped for that frame instead of being fed to
  walking and collisions.
- The system environment, while the home changes size (a resolution or foveation setting): the compositor no longer
  shows a frame turned to the wrong angle (or half of one frame's angle and half of the next), and no longer redraws
  with pictures it had already let go of. An older Frame Home with this compositor layer, or the other way round,
  still works.
- A home with damaged shaders leaves out the parts that use them (and says so) instead of handing broken code to the
  graphics driver. The check takes about 5 ms for all 172 of Haven's shaders.
- A home shown for more than 30 days is no longer deleted from the cache while it's on screen when another home is
  opened (Frame Home and Frame Studio): a home still in use anywhere is skipped.
- Lower latency in the system environment: the head pose is read again just before each frame goes to the GPU (after
  its drawing commands are made and the frame before is finished), so what you see is up to a millisecond or more
  newer, and several milliseconds newer when the GPU is busy.
- Desert Terrace and other homes with cut-out foliage draw it a little faster (about 2% less GPU time in Desert
  Terrace): the leaves' second pass tests depth before shading. They look exactly the same.
- The system environment: each frame Frame Home draws is now handed over to the compositor properly before the
  compositor reads it (Vulkan's hand-over between programs), so a GPU driver that keeps image data in caches or
  compressed can't show a stale or garbled frame. It costs no measurable GPU time, and an older Frame Home with this
  compositor layer, or the other way round, still works.
- With foveation, the pass that smooths the low-detail edges now waits until its picture is ready to be drawn into
  (a missing GPU wait that could, in principle, leave stray pixels).
