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
mod stores. It is `Documents\My Games\YFM ReDecomp` on Windows and
`$XDG_DATA_HOME/YFM ReDecomp` (`~/.local/share/YFM ReDecomp`) elsewhere; see
[`src/pc/platform/paths.h`](../src/pc/platform/paths.h). A mod in the user
directory replaces one the release ships with the same `id`.

| Variable | Effect |
|---|---|
| `MEMORIES_USER_DIR` | the user directory, instead of the platform's |
| `MEMORIES_MODS_DIR` | the only directory scanned for mods |
| `MEMORIES_MODS=0` | load no mods at all this run |
| `MEMORIES_MOD_<ID>=0/1` | settle one mod for this run; the id uppercased, everything that is not a letter or a digit an underscore (`MEMORIES_MOD_3D_MONSTERS=1`) |
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
| `library` | the mod's shared library, relative to its directory. Without a suffix the platform's is added (`.so`, `.dll`). Leave it out for a mod that is only data |
| `enabled` | whether the mod is applied the first time the game sees it |
| `restart` | whether changing it needs a fresh process. Data overrides default to `true`, because the game reads most of what they change while it starts; code mods default to `false` |
| `legacy_setting` | an older settings key to read the player's choice from, once |
| `data` | what the mod changes on the disc, below |

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

## Native mods

A native mod is a shared library with one exported symbol, described in
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
the player's settings file as `mod.<id>.<key>`), `disc_file_start`/
`disc_read`, and `pad`.

Beyond that, a mod is native code in the game's process and reaches the game
directly: the executable is linked with `-rdynamic`, so a mod's undefined
symbols bind to the game's and the port's own at load time. That is what
makes something like 3D Monsters possible -- it borrows the model loader, the
software GPU's texture banks and the duel's ordering table -- and it is why a
native mod is trusted code. Build against the repository's headers:

```sh
gcc -m32 -std=gnu11 -O2 -fPIC -shared -Isrc \
    -DMEMORIES_PC -D_LANGUAGE_C -DLANGUAGE_C \
    -o my-mod/my-mod.so my-mod/*.c -lm
```

`tools/pc/build_game32.py` does exactly this for every directory under
`mods/` and copies the manifest and the mod's files next to the executable,
so a mod developed in this repository needs no build wiring of its own.

## What a mod may and may not do

The host table has no network call in it, and no way to name a file outside
the mod's own directory and its data directory: relative paths only, and
`..`, absolute paths and drive letters are refused (`Paths_Contained`). Data
overrides only reach the disc image through the port's own reader and never
write to it.

A native library, though, is native code: nothing stops one from calling the
C library itself. The confinement above is what the mod system offers, not a
sandbox around the process, so installing a native mod is trusting its
author, as with any plugin. A data-only mod carries no code and is safe to
install on that ground alone. The Mods window shows every mod it found, and
the reason beside any that failed to load.

## The two mods the release ships

| Mod | What it is |
|---|---|
| `mods/3d-monsters` | face-up monsters on the duel field stand on their cards as animated models (`notes/pc-build.md`) |
| `mods/hand-camera` | L1/R1 turn and L3/R3 zoom the duel camera while the hand is up |

Both were part of the executable until they became mods; they are the worked
examples of a native mod that reaches deep into the game.

## Testing a mod

* `MEMORIES_TRACE=mods` logs discovery, loads, overrides and whatever the mod
  logs itself.
* `MEMORIES_HEADLESS=1 MEMORIES_DUMP_FRAME=N MEMORIES_DUMP_PATH=out.ppm`
  renders one frame without a window.
* `tests/pc/mods_test.c` (ctest `pc_mods`) covers discovery, manifests, the
  settings keys and what data overrides do to a sector;
  `tests/pc/json_test.c` (`pc_json`) covers the manifest reader.
