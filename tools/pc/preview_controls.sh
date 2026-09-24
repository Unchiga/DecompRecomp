#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")/../.."
mkdir -p tmp/pc
cc -std=gnu11 -O2 -ffunction-sections -fdata-sections -Isrc \
    $(pkg-config --cflags freetype2 fontconfig) tests/pc/controls_preview.c \
    src/pc/platform/controls.c src/pc/platform/controls_config.c \
    src/pc/platform/paths.c src/pc/platform/controls_runtime.c src/pc/platform/controls_art.c src/pc/platform/controls_window.c \
    -lpng16 -Wl,--gc-sections $(pkg-config --libs freetype2 fontconfig) -o tmp/pc/controls-preview
MEMORIES_CONTROLS=tmp/pc/preview-controls-unused.txt tmp/pc/controls-preview
