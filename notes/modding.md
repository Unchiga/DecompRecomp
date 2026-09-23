# Mods

A mod is a directory, not part of the game executable. The release ships its
own mods that way, and anyone else's mod is installed the same way: drop the
directory in, restart, apply it in **Game > Mods**.

## Where mods live

| Directory | What is in it |
|---|---|
| `mods/` beside the executable | the mods the release ships (`3d-monsters`, `hand-camera`) |
| `mods/` in the user directory | mods the player installed |

The user directory is where everything the player owns lives: settings,
controls, memory cards, save states, screenshots, their mods and whatever a
mod stores. It is `Documents\My Games\YFM Re-Decomp` on Windows and
`$XDG_DATA_HOME/YFM Re-Decomp` (`~/.local/share/YFM Re-Decomp`) elsewhere; see
[`src/pc/platform/paths.h`](../src/pc/platform/paths.h). A mod in the user
directory replaces one the release ships with the same `id`.

| Variable | Effect |
|---|---|
| `MEMORIES_USER_DIR` | the user directory, instead of the platform's |
| `MEMORIES_MODS_DIR` | the only directory scanned for mods |
| `MEMORIES_MODS=0` | load no mods at all this run |
| `MEMORIES_MOD_<ID>=0/1` | settle one mod for this run; the id uppercased, everything that is not a letter or a digit an underscore (`MEMORIES_MOD_3D_MONSTERS=1`) |
| `MEMORIES_MOD_<ID>_<KEY>=n` | one of a code mod's settings for this run, over what the settings file says (`MEMORIES_MOD_3D_MONSTERS_SCALE=5000`) |
| `MEMORIES_TRACE=mods` | log what the mod system finds, loads and overrides |

## The manifest

Every mod has a `mod.json`:

```json
{
    "id": "card-tweaks",
    "name": "Card tweaks",
    "version": "1.0",
    "author": "someone",
    "description": "What it does, in a sentence.",
    "library": "card-tweaks",
    "enabled": false,
    "restart": false,
    "data": []
}
```

| Key | Meaning |
|---|---|
| `id` | the name the settings and the user directory use; the directory's name when it is left out |
| `name` | what the Mods window shows |
| `library` | the mod's code: an object file relative to its directory, `.o` added when there is no suffix. The same file serves every system. Leave it out for a mod that is only data |
| `enabled` | whether the mod is applied the first time the game sees it |
| `restart` | whether changing it needs a fresh process. Data overrides default to `true`, because the game reads most of what they change while it starts; code mods default to `false` |
| `legacy_setting` | an older settings key to read the player's choice from, once |
| `data` | what the mod changes on the disc, below |
| `textures` | a directory inside the mod holding a texture pack, below |

`version`, `author` and `description` are for people; the game does not read
them.

## Data mods: no code at all

`data` is a list of entries, each naming a file on the disc by its retail
path (`"\\DATA\\CARD.MRG;1"`, as the game asks for it) or a raw sector
(`"lba"`), and either replacing it or patching bytes in it:

```json
"data": [
    { "file": "\\DATA\\CARD.MRG;1", "replace": "card.mrg" },
    { "file": "\\DATA\\WA_MRG.MRG;1",
      "patch": [ { "at": "0x5D800", "bytes": "26 25" } ] }
]
```

* `replace` names a file the mod ships. It stands in for the whole file: the
  sectors past its end read as zeroes, and anything past the original file's
  length is ignored, because the sectors after it belong to the next file.
  Replacing a raw `lba` region needs a `sectors` count as well.
* `patch` writes `bytes` (hexadecimal, spaces optional) at `at`, an offset
  into the file, or into the sector when the entry names an `lba`. A run that
  crosses a sector boundary is fine. This is the shape the community's
  hex-editor tutorials are written in, so their offsets carry over directly
  (`modding-tutorial-gameplay-patches.md`).

Overrides stand in for the disc for every reader in the port: the drive
model, the bulk reads a mod makes, and the file lookup itself. Nothing on the
real disc image is touched, and removing the mod puts the game back exactly
as it was.

## Texture packs: images by origin

A mod may carry a `textures` directory: PNGs named by where their images
come from on the disc, with a `manifest.json` describing each one, exactly
what `tools/pc/extract_images.py` writes (`notes/pc-build.md`, "Images from
the disc"). Extract the family you want to repaint (`cards`, `portraits`,
or `--assets` for what a capture drew), paint over the PNGs, and point a
manifest at the directory:

