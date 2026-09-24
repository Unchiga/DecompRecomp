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

Every mod has a `mod.json` (UTF-8; a byte order mark, as some Windows
editors write, is fine):

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
| `library` | the mod's code: an object file relative to its directory (a subdirectory is fine), `.o` added when the name has no `.` anywhere in it (`rules` is `rules.o`, `rules.v2` stays `rules.v2`); `build_mod.py` writes it under the same name. The same file serves every system. Leave it out for a mod that is only data |
| `enabled` | whether the mod is applied the first time the game sees it |
| `restart` | whether changing it needs a fresh process. Data overrides default to `true`, because the game reads most of what they change while it starts; code mods and `audio` default to `false` |
| `legacy_setting` | an older settings key to read the player's choice from, once |
| `data` | what the mod changes on the disc, below |
| `textures` | a directory inside the mod holding a texture pack, below |
| `cards` | cards the mod adds after the disc's 722, below |
| `audio` | songs, XA clips and sound effects the mod replaces with WAV or Ogg files, below |
| `fusions`, `equips`, `rituals`, `drops`, `decks` | changes to the duel's rule tables, below |
| `text`, `font` | a translation of the game's text, and fonts for letters it has none of, below |

`version`, `author` and `description` are displayed in the manager. Version
bounds in `requires` are checked before activation. API 3 also supports
`min_api`, `game`, `requires`, `after`, `conflicts`, `priority` and declarative
`settings`; see [the API 3 guide](mod-api-3.md) for schemas and examples.

Any other top-level key is a warning beside the mod in the Mods window, with
the key it most likely meant: a manifest that says `"libary"` or `"texture"`
would otherwise load a mod that does nothing, without a word
(`unknown key 'libary' (did you mean 'library'?)`). `enabled` and `restart`
that are not `true` or `false` are warned about too. A warning never stops
the mod loading; the manifest keeps its warnings for the whole session,
however often the mod is applied.

Numbers are JSON's, in base 10, and must be whole (`1e3` is fine, `1.5` is
an error, and so is `010`); arrays and objects nest at most 64 deep. A
number written as a string (`"at": "0x5D800"`) may be hexadecimal with its
`0x`, and is otherwise decimal.

Two folders in one mods directory with the same `id` do not replace each
other: the first by name is kept and says which one was left out. Across
directories the player's copy still replaces the shipped one.

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

* `replace` names a file the mod ships. Named data files may be larger than
  the original: the port assigns a virtual disc extent and file lookup
  returns its position and exact byte size. The final sector is zero-padded;
  shorter replacements retain zero-filled sectors through the original
  allocation. Named replacements always require a restart, including when
  a manifest says `"restart": false`, because game and mod code cache file
  positions. The last replacement in startup load order wins, then patches
  apply on top. Competing replacements are reported in the Mods window.
  Replacing a raw `lba` region needs a `sectors` count and must fit that
  allocation; an oversized raw replacement is rejected. XA/STR files retain
  their original allocation and raw streaming headers.
* `patch` writes `bytes` (hexadecimal, spaces optional) at `at`, an offset
  into the file, or into the sector when the entry names an `lba`. A run that
  crosses a sector boundary is fine. This is the shape the community's
  hex-editor tutorials are written in, so their offsets carry over directly
  (`modding-tutorial-gameplay-patches.md`).

Named patches may address the expanded tail. They are checked against the
final selected replacement; a shorter replacement still allows patches
within the original file's byte length. Reads and raw patches at original
LBAs continue to address the original file's replacement prefix. A raw
patch crossing into the next file still affects that next file, rather than
the expanded tail. Code mods should obtain the effective LBA through
`host->disc_file_start` and use `host->disc_read` to read larger files.

Virtual allocation is bounded by the SDK's CD position format (last LBA
449849), the physical image's size, and other replacements. The port
reserves space for the largest enabled candidate for each file plus a guard
sector, so a failed mod can fall back without changing cached addresses.
Insufficient space rejects the replacement with a diagnostic; it never
silently truncates it. Save-state compatibility includes the replacement
contents and layout. Restart after changing replacement files.

