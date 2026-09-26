# Native PC foundation

The port is not playable. Two native builds exist:

- The CMake target: portable adapters and their tests (guest-layout RAM,
  retail GPU packet collection, LIBGS ordering tables, software GTE, game RNG),
  plus the optional PSY-Z renderer probe. Builds as 64-bit or 32-bit.
- `tools/pc/build_game32.py` (shortcut: `./build-pc.sh [run|trace]`): **all 514
  resident game C units plus the main-menu and password/name-entry modules,
  linked as a 32-bit Linux executable that boots to the title screen and main
  menu in a window, and runs NEW GAME through name entry into the opening
  story scene.**
  Missing SDK/assembly routines are generated stubs that name themselves.

All 61 handwritten assembly routines (the model polygon drivers) have native
forms in `src/pc/overrides/model_polygon_drivers.c`; see the table below. Shared game sources
carry four small `#ifdef MEMORIES_PC` guards (listed below); `make match` and
`make match-overlays` still reproduce the retail hashes with them.

## Play from a checkout

On Linux and on Windows alike: put the `.bin` of your USA disc in `game/`
(any name), then run `./play.sh` or `play.bat`. The first run builds the game
and fetches what that needs into `tmp/` (about a minute on Linux, longer on
Windows); later runs rebuild what changed and start at once. `trace` and
`load [slot]` work with both.

What each needs installed: on Linux, `gcc` (with binutils) and `python3`, which
`play.sh` checks for and names the package to install; not their 32-bit
(multilib) parts, CMake or Ninja. On Windows, nothing: `play.bat` uses an
installed Python or fetches the official embeddable one into
`tmp\pc\tools\python`, and `tools/pc/fetch_tools.py` fetches llvm-mingw, CMake
and Ninja when they are not on PATH. Every download is pinned to a release and
its SHA-256.

The matching build (`make match`, which needs the MIPS toolchain) is not
needed: the retail addresses the link pins game variables to are in
`config/pc/guest_addresses.txt`, which `build_game32.py` rewrites from the
matching build's ELFs whenever they are present (commit it when it changes;
a build from the file and one from the ELFs are byte-identical). The game's
executable, SLUS_014.11, is read out of the disc image
(`src/pc/platform/game_files.c`); a path on the command line still names a
separate copy.

### Folder names

Native paths, environment values and command-line arguments use UTF-8.
On Windows, `src/pc/compat/fs.h` converts file operations to the wide Windows
APIs, independent of the user's system code page. Include it (or `posix.h`)
in native units that use filenames or environment variables. Library calls
need the same care: PNGs are read from an already-open file, and FreeType
faces from memory through `compat/font.h`.

The PC test workflow runs with Unicode temporary directories and relocates
an executable into a folder containing accents, combining characters, CJK,
emoji, spaces and shell metacharacters. Its Windows check also runs
`play.bat` through the embedded-Python fallback with delayed expansion
initially enabled. Run it with `python tools/pc/test_path_layout.py --build
<cmake-build-directory>` after building the tests. Existing path-length
limits and the operating system's filename restrictions still apply.

## 32-bit game executable (bring-up)

The user chose a 32-bit (ILP32) host build as the bring-up memory model on
2026-09-20; 64-bit is deferred.

```sh
python3 tools/pc/build_game32.py
tmp/pc/game32/memories-pc                       # exits 70 at the first stub
MEMORIES_STUB_TRACE=1 tmp/pc/game32/memories-pc # survey: stubs log and return
```

How it works:

- `src/pc/guest/image.c` maps 2 MiB at `0x80000000`, mirrors the same pages at
  `0xA0000000` and at physical `0x10000..0x200000` (hosts reserve the first
  64 KiB; 24-bit packet links use these addresses), maps the scratchpad at
  `0x1F800000`, and copies the user's PS-X EXE image to its load address.
  Guest pointers are therefore host pointers, and structure layouts, packet
  words and the `ygo_types.h` size assertions hold unchanged. Linux only so
  far; Windows needs the equivalent `VirtualAlloc`/`MapViewOfFileEx` calls.
- The driver reads symbol addresses from `tmp/project-build/SLUS_014.11.elf`
  and writes a linker script pinning every data symbol that game C leaves
  undefined or tentative (`-fcommon`) to its retail address: 612 symbols.
  Initialized data defined in C stays in host `.data`, which keeps function
  pointers in C tables native. Guest-image tables that hold MIPS function
  addresses are **not** handled yet and need an audit.
- Undefined functions become stubs calling `Memories_Unimplemented`. Current
  link report (`tmp/pc/game32/link-report.json`): 80 SDK, 61 handwritten
  assembly, 8 renamed host-colliding names (file I/O, `exit`, `ccos`/`csin`).
  A definition under `src/pc/sdk/` or `src/pc/guest/` replaces its stub.
- `config/pc/host_symbol_renames.txt` renames game references that would
  otherwise bind to host libc with a different contract: `rand`/`srand` go to
  the exact game RNG, `setjmp`/`longjmp` to a six-word i386 version that fits
  the game's 48-byte Psy-Q `jmp_buf` at `0x800E9DC0`, and `open`/`read`/... to
  `Psx_*` so host libc internals never reach a game stub.
- GTE inline assembly: `src/psyq/inline_c.h` selects
  `src/pc/compat/inline_c_native.h` natively, backed by the software GTE in
  `src/pc/compat/gte.c` (all COP2 commands found in the retail executable:
  RTPS, RTPT, MVMVA, SQR, NCDS, NCCS, NCCT, NCLIP, AVSZ3/4, GPF, plus the
  remaining colour commands). `pc_gte` holds hand-computed known answers;
  it has **not** been compared against hardware or an emulator yet.

### What runs (2026-09-20)

