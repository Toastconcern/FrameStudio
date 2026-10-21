# Bundles

A **bundle** (`.fbundle`) is several environments in one file: a collection to share, or to install on the headset
in one go. Each environment in it stays its own environment. On the headset, a bundle is unpacked into the `.fenv`
files it holds, and the Environments panel lists each one separately.

## Making a bundle in Frame Studio

Everything is in **File > Bundle** and on the **Bundle** tab in Properties.

1. **File > Bundle > New Bundle** starts an empty one, and the **Bundle** tab opens.
2. Give it a **Name**. It's the bundle's own name; each environment keeps its name.
3. Add environments:
   - **Add the Open Environment**: the environment you have open, as it was last saved. Save first (Ctrl+S) to
     include your latest changes. A home that has never been saved can't be added yet.
   - **My Environments ▾**: anything you've saved.
   - **Add .fenv...**: any `.fenv` file. You can pick several at once.

   Each row shows the environment's picture, name and size. **▲** moves it up: the order is kept in the file.
   **✕** takes it out of the bundle; the `.fenv` file itself stays where it is. Double-click a row to open that
   environment.
4. **Save Bundle...** writes the `.fbundle`. The environments are stored as they are, not packed a second time, so
   saving is quick, and the bundle is about as big as its environments together.

## Opening a bundle

**File > Bundle > Open Bundle...**, **File > Open...**, or dropping a `.fbundle` on the window opens the bundle on the
Bundle tab. Its environments are listed in place; nothing is unpacked yet.

- Double-click an environment to open it. It is unpacked into a scratch folder first.
- Add, remove or reorder environments, then **Save Bundle** (the same file) or **Save Bundle As...** (a new one).
- **Unpack into My Environments** copies every environment out of the bundle into My Environments. An identical
  environment already there is skipped. A different one with the same name is kept, and the bundle's copy is added
  as "name 2".

## Installing a bundle on the Steam Frame

- **From Studio**: with the bundle saved, choose **Install on Steam Frame...** on the Bundle tab or in the Bundle
  menu. The bundle is added to the **Headset** tab's install list. Then choose **Install Frame Home** there. The
  Headset tab's **Add .fenv or .fbundle...** button adds bundles too.
- **Without Studio**: give the bundle to the stand-alone installer like any environment
  (see [install-frame-home.md](install-frame-home.md)):

  ```sh
  sh install-frame-home.sh --host steamos@frame my-collection.fbundle
  ```

  On Windows, drop it onto `Install Frame Home.bat`, or run
  `powershell -ExecutionPolicy Bypass -File install-frame-home.ps1 my-collection.fbundle`.
- **By hand**: copy the `.fbundle` into `~/frame-home/envs` on the headset.

On the headset, Frame Home's Environments panel unpacks any bundle in `~/frame-home/envs` in the background. Each
environment becomes its own `.fenv`, and the bundle is then deleted to save space. An environment already there is
kept, as above. A bundle that's damaged, or still being copied, is left alone and tried again once it changes.

A bundle never changes which environment is shown. When you install `.fenv` files and bundles together, the first
`.fenv` is the one shown.

## From the command line

`fenv-tool` (built with Frame Home, see [building.md](building.md)) makes and reads bundles:

```sh
fenv-tool bundle "My collection.fbundle" --name "My collection" calm.fenv beach.fenv night.fenv
fenv-tool info "My collection.fbundle"                 # each environment: name, size, picture, damaged or not
fenv-tool unbundle "My collection.fbundle" out-folder  # the .fenv files, one each
```

## What's inside

A `.fbundle` is a ZIP archive:

```
my-collection.fbundle
├── bundle.json                 the bundle's name and its environments, in order
└── environments/
    ├── calm.fenv               each environment, stored as it is
    ├── beach.fenv
    └── night.fenv
```

```json
{
  "format": "fbundle",
  "version": 1,
  "name": "My collection",
  "environments": [
    { "file": "environments/calm.fenv", "name": "Calm", "size": 94371840 }
  ]
}
```

- The `.fenv` files are *stored*, not compressed again: they're already compressed inside, so packing and unpacking
  are just copies, and each one can be read in place (Studio reads names and pictures without unpacking).
- Bundles over 4 GB use ZIP64. Any ZIP tool can open a bundle.
- Two environments with the same file name get "name 2"; names are compared without regard to case, so they unpack
  separately on Windows too.
- Every member's checksum is verified when it's unpacked: a damaged bundle stops without leaving half a file.

The format is in `src/fenv_bundle.h`; `.fenv` itself is described in
[environments-and-fenv.md](environments-and-fenv.md).
