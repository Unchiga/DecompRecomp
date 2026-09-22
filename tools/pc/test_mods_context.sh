#!/bin/sh
# Exercise the real secondary SDL renderer without booting the game.
set -eu
cd -- "$(dirname -- "$0")/../.."
cc -m32 -std=gnu11 -O2 -ffunction-sections -fdata-sections \
    -Isrc -Itmp/port-research/psyz/external/SDL/include \
    -Itmp/pc/sdl-m32/include-revision tests/pc/mods_context_test.c \
    -Wl,--gc-sections tmp/pc/sdl-m32/libSDL3.a -lGL -ldl -lpthread -lm \
    -o tmp/pc/mods-context-test
SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_RENDER_DRIVER=opengl \
    tmp/pc/mods-context-test
