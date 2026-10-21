#!/bin/sh
# Frame Home installer — runs on the Steam Frame. Frame Studio uploads this folder and runs it over ssh
# (Frame > Install on Steam Frame); it also works by hand:
#   sh install.sh               install or update Frame Home and make it the headset's environment
#   sh install.sh uninstall     give the environment back to SteamVR (environments and binaries stay)
#
# Frame Home replaces Aurora, SteamVR's own environment: a Vulkan layer in SteamVR's compositor draws your home where
# Aurora goes (layer/), and frame-home --system renders it; the Environments panel (frame-switcher, started with
# SteamVR) keeps that running. No app, no dock entry; it shows at boot, behind the Steam menu and during game loads.
# Nothing is changed under /opt or /usr. ~/frame-home/layer/off turns it off (Aurora as usual).
set -u
P=$(cd "$(dirname "$0")" && pwd)
H="$HOME/frame-home"
CFG="${XDG_CONFIG_HOME:-$HOME/.config}/openvr/config"
MODE="${1:-install}"
say() { echo "frame-install: $*"; }
chmod +x "$P"/bin/* 2>/dev/null        # copied from Windows: no executable bit

# SteamVR not running: write the same registration into its config files (it reads them when it starts).
offline() {
    python3 - "$CFG" "$1" "$H" <<'PY'
import json, os, sys
cfg, mode, home = sys.argv[1:4]
def load(p):   # missing: start empty; there but unreadable: stop (saving would wipe the user's SteamVR settings)
    try:
        with open(p) as f: return json.load(f)
    except FileNotFoundError: return {}
    except Exception as e: sys.exit('frame-install: %s does not read (%s): left as it is' % (p, e))
def save(p, d):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p + '.tmp', 'w') as f: json.dump(d, f, indent=3)
    os.replace(p + '.tmp', p)
ac = os.path.join(cfg, 'appconfig.json'); a = load(ac); paths = a.setdefault('manifest_paths', [])
for m in (os.path.join(home, 'frame-home.vrmanifest'), os.path.join(home, 'frame-switcher.vrmanifest')):
    if mode == 'install' and m not in paths: paths.append(m)
    if mode == 'uninstall' and m in paths: paths.remove(m)
save(ac, a)
vs = os.path.join(cfg, 'steamvr.vrsettings'); s = load(vs)
s.setdefault('steamvr', {})['enableHomeApp'] = False   # the old way (Frame Home as SteamVR's home app) stays off
if mode == 'install': s['steamvr']['environmentMode'] = 1   # Aurora: what the layer replaces
s['steamvr'].pop('globalActionSetPriority', None)   # earlier versions took the sticks over the Steam menu: the menu's again
save(vs, s)
if mode == 'install':
    save(os.path.join(cfg, 'vrappconfig', 'framehome.switcher.vrappconfig'), {'autolaunch': True, 'last_launch_time': '0'})
PY
}

LAYERJSON="$HOME/.local/share/vulkan/implicit_layer.d/frame_home_layer.json"
if [ "$MODE" = uninstall ]; then
    mkdir -p "$H/layer"; : > "$H/layer/off"; rm -f "$LAYERJSON"           # Aurora is back once SteamVR restarts
    pkill -f "^$H/bin/frame-home --system" 2>/dev/null
    if [ -x "$P/bin/frame-home" ]; then "$P/bin/frame-home" --uninstall; else "$H/bin/frame-home" --uninstall; fi; rc=$?
    [ $rc -eq 3 ] && { offline uninstall; rc=$?; }
    [ $rc -eq 0 ] && say "SteamVR's own environment is back" || say "uninstall failed ($rc)"
    exit $rc
fi

[ -x "$P/bin/frame-home" ] || { say "this installer has no Frame Home binaries"; exit 1; }
# Missing libraries (or a build for another machine) show up here, before anything installed is touched, rather than
# as a silent black environment later.
if ! LD_LIBRARY_PATH="$P/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$P/bin/frame-home" --version >/dev/null 2>&1; then
    missing=$(ldd "$P/bin/frame-home" 2>/dev/null | grep "not found" | head -3 | tr -s ' \t\n' ' ')
    say "Frame Home can't run on this headset: ${missing:-unknown error} (nothing was changed)"; exit 1
fi
mkdir -p "$H/bin" "$H/envs" || exit 1
# New binaries beside the old ones, then renamed over them: a running copy keeps its file until it restarts.
for x in frame-home frame-switcher fenv-tool; do
    [ -f "$P/bin/$x" ] || continue
    cp -f "$P/bin/$x" "$H/bin/$x.new" && chmod +x "$H/bin/$x.new" && mv -f "$H/bin/$x.new" "$H/bin/$x" || { say "couldn't install $x"; exit 1; }
done
# The compositor layer and its shader (renamed over the old ones: a running compositor keeps its copy).
layerChanged=0
if [ -f "$P/layer/libframe_home_layer.so" ]; then
    mkdir -p "$H/layer" "$(dirname "$LAYERJSON")"
    cmp -s "$P/layer/libframe_home_layer.so" "$H/layer/libframe_home_layer.so" || layerChanged=1
    cmp -s "$P/layer/env_ps.spv" "$H/layer/env_ps.spv" || layerChanged=1
    for f in libframe_home_layer.so env_ps.spv; do cp -f "$P/layer/$f" "$H/layer/.$f.new" && mv -f "$H/layer/.$f.new" "$H/layer/$f"; done
    rm -f "$H/layer/off" "$H/layer/starting" "$H/layer/crashes"
    [ -f "$LAYERJSON" ] || layerChanged=1
    cat > "$LAYERJSON" <<JSON
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_FRAMEHOME_compositor",
    "type": "GLOBAL",
    "library_path": "$H/layer/libframe_home_layer.so",
    "api_version": "1.4.309",
    "implementation_version": "1",
    "description": "Frame Home: replaces the SteamVR compositor environment (active only in vrcompositor)",
    "functions": { "vkNegotiateLoaderLayerInterfaceVersion": "frameHome_NegotiateLoaderLayerInterfaceVersion" },
    "disable_environment": { "FRAME_HOME_LAYER_DISABLE": "1" }
  }
}
JSON
fi
# Libraries the binaries need that the headset may not have (none today; kept for future builds).
[ -d "$P/lib" ] && { mkdir -p "$H/lib"; cp -f "$P"/lib/* "$H/lib/"; }
# Environments that came with this install.
for f in "$P"/envs/*.fenv; do [ -f "$f" ] && mv -f "$f" "$H/envs/"; done

# The Environments panel starts with SteamVR and keeps frame-home --system running. Restarted here so the new
# binaries take over now; the compositor too when the layer changed (SteamVR brings it straight back).
if pgrep -x vrserver >/dev/null; then
    pkill -x frame-switcher 2>/dev/null; pkill -f "^$H/bin/frame-home --system" 2>/dev/null
    for i in $(seq 1 20); do pgrep -x frame-switcher >/dev/null || break; sleep 0.5; done   # gone before relaunching
    "$P/bin/frame-switcher" --install; rs=$?
    [ $rs -eq 0 ] || { say "SteamVR refused the Environments panel ($rs)"; exit 1; }
    sleep 3
    if ! pgrep -x frame-switcher >/dev/null; then   # SteamVR still counted the old one as running: start it ourselves
        (cd "$H/bin" && STEAMVR_APPKEY=framehome.switcher setsid nohup ./frame-switcher >/dev/null 2>&1 </dev/null &)
    fi
    if [ $layerChanged -eq 1 ]; then pkill -x vrcompositor 2>/dev/null; fi
    say "installed — Frame Home is now the headset's environment (in Aurora's place)"
else
    "$P/bin/frame-switcher" --install >/dev/null 2>&1   # writes its manifest (SteamVR isn't there to register it)
    offline install || { say "couldn't register with SteamVR"; exit 1; }
    say "installed — Frame Home becomes the environment when SteamVR next starts"
fi
# First environment: the newest one installed, unless one is already chosen.
if [ ! -s "$H/current" ]; then
    first=$(ls -t "$H"/envs/*.fenv 2>/dev/null | head -1)
    [ -n "$first" ] && basename "$first" > "$H/current"
fi
# Thumbnails for the switcher, in the background at low priority.
(nohup "$H/bin/frame-switcher" --thumbs >/dev/null 2>&1 &)
exit 0
