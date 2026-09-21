#!/bin/sh
set -eu
preview_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
preview_binary="$preview_dir/tmp/pc-preview/memories-pc-preview"
if [ ! -x "$preview_binary" ]; then
    printf '%s\n' 'Build the native preview first; see notes/pc-build.md.' >&2
    exit 1
fi
exec "$preview_binary" "$@"