Larger files do not automatically increase the game's model buffers or
change an MRG member's compiled offsets and transfer phases. Existing
record layouts continue to work; larger individual records require a loader
that requests and safely consumes their new layout. Higher-resolution PNG
textures already use texture packs, below. The implementation sequence and
remaining resource-loader work are in [the larger-file plan](larger-disc-files-plan.md).

Overrides stand in for the disc for every reader in the port: the drive
model, the bulk reads a mod makes, and the file lookup itself. Nothing on the
real disc image is touched, and removing the mod puts the game back exactly
as it was.

## Texture packs: images by origin

A mod may carry a `textures` directory: PNGs named by where their images
come from on the disc, with a `manifest.json` describing each one, exactly
what `tools/pc/extract_images.py` writes (`notes/pc-build.md`, "Images from
the disc"). Extract the family you want to repaint (`cards`, `portraits`,
`sheets` for the screens, `scenes` for the story's pictures, or `--assets`
for what a capture drew), paint over the PNGs, and point a manifest at the
directory:

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
the image was made for is replaced. A pack may carry the same words
several times, one entry per palette (the `sheets` family writes a screen's
sheet once per way the game reads it), and the entry whose palette the
primitive uses is the one drawn; at the console's resolution only the first
of them shows, the scaled picture shows all. The packs of every enabled
mod add up. The extracted images themselves are the game's, so a pack
ships painted images or a way to make them from the player's own disc,
never the originals.

A pack image does not need the extracted image's shape either: it is
stretched to the texture's width and rows (the crop's width, below), so a
4x image of a 102x96 card art is 408x384, and a wider or taller one is
squeezed to fit rather than cropped.

When two enabled packs replace the same image read the same way (the same
archive offset, size, depth and palette), the one later in the mods' load
order is drawn: the higher `priority` number, or the player's Order in the
Mods window, or the one that names the other in `after`. That holds however
the packs were applied, on Linux and on Windows alike. Entries of different
sizes at one offset are not the same image; which one a texel comes from
follows their size, not the packs.

### The pack's manifest.json

`manifest.json` is an array with one object per image. The texture pack
loader (`src/pc/render/texture_pack.c`) reads these keys:

| Key | Meaning |
|---|---|
| `file` | the PNG, relative to the pack's directory; `..` and absolute paths are refused |
| `archive` | the archive on the disc the offsets are relative to, as the extractor names it (`WA_MRG.MRG`, looked up as `\DATA\WA_MRG.MRG;1`) |
| `offset` | the image's first byte in the archive |
| `words` | its width in 16-bit VRAM words (1 to 1024) |
| `rows` | its height (1 to 512) |
| `bpp` | 4, 8 or 16: how the game reads the words |
| `clut_offset` | the palette's first byte in the archive; only used when `clut_entries` is not 0 |
| `clut_entries` | the palette's size (16, 256), or 0 for a 16-bit image without one |
| `stride` | words from one row to the next in the archive (default `words`) |
| `row_offsets` | instead of a stride, each row's byte offset from `offset`: a list of exactly `rows` numbers, or `null` |
| `crop_left` | the first texel of each row the image covers (default 0) |
| `width` | how many texels from there it covers (default: the rest of the row) |

The extractor also writes `alias` (what the image is), `height` (the rows
again), and `sheet` and `column` (where a sheet's column stands, for
`upscale_pack.py`); the game does not read them. Numbers are whole numbers,
as everywhere in a manifest.

An entry the loader cannot use is left out and counted, and the Mods window
shows one line for the pack, for example `2 images could not be read
(first: cards/001.png); 1 image is outside the pack (first: ../x.png)`: a
file that is missing or not a PNG, a path outside the pack, measures out of
range, a `row_offsets` list whose length is not `rows` (the image is then
read with the stride), an entry without `file` or `archive`, or more than
65535 images in all. PNGs are decoded the first time the game needs them;
at load only their signature is checked, and a PNG that fails to decode
later is reported on the console.

