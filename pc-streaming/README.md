# PC streaming environments (experimental — not for the Steam Frame)

This folder holds Frame Home's **PC-side** attempt to make your environments SteamVR's environment on a desktop PC,
separate from the Steam Frame build. It is kept apart on purpose: it does **not** work for the Steam Frame, and the
main project ships without it.

## What's here

| File | What |
|---|---|
| `frame_home_layer_d3d11.cpp` | A proxy `d3d11.dll` that wraps SteamVR's Windows compositor and draws the home where it draws Aurora — the Direct3D 11 counterpart of the Steam Frame's Vulkan layer (`../layer/frame_home_layer.cpp`). |
| `d3d11_proxy.def` | The proxy's exports: `D3D11CreateDevice` is ours, every other export forwards to the real `d3d11.dll`. |
| `install-frame-home-pc.ps1`, `*.bat` | The Windows installer that places the layer beside SteamVR's compositor. |
| `pc-steamvr.md` | Notes on how it was meant to work. |

The renderer's Windows support (importing the compositor's textures, the named-pipe hand-over, the `--system` path)
lives in the shared sources under `src/` behind `#ifdef _WIN32` / the `FRAME_HOME_PC` CMake option, so it only
compiles into a PC build and never affects the Steam Frame build.

## Why it's set aside

The compositor layer is the right design for a **tethered** PC headset (Index, Vive, Reverb), and likely for standard
streaming (e.g. a Quest over Steam Link), where the **PC** renders the SteamVR environment. It was never confirmed on
such hardware.

It **cannot** work for the **Steam Frame**. Tracing every PC process showed the Steam Frame renders its home
environment **on the headset itself** — `vrcompositor.exe` draws nothing and the `vrlink` streaming driver makes none
of the environment's shaders; only game video is streamed. So there is nothing on the PC to replace. The home on the
Steam Frame belongs to the **headset-side Frame Home** (the Vulkan layer in `../layer/`), which runs inside the Steam
Frame's own compositor.

## Building it

With the main project configured for a PC (`FRAME_HOME_PC=ON`, the default on Windows/x86-64), the `frame-home-d3d11`
target builds `d3d11.dll` from this folder. `install-frame-home-pc.ps1` installs it.
