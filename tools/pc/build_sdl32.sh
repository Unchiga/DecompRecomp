#!/bin/sh
# Build PSY-Z's bundled SDL3 as a 32-bit static library for the game executable
# (tools/pc/build_game32.py links it when tmp/pc/sdl-m32/libSDL3.a exists).
set -eu
cd -- "$(dirname -- "$0")/../.."
src=tmp/port-research/psyz/external/SDL
[ -f "$src/CMakeLists.txt" ] || { echo "SDL source missing at $src (PSY-Z checkout with its external/SDL submodule)" >&2; exit 1; }
cmake -S "$src" -B tmp/pc/sdl-m32 -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 \
    -DCMAKE_ASM_FLAGS=-m32 -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF
cmake --build tmp/pc/sdl-m32 -j"$(nproc)"
