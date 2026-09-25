#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")/../.."
mkdir -p tmp/pc
cc -std=gnu11 -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections -Isrc $(pkg-config --cflags freetype2 fontconfig) tests/pc/mods_preview.c \
    src/pc/platform/mods_window.c src/pc/mods/mods.c src/pc/mods/manager.c src/pc/mods/events.c \
    src/pc/mods/hooks.c src/pc/mods/mod_libc.c src/pc/mods/object_loader.c src/pc/mods/json.c \
    src/pc/platform/paths.c src/pc/platform/settings.c \
    -lpng16 -lm -lpthread $(pkg-config --libs freetype2 fontconfig) -o tmp/pc/mods-preview
MEMORIES_MODS_DIR="$PWD/mods" MEMORIES_SETTINGS="$PWD/tmp/pc/preview-mods-settings.txt" \
    MEMORIES_USER_DIR="$PWD/tmp/pc" tmp/pc/mods-preview
