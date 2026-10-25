#!/bin/sh
# Installs Frame Home on a Steam Frame from a Linux or macOS computer, without Frame Studio: the same steps as
# Studio's Frame > Install on Steam Frame. Needs ssh access to the headset (its developer key, or a password).
#
#   sh install-frame-home.sh [--host HOST] [--uninstall] [environment.fenv | bundle.fbundle ...]
#
# HOST: the headset's ssh address (default "steamos@frame": an entry named frame in ~/.ssh/config, or the
# headset's name on the network; an IP works too, e.g. steamos@192.168.1.50). Environments (.fenv) given are
# copied into its selector as well; the first .fenv is the one shown. Bundles (.fbundle) are copied as they are and
# the headset's Environments panel unpacks them into their environments (they never change the one shown).
# --uninstall gives the environment back to SteamVR (files stay).
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
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) ENVS="$ENVS
$1"; shift ;;
    esac
done
case "$HOST" in *@*) ;; *) HOST="steamos@$HOST" ;; esac
say() { echo "frame-home: $*"; }
# The name a file gets on the headset: lowercase, spaces as dashes, only [a-z0-9._-] (as Studio names them). When
# that drops characters (accents, other scripts) a short hash of the real name is added, so two such names don't
# become the same file, and an all-non-Latin one doesn't become a bare ".fenv".
frame_name() {
    base=$(basename "$1")
    low=$(printf '%s' "$base" | tr 'A-Z ' 'a-z-')
    n=$(printf '%s' "$low" | tr -cd 'a-z0-9._-')
    if [ "$n" != "$low" ] || [ -z "${n%.*}" ]; then
        h=$(printf '%s' "$base" | cksum | cut -d' ' -f1)
        stem=${n%.*}
        n="${stem:-environment}-$h.${n##*.}"
    fi
    printf '%s' "$n"
}
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
        name=$(frame_name "$f")
        say "copying $(basename "$f") -> $name"
        scp -q "$f" "$HOST:frame-home/envs/$name.part" && ssh "$HOST" "mv -f frame-home/envs/$name.part frame-home/envs/$name" || {
            ssh "$HOST" "rm -f frame-home/envs/$name.part" || true; say "couldn't copy $(basename "$f") (the headset's disk is full, or the connection dropped)"; }
    done
    ssh "$HOST" "sed -i 's/\r\$//' frame-home/.install/frame/install.sh; sh frame-home/.install/frame/install.sh"
    # the first environment given is the one shown (a bundle's are only known once the headset unpacks it: skipped)
    FIRST=$(printf '%s\n' "$ENVS" | grep -i '\.fenv$' | sed -n '1p')
    if [ -n "$FIRST" ] && [ -f "$FIRST" ]; then
        name=$(frame_name "$FIRST")
        # (only when it got there: a copy that failed leaves the one shown as it was)
        ssh "$HOST" "if [ -f 'frame-home/envs/$name' ]; then printf '%s\n' '$name' > frame-home/current; fi"
    fi
else
    # (never installed from a copy like this one: Frame Home itself puts SteamVR's environment back)
    ssh "$HOST" "if [ -f frame-home/.install/frame/install.sh ]; then sh frame-home/.install/frame/install.sh uninstall; else frame-home/bin/frame-home --uninstall; fi"
fi
say "done"