`tools/pc/upscale_pack.py` makes a pack of upscaled images from an extracted
set with Upscayl's command-line binary (Real-ESRGAN on the GPU): the same
files and manifest, enlarged (`--scale 4` by default, one to one with
Internal 4x; `--scale 25 --passes 2` is the Upscayl window's "5x, twice"),
with `mod.json` written beside them and, with `--zip`, the mod folder in a
zip that unpacks into a `mods` directory (the user directory's, for anyone
who downloads it). Several extracted sets make one pack (`--images` again
for each); entries whose pictures are identical share one file and one
upscale. A screen the `sheets` family knows but no dump has shown yet gets
its sheets at a guessed reading; to be sure of one, play it once with
`MEMORIES_DUMP_TEXTURES=<dir>` from a cold boot and pass
`--variants <dir>/assets.txt` to the extractor: every depth and palette the
run read a sheet with becomes a PNG of it. `--assets <dir>/assets.txt`
instead writes what such a run drew, as it cut it, for whatever no family
covers. Keep the scale in proportion: the game holds a pack's images in
memory at full size, and a 128x256 sheet at 4x is 2 MB. `--cuts
<dir>/assets.txt` makes the upscale treat each piece the game cuts from a
sheet (a box's slices, the field's tiles, glyphs) as its own picture, so
no line shows where pieces meet.

The first attempt at a full HD pack, the problems it met and why its
result was not published are in `notes/image-remaster.md`: read it before
making another.

## Cards: more than the disc has

A mod may add cards with a `cards` list, no code needed. Each new card is a
copy of a retail card, which lends it its 3D model, fusions and effect; its
name, picture, text, stats, type, level, attribute and guardian stars can be
its own:

```json
"cards": [
    { "copy": "Kuriboh", "name": "Dingus Shmingus", "art": "images/dingus.png",
      "description": "A round and cheerful fellow who has never once been on time.",
      "level": 7, "attribute": "Fire", "stars": ["Moon", "Venus"], "attack": 2500 },
    { "copy": "Kuriboh", "count": 100, "name": "Kuriboh {n}" }
]
```

The cards take the ids after 722, in the order the mods are found, and work
in the Library, Build Deck, duels, rewards, trades and saves.
[More cards](more-cards.md) has every key, how a new card is won, where what
the save holds of them is kept, and how the port does it. Like data
overrides, a mod with cards needs a restart.

## Audio: songs, voices and sounds from files

A mod may replace the game's music, its XA streams (the recorded voices and
jingles on the disc) and its sound effects with ordinary WAV or Ogg Vorbis
files, from `mod.json` alone:

```json
"audio": {
    "music": { "0x000": "title.ogg",
               "0x010": { "file": "menu.wav", "loop": true, "loop_start": 44100 } },
    "xa":    { "0x8020": "fanfare.ogg" },
    "sfx":   { "0x007": { "file": "click.wav", "volume": 80 } }
}
```

Each key is an id, in hexadecimal with `0x` or in decimal (`"0x2D0"` and
`"720"` are the same id; a bare `2D0` is refused). Each value is a file name
relative to the mod's directory, or an object:

| Key | Meaning |
|---|---|
| `file` | the WAV or Ogg Vorbis file, inside the mod (`..` and absolute paths are refused) |
| `loop` | play again from `loop_start` when the end is reached; `true` by default for music, `false` for `xa` and `sfx` |
| `loop_start` | where the loop starts, in sample frames at 44.1 kHz (44100 is one second in), whatever the file's own rate |
| `volume` | percent of the file's own level, 0-400, default 100 |

A mod with only `audio` applies and removes live in **Game > Mods**, with no
restart; a song that is playing when the mod is applied or removed switches
at once. When several applied mods replace the same id, the one applied last
wins, as data overrides do (load order: `priority`, `after`, `requires`, then
discovery order); the Mods window warns about the overlap.

### Finding the ids

Play with `MEMORIES_TRACE=mods` and the log names every id as it starts:

```
audio: music 0x0 starts
audio: sfx 0x7 plays (replaced by audio-replace)
audio: music 0x0 stops
audio: music 0x10 starts
audio: xa 0x8020 starts
```

Songs known so far: `0x000` is the title screen, `0x010` the main menu.
A sound effect the game starts every frame is logged every 60th time.

* **music** is the sound driver's song number, `SD_BGMPlay(0x2D0)` is song
  `0x2D0` (the number is a song package times 16 plus a track in it). A
  replaced song's own sequence keeps running with its voices silenced, so
  everything the game times on it is unchanged; the file starts when the
  sequence does and stops when it stops, is reset, or reaches its end. The
  game's fades and its music volume apply, and so does the port's
  **Music** volume.
