# Third-party sources

Code the port vendors unchanged from elsewhere, with its license.

| File | What | From | License |
|---|---|---|---|
| `stb_vorbis.c` | Ogg Vorbis decoder, v1.22 (audio replacement, `src/pc/audio/vorbis.c`) | <https://github.com/nothings/stb> | public domain, or MIT (the choice is in the file's last lines) |

Update a file by fetching the upstream version over it; do not edit it in
place. Warnings it raises are silenced in the unit that includes it.
