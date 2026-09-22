#!/bin/sh
# Build anything that changed, then launch the native PC game.
set -eu
cd -- "$(dirname -- "$0")"
exec ./build-pc.sh run "$@"
