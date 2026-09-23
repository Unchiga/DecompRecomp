#!/bin/sh
# Build the 32-bit native game executable; pass "run" to play, or "trace" to
# log unported calls and keep going. Keys: arrows, X cross, S circle, Z square,
# A triangle, Q/W L1/R1, E/R L2/R2, T/Y L3/R3, Enter start, right Shift select,
# Esc quit. Xbox (and other evdev) controllers are picked up automatically.
# Save states: F1-F4 pick a slot, F5 saves, F7 loads; "load [slot]" starts
# from a slot (default 1). States survive rebuilds of src/pc (notes/pc-build.md).
# The Windows executable (tmp/pc/win32/memories-pc.exe) is cross-built too, so
# both stay working; "run-windows" plays it under Wine. MEMORIES_SKIP_WINDOWS=1
# builds Linux only (play.sh does). The first build fetches the Linux
# libraries and SDL3 (tools/pc/build_linux_sysroot.py), and llvm-mingw and
# the Windows libraries (tools/pc/build_win32_deps.py), into tmp/pc. The
# matching build is not needed: its addresses are in config/pc/guest_addresses.txt.
set -eu
cd -- "$(dirname -- "$0")"
python3 tools/pc/build_game32.py
if [ "${MEMORIES_SKIP_WINDOWS:-0}" != 1 ]; then
    [ -f tmp/pc/win32-deps/lib/libfreetype.a ] && [ -d tmp/pc/llvm-mingw ] || python3 tools/pc/build_win32_deps.py
    python3 tools/pc/build_game32.py --target windows
    # Both builds and every mod share one layout of the game's structures.
    python3 tools/pc/check_layouts.py
fi
case "${1:-}" in
    run) exec tmp/pc/game32/memories-pc ;;
    trace) MEMORIES_STUB_TRACE=1 exec tmp/pc/game32/memories-pc ;;
    load) MEMORIES_LOAD_STATE="${2:-1}" exec tmp/pc/game32/memories-pc ;;
    run-windows)
        # A prefix of its own under tmp/pc, without the Mono and Gecko installers.
        WINEPREFIX="$PWD/tmp/pc/wine-prefix" WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all \
            exec wine tmp/pc/win32/memories-pc.exe ;;
esac
