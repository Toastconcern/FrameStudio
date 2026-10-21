#!/bin/sh
# Installs Frame Home on a Steam Frame from a Linux or macOS computer, without Frame Studio: the same steps as
# Studio's Frame > Install on Steam Frame. Needs ssh access to the headset (its developer key, or a password).
#
#   sh install-frame-home.sh [--host HOST] [--uninstall] [environment.fenv ...]
#
# HOST: the headset's ssh address (default "steamos@frame": an entry named frame in ~/.ssh/config, or the
# headset's name on the network; an IP works too, e.g. steamos@192.168.1.50). Environments (.fenv) given are
# copied into its selector as well. --uninstall gives the environment back to SteamVR (files stay).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PAYLOAD="$HERE/payload"
HOST=steamos@frame
MODE=install
ENVS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --host) HOST=$2; shift 2 ;;
        --uninstall) MODE=uninstall; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) ENVS="$ENVS
$1"; shift ;;
    esac
done
case "$HOST" in *@*) ;; *) HOST="steamos@$HOST" ;; esac
say() { echo "frame-home: $*"; }
[ -f "$PAYLOAD/install.sh" ] || { say "the headset files are missing: expected $PAYLOAD/install.sh"; exit 1; }

say "connecting to $HOST"
ARCH=$(ssh -o ConnectTimeout=8 "$HOST" "mkdir -p frame-home/envs frame-home/.install && uname -m") || {
    say "can't reach $HOST over ssh (is the headset on, in developer mode, with ssh enabled?)"; exit 1; }
[ "$ARCH" = aarch64 ] || { say "$HOST is a $ARCH machine, not a Steam Frame"; exit 1; }

if [ "$MODE" = install ]; then
    say "copying Frame Home"
    ssh "$HOST" "rm -rf frame-home/.install/frame"
    scp -q -r "$PAYLOAD" "$HOST:frame-home/.install/frame"
    printf '%s\n' "$ENVS" | while IFS= read -r f; do
        [ -n "$f" ] || continue
        [ -f "$f" ] || { say "no such file: $f"; continue; }
        # the name it gets on the headset: lowercase, spaces as dashes, only [a-z0-9._-] (as Studio names them)
        name=$(basename "$f" | tr 'A-Z ' 'a-z-' | tr -cd 'a-z0-9._-')
        say "copying $(basename "$f") -> $name"
        scp -q "$f" "$HOST:frame-home/envs/$name.part" && ssh "$HOST" "mv -f frame-home/envs/$name.part frame-home/envs/$name"
    done
    ssh "$HOST" "sed -i 's/\r\$//' frame-home/.install/frame/install.sh; sh frame-home/.install/frame/install.sh"
    # the first environment given is the one shown
    FIRST=$(printf '%s\n' "$ENVS" | sed -n '/./{p;q;}')
    if [ -n "$FIRST" ] && [ -f "$FIRST" ]; then
        name=$(basename "$FIRST" | tr 'A-Z ' 'a-z-' | tr -cd 'a-z0-9._-')
        ssh "$HOST" "printf '%s\n' '$name' > frame-home/current"
    fi
else
    # (never installed from a copy like this one: Frame Home itself puts SteamVR's environment back)
    ssh "$HOST" "if [ -f frame-home/.install/frame/install.sh ]; then sh frame-home/.install/frame/install.sh uninstall; else frame-home/bin/frame-home --uninstall; fi"
fi
say "done"