```json
{
    "id": "hd-portraits",
    "name": "HD portraits",
    "textures": "images"
}
```

While the mod is applied, every upload the game makes from the disc is
traced to its bytes (`src/pc/render/texture_dump.c`), and the words an image
of the pack covers get its pixels in a shadow of VRAM; a primitive that
samples them through the palette the image was extracted with takes them
from the shadow instead (`texture_pack.c`). A pack image may be any size:
at the console's resolution it is resampled to the texture's own size, and
at an internal resolution (View > Internal 2x, 4x; `notes/pc-build.md`) it
is sampled at its own, so a bigger image shows its detail there. The
palette rule is what keeps a sprite the game draws through several
palettes (a selection bar, a greyed icon) looking right: only the palette
the image was made for is replaced. One pack is active at a time; the
extracted images themselves are the game's, so a pack ships painted images
or a way to make them from the player's own disc, never the originals.

`tools/pc/upscale_pack.py` makes a pack of upscaled images from an extracted
set with Upscayl's command-line binary (Real-ESRGAN on the GPU): the same
files and manifest, enlarged (`--scale 4` by default, one to one with
Internal 4x; `--scale 25 --passes 2` is the Upscayl window's "5x, twice"),
with `mod.json` written beside them and, with `--zip`, the mod folder in a
zip that unpacks into a `mods` directory (the user directory's, for anyone
who downloads it). To cover a screen, play it once with
`MEMORIES_DUMP_TEXTURES=<dir>` from a cold boot, then
`extract_images.py --assets <dir>/assets.txt --out <images>`: every image
the screen drew from the disc is in `<images>/assets`, backgrounds as the
128x128 tiles the game uploads them in. Keep the scale in proportion: the
game holds a pack's images in memory at full size, and a 128x128 tile at
25x is 40 MB.

## Code mods

A code mod is **one object file**, `<library>.o`, that runs on both the
Linux and the Windows game. Nobody builds a mod twice. Both games are 32-bit
x86 code with the same calling convention, so the machine code is the same;
the game reads the file with its own loader
([`src/pc/mods/object_loader.c`](../src/pc/mods/object_loader.c)) rather than
the system's, so the container is the same too.

The mod exports one function, described in
[`src/pc/mods/modapi.h`](../src/pc/mods/modapi.h):

```c
#include "pc/mods/modapi.h"

static const MemoriesModHost *host;

static void draw_frame(void) { /* once a frame, while the mod is applied */ }

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    host = from;
    mod->api = MEMORIES_MOD_API;
    mod->frame = draw_frame;
    return 1;   /* 0 refuses the load */
}
```

The hooks are `frame` (after the game has queued its own drawing, which is
where an extra pass can draw over the finished picture), `applied` (the
player applied or removed the mod), `reset` (a save state was loaded, so
anything cached from the old game is stale) and `shutdown`.

The host table is what a mod is given: `log`/`log_enabled`, `open_asset` (a
file the mod ships), `open_data` (the mod's own file in the user directory,
the only place it may write), `setting`/`set_setting` (whole numbers kept in
the player's settings file as `mod.<id>.<key>`, and read from
`MEMORIES_MOD_<ID>_<KEY>` first when that is set), `disc_file_start`/
`disc_read`, `pad`, and from mod API 2 `now_us` (a clock) and `map_fixed`
(memory at an address the mod chooses, as 3D Monsters' model arenas need).
A mod that uses an entry newer than API 1 should refuse to start when
`host->api` is older.

### Building one

```sh
python3 tools/pc/build_mod.py my-mod            # writes my-mod/<library>.o
```

`build_mod.py` compiles every `.c` in the directory and merges them into the
one object. Beside a released game the same script is
`sdk/tools/build_mod.py`, and it builds against `sdk/include` there. It needs
clang (on Windows, the llvm-mingw clang; it builds the Linux object format
there too) or, on Linux, gcc with 32-bit support. `./build-pc.sh` builds
every directory under `mods/` this way, once, and copies the same file into
both games' `mods/` directories.

A mod reaches the game directly. Its undefined names are bound when it is
loaded, against a table compiled into the game (`mod_exports.c`, generated
by `tools/pc/build_game32.py`). The table holds every game function and
variable, every guest variable pinned to its retail address, and the
port's own globals. That is what makes something like 3D Monsters possible:
it borrows the model loader, the software GPU's texture banks and the duel's
ordering table. `build_mod.py` checks the names against the game builds it
can see and says which are missing, rather than leaving it to the game.

### What a mod is built without, and why

A mod is built with **no system headers at all**. glibc and the Windows C
runtime disagree about `FILE`, `errno`, `stdin` and more, so a mod built
against either would only work on one system. Instead:

* the compiler supplies `stddef.h`, `stdint.h`, `stdarg.h`, `stdbool.h`,
  `limits.h` and `float.h`;
* the SDK's own `stdio.h`, `stdlib.h`, `string.h` and `math.h`
  ([`src/pc/mods/sdk`](../src/pc/mods/sdk)) declare exactly the C library
  the game lends a mod (`src/pc/mods/mod_libc.c`): memory and string
  functions, `snprintf`/`vsnprintf`, `malloc` and friends, `strtol`,
  `qsort`, the usual maths, and `fread`/`fwrite`/`fseek`/`ftell`/`fgets`/
  `fclose` for the files the host opens. There is no `fopen`, `getenv`,
  `printf`, `time` or `exit`: files come from `open_asset`/`open_data`,
  knobs from `setting`, the time from `now_us`, and output goes to `log`.

The compiler flags close the gaps between the two ABIs, and each one is
covered by a test (`tools/pc/test_object_loader.py`):

| Flag | Why |
|---|---|
| `-fno-pic -fno-common` | plain relocations only; the loader has no GOT and no COMMON symbols |
| `-fno-stack-protector` | the canary is read from Linux thread storage (`%gs`), which Windows does not have |
| `-march=i686 -mno-sse` | x87 floating point, like the game's own code |
| `-mstackrealign` | Windows only promises a 4-byte-aligned stack on the way in, and the Linux game (SSE2) needs 16 on the way out |
| `-fstack-clash-protection` | a frame over 4 KiB touches each page, as Windows' stack guard page requires |
| `-ffreestanding -nostdinc` | no system C library, as above |

The loader refuses anything it does not handle, with the reason in the Mods
window: position-independent code, relocations other than plain absolute
and relative ones, COMMON symbols, thread-local storage, constructors and
destructors (do that work in `MemoriesModInit`), and a name the game does
not provide. A crash inside a mod names the function it was in
(`3d-monsters:draw_frame+0x40`).

## What a mod may and may not do

The host table has no network call in it, and no way to name a file outside
the mod's own directory and its data directory: relative paths only, and
`..`, absolute paths and drive letters are refused (`Paths_Contained`). Data
overrides only reach the disc image through the port's own reader and never
write to it.

A code mod, though, is native code in the game's process: nothing stops one
from reaching past the host table, since the whole game image is in reach
by design. The confinement above is what the mod system offers, not a
sandbox around the process, so installing a code mod is trusting its
author, as with any plugin. A data-only mod carries no code and is safe to
install on that ground alone. The Mods window shows every mod it found, and
the reason beside any that failed to load.

## The two mods the release ships

| Mod | What it is |
|---|---|
| `mods/3d-monsters` | face-up monsters on the duel field stand on their cards as animated models (`notes/pc-build.md`) |
| `mods/hand-camera` | L1/R1 turn and L3/R3 zoom the duel camera while the hand is up |

Both were part of the executable until they became mods; they are the worked
examples of a code mod that reaches deep into the game. 3D Monsters' knobs
are its settings `depth`, `pixels`, `scale`, `lift`, `pitch` and `test`
(`MEMORIES_MOD_3D_MONSTERS_SCALE=5000` for one run; they were
`MEMORIES_MODS_SCALE` and so on before it became one object for both systems).

## Testing a mod

* `MEMORIES_TRACE=mods` logs discovery, loads, overrides and whatever the mod
  logs itself.
* `MEMORIES_HEADLESS=1 MEMORIES_DUMP_FRAME=N MEMORIES_DUMP_PATH=out.ppm`
  renders one frame without a window.
* `tests/pc/mods_test.c` (ctest `pc_mods`) covers discovery, manifests, the
  settings keys, the names a mod binds to and what data overrides do to a
  sector;
* `tools/pc/test_object_loader.py` (ctest `pc_object_loader` for Linux,
  `smoke.py --windows` for Windows) loads and runs one object on both
  systems and feeds the loader broken and damaged ones;
* `tools/pc/check_mod_exports.py` (run by `smoke.py`) holds each game's
  table of names against its link;
  `tests/pc/json_test.c` (`pc_json`) covers the manifest reader.