* **xa** ids are `0x8xxx`, `0x9xxx` or `0xAxxx`, as the game asks for them
  (the duel's `0x8020`-`0x8022`, for instance). The disc still reads the
  original clip at its own pace and its sound is dropped, because the game
  waits for the clip, not for the file: **the original clip's length decides
  when the game carries on**. A longer file keeps playing over what follows
  until the game stops XA playback or starts another clip; a shorter one
  leaves silence. Match the original's length where it matters. XA
  replacements go through the game's CD volume and fades and the port's
  **Stream** volume. An XA clip a song plays through (`SD_BGMPlay` of an XA
  id) is an `xa` entry too.
* **sfx** ids are the sound bank's, the number the trace prints. A replaced
  effect takes no SPU voice; it plays once with the game's volume and pan
  for it (and `loop: true` repeats it until every effect is keyed off, which
  most looped effects are not built for). The port's **SFX** volume applies.

### Formats and limits

* WAV: PCM 8, 16, 24 or 32-bit, or 32/64-bit float, including
  `WAVE_FORMAT_EXTENSIBLE`; any sample rate and channel count. More than two
  channels fold to stereo: even channels left, odd ones right.
* Ogg Vorbis, decoded by [stb_vorbis](../src/pc/third_party/README.md)
  (public domain). Opus, MP3 and FLAC are not read.
* Files are decoded when the mod is applied, resampled to 44.1 kHz stereo
  (linearly) and kept in memory: about 10 MB a minute. A clip is at most 12
  minutes and a file at most 256 MB. The 32-bit game has little room to
  spare, so prefer Ogg files for the mod and keep long songs few.
* A file that will not decode is skipped, with the reason beside the mod in
  the Mods window (and on stderr); the rest of the mod still applies.
* At most eight replaced effects sound at once; a ninth takes the oldest's
  place.
* Save states do not hold replacement sounds. Loading one starts the loaded
  game's song replacement from its top; an XA clip or effect that was playing
  is not resumed.

The same mod plays the same on Linux and Windows and with every audio
output (SDL, ALSA, the headless dump). `examples/mods/audio-replace` is a
worked example: run its `make_tone.py`, copy the directory into the user
mods folder and apply it. To check a replacement without speakers:

```sh
MEMORIES_MOD_AUDIO_REPLACE=1 MEMORIES_TRACE=mods MEMORIES_HEADLESS=1 \
MEMORIES_DUMP_AUDIO=out.raw MEMORIES_DUMP_FRAME=1500 MEMORIES_DUMP_PATH=out.ppm \
MEMORIES_INPUT="700:0008,706:0000" tmp/pc/game32/memories-pc
```

`out.raw` is s16le stereo at 44.1 kHz. How it is done:
[`src/pc/audio/replace.h`](../src/pc/audio/replace.h).
## Rules: fusions, equips, rituals, drops and decks

A mod may change what fuses into what, what an equip card may equip, what a
ritual needs and makes, what each opponent drops and what its deck is dealt
from, with no code and naming cards by name:

```json
"fusions": [ {"with": ["Kuriboh", "Mystical Elf"], "result": "Celtic Guardian"},
             {"with": ["Baby Dragon", "Time Wizard"], "result": null} ],
"equips":  [ {"card": "Legendary Sword", "add": ["Dragon"]} ],
"drops":   { "Simon Muran": {"pow": {"Blue-eyes White Dragon": 20}} },
"decks":   { "Heishin": {"Dark Magician": 60, "Kuriboh": 0} }
```

Weights are out of 2048, as the game's are, and the pools are always brought
back to 2048. Several mods' edits of the same opponent add up rather than
replace each other. [Gameplay tables](gameplay-tables.md) has every key, the
opponents' names, and how the rules combine. Like cards, they need a restart.

## Translations

A mod may put the game's text in another language: dialogue, menus, card
names and texts, types and duelists, accented letters included.
`tools/pc/text_listing.py extract` writes the text out of the player's disc
as an editable UTF-8 listing; the mod ships the translated file:

```json
{ "id": "spanish", "name": "Español", "text": "text.txt" }
```

[Translations](translation.md) describes the listing, its codes, the
letters the port draws and how a font is added. Like cards, a translation
needs a restart.

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

The legacy hooks are `frame` (after the game has queued its own drawing, which is
where an extra pass can draw over the finished picture), `applied` (the
player applied or removed the mod), `reset` (a save state was loaded, so
anything cached from the old game is stale) and `shutdown`.

The host table is what a mod is given: `log`/`log_enabled`, `open_asset` (a
file the mod ships), `open_data` (the mod's own file in the user directory,
the only place it may write), `setting`/`set_setting` (whole numbers kept in
the player's settings file as `mod.<id>.<key>`, and read from
`MEMORIES_MOD_<ID>_<KEY>` first when that is set; a key is letters, digits,
`_` and `-`, and `order` is the manager's), `disc_file_start`/
`disc_read`, `pad`, and from mod API 2 `now_us` (a clock) and `map_fixed`
(memory at an address the mod chooses, as 3D Monsters' model arenas need). API 4 adds `hook`/`unhook`/`symbol`, below.
A mod that uses an entry newer than API 1 should refuse to start when
`host->api` is older.

API 3 adds managed gameplay events, named configuration profiles and registered
save-state buffers. The [API 3 guide](mod-api-3.md) describes damage, reward,
fusion, effect, AI, input and scene hooks, their ordering/cancellation rules,
and stable card identities. The [manager](mods-window.md) exposes descriptions,
settings, compatibility and staged batch changes.

### Replacing or wrapping a game function (API 4)

Every function of the game can be taken over by a mod, not only the ones
with an event. `host->hook` names the game function directly and gives the
replacement, which has the same signature:

```c
extern void DuelScene_UpdateResultRewards(void);   /* the duel's result screen */
static void *original;   /* static: the host keeps it up to date */

static void my_result_screen(void)
{
    /* ... before ... */
    ((void (*)(void))original)();   /* the game's own, or the mod hooked before this one */
    /* ... after: change or observe what it did ... */
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 4) return 0;
    host = from;
    mod->api = 4;
    return host->hook(host, (void *)DuelScene_UpdateResultRewards, (void *)my_result_screen, &original) != 0;
}
```

While the mod is applied every call, from anywhere in the game, goes to the
replacement; `original` leads to what it displaced, so calling it wraps the
function and not calling it replaces it. Several mods may hook one
function: the one applied last is called first, and each one's `original`
leads to the one before. Removing a mod in the Mods window takes its hooks
out at once, and a function nobody hooks is the game's again, byte for
byte. `hook` returns 0 for anything that is not a game function (the port's
own code, the C library, a function of another mod). `host->unhook` removes
one hook, and `host->symbol("name")` looks a name up at run time, for a mod
that can do without it.

How it works: every game unit is compiled with
`-fpatchable-function-entry=8,6`, which leaves six bytes of `nop` before
each function and two at its entry. A hook turns the six into an indirect
jump through a pointer the host keeps and the two into a short jump back to
it; see [`src/pc/mods/hooks.c`](../src/pc/mods/hooks.c). The game runs the
same with no mod hooking anything (the smoke screenshots are unchanged).

### Building one

```sh
python3 tools/pc/build_mod.py my-mod            # writes my-mod/<library>.o
```

`build_mod.py` compiles every `.c` in the directory and merges them into the
one object. Beside a released game the same script is
`sdk/tools/build_mod.py`, and it builds against `sdk/include` there. The
release's `sdk/` also carries `extract_images.py` and `upscale_pack.py` in
`sdk/tools`, the example mods in `sdk/examples/mods`, and this note with
`mod-api-3.md` and `more-cards.md` in `sdk/notes`. It needs
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
are its declared settings `scale`, `pixels`, `lift`, `pitch` and `depth`, in
the Mods window (`MEMORIES_MOD_3D_MONSTERS_SCALE=5000` for one run; they were
`MEMORIES_MODS_SCALE` and so on before it became one object for both systems).
One more, `test`, is read but not declared, so the window does not show it:
`MEMORIES_MOD_3D_MONSTERS_TEST=<card>` stands a different monster in every
zone from that card on, for measuring the cache and the arenas.

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
  `tests/pc/json_test.c` (`pc_json`) covers the manifest reader;
* `tests/pc/audio_replace_test.c` (`pc_audio_replace`) covers WAV and Ogg
  decoding, resampling, the `audio` object, which mod wins an id and what
  the mixer plays for the sound driver's calls.
