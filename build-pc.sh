#!/bin/sh
# Build the 32-bit native game executable; pass "run" to play, or "trace" to
# log unported calls and keep going. Keys: arrows, X cross, S circle, Z square,
# A triangle, Q/W L1/R1, E/R L2/R2, T/Y L3/R3, Enter start, right Shift select,
# Esc quit. Xbox (and other evdev) controllers are picked up automatically.
# Save states: F1-F4 pick a slot, F5 saves, F7 loads; "load [slot]" starts
# from a slot (default 1). States survive rebuilds of src/pc (notes/pc-build.md).
set -eu
cd -- "$(dirname -- "$0")"
[ -f tmp/project-build/SLUS_014.11.elf ] || make -j"$(nproc)" match
[ -f tmp/pc/sdl-m32/libSDL3.a ] || [ ! -f tmp/port-research/psyz/external/SDL/CMakeLists.txt ] || tools/pc/build_sdl32.sh
python3 tools/pc/build_game32.py
case "${1:-}" in
    run) exec tmp/pc/game32/memories-pc ;;
    trace) MEMORIES_STUB_TRACE=1 exec tmp/pc/game32/memories-pc ;;
    load) MEMORIES_LOAD_STATE="${2:-1}" exec tmp/pc/game32/memories-pc ;;
esac
