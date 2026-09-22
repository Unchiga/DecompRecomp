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
| `library` | the mod's library, relative to its directory; without a suffix `.mod` is added (one file for every platform, below). Leave it out for a mod that is only data |
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

A native mod is a library with one exported symbol, described in
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

### One file for Linux and Windows

A mod's library is one `.mod` file, and the same file runs on Linux and on
Windows. The game loads it with its own loader
([`src/pc/mods/modload.c`](../src/pc/mods/modload.c)), not with the
operating system's, so there is no `.so` or `.dll` to build twice and nothing
in a mod differs between platforms. (It is a 32-bit x86 ELF shared object;
both games are 32-bit x86 with the same calling convention and the guest RAM
at the same addresses, which is what makes one file enough.)

Build it with
[`tools/pc/build_mod.py`](../tools/pc/build_mod.py), from a checkout of this
repository, on either platform:

```sh
python3 tools/pc/build_mod.py path/to/my-mod     # writes path/to/my-mod/my-mod.mod
```

It needs clang with `ld.lld` -- llvm-mingw has both, so the Windows toolchain
in `notes/pc-build.md` already does -- or GCC with `-m32` on Linux. The file
is named after the manifest's `"library"` (`"library": "my-mod"` loads
`my-mod.mod`). `tools/pc/build_game32.py` builds every directory under
`mods/` the same way and copies it next to the executable, so a mod developed
in this repository needs no build wiring of its own.

### What a mod links to

A mod is compiled against the game's headers (`src/`) and against
[`src/pc/mods/libc`](../src/pc/mods/libc), the C library the game gives
mods, never against its system's. When the mod is applied, every name it
uses is looked up in exactly two places:

* **the game and the port**: every function and variable of the game, and
  the port's SDK, renderer, sound, overrides and GTE -- what 3D Monsters
  uses to borrow the model loader, the software GPU's texture banks and the
  duel's ordering table. A mod naming a game function gets the game's own
  address, so comparing it with a pointer the game stored works;
* **the mod C library**: memory, strings, maths, `printf`-style formatting,
  `qsort`, `getenv`, clocks, anonymous `mmap`, and reading and writing the
  `FILE`s the host opens for it. `time_t` is 32 bits, `CLOCKS_PER_SEC` is a
  million, `RAND_MAX` is `0x7fffffff` and `rand()` is one sequence -- the
  same on both platforms, where the systems' own differ.

A name in neither refuses the load, and the Mods window shows which:
`it uses fopen, remove, socket, which are not available to mods`. So a mod
has no `fopen`, `remove` or `rename`, no sockets or network, no `system` or
`exec`, and no way to load another library; the platform layer (files,
paths, settings, the window, logs) is not in the game's table either. The
loader also refuses a library that names another library it needs, uses
thread-local storage, or is not a 32-bit x86 shared object.

Two things a mod must not rely on, because the two hosts differ there: a
function returning a structure by value (the Linux and Windows 32-bit
conventions return small ones differently), and a `long long` or `double`
inside a structure it shares with the game, whose alignment differs too.
Neither occurs in the game's structures or the host table.

## What a mod may and may not do

The host table has no network call in it, and no way to name a file outside
the mod's own directory and its data directory: relative paths only, and
`..`, absolute paths and drive letters are refused (`Paths_Contained`). Data
overrides only reach the disc image through the port's own reader and never
write to it. A native mod is linked to nothing else the operating system
offers (above), so a mod built from its source with these tools cannot reach
the network or delete or overwrite the player's files.

That is a boundary around what a mod can link to, not a sandbox around the
process: a library is still native code in the game, and one written to get
round the loader deliberately could. Installing a native mod is trusting its
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
  `tests/pc/json_test.c` (`pc_json`) covers the manifest reader;
  `tests/pc/modload_test.c` (`pc_modload`, in 32-bit x86 builds) loads the
  libraries in `tests/pc/mod_fixtures`, built with `build_mod.py`: the
  relocations, the mod C library, the game's addresses, and the refusals.
