#!/bin/sh
# The Steam Frame programs in frame/payload are built on the headset (arm64 SteamOS, its compositor's Vulkan driver)
# and committed; every Frame Studio download and the Frame Home installer ship them. frame/payload/SOURCE records the
# source they were built from, so Studio is never released beside an older headset build (Studio would save
# environments the headset can't open).
#
#   scripts/frame-payload.sh stamp    after copying freshly built programs into frame/payload
#   scripts/frame-payload.sh check    fails when the headset's source changed since (CI runs it before releasing)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAMP="$ROOT/frame/payload/SOURCE"

# What the headset programs are built from (studio/ is Studio's alone), as the files are now: committed or not.
source_hash() {
    list=$(mktemp)
    (cd "$ROOT" && git -c core.quotepath=off ls-files -- src third_party layer CMakeLists.txt > "$list" &&
        git hash-object --stdin-paths < "$list" | paste -d ' ' - "$list" | sha256sum | cut -c1-64)
    rm -f "$list"
}

case "${1:-}" in
    stamp) source_hash > "$STAMP"
           echo "frame/payload/SOURCE: $(cat "$STAMP")" ;;
    check) want=$(source_hash)
           have=$(cat "$STAMP" 2>/dev/null || true)
           if [ "$want" != "$have" ]; then
               echo "frame/payload is out of date: the headset's source changed since its programs were built." >&2
               echo "Rebuild frame-home, frame-switcher and fenv-tool on the Frame, copy them into frame/payload/bin," >&2
               echo "then run scripts/frame-payload.sh stamp (docs/building.md)." >&2
               exit 1
           fi
           echo "frame/payload matches the source ($want)" ;;
    *) sed -n '2,8p' "$0"; exit 2 ;;
esac