`./build-pc.sh run` opens a 960x720 window and boots from the user's disc
image: Konami logo, company screen, the intro FMV with its soundtrack, title
screen with music, main menu and the Options screen. Menu input and sound
effects work; Start skips the movie. NEW GAME runs name entry (a name, END,
YES, "Your duelist code has been recorded") and continues into the opening
story scene with its dialogue, Simon's "Run away" branch, and the 3D town map:
moving between locations and entering them works (checked: the palace, the
shrine to its right, the location below), the card shop menu, SAVE/LOAD and
BUILD DECK (both panes, moving cards, paging, leaving, and the card viewer on
Triangle: turn animation, art, name, level, attribute, type, guardian stars,
text, ATK/DEF, from either pane), and the post-LOAD menu with LIBRARY: the
722-card grid, crosshair and scrolling, the card view, and the 3D model view
(Square or Cross in the card view): the monster stands on the wireframe floor
and animates. Checked with Ryu-Kishin only; its colours have not been compared
with hardware. The Free Duel `0x80168000` module is integrated; its opponent
grid initializes from the save's unlock flags and accepts cursor input.
The duel's 3D battle presentation loads and renders both monster models, the
arena and camera sequence, then returns to the field. Per-monster MODEL
control modules and the shared WA effect dispatcher are retail MIPS overlays
with no C source, and by default (2026-09-21) they run as they are: see
[MIPS-only effects](#mips-only-effects) below. Every duel effect id and every
monster's own attack choreography therefore plays with its retail timing,
colours and particles.
Music tempo was checked by measurement: the retail `SetRCnt` (read from the
resident assembly) programs counter 2 as mode `0x248`, sysclk/8, so target
`0xE000` is 73.83 Hz; `MEMORIES_TRACE_FRAMES=1` reports 73.7-74.0 delivered
sequencer ticks/s and 59.9-60.0 VBlanks/s, and each tick runs eight sequence
steps with no other time source (`GetRCnt`'s value is discarded).
Audio has otherwise only been checked by signal level and waveform statistics from
`MEMORIES_DUMP_AUDIO`, not by ear or against hardware. Nothing here has been compared frame-by-frame against an
emulator yet; the screens were checked by eye from headless frame dumps.

The native mixer bus split was verified with `MEMORIES_TRACE_SPU=1`: title
music key-ons used only voices 0 through 19, while scripted main-menu cursor
moves keyed masks `0x100000`, `0x200000` and `0x400000` (voices 20 through
22). This agrees with `SD_VOICE_SLOT_FIRST_VOICE == 20` and the four-slot
allocator in `sound_effect_voices.c`; voice 23 is the fourth slot in the same
mask range. CD/XA remains a separate streamed input.

Input. Keyboard, mouse and controllers are all live at once and OR together
on port 1.

**Game > Controls...** opens the keyboard/controller editor: a resizable window
with a grouped binding table, the PlayStation pad picture, live input preview,
rebinding, saved profiles and explicit controller selection for either port. See [Controls](controls-menu.md). The table below lists factory defaults;
custom bindings are stored separately in `controls.txt`.

| PlayStation | Keyboard | Xbox controller | Mouse |
|---|---|---|---|
| D-pad | Arrow keys | D-pad, left stick | wheel = up/down tap |
| Cross | X | A | |
| Circle | S | B | right button |
| Square | Z | X | |
| Triangle | A | Y | middle button |
| L1 / R1 | Q / W | LB / RB | |
| L2 / R2 | E / R | LT / RT (past a third of travel) | |
| L3 / R3 | T / Y | stick clicks | |
| Start | Enter | Menu/Start | |
| Select | Right Shift | View/Back | |

Esc quits (it closes an open menu first); F1, F2 and F4 select those state
slots, F5 saves, and F7 loads. F3 cycles the debug HUD; slot 3 is selectable
from File. Controllers
(`platform/gamepad_evdev.c`) are read through evdev, which names controls by
meaning, so the one table covers Xbox pads on xpad, xone and xpadneo and most
other pads. `/dev/input` is rescanned about once a second while a port is
free: plugging in mid-game works, and unplugging frees the port. The first
controller shares port 1 with the keyboard; a second becomes port 2, which
otherwise reports "no pad" as before. `MEMORIES_NO_GAMEPAD=1` turns the scan
off. Checked with a virtual Xbox 360 pad made through uinput (hat, stick,
A, RT, removal); not yet with physical hardware. The account needs read access
to the pad's `/dev/input/event*` node, which desktop logins get by default.

Environment switches: `MEMORIES_DISC` (the raw MODE2/2352 image; without it
the first `.bin` of the USA disc is taken from `game/` beside the executable,
the executable's own folder, `game/` in the user directory, then `game/` in
the current directory), `MEMORIES_SCALE` (1-8, default 4, which puts the 320x240
picture on screen at 1280x960), `MEMORIES_VOLUME` (0-100, overrides the
stored volume and applies headless too), `MEMORIES_SETTINGS` (another
settings file), `MEMORIES_HEADLESS=1`,
`MEMORIES_DUMP_FRAME=N` with `MEMORIES_DUMP_PATH` and optional
`MEMORIES_DUMP_VRAM=1` (write frame N as PPM and exit),
`MEMORIES_WINDOW_SHOT=N` (save the window as shown at frame N, as the
screenshot key does, into `MEMORIES_SCREENSHOT_DIR` or the user folder),
`MEMORIES_SCALE_AT=N:S` (change the internal resolution to S at frame N, as
the View menu would), `MEMORIES_MODE_AT=N:M[,N:M...]` (from frame N on, the
next main mode a screen publishes becomes M, `main_modes.h`; M 0 is the
game's debug menu, reached with the options case's input; see
`notes/image-remaster.md`),
`MEMORIES_INPUT="700:0008,706:0000"` (scripted pad bits from a frame on;
`MEMORIES_INPUT2` the same for the second pad, which then counts as
connected: two-player trades and duels),
`MEMORIES_DEBUG_CHEST=N` (N of every card in the trunk) and
`MEMORIES_DEBUG_DECK="723-762"` (the deck, as ids and ranges repeated to
forty), both once a save is live ([More cards](more-cards.md)),
`MEMORIES_NO_AUDIO=1`, `MEMORIES_DUMP_AUDIO=path` (raw s16le stereo 44.1 kHz
instead of a device), and
`MEMORIES_STUB_TRACE=1`. Traces on stderr: `MEMORIES_TRACE_SPU=1` (every
`SpuSetKeyOnWithAttr`), `MEMORIES_TRACE_INPUT=1` (scripted pad changes with
frame and VBlank numbers; script frames are presented frames, which run
behind VBlanks during loads and the movie), `MEMORIES_TRACE_FRAMES=1`
(per-frame game/present/rasterizer time and missed VBlanks) and
`MEMORIES_TRACE_DISC=1` (each sector's destination),
`MEMORIES_TRACE_MEMCARD=1` (card checks, completed commands and directory
entries), and
`MEMORIES_TRACE_MENU=1` (menu-bar clicks) and
`MEMORIES_TRACE_MODEL_MODULES=1` (MODEL bridge commands) and
`MEMORIES_TRACE_DUEL_EFFECTS=1` (WA effect requests and payloads). Without an audio device
(or headless) a silent sink still runs the SPU: the game polls envelopes and
the disc service waits for CD-input room, so a stopped SPU hangs the game
after the movie.

Key-on and key-off requests are applied by the mixer at its next 256-frame
period (5.8 ms), but the hardware answered `SpuGetKeyStatus` and
`SpuGetVoiceEnvelope` at once, and the sound driver relies on that: it picks
a free SFX voice by envelope 0 and spins on status after a key-off. So both
reads answer from the request (`keyed_mask`; a pending key-on reads as full
attack), not from the mixer. Before that, two sounds keyed within one mix
period could land on the same voice and the first never played, which
showed as missing sound effects at 200% and above; `MEMORIES_TRACE=spu`
logs "replaces a key-on not yet mixed" whenever it still happens.

### MIPS-only effects

Two kinds of duel code exist only as MIPS bytes inside the archives:

- the shared WA effect bank that every duel package copies to `0x80146000`,
  entered at `0x801462B0` with an effect id: fusion (1), battle damage (2),
  destruction (3), the magic, trap, ritual, terrain and field effects up to
  id 23;
- the per-monster MODEL control modules the loader swaps into
  `0x8013A000`/`0x8013B000` (slot A) and `0x8017A000`/`0x8017B000` (slot B)
  with each monster: a primary module (`return 2` in every record seen) and
  a variant module holding that monster's attack choreography and impact
  particles. MODEL.MRG has 621 records and 1,181 distinct variant modules.

Translating those one by one was the earlier plan; the default now runs the
loaded bytes through the interpreter in `src/pc/guest/mips.c` (integer MIPS I
plus COP2 through the software GTE), which bridges every call that leaves an
overlay to the native function of that address. A code-following scan of the
WA bank and all 1,181 variant modules (`tmp/`, 2026-09-21) found only
instructions the interpreter has, no GTE opcodes, and 26 SDK routines the
resident C never references (`GetTPage`, `SetPolyFT4`, `RotMatrixX`,
`gteMIMefunc`, `catan`, `GsGetLs`, ...); those are ported in
`src/pc/sdk/libgte_extra.c` from the resident assembly so they enter the
function map. Calls the other way (a native routine reaching a callback an
effect installed) go through the guest-call fault handler into the
interpreter. Interpreted code runs on its own stack mapped at `0x9FF00000`,
negative like every console address, and nests through native calls.

Checked from `slot1.state`: the AI's attack (card clash, damage lettering,
burn destruction: ids 2 and 3), the player's attack committed with Square
(the 3D battle: arena, both monsters, the variant module's setup command and
70 update frames with its sparkle burst, then the return to the field) and
effect 11, with no interpreter failures. The pad word the game reads is
byte-swapped relative to the raw pad, so in `duel_scene_field_actions.c`
`0xC0` is Cross or Square: either starts an attack, Square commits the
target with the 3D presentation (`D_8009B229 = 1`), Cross without it.

Switches: `MEMORIES_DUEL_EFFECTS=native` restores the bring-up behaviour
(ids 1-3 interpreted, others complete at once); `MEMORIES_MODEL_MODULES=native`
uses the resident spark burst instead of the monster's module; an effect or
module the interpreter cannot run is reported once on stderr and falls back
the same way. `MEMORIES_TRACE_MIPS_PRINTF=1` prints the modules' own
`printf` format strings.

### Credits

The ending's credits (`Main_RunCredits` phase 2, `func_800507D0`) load 16
SU sectors from `0x4C7` into `0x80180000`, where the main-menu overlay is
otherwise linked natively, and call the loaded MIPS directly
(`func_801807B0`, `func_80181C4C`, ...). `Memories_MipsInOverlay` therefore
counts `0x80180000-0x80188000` as interpreted whenever no native module is
resident in that bank (the bank's identifier word decides), and the
outermost interpreted frame starts 64 bytes below the stack's top because
the module stores its arguments at `sp+0` on entry. The module's external
calls are `LoadImage2`, `IsIdleGPU`, `GsSortPoly`, `strlen` and
`Krom2RawAdd2`; the retail string routines are bridged by address in
`call_native` next to `memset`, and `Krom2RawAdd`/`Krom2RawAdd2`
(`sdk/libapi_krom.c`) stand in for the BIOS kanji ROM the port has no copy
of: each Shift-JIS character is rendered once with FreeType from the face
fontconfig names for Japanese (Noto Sans CJK here) into the ROM's 16x15,
30-byte, one-bit pattern, which the module reads through the returned host
address. Checked from a state at the ending's last dialogue, mashing Cross
(`MEMORIES_INPUT`) at 400%: names and the wireframe monsters through
"Created by Konami Computer Entertainment Japan" with no interpreter failure.

The retail game never leaves the credits: `Main_RunCredits` runs the scene in
its last phase, after the save and the secret number, and drops the answer of
`Model_IsCreditsPresentationComplete`, so the screen stays on the last credit
until the console is reset. Game > Title screen after the credits (the
`return_after_credits` setting, `MEMORIES_RETURN_AFTER_CREDITS`, on by
default) has the port publish the main menu's mode three seconds after the
presentation is complete (`platform/credits.c`); the game's code is
unchanged. Checked by entering the ending with `MEMORIES_MODE_AT=1000:15`,
declining the save and running 60,000 frames: the main menu with the
setting, the last credit without it.

### Where the player's files go

Nothing the player owns lives beside the game any more. `platform/paths.c`
resolves one user directory -- `Documents\My Games\YFM Re-Decomp` on Windows,
`$XDG_DATA_HOME/YFM Re-Decomp` (`~/.local/share/YFM Re-Decomp`) elsewhere,
`MEMORIES_USER_DIR` instead of either -- and everything the port writes goes
under it:

| File | What it is |
|---|---|
| `settings.txt` | the settings (`MEMORIES_SETTINGS` names another file) |
| `controls.txt` | the control bindings (`MEMORIES_CONTROLS`) |
| `saves/slot01.sav` ... `slot10.sav` | the game saves, one per slot (see "Saves" below) |
| `memcard1.mcd`, `memcard2.mcd` | memory cards of older builds, only read now (`MEMORIES_MEMCARD1/2`) |
| `states/slot1.state` ... | save states (`MEMORIES_STATE_DIR`) |
| `screenshots/` | F12 and **File > Screenshot** (`MEMORIES_SCREENSHOT_DIR`) |
| `mods/` | mods the player installed (`notes/modding.md`) |
| `mod-data/<id>/` | whatever a mod stores, the only place one may write |

What an older build left in `./saves` is carried over on the first launch
that finds the destination missing (`Paths_MigrateLegacySaves`), so an
existing card, settings and bindings survive the move. The game's own files
(the disc image, `mods/` as shipped) stay where the release put them and are
only read.

### Window and menu bar

Two window backends exist under `src/pc/platform`, chosen at build time
(`tools/pc/build_game32.py --backend sdl|x11`, or `MEMORIES_BACKEND`; SDL by
default: SDL 3.4.16, the release the Windows build ships, built as a 32-bit
static library by `tools/pc/build_linux_sysroot.py`). Both share the menu (`menu.c`), the interrupt clock and the
scripted input (`platform_common.c`).

**SDL3/OpenGL** (`sdl.c`, the default) is the portable one: window, keyboard,
mouse, controllers (`SDL_Gamepad`, so any pad SDL knows, hot-plugged, two
ports) and audio (`SDL_AudioStream`, 256-frame periods) through the one
library that exists for Linux, Windows and macOS. An explicit OpenGL presenter
is preferred, with SDL_Render as a fallback when a GL context is unavailable.
The picture is a 320x240 streaming texture the GPU scales with nearest filtering, and the menu is a
transparent ARGB texture blended over it, uploaded only where it changed; so
the CPU never scales a frame. Window layout, the menu/HUD and pointer input
use SDL logical coordinates; SDL scales the complete composition to the
physical render target on high-DPI displays. SDL selects the native desktop
backend, avoiding XWayland cursor and DPI mismatches on Wayland; set
`SDL_VIDEODRIVER` only to override that choice for diagnostics. On this
machine (renderer `opengl` under X11)
the present path is about 0.25 ms a frame, down from 1.6 ms for the software
scaling below. Signals are blocked while SDL creates threads so the SIGALRM
clock stays on the main thread; Windows will still need the clock replaced
(`platform_common.c`). `MEMORIES_SDL_SCRIPT="200:click:20:13,260:move:60:69,
420:key:escape"` pushes pointer and key events at presented frames for
tests: synthetic X input does not reach SDL correctly (XI2, and XTest is
refused by this compositor), so the menu was verified that way.

**X11** (`x11.c`, `audio_alsa.c`, `gamepad_evdev.c`) is the Linux-only
fallback with no dependency beyond Xlib: it is plain Xlib. The frame it shows is one ARGB32 buffer the port
composes in software: the 320x240 picture scaled by an integer (Video menu,
`MEMORIES_SCALE`, 1-8) under a 26-pixel menu bar. The buffer lives in MIT-SHM
memory the X server reads directly (`XShmPutImage`), so a frame costs a
request rather than a copy of the whole picture down the socket, and the
menu's own activity (hover, an open menu, a slider drag) repaints and shows
only the rectangle the menu covers (`Menu_Bounds`), never a frame. With
`MEMORIES_TRACE_FRAMES=1` on this machine the whole present path, scaling
included, is about 1.6 ms a frame at 4x with no missed VBlanks.
`MEMORIES_NO_SHM=1` forces the `XPutImage` path.

Menu text (both backends) is FreeType through fontconfig's `sans-serif` face at 13 px, cached
once as coverage bitmaps and blended into the frame; a machine without a face
falls back to a built-in 5x7 font at double size. The bar is dark with an
accent highlight; menus have hover rows, separators, shortcut hints, check
and radio marks, and a shadow. Keyboard: F10 opens the first menu, arrows
move, Enter activates, Esc closes (Esc quits only when no menu is open).

| Menu | Items |
|---|---|
| File | Save/load state, slots 1-4, screenshot, reload settings, exit |
| Audio | Master/music/SFX sliders, mute and focus-loss mute, Gaussian (console) or cubic (sharper) voice interpolation (`audio_interpolation`) |
| View | Window scale and Menu size submenus, window mode, scaling/aspect/filter/VSync choices |
| Game | Game speed, Frame rate and Cheats submenus (Give 3 of every card; Unlock all Free Duel CPU duelists) |
| Mods | opens the mods window, which lists every mod found in `mods/` beside the executable and in the user directory (`notes/modding.md`) |
| Debug | HUD levels, pause/step, frame and VRAM dumps |
| Trace | Live frames, disc, SPU, input and state log-channel switches |

After starting or loading a game, **Game > Cheats > Unlock all Free Duel CPU
duelists** unlocks the full CPU roster without changing story progress or
win/loss records. If Free Duel is already open, leave and reopen it to refresh
the portraits and selection grid. Save normally to keep the unlocks.

`MEMORIES_TRACE_MENU=1` logs menu clicks and keys. The menu never reaches the pad:
a click on the bar or in an open menu, and the wheel there, are the menu's.
Menu changes from events (hover, clicks, keys, resizes) mark the menu dirty
and it is repainted once with the next game frame, or at most 120 times a
second while paused; a 1000 Hz mouse sweeping the bar used to present a
frame per motion event. Events are pumped before a frame is composed, so the
frame shows the input and menu state of that moment. The overlay is only
recomposed when the menu is dirty or the HUD's text changes (`Hud_Signature`;
the full-statistics level changes every frame), and a dropdown's shadow
blends only the strips outside the box: composing every frame with twelve
alpha passes over a 4K dropdown made the game crawl whenever a menu was
open. `MEMORIES_TRACE=window` reports compositions per 120 frames.
A row with a triangle opens a submenu beside it (one level: `ITEM_SUBMENU`,
`submenus[]`), on hover, click, Enter or Right; Left or Esc closes it.

The menu draws at a size multiple (`menu_scale`, `MEMORIES_MENU_SCALE`, View >
Menu size): bar, rows, marks, font and the HUD all scale together, and the
window is sized for the bar it gets. Automatic (0) follows the window height
(`Menu_AutoScale`): 1 below 1200 rows (including a 4x window), 2 at 1200 rows
and above (including a 4K display). This is one step smaller than the original
automatic size, with a minimum of 1; explicit 1x–4x choices are unchanged.
`MEMORIES_SDL_SCRIPT` accepts `frame:shot` to save the composed
window, which is how the menus are checked.

### Back to the title screen

Debug > Jump to > Title Screen leaves whatever is running for the title, the
way the retail game leaves a campaign loss. `Main_RunGameOver` fades the
music and the screen out, asks for the title menu (`D_8009B268 = 1`,
`D_8009B26D = 0`, mode 8), and longjmps to the point `Main_Init` set up after
the boot sequence. From there it resets the frontend runtime, loads the
main-menu package and runs the title again. The platform's `title_jump.c`
dispatches that sequence through `src/pc/overrides/title_jump.c`, after
`DebugMenu_Exit`'s `DisplayObject_Reset` and `func_80035A64`, with the
disc left idle first. That way, no transfer the old screen asked for lands on
the title's package. While the save slot menu is open, the request waits
for it to close, so the menu is never left drawn over the title.

The request is only taken between two mode runners. A `MEMORIES_PC` hook in
`Main_Loop` polls after each frame. That is the one point where no runner is
half way through a step, and where no nested frame loop is on the stack
(a fade, a disc wait).
The item is off while the title's own loop (`Main_RunFrontendLoop`) runs,
before `Main_Loop` starts and after every return, including a normal game
over or debug-menu exit. It is also disabled during the jump's disc wait and
fade, so another click cannot reset the following game. At the title, a
request would otherwise wait for the next screen. Progress not saved is
lost, as with a reset.

Save states carry the item's enabled state and discard pending UI requests
on load. `pc_title_jump` tests title entry, save-menu deferral, repeated
requests during a jump and state restoration.

`MEMORIES_TITLE_AT=N[,N...]` makes the request at presented frames N, for
checks. Requests scheduled while the item is disabled are consumed and
ignored at that frame. Checked headless: the jump works from a campaign duel,
and so do two jumps, each followed by New Game played to the duel again. It also works from
eleven screens reached with `MEMORIES_MODE_AT` from the options case: debug
menu, campaign, Library, map, Free Duel, Build Deck, name entry, Password,
Options, game over and Trade. After each one, the title and then the main
menu on Start come back pixel-identical. In the credits, a request made while
their save slot menu was open waited, and the jump came once Cross had saved
to a slot. By mouse (`MEMORIES_SDL_SCRIPT`), the item jumps from a duel and
is disabled at the title. 2P Duel setup forced by `MEMORIES_MODE_AT` stops
presenting frames right after the switch, with or without this change.
Reached from the menu with no saves, it stays in the title's loop.

### Deck slots

Game > Deck slots (F6) keeps up to ten decks beside a save. Switching to one
takes one step, for example between a farming deck and the campaign deck.
The game has one deck: the forty ids at the start of the save state. Its
trunk counts only the cards outside that deck, and Build Deck moves a card
from one to the other. Using a kept deck therefore puts the current forty
back in the trunk and takes the kept forty out (`src/pc/saves/deck_slots.c`).
It is refused, with the reason, in three cases:

- a kept card is in neither the trunk nor the deck ("Missing 3 x Mystical
  Elf");
- a returned card would take the trunk past the 250 a byte of it holds. The
  game's own return to a full trunk drops the card;
- the slot is not forty cards this game has, at most three of each.

One slot is **active**: the one holding the save's forty cards. It is not
written anywhere; each time the decks are read, the active slot is the one
whose deck the save has (the one the screen last made active, if it still
does). A deck in no slot takes the first empty one.

The decks are a draft kept with the save. Using a deck, making one or
clearing one changes the draft, and so does leaving Build Deck: the active
slot takes the deck Build Deck wrote, when it is forty cards (the not-ready
way out can leave fewer, and then the slot stays as it was). The file is
written only when the game is saved through the save slot menu, and read
again when a save is loaded (`SaveMenu_SaveCount`, `SaveMenu_LoadCount`).
So the decks go with the save: a game left unsaved loses its deck changes
with the rest. A save state holds the draft (the `deck-slots` chunk); a
state from before it reads the file. A restored draft is written on the next
game save even if it was clean in the snapshot, since the file may have
changed afterwards. Loading a state also closes any old deck picker.
The screen shows "saved with the game"
while the draft differs from the file.

The screen (`deck_menu.c`) is drawn in the overlay like the save slot menu.
It is read from the pad, or the keys mapped to it: Cross uses a deck (on an
empty slot, it makes a new one, a copy of the current deck), Triangle clears
a slot other than the active one, Circle or Esc closes. While it is open,
the game gets no buttons, and it gets them back once the button that closed
the screen is released, so the game never takes that press as its own.

Build Deck opens with it. `Main_RunBuildDeckMenu` asks
`DeckMenu_BuildDeckEntry` before it copies the deck (`func_800323F8`):
while the list is up, the mode is not set up yet (0x40 clear), so Build Deck
has no copy yet. An incomplete deck bypasses the picker so it can be repaired. Cross picks the deck to edit, which becomes the active one,
and Build Deck is then set up from the save. Circle goes back where Build
Deck was entered from (`D_8009B269`), as its own way out does. As Build Deck
leaves, `DeckMenu_BuildDeckLeft` puts the deck it wrote in the active slot.

The chest a campaign duel opens first (`Main_RunDuel`'s step 0, the same
Build Deck screen) takes F6 the same way: it is left through step 4, whose
confirm there has no EXIT, so a short deck cannot reach the duel; then
`DeckMenu_DuelChestLeft` sends the duel back to step 0, where
`DeckMenu_DuelChestEntry` shows the list (Circle keeps the deck) before the
chest again. The duel's own way in is unchanged: the list shows only after
that F6.

In Build Deck, waiting on a pane, F6 (or the menu item) goes back to the
list: Build Deck is left the way Circle leaves it (step 4,
`func_800339D0`, which writes the deck back) and entered again to the list
instead of returning. With fewer than forty cards, that step asks first, as
the game does. Its BUILD DECK goes back to editing and drops the list. The
shop's EXIT leaves with the deck short, and then the game itself refuses the
campaign ("YOU MUST PREPARE A DECK..."). The list is not shown for a deck
short of forty: Build Deck opens straight on it to be finished, since no
deck can be used in its place. Nothing here makes a deck short of forty:
slots hold forty cards, and a deck is used only when it checks out.

The screen opens with a game loaded, on screens that keep no copy of the
deck:

- the main menu with Campaign. It runs in the title's own loop until the
  player first leaves it, so `Main_RunFrontendLoop` polls there too. The
  title's menu, or a save left in the workspace by a jump to the title, does
  not count: the entry byte `gMain_bMenuID` is 5 or more only on the loaded
  one;
- the campaign map;
- a card shop's menu (Save / Build Deck / Return to Title / Leave Shop)
  while it waits for a choice: the only way to Build Deck in the present,
  which has no map (`Script_OpSavePrompt`, scene-script command 13);
- Free Duel's opponent select;
- Build Deck, as it is entered, and from a pane (above).

Changes are made where `Main_Loop` or that loop is between two steps.
Elsewhere, the screen says where it opens. The setting `deck_slots`
(Game > Use deck slots) turns all of it off: Build Deck, the shop's menu and
the file are then the game's alone.

The card shop's own menu also offers it: DECK SLOTS under BUILD DECK.
`Script_OpSavePrompt` asks `DeckMenu_ShopMenu` as it makes the menu (a row
taller) and hands the choice to the screen; the rest of its code keeps
numbering its four entries (`DeckMenu_ShopChoice`). The menu's text,
string 0x11, is the retail listing with the line added, compiled as a
mod's text is (`Text_CompileOwn`) so its `{choose}` jumps land. The game
keeps four entries' worth of enabled bits (`Text_HandleChoiceCommand`), so
wherever it sets the menu's choices up (after its text, the memory card and
the title confirm) `DeckMenu_ShopRestore` moves the cursor and enables all
five entries. It does not reuse a nested prompt's enabled mask. The
`deck-shop` save-state chunk keeps whether the menu has the extra entry
and rebases pointers into its compiled text before restoring game memory,
so loading a shop state also works in a fresh process. With the setting off, or a translation that rewrites
string 0x11, the menu is the game's. Checked on a save in the tournament's
shop: the setting off is pixel-identical to master (the menu, and the cursor
on LEAVE SHOP); DECK SLOTS opens the screen, and Circle brings the menu back
with the cursor on it; LEAVE SHOP has its normal highlight and leaves;
RETURN TO TITLE's No returns to it; SAVE cancelled, then LEAVE SHOP;
BUILD DECK opens.

The slots are kept in `decks/<duelist code>.txt` in the user folder, one
file per game, as text. Each line is `slot: forty cards`, a retail id or, for
a card a mod adds, its identity, because those ids change with the mods
applied. A line that does not read as forty cards leaves its slot empty, and
the log says so. Card names come from the game's own text lookup and glyph
table.

`MEMORIES_DECKS_AT=N[,N...]` opens it at frames, for checks. `tests/pc/deck_slots_test.c`
covers the rules and the file. These were checked by driving the pad on a
save loaded through the save slot menu:

- a deck kept in slot 1 and another used from slot 2. The saved game then
  holds slot 2's deck, and each of the 722 trunk counts is exactly what the
  swap should leave;
- the missing-card refusal with the card's name, and an invalid slot;
- the screen on Free Duel's opponent select;
- F6 and the menu item, which are off on the title's menu and with the
  setting off, and Esc closing the screen without quitting;
- Build Deck from the main menu: the list first, slot 2 picked, Build Deck
  set up with its deck; left, the decks file unchanged until SAVE;
- an empty slot picked (a copy), left, then SAVE and Overwrite: the file
  gains the slot; the same without SAVE: the file is unchanged;
- the active deck picked, a trunk card swapped for a deck card in Build
  Deck, left, SAVE: the active slot has the swap;
- a new slot left unsaved, F5, then a new session loading that state and
  saving: the file gains the slot (the draft came with the state);
- Build Deck from the card shop: Circle on the list goes back to the shop,
  a deck picked opens Build Deck, which returns to the shop;
- the setting off: Build Deck from the main menu and from the shop
  pixel-identical to master.

### Present pass (Video > Color)

The OpenGL presenter can draw the game picture through one fragment program
(`src/pc/render/present_pass.c`), under the menu and the HUD, which are not
filtered. The program runs where the picture's quad is drawn in `show()`
(`sdl.c`), both for the OpenGL picture and for the one the CPU uploads, so
it works at every internal resolution and in widescreen. The effects are
settings that are off at their defaults:

- Brightness, Contrast, Saturation and Gamma (percent, 100 leaves the
  picture alone). They are applied in that order in the shader: gamma,
  contrast about mid grey, brightness, then saturation against Rec. 601
  luma. The sliders are in Video > Color, with Reset.
- CRT scanlines (Video > Effects, `crt`). There is one scanline per line of
  the console's picture: 240, the source height over its nearest multiple
  of 240, so the lines match at every internal resolution. Each line is
  darkened towards its edges, and an aperture grille is drawn over window
  pixels, with the lost brightness given back.
- Reduce flashes (Video > Effects, `reduce_flashes`). The picture's average
  brightness may rise by at most 2.0 a second (black to white is 1.0). A
  frame that brightens faster is darkened as a whole to that rise, and
  darkening is never held back. Small moving things barely change the
  average, and the game's own fades are slower than the limit. The rate is
  timed in real time, so it holds at every speed. Without framebuffer
  objects, only this effect is unavailable.
- Sharp bilinear (Video > Filtering, `filter=2`; Nearest is 0, Smooth 1).
  Each texel is drawn as a flat block of whole window pixels. Only the
  window pixel a texel's edge falls inside blends the two neighbouring
  texels. So an uneven scale, such as 4:3 at 4.5x on 1080 lines, shows
  texels of equal width without bilinear's blur. At a whole scale every
  window pixel samples its texel's centre, so it is identical to Nearest.
  At a scale ending in .5 some edges fall exactly on a pixel's centre, and
  rounding decides whether that column blends or not. xBR takes precedence
  over it.
- xBR pixel smoothing (Video > Effects, `xbr`). The picture is read through
  xBR level 2 (written from its published rules, in the same program). A
  texel's corner is cut along a 45, 30 or 60 degree edge found in its
  neighbours and filled with the nearer neighbour's colour. The cut is
  antialiased over one window pixel, so it works at any window size.
  - At internal resolution 1x it works on the finished picture's texels,
    and replaces Smooth and Sharp bilinear filtering while it is on.
  - At 2x and up (the OpenGL picture) it works on the textures instead, in
    `gl_picture.c`: each textured primitive's texels go through the same
    rules as it is drawn, so sprites, fonts and 3D textures are smoothed
    at the internal resolution, over whatever lies under them, and the
    picture is not smoothed again (Filtering still applies to it).
    Transparent texels count as one colour
    far from all others, so a sprite's outline rounds too, and where a
    corner is filled with transparency nothing is drawn. A primitive only
    sees its own rectangle of texture (the edge repeats past it), so a
    picture made of several rectangles shows no seams. HD text and texture
    pack images are left as they are. A texel like the four beside it is
    drawn after five reads, so at 4x the duel's replay time stays within
    its noise (about 2 to 3.5 ms here, on or off).

While every effect is at its default, the pass is not used, and the picture
is drawn by the fixed-function quad exactly as before. The SDL_Render
fallback (no OpenGL) has no pass.

### Speed, frame rate and vsync

Three independent controls (`platform.h`, `platform_common.c`):

- **Game speed** (`speed`, `MEMORIES_SPEED`, Game menu, 25-400 or -1) scales
  the game clock: VBlanks, disc timing and SFX run that much faster, so 200%
  is 119.88 game frames a second. Music sequencing runs on real time and
  keeps its tempo. Tab holds 400% while pressed; P pauses; `.` steps a frame.
  Uncapped (-1) fires a VBlank whenever the game waits for one.
- **Frame rate** (`fps`, `MEMORIES_FPS`, Game menu) is how many of those game frames
  reach the window: 0 (default) follows the display's refresh rate, -1 shows
  every game frame, or a number. Presentation is paced on a fixed grid apart
  from the game clock, so a cap of 60 at 400% shows every fourth frame and
  the game never waits for the window. Frames that are not shown still poll
  input and the menu.
- **VSync** (`vsync`) blocks the present on the display. That may only pace
  the game while game frames come no faster than the display refreshes;
  above that (200% on a 60 Hz display, or uncapped) the backend presents
  unsynced and the frame-rate cap alone limits presents. At 100% on a 60 Hz
  display the game's VBlank is re-phased to the display so the two rates do
  not beat.

`VSync(0)` presents, then waits for the next VBlank after entry. It must
not return at once because a VBlank passed since the previous call:
`Graphics_SyncFrame` resets the game's own VBlank counter to -1 just before
calling, and `Input_UpdatePads` takes a counter still at -1 as a lag frame
and publishes every press a second time on the next frame. A catch-up
variant was tried and doubled inputs at 300%. A frame that overruns its
VBlank therefore costs a whole slot, as on the console; 200% and 400% hold
their rates because presents are cheap on the accelerated path (below).

The HUD (F3) shows game frames a second and shown frames a second; with
`MEMORIES_TRACE=frames` the same appears every 120 frames with the clock
rate, VBlank rate and sequencer rate.

**Software OpenGL on Wayland.** The 32-bit build on an NVIDIA Wayland
desktop gets Mesa's `llvmpipe` through EGL (there is no 32-bit NVIDIA EGL
Wayland path), and a software renderer takes 6 ms and more to present a
frame, which alone breaks 200%. `Platform_Open` therefore retries with the
`x11` (XWayland) driver when the GL renderer is software, where the NVIDIA
driver presents in about 0.2 ms. `SDL_VIDEODRIVER` pins a driver and skips
the retry; `MEMORIES_TRACE=window` logs the renderer, the refresh rate and
every vsync change. XWayland reports no refresh rate, so the rate the
Wayland driver reported before the retry is kept. The pointer over an XWayland
window is Xlib's core font cursor (tiny, unthemed) unless the 32-bit Xcursor
library is installed: Xlib loads `libXcursor.so.1` itself to substitute the
desktop theme at the size KDE publishes for Xwayland (`xrdb -query`:
`Xcursor.size`). On Arch that is `lib32-libxcursor`; without it there is no
fix from inside the game short of drawing its own pointer.

### Cooperative clock

The cooperative clock is the default (since 2026-09-24, after a 29-minute
session played on Windows with it and no problem). It runs the game with no
interrupt at all. `MEMORIES_CLOCK=interrupt` brings back the old clock, a
timer that interrupts the game as the console's VBlank would. The sampling
profiler takes its samples from that timer, so `MEMORIES_PROFILE` without
`MEMORIES_CLOCK` chooses it too, and the log says so. The console's interrupt
work (the VBlank callback, the sequencer's root counter, disc delivery,
MDEC, memory card) is run on the main thread where the game calls in to
wait or to read the time: every `VSync`, `Platform_WaitVBlank` (which wakes
every 0.5 ms and runs what is due) and `Platform_PollTime`. The game's
interrupt code therefore never runs in the middle of anything, which is what
the SIGALRM holds and, on Windows, the suspend-and-redirect clock thread and
its repairs exist for; in this mode that thread only watches for stalls.
It is the model the deterministic runs (the smoke fixtures) have always used,
with real time instead of synthetic steps.

Why it holds: every loop in which the game waits for something its interrupt
code sets goes through one of those calls (`Main_AdvanceFrame`, the VBlank
waits; `File_WaitForTransfers` calls `Main_AdvanceFrame` each turn). In a
deterministic run the fallback timer reports any second the game spins
without a wait, with the address (`clock: the game spun a second without a
wait`; checked to fire with `MEMORIES_CRASH_TEST=spin`); it never fired
through boot, the movie, title, menu, options, the story, both duel cases,
and the debug menu, Library, campaign map, Free Duel, Build Deck, name
entry, password, game over, trade and credits entered with
`MEMORIES_MODE_AT`.

Measured on Windows at 100% (`MEMORIES_TRACE=frames` reports the
sequencer's lateness against when each tick was due, every 1000 ticks, and
each stretch of more than 20 ms between two `VSync` calls):

| run | clock | sequencer lateness, mean / worst | ticks later than 5 ms | clock repairs |
|---|---|---|---|---|
| 3D duel case, first | interrupt | 818 us / 135.6 ms | 18 | 293 deferred ticks |
| 3D duel case, first | cooperative | 75 us / 4.1 ms | 0 | none |
| 3D duel case, second | interrupt | 724 us / 4.5 ms | 0 | 288 deferred ticks |
| 3D duel case, second | cooperative | 74 us / 4.5 ms | 0 | none |
| options case | interrupt | 740 us / 1.7 ms | 0 | 10 deferred ticks |
| options case | cooperative | 27 us / 4.0 ms | 0 | none |

VBlanks ran at 59.9-60.0 a second and the sequencer at 73.4-74.0 in both.
The options case ends on the same frame, pixel for pixel, with either clock;
the duel cases do not, with either clock, from one real-time run to the
next: the deck is shuffled from time-dependent state, so the scripted
presses meet a different hand (checked on the hand camera case: three
clocks, three hands).

Not yet: Linux has only been compiled with it (its stall reports come from
the crash monitor process, since no tick runs); the sampling profiler
(`MEMORIES_PROFILE`) takes its samples from the interrupt and gets none;
save states made in one mode have not been loaded in the other. Removing the
interrupt clock is a later step, once this has been played on both systems.

### Mods > Hand camera

On by default (`mod.hand-camera=0` in `settings.txt` in the user directory
turns it off; older files' `hand_camera=0` is still read). While
the hand is up (duel scene state 4, the human's hand actions), where the
console ignores the shoulder buttons, L1 and R1 turn the camera around the
mat and L3 and R3 (T and Y, or the stick clicks) zoom it in and out; L2 and
R2 stay the game's own top-down look at the opponent's field: `mods/hand-camera/hand_camera.c` moves the
view state's own heading and distance (`D_800F2848.angle`, 20 units of a
0x1000 turn a frame; `field_00`, 6 units a frame between 200 and 1400, the
duel's own view being 600 away) and re-applies `ViewState_ApplyOrbit`, which
the game itself only does while one of its own camera tweens runs (the first
version moved the heading alone and only the 3D Monsters turned); the mat,
the card sprites and the monsters all follow. The view stays where it is put:
nothing eases it back on release and nothing resets it when the hand closes,
because every camera move the game makes from there (placing a card, the turn
switch, an attack) is a tween from the current view to an absolute target,
so the game's own moves carry the camera back smoothly. Checked from
`slot1.state` (2026-09-21) with `MEMORIES_INPUT="60:0400,150:0000,210:0002,
270:0000,280:0004,320:0000"`: turned at 120, still turned at 200, zoomed in
at 260, out again at 330.

### Mods > 3D Monsters

A window of extras the console could not run, one directory each under
`mods/` (`notes/modding.md`), off by default. The first is **3D Monsters**: every face-up monster on the duel
field stands on its card as the model the battle presentation uses, animating
on the spot, the near side turned to face the opponent. Ticking it in
**Game > Mods** takes effect on the next frame and is kept in `settings.txt`
in the user directory as `mod.3d-monsters` (`MEMORIES_MOD_3D_MONSTERS=1` sets
it for one run).

What it costs the console, and why the port does not pay it: a monster's
`MODEL.MRG` record is 276 sectors -- 96 of them model data, a quarter of the
machine's RAM -- and its textures are a 256x256 block of VRAM, and the duel is
already using both. So each monster here gets

- a private 256 KiB arena, mapped at `0x90000000` upwards, outside guest RAM
  and still at a negative address, because model code tells a pointer from a
  small number by its sign;
- a private texture bank in the software GPU: VRAM-shaped memory that a
  primitive selects through bits 11-14 of its texture-page word, which the
  hardware ignores and retail always leaves zero (`SoftGpu_Bank`,
  `src/pc/render/soft_gpu.h`). Nothing else in VRAM moves, and a bank holds
  the palettes at their own coordinates too, so the model's own primitives
  need no other change.

Loading is synchronous and beside the game's streaming
(`Memories_DiscReadSectors` reads the whole record in one call, about 1 ms),
so a monster appears without the duel's own transfers noticing. The record's
seventeen phases are replayed exactly as `func_80056D7C` programs them, into
the arena instead of the two fixed duel arenas; the phases that upload to VRAM
upload for real, because the setup's palette phase reads them back through the
GPU, and the duel's pixels are put back afterwards. `func_80056828` then runs
the game's own eleven setup phases. The slot is flagged `0x80`, which is what
the game itself uses for a quiet load: no sequence bank, no voice block, and
the three control-module command words left at -1, so no per-monster module is
ever called.

Drawing is `func_800540B4` and `func_800556E8`, the pair the Library's card
model view drives, with the slot placed by `func_8005A4C4` at the card's own
field coordinates (`D_800908A0`). It is sorted into the game's own model
ordering table before the frame is sent, so the game's layering applies:
monsters stand over the field and under the hand and the interface. Two
measurements make them stand right:

- **where the body is.** A duel model is built around the point between the
  two duellists, so its parts sit some 250 units up the field from the origin
  `func_8005A4C4` places and about 200 above it. Walking the parts' world
  matrices once gives the offset, and the feet land on the card.
- **how big it is.** A model's parts say where its joints are, not how far its
  skin reaches -- Korogashi is one big ball around a single joint -- so the
  monster is sorted into a scratch table that is never drawn and its packets
  are read for the height they cover. Drawing it at the square root of that
  against a middling monster keeps the order, a dragon still towering over
  Sangan, while bringing a sevenfold range down to about two and a half.

Which way a monster faces is fixed to the side that owns its zone: the
player's monsters face up the mat, the opponent's face down it, as the
battle presentation stands its two slots. It is not fixed to whose turn it
is, which was the first version's mistake: the turn switch
(`DuelScene_UpdateTurnSwitch`) swings the camera a half turn round the mat
over 48 frames and flips `D_8009B1D5`, the acting side, a third of the way
through, so choosing by that snapped every monster round mid-swing and left
the player's showing their backs for the opponent's turn. The monsters ride
round with their cards because they are placed in field coordinates and the
pass draws through the duel's own `GsRVIEW2`. Each also floats `LIFT_PIXELS`
(8) above its card, so the card shows beneath it.

Two more things the field taught it: the cards sort at a sixteenth of their
distance and a model's primitives at a quarter of theirs, so a monster is
sorted at its own scale into a table of its own (the other frame's packet
area), and that run of packets goes into the game's table whole, at a
quarter of its nearest entry and three entries nearer, which draws it on the
card rather than under it. The first version quartered the GTE's Z factors
instead, which put four of a model's depths in one entry; parts sharing an
entry draw in the order they were sorted, so a skirt's inside or a limb
showed through the body and flickered as the animation moved it, which the
internal resolution made plain; and the duel flies
its camera down to eye level for the zone picker, using a card and the
guardian-star presentation, where a monster standing on a card has nothing to
stand on, so the pass runs only while the camera is above the mat (pitch below
512 of a 4096-unit turn).

Switches: `MEMORIES_MOD_3D_MONSTERS=0/1`, and the mod's settings (the
`mod.3d-monsters.<key>` lines in the settings file, or
`MEMORIES_MOD_3D_MONSTERS_<KEY>` for one run): `SCALE` (4096 = as
measured), `PIXELS`, `DEPTH`, `PITCH`, `LIFT` (field units above the card, 2
per pixel from the duel's view), and `TEST=<card id>`, which stands a
different monster in all ten zones; that is how the cache, the arenas and the
banks were measured together. `MEMORIES_TRACE=mods` logs it.

Checked from the duel in `tmp/pc/states/slot1.state`: the monster loads in
about 1 ms and animates, ten of them at once keep the frame rate, the duel's
own graphics are unchanged with the mod on or off, the menu item takes effect
live and persists, and picking up a card, choosing a zone, the guardian star
and the return to the field all behave as they did. The turn switch was
watched frame by frame with `tmp/pc/mods/turn/cap.sh` (plays the first hand
card from slot 1, ends the turn with Start, dumps the frames it is given and
tiles them): the monster keeps its facing through the swing and ends facing
the camera on the opponent's turn. A battle presentation with the mod on has
not been watched yet.

### Images from the disc

`python tools/pc/extract_images.py [family ...]` writes the game's images
from `game/DATA/*.MRG` as PNG under `tmp/pc/images/`, named by where they
come from, with `manifest.json` giving each one's provenance (archive, byte
offset, size in VRAM words and rows, depth, palette offset): the identity a
texture pack goes by, with names as aliases on top. The archives hold no
image files, so the tool replays the game's own loaders, one family at a
time; so far `cards`, the 722 cards' artwork from `func_800289BC` (WA
sector `(n-1)*7 + 722`, seven sectors: the 102x96 8-bit picture, its
256-entry palette, the name strip under it and the strip beside it), 2,166
files, and `portraits`, the 48x48 dialogue portraits through their 64-entry
palettes (the campaign's 25 and Free Duel's 40, `0x980`-byte records).
`--names cards.tsv` (card_number, name) puts the card's name in the file
name. `sheets` replays every screen loader that streams its images through
the GPU path (`file_transfer_runtime.c`: a sector is a 64x16-word tile,
sixteen stack into a 64-word column of 256 rows, contiguous in the archive,
the next column 64 words to the right): the main menu (`SU.MRG`), the boot
UI and the title, the story dialogue UI, the campaign, Free Duel, name
entry, password, options, game over, the duel results and rewards, the
Library, the seven duel terrains, the campaign map's strip and the 65
display-effect records of the duel. A sheet is one PNG per column and per
way the game reads it, depth and palette (`notes/mrg-files.md` for the
loaders; the readings come from the draw code and from dumps, `--variants
<assets.txt>` adds a dump's). `scenes` is the story's pictures
(`ScriptImage_RequestTransfer`: 33-, 81- and 113-sector records from WA
sector `0x21D5`, the card shop among them). Records laid out alike (the
terrains, a mode's story pictures) share the readings a dump shows for one
of them, and entries whose pixels come out identical (the duel-hand block
in all seven terrains and the Library) share one file. `--assets
<assets.txt>` extracts whatever a texture-dump run drew (below), under
`assets/`, named by archive, offset, size, depth and palette, skipping
what a sheet covers: the way to cover what no family describes yet. Every
dump so far (title, main menu, options, the story, both duel cases) comes
out pixel-identical to the sheets' crops, and a pack of the sheets as they
are draws the same frame as no pack at 1x, 2x, in software and in GL. A
mod with `"textures"` in its manifest replaces the images at draw time
from such a directory (`notes/modding.md`, "Texture packs"). Not yet: the
monster textures (`MODEL.MRG`) and the campaign map's own pictures, which
its overlay uploads from a 134-sector block.

### Texture dump (what is on screen)

`MEMORIES_DUMP_TEXTURES=<directory>` writes every texture the software GPU
draws as a PNG named by its hash (`src/pc/render/texture_dump.c`), a
discovery tool: what a screen is made of, at what size and depth, through
which palette. A texture is the rectangle of texels one textured primitive
covers, decoded through its palette, so a sprite comes out at its own size
and colours and the same one drawn again is the same file; the hash covers
the texel indices and the palette entries, so a palette swap is another
image. `textures.txt` in the directory lists each hash with its size, depth,
page and palette coordinates. Dumping costs a hash per primitive, so it is
for a capture session, not play; a duel dumps about 1,900 images. The hash
is not what a texture pack goes by: images are named by where they come
from on the disc (the archives stream raw VRAM blocks, `notes/mrg-files.md`;
the loader call sites are the asset table), the way a decompiled port can
and an emulator cannot. So the same run also traces provenance: the disc
layer reports every copy of sector data into game memory
(`TextureDump_Delivered`), an upload looks its pixels up in those and tags
each VRAM word with its disc byte offset, moves carry the tags, fills and
drawing clear them, and a primitive whose texels and palette are all tagged
adds a line to `assets.txt`: offset, row layout (a stride, or each row's
offset when the streamer laid the blocks side by side), size, depth,
palette offset, the pixel crop within the first word, and the hash of the
PNG it was drawn as. `extract_images.py --assets <dir>/assets.txt` then
writes those images from the archives, and every one comes out identical
to the PNG the game drew (76 of 76 through the title and main menu), which
is the proof of the provenance. `MEMORIES_DUMP_TEXTURES_FROM=<frame>` starts
both lists over at that frame, so a dump holds everything one screen draws,
also what an earlier screen drew first (the duel's digits after the Build
Deck's). A state load restores VRAM without
deliveries, so it clears the tags: the textures traced after it are the
ones loaded after it.

### Internal resolution

View > Console resolution / Internal 2x, 4x (the `internal_scale`
setting, `MEMORIES_INTERNAL_SCALE=N`, 1, 2, 4 or 8) shows a picture of the
whole of VRAM at N x N pixels per VRAM word in 24-bit colour instead of
VRAM. VRAM itself stays exactly what the console's would be: the game reads
it back and states hold it, and the 1x frame the smoke fixtures hash is
byte-identical at any scale. No dithering in the picture; the mask bits
are VRAM's; a texture pack's image is sampled at its own resolution there,
VRAM's own texels otherwise. A line is the console's one-pixel line made N
times thicker, one quad covering each picture pixel once (`line_quad` in
both renderers), so a semi-transparent line blends once per pixel as on the
console. Drawing it as an N x N block per picture step blended the Library's
grid up to N times, too bright, and at 4x cost the OpenGL replay about
880,000 vertices a frame (1200 frames of the Library took 30 s, now 2.3 s).

Two renderers draw it. With the SDL backend on OpenGL 3.0 or later
(`gl_picture.c`) the software GPU only records what it does to VRAM (every
GP0 batch, every transfer, `SoftGpuRecorder` in `soft_gpu.h`) and the
record is replayed at present into a framebuffer: VRAM is an integer
texture the fragment shader decodes (4, 8 and 16 bits per texel through
the palette) as the software GPU samples it, a mod's texture banks are an
array texture uploaded when a replay finds them changed, and a pack's
images are textures of their own with the pack's word-to-entry maps beside
them. Opaque pixels and blending modes 0, 1 and 3 share one draw (the
colour comes out pre-multiplied, the destination's factor in alpha); mode
2 draws its opaque texels, then its semi-transparent ones subtracted.
What a primitive draws is not sampled by a later one of the same frame
(the game never renders to a texture); across frames VRAM is uploaded
whole after each replay. A duel with 3D Monsters replays in 1.5-2.5 ms a
frame at 2x and 2.5-4 ms at 4x on the development machine.

Without that (the X11 backend, `MEMORIES_GL_PICTURE=0`, an older GL) the
software GPU draws every primitive a second time into the picture
(`soft_gpu.c`, `picture_*`), the picture's pass before the word's so both
see the same mask bits; uploads, fills and moves keep it in step; a state
load redraws it from VRAM. That costs about 9 ms a frame at 2x and 30 ms
at 4x in the same duel. It is the OpenGL pass's oracle: `MEMORIES_DUMP_FRAME`
with `MEMORIES_DUMP_PICTURE=1` writes the picture (from either renderer)
instead of the frame, and `MEMORIES_DETERMINISTIC=1` makes a windowed run
with a frame dump as deterministic as a headless one, so the two can be
compared on one frame of the same build; the title, the main menu, a 2D
duel frame and a 3D one come out identical pixel for pixel at 2x and 4x
(three edge pixels differ on the 3D monster). The X11 backend shows VRAM
as before (`Platform_PresentPicture` returns 0).

Widescreen draws the game's full-screen drawing areas into wider targets
(`soft_gpu.c`, `SoftGpu_WideMargin`). Each target is its area widened by a
margin on each side. Every primitive drawn to the area is drawn a second
time into the target, shifted right by the margin: polygons and lines are
unclipped at the sides, sprites keep the 4:3 clip. Transfers into the area
are copied into the target's centre. VRAM itself is never widened.

The software GPU keeps the 1x targets. At 2x and up the OpenGL pass draws
its own at the scale (`gl_picture.c`, "widescreen"). Its targets follow the
software GPU's rule and are laid out the same way. Each is a texture of
the widened area alone. Its primitives go through the same runs as the
picture's, in the same order, drawn with the viewport moved. Since each
primitive is gathered for the picture and then for its target, the runs
alternate between the two; `flush_runs` draws the picture's runs first and
then each target's, each in its own order and with neighbours in one state
joined (`group_runs`). Nothing in one flush reads the picture or a target
back, so the picture is the same, and the 3D Monsters duel takes 17-22
draws and 2-4 framebuffer switches a frame instead of about 400 of each
(4x, NVIDIA: 1.6-1.9 ms per replay instead of 5-6.7 ms). Before this,
the software GPU drew the scaled targets on the CPU next to the OpenGL
pass. Unthrottled, the 3D Monsters duel case to its frame now takes 40 s
instead of 128 s at 2x, and 41 s instead of 350 s at 4x. Against the
software picture, the widened duel comes out identical save 3 edge pixels
at 2x and 20 scattered ones at 4x (4:3 has 3 and 14: polygon edges and
texture rounding), and the main menu save the corner below.
After a resync (an overflowed record, a state load) a target's sides stay
black until the next frame draws them.

A target made in the middle of a frame starts from what is already there:
the software GPU seeds its scaled centre from VRAM's 1x words, the OpenGL
pass copies the scaled picture, which still holds what was drawn there at
the scale (in this frame or an earlier one). So a corner the frame does not
draw again shows at 1x in the software picture and at the scale here: two
texels of the main menu's top row. Whatever the game draws every frame
comes out the same. As in the software GPU, a target's sides are made
black when it is shown with nothing drawn into it since it was last shown
(a movie, a still loaded into VRAM).

Video > Anti-aliasing (`msaa`, `MEMORIES_MSAA`: 0 off, 2, 4 or 8 samples)
draws the OpenGL picture and widescreen's targets into multisampled
renderbuffers, which smooths the edges of polygons: the 3D monsters, the
duel table, the cards laid on it. Sprites and textures are drawn as before.
Each buffer is resolved into its texture wherever the texture is read: a
move's source, a target's centre, and the end of each replay, for
presenting and frame dumps. A GPU cannot copy into a multisampled buffer,
so the pass draws what it copies in as a quad.

The buffers cover all of VRAM at the scale. With 8 samples that is about
256 MB of video memory at 4x and 1 GB at 8x. Where the driver has no room,
it says so once, and the picture is drawn without until the setting or the
scale changes. (With the allocation made to fail, the duel comes out
identical to off, in 4:3 and widescreen.) A driver with fewer samples
gives what it has, and says so.

Turning anti-aliasing on or off mid-game carries the picture over. After
a switch, the duel case's frame is identical to one run with the new
setting from the start. The widescreen targets are made again, so their
sides are black for a frame, as after a resync. With anti-aliasing off,
the picture is identical to before. In widescreen the centre of a target
is identical to the 4:3 picture, with it on or off.

It changes nothing at 1x, and nothing in the software picture.

### HD text

Video > HD text (`hd_text`, `MEMORIES_HD_TEXT=1`, off by default) sets the
text's letters in a font at the internal resolution instead of drawing the
retail 8x12 and 16x16 cells texel by texel (`src/pc/text/hd_text.c`). It is
a change to the OpenGL picture above, so it shows at internal 2x and up,
in widescreen too. The software picture (`MEMORIES_GL_PICTURE=0`) and 1x
are as before.

The retail font is anti-aliased in its indices, which the text palettes run
from black up to the text's colour. The dark outline is the lowest index
(1, and 2-3 in the large font). Above it, an index is how bright the texel
is, and the letters are shaded brightest at the top of the cell. An HD
letter is made the same way at N pixels per texel:

- the character is set in the font glyphs.c sets added letters in (a mod's
  `font`, else the system's sans-serif);
- the retail font is measured once from its letters and digits: its
  baseline, x-height, capital, ascender and descender lines, its stem and
  bar weights, its outline and each row's shading. A texel at 70% of its
  row's brightest counts as fully covered, because the large font is shaded
  across its strokes as well as down;
- a letter or digit is set to those lines, so every small letter is the
  same height, and so is every capital; anything else takes its cell's
  height. Across, it stands where its cell's letter does, as wide as it
  (at most a quarter wider than the font's proportions; a bare stem like
  an l keeps them);
- stems and bars are thickened or thinned to the retail weights. Where the
  font's I is a bare stem and the retail one has serifs, it gets serifs,
  so it is not taken for an l;
- each pixel's coverage goes onto the run from the outline's index to the
  row's shading (half-way from its average stroke to its brightest, so the
  colour is the cell's), and the outline's index goes in a band a texel
  wide round it.

The shader samples the HD indices in place of the cell's, and the glyph's
palette does the rest. So colours, fades, flashes, semi-transparency and
the order the game draws in are the game's, including turned and leaning
letters and the letters translations add (texture bank 15).

func_80035E20 marks its glyph primitives with bit 15 of the texture-page
word, which the hardware leaves unused (bits 11-14 are the bank). Only
marked primitives are drawn this way. The mark changes nothing else: the
smoke cases give the same hashes with `hd_text=1`.

Glyphs that are not letters, digits or ASCII punctuation (the card-type
icons, the arrows) keep their texels. So does any letter no font sets. The
pictures are made the first time a letter is drawn, and made again if its
cell changes; the atlas holds 896 (its last four rows of cells are the
titles').

Card titles go the same way. The plate on a card (the 96x14 4-bit strip
func_800289BC uploads under the art) is set anew from the card's name as it
reads now: the base cards', a mod's added cards' and a translation's alike
(`Cards_NameUtf8`). It is the port's own plate renderer (`src/pc/cards/art.c`,
the one that makes the plates of added cards: Times at 13 pixels, the
baseline under row 11, squeezed past 90 columns) at N times the size, and
its coverage goes onto the plate's inks, 1 the darkest to 7 the faintest.
The card view subtracts those inks from the frame, so through the plate's
palette the letters come out dark with smooth edges on every frame colour.
func_800289BC notes where each plate went (`HdText_TitleUploaded`); a 4-bit
primitive sampling those words while they still hold the plate (the flat
strip, or a piece of the turning card) samples the picture instead. Twenty
titles are kept, the least recently drawn made over. So no pack needs to
carry names, and a card a mod adds reads like the rest.

### HD numbers and labels

Video > HD numbers and labels (`hd_hud`, `MEMORIES_HD_HUD=1`, off by
default) does for the duel's numbers and labels what HD text does for the
text. They are sprites from sheets of their own, not the font's cells, so
HD text never reached them. It works in the OpenGL picture at 2x and up; 1x
and the software picture never change.

- **Digits.** Covered: the life points, the deck counts, the hand's and
  field's ATK and DEF (8x8, 8-bit, beside the duel's terrain at (896, 256)),
  the field cards' 12x16 numbers and a second set of small ones, and the
  menus' 8x8 digits (the deck builder's list and the card bar, at (704, 0)).
  - Each sheet is measured from its own ten digits: the outline (the index
    next to nothing), the fill (the commonest inside), the ramp of indices
    between them, their feet, heads and stroke weight.
  - A digit is then set in the text's font like an HD glyph and coloured
    through the ramp, so the game's palettes still decide the colours (the
    inactive side's dimming too).
  - A colour off the way from outline to fill, like the purple the duel's
    digits have in a few corners, is left out.
- **Labels.**
  - The life-point panel's LP, COM and YOU are set anew in the font, over
    the panel's own texels made larger, in the box's colours. Only where
    the retail panel is (a hash of its words); a mod's own panel is left as
    it is.
  - The card kinds (Magic, Equip, Trap, Ritual), in the hand and on the
    card bar, are set in the plates' serif face (Times) with their outline.
  - The FIELD box's word (with its shadow) and the terrains' names. A name
    the game draws in two sprites (MEAD + OW) is set whole and cut where
    the sprites meet.
  - The card view's ATK and DFD and its digits are set in the plates' serif
    face, in the plates' subtracted inks.
  - The labels' rectangles are those #47's HD pack recipe lists.
- **Texture packs come first.** A sprite a pack paints is drawn from the
  pack, so an HD pack's art for these is never overridden: with a pack that
  covers them the picture is the same, pixel for pixel, with this on or off.

The pictures share HD text's atlas (four rows of cells above the titles).

### Opponent's name for COM

Video > Opponent's name for COM (`opponent_name`, `MEMORIES_OPPONENT_NAME=1`,
off by default) shows the opponent's name in the life-point panel's COM
box, in the OpenGL picture at 2x and up. A pack can't do this, because the
panel is one texture for every opponent.

- The name comes from the opponent id (`gDuel_bOpponentID`, 1-39) through
  `Tables_DuelistShortName`. A name of up to 11 letters is shown whole.
  Longer ones show the part that tells the duelist apart, a High Mage or a
  Guardian with the title shortened: Weevil, Mai, Keith, Soldier,
  H.M. Secmeton, H.M. Anubisius, Mountain, H.M. Atenza, H.M. Martis,
  H.M. Kepura, Labyrinth, G. Sebek, G. Neku, Master K.
- The box is COM's, made from the panel's own texels: its left end, then its
  rows' border and background, as long as the name needs, growing leftwards
  from where it meets the panel. The name is set in the text's font in COM's
  colours, through the panel's palette, so the inactive side's dimming still
  applies.
- It is drawn over the panel whatever drew the panel: the retail panel, HD
  numbers and labels, or a texture pack's image.
- A 2P duel (no opponent id) and a panel other than the retail one keep COM.
- YOU's box shows You, in the name's case, made the same way.
- The result screens name the sides too (You, and the opponent over COM's
  column). They call the dialogue bank's YOU and COM labels by their place,
  which `Text_Retarget` points at the names. Their small font has no full
  stop and odd digits, so there the name is the panel's when it is letters
  only and at most 9 of them, else its longest word of letters (G. Sebek:
  Sebek, Teana 2nd: Teana, Simon Muran: Simon). A copy of the result
  strings steps less before COM, so the name ends where COM did.

Not covered yet: the sword and shield icons (pictures, not lettering).

### Precise geometry (PGXP)

Precise geometry (PGXP) is currently disabled and has no Video menu option.
The `pgxp` setting is clamped to zero, including saved preferences,
`MEMORIES_PGXP` overrides and runtime changes. Its implementation and tests
remain in place for future use. The modes described below are inactive;
when enabled in the code, they affect the OpenGL picture at 2x and up.

- **Affine textures** (`pgxp=1`, *Textures*). The GTE keeps no depth with a
  vertex, so textures on 3D polygons bend. Textured polygons are drawn in
  perspective, at the console's whole-pixel vertices.
- **Rounded vertices** (`pgxp=2`, *Textures and positions*, experimental).
  The GTE's perspective transform rounds each vertex to a whole console
  pixel, which makes 3D polygons wobble as they move. Polygons are also
  drawn at the vertices' precise positions. A model's parts are projected
  each with its own matrix, and where they meet, the vertices lie up to
  about a console pixel apart; the console's rounding closes those seams.
  So a frame word that carries two different precise positions keeps its
  whole-pixel one (`snap_seams` in `libgpu.c`; its depth stays precise).
  Still experimental, so not the default level.

**How it works** (`src/pc/compat/pgxp.c`):

1. `rtp()` in `gte.c` works each vertex's position out from the view
   position before the shift and the division in full, not from the GTE's
   rounded quotient. It keeps it with its depth beside the vertex's SXY
   FIFO entry (`Memories_GtePrecise`); anything else writing the entry
   drops it. A vertex whose precise position does not round near its screen
   word (clamped off the screen, say) has none.
2. Drawing that is ours or the game's C carries it to the packet, keyed by
   the packet word's physical address (`Pgxp_StoreAt`):
   - the HMD polygon drivers (`model_polygon_drivers.c`, the map) tag each
     vertex word they write, and the pre-pass tags its results for them;
   - the game units' `gte_stsxy` stores go through `Memories_GteStore`
     (`Pgxp_Stored`), and their `addPrim` (redefined in `pgxp_game.h`,
     which `build_game32.py` puts before every game unit and no mod
     includes) tags the words of the primitive that hold those vertices
     (`Pgxp_AddPrim`). This is how the duel's models get there
     (`func_80033DB0`, `func_80034830`).
   A vertex stored with no precise value is tagged as such and stays
   rounded.
3. Every other road (`GsSortPoly`, the scratchpad, the interpreter) falls
   back to the word's value, as in DuckStation's vertex cache: `rtp()` also
   records each vertex keyed by its screen word (x | y << 16), and a word
   two vertices of one frame round to with different precise values is not
   matched.
4. `DrawOTag` collects every word of the frame with its address
   (`Memories_GpuCollectAt`) and looks each up by address, then by value.
   Projections up to that point are the frame's, while the actual drawing
   happens later, after the next frame has begun projecting.
5. The matches go with the batch to the OpenGL pass (`SoftGpu_SetPrecise`,
   the recorder's `precise`, arena op `OP_PRECISE`). There `polygon()`
   places each vertex at its precise position. Below level 2, `DrawOTag`
   puts the word's own whole-pixel position there first, so only the depth
   is new.
6. A textured triangle whose three vertices all have their depths
   interpolates uv / w and 1 / w and divides back per pixel (flag 32). Every
   other triangle takes the path it took before, so with PGXP off the
   picture is identical.

**Effects:**

- The software GPU, VRAM and the game see nothing of it. The smoke cases
  give their hashes with `pgxp=1`, and nothing changes at 1x.
- In the 3D Monsters duel about 300 of a frame's 1,400 words are precise
  vertices, which is most of the 3D, nearly all of them by address. The
  rest is 2D drawn without the GTE. `MEMORIES_TRACE=frames` logs the counts
  (by address, by value). The main menu and Options come out identical
  with PGXP on: their 2D is not projected.
- One limit: vertices `GsSortPoly` moves by its offsets no longer match
  their word and stay rounded.

### Deterministic PC checks

`make check-pc` rebuilds the native game and portable C tests, runs every
`pc_*` CTest, then boots the game headless once per case in
`tests/pc/smoke/`. It compares PPM hashes for the title at frame 900, the
main menu after one cursor move (4:3 and widescreen), Options, and the first
campaign duel with both code mods on (a 3D Monsters model standing on a
face-up card at frame 6760; the field turned by the hand camera's L1 at 6560).
Failures retain the differing image beneath `tmp/pc/smoke/`. After an
intentional rendering change, inspect those images and update the fixtures
with `python3 tools/pc/smoke.py --record`; immediately run the normal command
twice before committing new hashes.

The smoke runner clears other `MEMORIES_*` switches (except a caller-supplied
`MEMORIES_DISC`) and gives each case its own settings file and an emptied user
folder (`tmp/pc/smoke/<case>.user`, so the player's memory cards take no part),
no gamepad and no audio device. A case's `settings` fill that file
(`"aspect": 2`, `"mod.3d-monsters": 1`). It still requires the private disc
image and the native build prerequisites described above. Each boot gets
120 s; `MEMORIES_SMOKE_TIMEOUT=<seconds>` raises it where the headless game
runs slower.

A run that is headless, uncapped (`MEMORIES_SPEED=-1`) and dumps a frame is
deterministic: the same input gives the same frame on Linux, on the Windows
build (under Wine), and with the machine under load. Its game time moves only
where the game waits: `Platform_WaitVBlank` steps it 1 ms at a time to the next
VBlank, and a VSync that only reads the count steps it 1 ms (movie playback
polls that way). The host timer steps it only after a second without a wait,
so a spinning loop cannot hang. Driven by the host timer, as it used to be, a
frame that took longer to compute got more ticks, so more disc sectors, and the
Linux and Windows builds dealt different hands in the first duel. It is also
fast: frame 1100 in about 3 s on Linux.

### AI thinking time

The opponent's interpreter (`AiScript_Run`) yields to the next frame once
`VSync(1)` reports 240 lines. On the console a heavy decision therefore spans
several frames. Natively it rarely spans more than one, and in a deterministic
run the 1 ms step per `VSync(1)` makes the interpreter yield after about 17
commands. The decision would change only if other code drew from `rand()` in
those frames, between two of the AI's own draws.

`MEMORIES_AI_TRACE=<file>` logs every frame, every `VSync(1)` made by
`AiScript_Run` (the command's script offset and the opponent id) and the
caller of every `rand()`. `MEMORIES_AI_YIELD=N` makes every Nth of those
queries report a full frame, so the interpreter yields there as a slow machine
would. `python3 tools/pc/ai_trace_check.py <traces> --listing <dir>` groups
the trace into decisions and lists the `rand()` callers. It also lists the
ones that drew while a decision was in progress. With `--listing` (the output
of upstream's
`ai_script_disasm.py`, from
[krystalgamer/memories-decomp#6006](https://github.com/krystalgamer/memories-decomp/pull/6006)),
it checks every logged offset against the disassembly.

Measured on 2026-09-24: 16 deterministic sessions of 60,000 frames of the
`duel-hand-camera` case, driven by random button presses. They covered 943
decisions against Simon Muran and Teana and 156,234 commands. Four sessions
left the interpreter alone. Nine forced a yield after every command (up to
281 frames per decision), three of them with the code mods off. Three forced
one after every eighth command. Results:

- No code but the AI's own `AiScript_JumpRandom` drew from `rand()` while a
  decision was in progress. The AI's rolls are the same consecutive draws
  whatever the frame count, so a faster machine does not change them.
- The other duel-time callers (`DuelScene_UpdateBattle`'s shake, the result
  screen, the shuffle) run between decisions, for a fixed number of frames.
- Every logged offset is an instruction start of the disassembly: 489 of the
  hand script's 1314 and 296 of the field script's 795 were reached.

### Save states

F1, F2 and F4 pick those slots (shown in the File menu), while slot 3 is
available from File; **F5 saves and F7 loads**. Slots
are `states/slot<N>.state` in the user directory (`MEMORIES_STATE_DIR` moves them), about
4 MiB each. `./build-pc.sh load [slot]` or `MEMORIES_LOAD_STATE=<slot or
path>` starts from a state: the process boots for 30 frames so every
subsystem is initialized, then resumes the state (under a second).
`MEMORIES_SAVE_STATE="<frame>:<path>"` saves from a script, for headless work.
On this machine `slot1.state` is the "Run away / Keep listening" choice at the
end of the opening scene and `slot2.state` is the town map with the cursor on
the palace. Both were regenerated after the game-source guards below, which
moved code; older copies no longer resume.

**States survive a rebuild of the native side**, which is the point: reach a
stub, implement it, rebuild, load. Checked by shifting every native address
with a rebuild and loading an older state: the frame 420 frames later was
bit-identical. How (details in `src/pc/guest/state.h`):

- A state is only taken or resumed at a `VSync(0)` called from game code.
  `VSync` is a small assembly entry (`guest/state_i386.S`) that records the
  caller's callee-saved registers and return-address slot; resuming is
  returning from that call.
- The build collects every game object's code and variables into
  `game_text/rodata/data/bss` (and the `ovl_<module>_*` sections) and links
  them at fixed addresses (`FIXED_SECTIONS` in `tools/pc/build_game32.py`);
  the game runs on a stack mapped at `0x70000000`. Return addresses and
  pointers inside a state therefore mean the same in the next build.
- Stored: guest RAM, scratchpad, those sections, the game stack above the
  call, and one self-described chunk per native subsystem (`*_State`
  functions: soft GPU, SPU, LIBSPU, LIBDS including buffered movie frames,
  LIBETC, LIBGPU, LIBGTE, MDEC, VBlank count). A chunk whose layout changed is
  reported and skipped, leaving that subsystem as it is. Nothing native is
  stored by address; timers, the disc file, the window and the audio device
  belong to the process.
- Game data words the linker relocated (pointers to native functions) are
  taken from the running build when the game never changed them; the file
  keeps the startup image of that data to tell.
- **States are carried between builds by relocation.** A change to game
  sources moves game code, and *any* rebuild can move native routines the
  game holds pointers to (the town map keeps the addresses of the HMD drivers
  `GsU_*` in its model data; a state taken there broke as soon as `libgs.c`
  grew). Every build files all of its functions and the game objects'
  variables in `tmp/pc/game32/symbols/<build id>.txt`, the id being the
  table's hash (also written to `tmp/pc/game32/buildid`, which the runtime
  reads); the state header carries the id (format version 2; version 1
  carried the game-source fingerprint and its tables list game code only).
  A state from another build a state from another build is
  rewritten by name when loaded: function starts wherever callbacks live
  (guest RAM, game variables, the callback part of the LIBDS/LIBETC/MDEC
  chunks) and any address inside a function on the stack. The load is refused,
  with the reason, if a function that was running has changed size, if a game
  variable moved, or if either symbol table is missing (keep the `symbols/`
  directory; a table for a lost build can be regenerated by building those
  sources with `--build <other dir>`). First use: the three states saved
  before the pointer-sign fix below load in the fixed build (about 425
  addresses moved each). A new native static that the game depends on still
  needs a field in its subsystem's `*_State`.

### Saves

The port does not use memory cards for saving. Every load and save the game
asks for goes through `MemCardDialog_Request` / `MemCardDialog_Poll`
(`src/game/mem_card_dialog_runtime.c`); under `MEMORIES_PC` those hand the
request to the save slot menu (`src/pc/saves/save_menu.c`) instead of the
card dialog. The menu is drawn in the overlay (`debug/hud.c` calls
`SaveMenu_Draw`), is driven by the game pad (Cross picks, Circle backs out,
Up/Down move, Left/Right in the overwrite prompt), and returns the outcome
the card dialog would have (1 done, 2 failed, 3 cancelled), so every caller
is unchanged. What it covers:

| Dialog step | Caller | Slot menu |
|---|---|---|
| `0` load | title LOAD | pick a slot with a save; empty and damaged slots cannot be picked |
| `1` load, unprompted | 2P DUEL and TRADE, once per player | "Player 1/2: choose a save"; player 2 cannot pick player 1's slot |
| `2` save | SAVE in the main menu, the campaign's save prompt, the completion save after the credits | pick any slot; an occupied one asks "Overwrite it?", defaulting to Overwrite only when it holds the game being saved as it was last loaded or saved (same duelist, the previous sequence number) and to Cancel otherwise, with a line saying it is another duelist's, an earlier save of this game or further along |
| `4` trade write-back | TRADE | no menu: writes both traded saves back to the slots they were loaded from, after checking the duelist code as the card dialog did |

The side effects the card dialog kept on success are kept too:
`gSaveDataSequence` goes up after a save and `D_8009B3D4` is cleared after a
save or a prompted load.

A slot file (`src/pc/saves/save_slots.c`) is exactly the 8 KiB block the
game writes to a card: the 0x200-byte "SC" header with icon and title, the
0x680-byte state and its duplicate, zeros after. A slot can therefore be put
back onto a card image with any memory card manager. In the padding after
the duplicate (0xF00) the port keeps a token, `YFMSLOT\1` and a number drawn
afresh at each save; what the save holds of the cards mods add
(`cards/<duelist>.txt`, notes/more-cards.md) is filed under that token, so
two slots of one duelist never share it. A slot is written to
`slotNN.sav.partial`, flushed to the disk and renamed over the old file, so
a failed write or a power cut keeps the previous save (on Windows the
rename writes through, and is retried for a second while another program,
a cloud sync say, holds the file). The list is read from the files again
before a pick, so a list brought back by a save state cannot save over a
slot without asking. When the first copy fails `SaveData_ValidateIntegrity`
the duplicate is used; when both fail the slot shows as damaged. The menu
lists the player name, starchips, cards owned (chest plus deck), wins and
losses, and the file's modification time.

Trade write-back checks both destination duelists before writing either
slot. It updates both copies in each slot together, preserving the valid
copy's progress outside the traded card data. Each slot replacement is
atomic, and when the second write fails the first slot is put back as it
was, so the trade happens to both saves or to neither.

The first time the saves are used, the save on each old memory card image
(`memcard1.mcd`, `memcard2.mcd`) is copied into slot 1 and 2, never over a
slot that is already there. `saves/.cards-imported` marks that done; a card
that could not be read is tried again at the next load or save.

The first time the `saves` folder is created, the game's save
(`BASLUS-01411-YUGIOH`) on `memcard1.mcd` and `memcard2.mcd` is copied into
slots 1 and 2. The card images are only read. `pc_save_slots` tests the slot
files and the import. `pc_save_menu` covers save/load/cancel, overwrite
defaults, pair selection, trade validation and backup consistency. The
menu's own state is a save-state chunk (`save-menu`), so a state taken with
the menu open resumes in it; each poll supplies the current build's
integrity callback, including after loading a state in a fresh process.

Checked on Linux and under Wine: LOAD on the title imports the card's save
and loads it, SAVE writes an empty slot at once and asks before overwriting
slot 1, and an overwrite replaces the file on the Windows build.

### Memory cards

Nothing on the normal paths reaches LIBMCRD any more (see "Saves" above).
`sdk/libmcrd.c` implements LIBMCRD over standard 128 KiB raw card images
(`.mcd`/`.mcr`, the format emulators use, so saves can be exchanged with them).
Slot 1 is `memcard1.mcd` in the user directory, created formatted when
missing; slot 2 is `memcard2.mcd` beside it and exists only if the file does; `MEMORIES_MEMCARD1/2`
name other files. The image is re-read on every command and written through a
temporary file. Commands complete after the time the hardware would take
(about one 128-byte frame per VBlank), so the game's "accessing memory card"
messages stay up. The first `MemCardAccept` of a card reports `McErrNewCard`,
as after an insertion. Checked: SAVE at the card shop writes
`BASLUS-01411-YUGIOH` (one block, valid "SC" header, icon and title,
directory checksums), a second SAVE asks to overwrite, and LOAD from the title
menu returns to the shop. The low-level `_card_*`/`InitCARD` path in
`mem_card_driver.c` is still stubbed; nothing reached so far uses it.

Native pieces (all under `src/pc/`):

| Area | File | Notes |
|---|---|---|
| GPU | `render/soft_gpu.c` | Software rasterizer: flat/Gouraud/textured polygons, sprites, lines, fills, VRAM transfers, 4/8/15-bit textures, texture window, four blend modes, mask bits, dithering. `pc_soft_gpu` checks the fill rule, CLUT path, clipping and wraparound. Replaces PSY-Z for the 32-bit build (no 32-bit SDL installed here) |
| Mods | `src/pc/mods/mods.c`, `mods/3d-monsters/field_models.c` | The mod system (`notes/modding.md`) and **3D Monsters**, described above: the duel field's face-up monsters as animated models, on their own arenas and software-GPU texture banks. Off by default; nothing in it runs while it is off |
| Menu bar | `platform/menu_x11.c` | **File > Exit**, **Audio > Volume** and **Mods**, one checked item per extra (a 0-100 slider: drag it, click the track, or use the wheel over it). Drawn with plain Xlib, since the port has no toolkit; the window is `Menu_Height()` (22 px) taller than the picture and the picture sits below it. Labels use an X core font, falling back to a small built-in glyph table because a server started under Wayland often has no core fonts. The volume is kept in `settings.txt` in the user directory (see `MEMORIES_SETTINGS`) and applied through `Spu_SetOutputVolume`, which is the port's own control and deliberately outside save states. While a menu is open it owns every mouse event, including the wheel: otherwise the wheel stepped the game's cursor behind the menu and played its sound |
| Window/input | `platform/x11.c` | Plain Xlib. The 59.94 Hz VBlank is a `SIGALRM` tick on the main thread, standing in for the interrupt, so the game's busy-waits on VBlank counters work unchanged. Game units are built `-O0` so those non-volatile polls are not hoisted. The frame and the menu bar are composed in an offscreen pixmap and reach the window in one `XCopyArea`: an open menu hangs over the picture, so drawing both straight to the window made the menu flash once a frame |
| LIBETC/pads | `sdk/libetc.c` | Callbacks, `VSync` (presents, then waits), critical sections that defer the tick, BIOS pad buffers |
| LIBGPU | `sdk/libgpu.c` | Environments, `DrawOTag` through `Memories_GpuCollect`, image transfers. `DrawOTag` snapshots the list and it is rasterized at `DrawSync` or before the next VRAM access, where the hardware would have finished it. Drawing inside `DrawOTag` put ~8 ms between `VSync` and `Input_UpdatePads`; whenever a second VBlank got in there the pad code published each press twice (two cursor steps, two sounds) |
| LIBGS | `sdk/libgs.c` | Ported from the resident assembly against the library's guest globals (`GsDRAWENV` `0x800FE048`, `GsDISPENV` `0x800FE0A8`, ...): graph init, display-buffer swap, OT clear/sort, `GsSortSprite`/`FastSprite`/`FlipSprite`/`Poly`/`BoxFill` |
| LIBDS/LIBCD | `sdk/libds.c` | ISO9660 lookup and sector delivery from the disc image on the VBlank tick (8 sectors per tick; faster than hardware). Resolved LBAs equal `disc_layout.json`. XA "play" only advances the head |
| SPU | `audio/spu.c`, `sdk/libspu.c`, `platform/audio_alsa.c` | 24 ADPCM voices, hardware ADSR, pitch, volumes, Gaussian interpolation, CD/XA input, mixed on an ALSA thread at 44.1 kHz. The menu's output volume is a separate gain the mixer walks to its target over about 36 ms, because stepping it mid-waveform is an audible click and dragging the slider made a burst of them. No reverb, noise, sweeps or pitch modulation. Key on/off cross threads as atomic bit sets (a key-off after a still-pending key-on is applied after it); no locks where a signal handler runs. `SpuSetVoiceAttr` follows the decompiled library (`tmp/port-research/psyz/decomp/src/libspu/sr_sv.c`): pitch, then sample note, then note, so a note overrides a pitch in the same call, with the library's integer note-to-pitch; ADSR modes are written only with their rates. The sound-effect voice sends mask `0xFFFF` with pitch `0x1000` and note `0x2400` against sample note `0x3C00`; applying the pitch last played every effect two octaves high |
| Modules | `guest/modules.c`, `MODULES` in `tools/pc/build_game32.py` | Modules built for the shared `0x80168000` bank are all linked in. Clashing symbols become `<module>__<name>`, their variables move to `ovl_<module>_data/bss` and are restored from a startup copy when the module's first sector lands on the bank (`CdGetSector` reports loader writes), and guest-address calls are resolved against the identifier word at the start of the loaded image. Data the module's C leaves undefined is pinned to the guest addresses the loader fills. Linked so far: `password` (`0x15`, also name entry) |
| Interrupts | `sdk/libetc.c`, `platform/x11.c` | A 1 kHz `SIGALRM` drives VBlank (59.94 Hz), root counter 2 (the sequencer: `SetRCnt` without the system-clock flag selects sysclk/8, so `0xE000` is 73.8 Hz), disc delivery and MDEC completion. Critical sections defer them |
| XA/STR/MDEC | `sdk/libds.c`, `sdk/libpress.c` | XA ADPCM sectors decode into the SPU's CD input; streaming reads run at the drive's real rate (75/150 sectors per second). `St*` assembles STR frames in host slots; `DecDCTvlc2` emits genuine MDEC run-level words (bitstream v2), and the software MDEC fills each `DecDCTout` strip and then runs the game's callback. Floating-point IDCT; 15- and 24-bit output; 24-bit display mode is presented |
| LIBGTE | `sdk/libgte.c` | Register setters, and ports of the resident routines: `rsin`/`rcos` and the matrix builders read the library's own tables from the resident image (`0x80094938` quarter sine, `0x80095638` sin/cos pairs, `0x800951A8` square roots); `RotMatrix`, `RotMatrix_gte`, `RotMatrixZYX_gte`, `RotMatrixYXZ_gte` (each keeps the retail rounding: some negate before the shift, the GTE ones after), `MulMatrix`/`MulMatrix2`/`ApplyMatrixLV` issued as the same MVMVA commands, `TransposeMatrix`, `SquareRoot0`, `ratan2` (table at `0x80099638`), `RotAverage3/4`, `RotAverageNclip3/4` and `_nom`, `AverageZ3`, `NormalClip`, `RotTrans`, `RotTransSV`, `RotTransPersN`, `RotColorDpq`, `NormalColorCol`, and `DivideFT4` with its recursive subdivider and packet emitter (`RCpolyFT4A`, `func_80089910`) over the caller's `DIVPOLYGON4` work area: the card viewer draws the large card with it. The earlier `RotMatrix` was the Z-Y-X formula under the wrong name |
| LIBGS units | `sdk/libgs_unit.c` | The HMD path the town map uses: `GsMapUnit`, `GsMapCoordUnit`, `GsScanUnit`, `GsSortUnit` (primitive drivers are function pointers the game installs), the library's null and image-upload drivers, `GsGetLwUnit`/`GsGetLsUnit`/`GsGetLwsUnit` with their per-frame coordinate cache, `GsMulCoord2/3`, `GsSetRefView2`, `GsSetLightMatrix`, `GsSetFlatLight`, `GsLinkAnim`, `GsScanAnim`; `GsSortLine`/`GsSortGLine` are in `libgs.c`. `tests`: a scratch harness checked that the reference point lands on the view axis at the right distance and `ApplyMatrixLV` against 64-bit math; not yet a CTest |
| Model drivers | `overrides/model_polygon_drivers.c` | The 61 hand-written GTE routines at `0x800612C0`-`0x8006ADE8` are HMD primitive drivers, and one algorithm under switches: triangle/quad x flat/Gouraud, back-face culled (`0x0020xxxx`) or both-sided (`0x0030xxxx`), plain or tiled (`0x02xx`: each polygon wrapped in its texture-window word and a reset), a second bank that forces semi-transparency, a shared-vertex bank (`0x012x/0x013x`) fed by a pre-calculation pass at `0x80067220`, and twelve outline drivers. Record layouts, the lighting modes of `D_8009AFE4`, the colour cache and the translucent second pass are described at the top of the file. Read in full for each shape and by diff for each variant; the outline drivers and most variants have not been seen running yet |
| Null page | `guest/image.c` | Retail code dereferences null pointers that land in kernel RAM on the console (`CardList_CreateSlotTextBox` clears a flag through `box->field_28` one call before that object is created; it made BUILD DECK fault). A faulting access below 64 KiB is redirected: the handler decodes the instruction's base register, points it at a mapping of guest RAM's first 64 KiB, single-steps (trap flag) and restores the register. Each site is reported once on stderr, which makes these bugs visible instead of fatal |
| Guest calls | `guest/image.c` | Guest RAM is non-executable. A call through a MIPS address stored in the data image faults; the handler redirects to the native function via the generated `Memories_FunctionMap` |
| Overrides | `overlays/boot_check.c`, `sdk/deferred.c` | The boot package's console-modification check has no source and is passed. `GsSetFlatLight` and the debug font log once and do nothing |

A native definition with a game function's name replaces it (the driver
weakens the game's symbol). Sources that still use address-based names
(`func_800434F4`) are aliased to the renamed C definition, as the PS1 link
does. `src/overlays/main_menu` is linked in because its load address
(`0x80180000`) is private.

Software GTE fix found through the map camera: MVMVA and the light-colour
stage take IR1-IR3 as input and write them row by row; the inputs are now
latched first (`pc_gte` has the regression case). Before it, every
`ApplyMatrixLV` translation and every lit colour was wrong in rows 2 and 3.

Pointer-sign tests: retail tells a callback from a small number by the sign,
every console address being negative. `func_80041F90`
(`display_object_projection_checks.c`) did `if (cb < 0) cb(obj, otz)` on
`DisplayObject.field_10`; host code addresses are positive, so the callback
that swaps a card to its back texture never ran and the card viewer showed the
blank front frame through the first half of its turn. Under `MEMORIES_PC` the
test is `(u32)cb >= 0x10000`. A scan for sign and top-bit tests next to
indirect calls found only this one; tests on data pointers may still exist.

Calls that rely on MIPS argument registers passing through: the matching
sources write three calls without arguments because retail leaves the
caller's `$a0`-`$a3` in place (`func_80056250` -> `func_8004CB0C`,
`func_80023D08` -> `func_8002348C`, `FreeDuel_UpdateSparkle` ->
`DisplayObject_ReleaseIfPresent`; the last confirmed from the matched
disassembly). Under `MEMORIES_PC` they pass the arguments; `make match` and
`make match-overlays` still reproduce the retail hashes. A scan for calls
with empty parentheses to functions defined with parameters found only these.

The overworld module references six resident addresses that fall inside
functions (`func_800158C8`, ...): its `alternate_*` code was linked against
another build of the executable and is dead in retail. They stay stubs.

Symbols identified while porting are recorded with their evidence in
`notes/pc-port-symbol-evidence.csv`, in the schema of
`notes/semantic-symbol-map.csv`. It is a staging file: the main map is
enforced by `tools/project/apply_semantic_names.py --check` and applying it
renames sources, so rows move there (and get applied, followed by `make
match`) as a separate step. Add a row whenever a port establishes what an
address is; say what was read and what was observed working, not just the
name.

Shared-source guards added for the host compiler (GCC 16):
`src/game/graphics_frame.h` / `graphics_frame_buffer.h` / `main_init.c` (an
array of incomplete type is declared after the layout instead),
`src/psyq/stdarg.h` (compiler builtins), `src/psyq/inline_c.h` (native GTE
macros). The build also defines `_LANGUAGE_C`/`LANGUAGE_C`, which the MIPS
front end predefined, and uses `-fpermissive` for GCC 2.8.1-era pointer
conversions.

## Sharing a build

```sh
python3 tools/pc/package.py        # dist/yfm-redecomp-<date>-<commit>-{windows.zip,linux.tar.gz}
```

Builds both, smoke tests both, and packs each as a folder a player unpacks
and runs: the executable, the shipped mods, the mod SDK, this build's symbol
table, an empty `game/` for their own `.bin`, and `tools/pc/release/README.txt`.
Nothing from the disc goes in. A missing disc image is reported in a message
box naming the folder to put it in. Crash and hang reports, minidumps and
menu frame dumps go to `reports/` in the user directory when the game is not
run from a checkout (`Crash_ReportDir`; `tmp/pc` in one).

### Crash reports

The executable runs the game as its own child and stays as its monitor
(`src/pc/debug/monitor.c`), so whatever ends or freezes the game, a report
is written: `crash-<pid>.txt` or `hang-<pid>.txt` in `Crash_ReportDir`,
with a message box naming it (not when headless or scripted;
`MEMORIES_CRASH_DIALOG=0/1` decides). The two share a block of memory the
game writes and the monitor reads, so what the game knew survives however
it ended: facts (build and commit, OS or Wine version, CPU, memory, GPU and
driver, SDL video and audio drivers, every setting, the applied mods), the
runtime module last loaded, the frame and VBlank counts, and the last 128
lines of the log. The mods, state, memory card and duel model channels are
kept there even when not traced (`Log_Wanted`). The game's console output
passes through the monitor into `last-session.log` (the run before in
`previous-session.log`) and its last 200 lines into the report. The home
folder is written `~` in the monitor's part of a report.

- A crash the game's handlers see (`crash.c`) is reported by them first,
  in the same file; the monitor adds its section after it. What they cannot
  see is the monitor's alone: a fail-fast, the out-of-memory killer, a
  stack Windows could not deliver an exception on, an unexplained exit.
  On Linux it adds `coredumpctl info` (a stack per thread) when
  systemd-coredump kept the core.
- A freeze: no VSync for `MEMORIES_WATCHDOG` + 1 seconds (5 + 1 by
  default; 30 before the first frame; never while paused, in a message
  box, or quitting). The monitor lists every thread with its registers and
  frames, taken from outside (ptrace and `/proc` on Linux, where the
  game's own watchdog in the tick handler cannot see a freeze of the tick
  itself; `GetThreadContext` on Windows) and writes a minidump on
  Windows. If frames come again the report says so; if the player closes
  the game, that too.
- Errors the game cannot go on from (an unimplemented routine, a bad
  ordering table, a MIPS overlay failure, a lost disc image) go through
  `Crash_ReportFatal` to `crash-<pid>.txt` before the exit. Windows gets
  `abort()` and the C runtime's invalid-parameter handler (logged and
  ignored, as MinGW's own did) too, and the exception code for the exit
  status of a crash.
- `MEMORIES_NO_MONITOR=1`, or a debugger, runs the game alone. On Windows a
  restart (mods window) is the monitor starting the game again; on Linux
  `execv` keeps the same process.

`MEMORIES_CRASH_TEST=<kind>@<frame>` fails on purpose
(`src/pc/debug/crash_test.c`: segv, thread, overflow, abort, fatal, kill,
hang, spin, deadlock, tickhang, slow, restart, null), and
`tools/pc/crash_check.py [--windows]` runs every kind headless and checks
its report; all 13 pass on Linux and under Wine. Not yet seen on real
Windows: the message box, the minidump's size (Wine's is small), and the
job that ends the game with the monitor.

Every Linux build, the everyday one included, is the one that ships. A
program built against this machine's libraries would ask for its glibc (2.43
on Arch), so `build_game32.py` builds against Debian 11's i386 libraries, which
`tools/pc/build_linux_sysroot.py` fetches into `tmp/pc/linux-sysroot` (no root,
no container; each package is checked against the archive's SHA-256), with
SDL3 built against them into `tmp/pc/sdl-m32-portable`. Debian's 32-bit
start-up objects and libgcc are linked by name (`startfiles()`/`endfiles()`,
`-nostartfiles`), so the host's multilib files are never used or needed. The
executable asks for glibc 2.29 at most, links FreeType, fontconfig and libpng in, and needs only libc and the
32-bit GL driver from the system (and a 32-bit PulseAudio or ALSA library for
sound; SDL loads those at run time). Its SDL has X11 but not Wayland (Debian
11's is too old); it runs through XWayland, which is also the path that reaches
the real GPU (llvmpipe otherwise). The smoke frames match the old host build.

Only this build's symbol table ships, so a save state from an earlier release
will not carry over to a later one unless that release's table is added to
`symbols/` as well.

## Windows

The same driver builds `tmp/pc/game32/memories-pc.exe` on Windows 10/11 with
the SDL backend. Status (2026-09-22, Windows 11, i686 on x64): headless and
in the SDL/OpenGL window at 59.94 fps, with the menu bar, through the Konami
logo, movie, title, main menu, name entry and the opening story into the
deck (CHEST) screen. Audio output and the rest of the game are not checked
yet on Windows.

`play.bat` is the way in ("Play from a checkout" above). By hand, from any
shell with Python 3:

```sh
python tools/pc/build_game32.py       # fetches llvm-mingw, CMake, Ninja and the libraries the first time
tmp/pc/game32/memories-pc.exe
```

### From Linux

`./build-pc.sh` builds the Windows executable as well as the Linux one, so
every change is compiled for both (`MEMORIES_SKIP_WINDOWS=1` skips it). The
first run of `tools/pc/build_win32_deps.py` fetches the pinned llvm-mingw
release for Linux into `tmp/pc/llvm-mingw` (the same toolchain as on
Windows) and builds the libraries with it; `build_game32.py --target windows`
then writes `tmp/pc/win32/memories-pc.exe`, its mods and `SDL3.dll`,
beside the Linux build rather than over it. Needs cmake, ninja and Wine to
run it:

```sh
./build-pc.sh run-windows               # play it under Wine (prefix tmp/pc/wine-prefix)
python3 tools/pc/smoke.py --windows     # the smoke frames, under Wine
```

The smoke frames under Wine match the Linux hashes. Wine is not Windows: it
presents through SDL's Direct3D renderer (its 32-bit OpenGL is not there),
and its clock reads leave the executable, which is how the tick came to be
serviced from `Platform_VSyncHeartbeat` (a `VSync(-1)` poll, as the movie's
wait for sectors, otherwise took ticks only by chance: 3.5 frames/s). Check
anything timing- or driver-sensitive on Windows itself. `SDL3.dll` is copied
beside the executable. Everything Windows-specific is behind `_WIN32`; the
Linux build is unchanged.

For play-testing, `tools/pc/run_debug_windows.bat` runs the game with
problem reporting on. It keeps a rolling state every 30 s
(`MEMORIES_AUTOSAVE=<seconds>`, slots `auto1`..`auto3` in
`tmp/pc/debug/states` through `MEMORIES_AUTOSAVE_DIR`; F5 states stay in the
user directory), traces in `tmp/pc/debug/trace.log` and the console in
`tmp/pc/debug/console.txt`. `tmp/pc/hang-*.txt` / `crash-*.txt` hold named
backtraces (see "Crash reports"): PE symbols have no sizes, so the build
sizes each function up to the next symbol, and addresses in a DLL are named
by module. The game's own hang watchdog runs on the clock thread
(`Win32_SetStallReporter`), so it also reports a main thread stuck in a
driver or a lock, which the tick cannot reach; a pause counts as alive.

What differs from Linux, and why:

- **Clock.** No signals: `platform/win32.c` runs a 1 kHz timer thread that
  suspends the main thread and, if it is executing code of the executable
  and does not hold SIGALRM, redirects it through an assembly trampoline
  that saves every register and the FPU/SSE state, runs the tick and returns
  to the interrupted instruction. Code outside the executable (C runtime,
  SDL, drivers) is never interrupted; a tick missed there is taken by
  `Win32_ServiceInterrupt` from `Platform_WaitVBlank`. `pc/compat/signal.h`
  maps `sigprocmask`/`pthread_sigmask` on SIGALRM to that hold flag.
- **Faults.** A vectored exception handler in `image.c` does what the
  SIGSEGV/SIGTRAP handlers do (guest-call redirect, low-address fixup);
  32-bit processes on 64-bit Windows can report the single step as
  `STATUS_WX86_SINGLE_STEP`. Fatal exceptions raised in the executable are
  reported by `crash.c` through `Win32_SetCrashReporter`.
- **Physical RAM mirror.** Windows already occupies `0x10000..0x200000` when
  the program starts (process parameters, locale tables, the WoW64 stack),
  so the mirror cannot be mapped; `Memories_Resolve` returns the
  `0x80000000` alias for physical RAM addresses, and the fault handler sends
  any other access there through guest RAM (each site reported once).
- **Stacks.** The game stack is at `0xB0000000` (32-bit Windows loads system
  DLLs around `0x70000000`; the mods keep `0x90000000`). `state.c` switches stacks with
  `Memories_ContextSwitch` (`state_i386.S`) and moves the TEB's stack bounds,
  `DeallocationStack` and exception chain with it, as fibers do. A guard
  page 64 KiB above the game stack's bottom, below `DeallocationStack` so
  that Windows and Wine take it for a plain guard page and not stack growth,
  turns an overflow into a report; running off the end left no stack to
  deliver the exception on, and the process just ended. A context is the
  stack pointer of a suspended switch, with the registers kept on that
  stack; unlike a ucontext, it is gone once that stack is used again. So a
  state load enters the game through a switch too (`apply`, `resume_game`),
  which takes the service context again each time: resuming the startup
  one after `apply` had run over it crashed every second load of a session
  (EBP 0 at `Memories_StateRunGame`).
- **Icon.** The window and the taskbar show the game's memory card icon,
  the one its saves carry (Yugi, frame 0 of three), decoded from the save
  header template `gSaveData_aHeaderTemplate` of the game being played
  (`src/pc/platform/save_icon.c`) and pixel-doubled to 32, 48 and 64. On
  Windows the executable carries it too: `build_game32.py` (`exe_icon`)
  makes `icon.ico` from `game/SLUS_014.11` and links it in as a resource
  through windres. Nothing of the game's is in the repository; without the
  game files or windres, the executable has no icon.
- **Link (lld, PE).** C symbols carry a leading underscore; `asm("name")`
  labels are renamed to match. No GNU linker script: pins are absolute
  symbols from `guest_symbols.s`, and the game units' COMMON symbols for
  pinned names are turned into references, since lld would prefer the
  COMMON. Section renames edit the COFF headers (`rename_coff_sections`) and
  `__start_`/`__stop_` come from `$a`/`$z` marker sections. Overrides win by
  link order (`--allow-multiple-definition`, native objects first); any
  other duplicate definition is still an error. PE cannot place sections at
  chosen addresses, so the fixed game sections are not there: save states
  work within one build but are not carried across rebuilds.
- **Bitfield layout.** MinGW compilers lay bitfields out as MSVC does by
  default, where fields of different declared types do not share a unit:
  `GsOT_TAG` (`unsigned p:24; unsigned char num:8`) became 8 bytes and every
  LIBGS ordering table was walked with the wrong stride, so semi-transparent
  and shaded primitives were overwritten or lost (the title's lower
  gradient, the yellow selection bar). The build passes `-mno-ms-bitfields`
  to game and native units, and `libgte_extra.c` asserts the size.
- **Clock races.** Besides the fault race above, a redirect made while an
  exception is being delivered can be dropped by Windows; the tick then never
  runs. It is released wherever it shows: by the exception handler, which
  sees the redirect still marked while the registers it got are not at the
  tick's entry; by the clock thread, when the main thread's stack pointer is
  back above the slot it pushed; and by `Win32_ServiceInterrupt`, since a
  wait on the main thread is not the tick. Before this, the game froze in
  `Platform_WaitVBlank` after a few minutes of duelling. The game stack runs
  with an empty SEH chain, so the crash reporter also takes any fatal
  exception raised while the chain is empty, in a DLL too; a stack overflow
  is reported from a thread of its own, the faulting one having too little
  stack left. Crash and hang reports come with a minidump (`tmp/pc/*.dmp`,
  `lldb -c` reads it). Other threads' unhandled exceptions go through
  `SetUnhandledExceptionFilter`, and every exception nothing claimed is
  logged once per address.
- **Low accesses without a trap.** A plain 32-bit `mov` to or from a low
  address is carried out by the fault handler through guest RAM
  (`emulate_low_mov`) instead of rebase and single step: WoW64 mishandled
  the trap for `movl 0x4c(%eax),%eax` (destination = base register) in
  `func_800540B4` under the 3D Monsters mod, and the process died in the
  exception dispatcher.
- **Mods.** A code mod is the same ELF object on Windows as on Linux, loaded
  by the game's own loader (`notes/modding.md`), so there is no DLL and no
  export table. `mods.c` reads a replacement file into memory instead of
  mapping it. Both mods load under Wine; they have not been run in a duel
  on Windows yet.
- **Tests.** On MinGW every CMake test links `-static`: otherwise a 32-bit
  test loads whichever `libwinpthread-1.dll` PATH finds first, often a
  64-bit one, and fails to start with 0xc000007b. Run them with a native
  Windows `ctest`; an MSYS one mangles the test paths.
- **rename.** Windows' `rename` does not replace an existing file; states,
  settings, controls and memory cards save through `MoveFileEx`
  (`pc/compat/posix.h`).
- **Narrow returns.** clang leaves the upper bits of a `char`/`short` return
  undefined, which GCC happens to fill. Two matching definitions are read
  wider by callers (`Ai_GetHandSize`, `MemCard_FindLoadedEntry`; an IR
  comparison of declared and defined return types over all game units found
  only these), and `src/pc/overrides/narrow_returns.c` returns full words for
  both. Before it, the opponent's turn in a duel ran off guest RAM.
- **Libraries.** fontconfig is replaced by fonts from `%WINDIR%\Fonts`
  (`Win32_FontPath`), iconv by code page 932, and the few POSIX calls by
  `pc/compat/posix.h` and `pc/compat/mman.h`.

## Launch the local graphics preview

```sh
./launch-pc-preview.sh
```

The installed Linux executable is `tmp/pc-preview/memories-pc-preview` (about
4.7 MiB stripped). Both entrypoints open a persistent preview window with the
texture test and a port-in-progress label. Close the window to exit. The game
does not boot from this executable. No game assets are required by the preview.

Rebuild/install it after configuring the optional PSY-Z build below:

```sh
cmake --build tmp/pc-psyz --target memories-pc-preview
cmake --install tmp/pc-psyz --prefix "$PWD/tmp/pc-preview" --strip
```

`./launch-pc-preview.sh --frames 3` was tested successfully with the local
Vulkan window backend. `--help` lists preview/capture options; `--headless`
performs the render checks and exits without entering the presentation loop.
This binary targets the local Linux x86-64 environment, not Windows or an
arbitrary older Linux distribution.

## Build without assets or an SDK

From the repository root, with CMake 3.21+, a C11 compiler and optionally Python:

```sh
cmake -S . -B tmp/pc -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/pc --config Debug
ctest --test-dir tmp/pc -C Debug --output-on-failure
cmake --build tmp/pc --target pc_audit
```

The audit writes `tmp/pc/port-audit.json`. It inventories all 61 resident ASM
targets, the five configured overlay modules, SDK references and source hazards.
Initial results: 1,071 scanned C/header files, 215 distinct resident SDK symbols,
120 scratchpad-literal lines, and 192 address-to-32-bit-cast candidates. This is
a lexical review aid, not a complete call graph, data-flow analysis or proof that
all executable overlay packages have been found.

For GCC/Clang sanitizers, configure a separate build with
`-DMEMORIES_SANITIZERS=ON`. The core test covers original game RNG integration,
overflow, signed comparison boundaries, guest address aliases, scratchpad ranges,
endianness, alignment and invalid/overflowing spans. Release builds retain checks.
The new GitHub workflow defines Linux and Windows core builds and Linux sanitizer
checks; remote workflow execution has not been observed in this workspace.

Guest storage is currently used by adapter tests only. It does not yet supply
the game's linker globals, relocations or overlay data, and cannot execute guest
code. Unsupported RAM mirrors, BIOS and MMIO accesses are rejected explicitly.

## Host compile census

```sh
python3 tools/pc/host_census.py
```

This syntax-checks all 546 game/overlay C units with the host GCC and writes
`tmp/pc/host-census.json`. First result on 2026-09-20: **465 pass as ILP32 (`-m32`),
80 pass as LP64**; with the guards and flags above, **546 / 546 pass as ILP32**
(83 as LP64). Nearly all LP64 failures are the size/offset assertions in
`src/ygo_types.h` firing on pointer-bearing structures. The 81 ILP32 failures
come from a few headers: the incomplete `GraphicsFrameBuffer` array declaration
(54 errors), `jmp_buf`, `struct DIRENTRY`, and `DisplayObject_*` pointer-type
mismatches. It is a front-end check only; it says nothing about linking,
fixed addresses, linker-placed globals or behavior.

## Optional pinned PSY-Z probe

```sh
git clone https://github.com/Xeeynamo/psyz.git tmp/port-research/psyz
git -C tmp/port-research/psyz checkout e2d3a84eb4432c0a80b193eb844edc2bdde90c62
git -C tmp/port-research/psyz submodule update --init --depth 1 external/SDL
cmake -S . -B tmp/pc-psyz \
  -DMEMORIES_PSYZ_ROOT="$PWD/tmp/port-research/psyz" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/pc-psyz --config Debug
ctest --test-dir tmp/pc-psyz -C Debug --output-on-failure
```

If that checkout already exists, skip cloning. On PowerShell pass an absolute
Windows path for `MEMORIES_PSYZ_ROOT` and use one line for the configure command.
The SDK needs a C++ compiler and SDL platform build dependencies. CMake refuses
an incorrect SDK revision or a missing SDL checkout. It does not fetch or build
the original proprietary SDK. The SDL commit recorded by this PSY-Z revision is
`147a8ee32dbf9ac02f3794964490687b6bbda1bc`.

The probe checks GTE signed triangle area and native ordering-table pointers.
It does not render a frame or boot the game. It must call `ResetGraph` to install
the SDK's native callbacks before using `ClearOTagR`. PSY-Z logs its unimplemented
`ResetCallback` during initialization; this remains a known integration gap.
See [SDK evaluation](pc-sdk-evaluation.md) for the remaining coverage gaps.

## Retail GPU packet render test

After building the optional SDK targets:

```sh
tmp/pc-psyz/memories_render_smoke tmp/pc-psyz/retail-packets.ppm
```

On this Linux machine it also passes without a window:

```sh
SDL_VIDEODRIVER=offscreen tmp/pc-psyz/memories_render_smoke tmp/pc-psyz/retail-packets.ppm
```

For multi-configuration generators use the configuration subdirectory, e.g.
`tmp/pc-psyz/Debug/memories_render_smoke.exe`. The optional argument writes a PPM
of the checked 320x240 VRAM region. It is captured before the queue stress test.
Configure `-DMEMORIES_RENDER_TESTS=ON` to register the render executable in CTest;
the default tests do not require a graphics driver.

The test submits original-width DMA packet links with draw-state commands, a
16bpp sprite, a 4bpp palette-indexed sprite, and terminal NOPs. All 76,800 pixels
are checked against a synthetic expected RGB555 image, excluding the mask bit.
It then submits 18,003 words, exceeding PSY-Z's 16K queue, and checks the final
draw arrived. A malformed stream must fail without partially changing the image.
The render log is `tmp/render-smoke.log`. Offscreen Vulkan reports that swapchain
presentation is unavailable; GPU rendering and VRAM readback still pass. Window
presentation, retail visual fidelity, transparency and all primitive variants
have not been validated by this test.

Packet traversal also has asset-independent sanitizer tests covering empty
entries, split commands, cyclic/unaligned/out-of-range links, insufficient buffer
space, terminal payloads and unsupported/truncated commands. See
[frontend contracts](pc-frontend-contracts.md) for the recovered LIBGS/LIBDS and
frame-service requirements.

## Reference inputs and build

The user's BIN/CUE in `/home/codyj/Documents/YFM` were read without modification.
An ignored local BIN copy, canonical CUE, executable and seven DATA files are
under `game/`; all ten tracked hashes pass `make verify-inputs`.

This machine's default Python 3.14 and GCC 16 do not directly build the pinned
reference toolchain: the Python lock requires 3.10, and binutils 2.42 uses a
`static_assert` identifier that conflicts with GCC's default C23 dialect.
The local workaround uses a standalone Python 3.10.21 under
`tools/environments/bootstrap-python` and `CFLAGS='-O2 -std=gnu17'` when building
binutils. These are local toolchain selections, not changes to the game's
reference compiler profiles. The pinned prebuilt MIPS GCC 2.8.1 installs normally.

Validation completed on Linux x86-64:

- `make verify-inputs`: all ten tracked hashes pass.
- `make -j8 match`: resident executable matches SHA-256
  `84a54ed74f3d0edd6d81380839f7e4ef5bfb21ecea18be9a062bd6bfa5a45c88`.
- `make -j8 match-overlays`: all five configured modules match.
- Core and packet CTests with address/undefined-behavior sanitizers: pass.
- Optional SDK build and CTest: core, packets and PSY-Z probe pass.
- Offscreen Vulkan render smoke: full texture readback, queue stress and atomic
  malformed-stream rejection pass.
- Audit spot checks: comment/string masking, manifest counts, and known SDK/
  scratchpad sites pass.

Reference logs are under `tmp/reference-match.log` and
`tmp/reference-overlays.log`; these generated files are ignored. Windows CI is
configured but has not been run here. No native game boot, retail-frame fidelity,
audio, or assembly-replacement equivalence claim is made by these checks.
