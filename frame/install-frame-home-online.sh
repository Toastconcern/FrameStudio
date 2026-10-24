#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -m)" != aarch64 ]]; then
    echo "Frame Home must be installed on the Steam Frame (ARM64)." >&2
    exit 1
fi

for tool in curl unzip; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing required command: $tool" >&2
        exit 1
    }
done

tag=${FRAME_HOME_TAG:-latest}
if [[ -z "$tag" || "$tag" == *[^a-zA-Z0-9._-]* ]]; then
    echo "Invalid release tag: $tag" >&2
    exit 1
fi

url="https://github.com/Toastconcern/FrameStudio/releases/download/$tag/FrameHome-installer.zip"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/frame-home-install.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT

archive="$tmp/FrameHome-installer.zip"
echo "Downloading Frame Home release $tag..."
curl --fail --location --silent --show-error --retry 2 --output "$archive" "$url"
unzip -tq "$archive"
unzip -q "$archive" -d "$tmp"

installer="$tmp/FrameHome-installer/payload"
if [[ ! -f "$installer/install.sh" || ! -x "$installer/bin/frame-home" ]]; then
    echo "The downloaded release does not contain a valid Frame Home installer." >&2
    exit 1
fi

sh "$installer/install.sh" "$@"
