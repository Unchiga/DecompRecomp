#!/bin/sh
# Play the native PC game: put your disc image (the .bin of the USA disc) in
# game/, then run this. The first run builds the game, which fetches what it
# needs into tmp/ (a few minutes); later runs rebuild only what changed.
# "trace" logs unported calls, "load [slot]" starts from a save state. The
# Windows counterpart is play.bat. build-pc.sh builds for Windows as well.
set -eu
cd -- "$(dirname -- "$0")"

missing=""
for tool in python3 gcc ld objcopy nm readelf objdump; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    echo "Building the game needs:$missing"
    echo "  Debian/Ubuntu: sudo apt install gcc python3"
    echo "  Fedora:        sudo dnf install gcc python3"
    echo "  Arch:          sudo pacman -S gcc python"
    exit 1
fi
found=""
for image in game/*.bin game/*.BIN game/*.Bin; do
    [ -f "$image" ] && found=1
done
if [ -z "$found" ]; then
    echo "Put your disc image in the game folder first: the .bin of"
    echo "Yu-Gi-Oh! Forbidden Memories (USA, SLUS-01411). Any file name ending in .bin."
    mkdir -p game
    exit 1
fi
[ $# -gt 0 ] || set -- run   # "trace" and "load [slot]" build and run too
MEMORIES_SKIP_WINDOWS=1 exec ./build-pc.sh "$@"
