#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")/../.."
cc -m32 -std=gnu11 -O2 -ffunction-sections -fdata-sections \
    -Isrc -Itmp/port-research/psyz/external/SDL/include -Itmp/pc/sdl-m32/include-revision \
    tests/pc/controls_backend_test.c src/pc/platform/controls.c \
    src/pc/platform/controls_config.c src/pc/platform/controls_runtime.c \
    src/pc/platform/controls_art.c src/pc/platform/controls_window.c src/pc/platform/controls_linux.c \
    -lpng16 -Wl,--gc-sections tmp/pc/sdl-m32/libSDL3.a -lGL -ldl -lpthread -lm -o tmp/pc/controls-backend-test
SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_RENDER_DRIVER=opengl tmp/pc/controls-backend-test
