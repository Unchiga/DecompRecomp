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

## 32-bit game executable (bring-up)

The user chose a 32-bit (ILP32) host build as the bring-up memory model on
2026-09-20; 64-bit is deferred. Requires `gcc -m32` with a 32-bit libc, the
matching build's ELF (`make match`) and `game/SLUS_014.11`.

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

Environment switches: `MEMORIES_DISC` (default `game/rpg-yfm.bin`, raw
MODE2/2352), `MEMORIES_SCALE` (1-8, default 4, which puts the 320x240
picture on screen at 1280x960), `MEMORIES_VOLUME` (0-100, overrides the
stored volume and applies headless too), `MEMORIES_SETTINGS` (another
settings file), `MEMORIES_HEADLESS=1`,
`MEMORIES_DUMP_FRAME=N` with `MEMORIES_DUMP_PATH` and optional
`MEMORIES_DUMP_VRAM=1` (write frame N as PPM and exit),
`MEMORIES_INPUT="700:0008,706:0000"` (scripted pad bits from a frame on),
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

### Where the player's files go

Nothing the player owns lives beside the game any more. `platform/paths.c`
resolves one user directory -- `Documents\My Games\YFM ReDecomp` on Windows,
`$XDG_DATA_HOME/YFM ReDecomp` (`~/.local/share/YFM ReDecomp`) elsewhere,
`MEMORIES_USER_DIR` instead of either -- and everything the port writes goes
under it:

| File | What it is |
|---|---|
| `settings.txt` | the settings (`MEMORIES_SETTINGS` names another file) |
| `controls.txt` | the control bindings (`MEMORIES_CONTROLS`) |
| `memcard1.mcd`, `memcard2.mcd` | the memory cards (`MEMORIES_MEMCARD1/2`) |
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
(`tools/pc/build_game32.py --backend sdl|x11`, or `MEMORIES_BACKEND`; SDL when
`tmp/pc/sdl-m32/libSDL3.a` exists, which `tools/pc/build_sdl32.sh` builds
from PSY-Z's bundled SDL 3.4 as a 32-bit static library; `./build-pc.sh`
runs that first). Both share the menu (`menu.c`), the interrupt clock and the
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
composes in software: the 320x240 picture scaled by an integer (View menu,
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
| Audio | Master/music/SFX/movie sliders, mute and focus-loss mute |
| View | Window scale and Menu size submenus, window mode, scaling/aspect/filter/VSync choices |
| Game | Game speed, Frame rate and Cheats submenus (Give 3 of every card) |
| Mods | opens the mods window, which lists every mod found in `mods/` beside the executable and in the user directory (`notes/modding.md`) |
| Debug | HUD levels, pause/step, frame and VRAM dumps |
| Trace | Live frames, disc, SPU, input and state log-channel switches |

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
(`Menu_AutoScale`): 1 up to about 720 rows, 2 for a 4x window, 3 on a 4K
display. `MEMORIES_SDL_SCRIPT` accepts `frame:shot` to save the composed
window, which is how the menus are checked.

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
distance and a model's primitives at a quarter of theirs, so the GTE's Z
factors are quartered for the pass and the monster is moved three entries
nearer, which draws it on the card rather than under it; and the duel flies
its camera down to eye level for the zone picker, using a card and the
guardian-star presentation, where a monster standing on a card has nothing to
stand on, so the pass runs only while the camera is above the mat (pitch below
512 of a 4096-unit turn).

Switches: `MEMORIES_MODS_MONSTERS=0/1`, `MEMORIES_MODS_SCALE` (4096 = as
measured), `MEMORIES_MODS_PIXELS`, `MEMORIES_MODS_DEPTH`,
`MEMORIES_MODS_PITCH`, `MEMORIES_MODS_LIFT` (field units above the card, 2 per
pixel from the duel's view), `MEMORIES_TRACE_MODS=1`, and `MEMORIES_MODS_TEST=<card
id>` which stands a different monster in all ten zones, which is how the
cache, the arenas and the banks were measured together.

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

### Deterministic PC checks

`make check-pc` rebuilds the native game and portable C tests, runs every
`pc_*` CTest, then boots the game headless three times. It compares PPM hashes
for the title at frame 900, the main menu after one cursor move, and Options.
Failures retain the differing image beneath `tmp/pc/smoke/`. After an
intentional rendering change, inspect those images and update the fixtures
with `python3 tools/pc/smoke.py --record`; immediately run the normal command
twice before committing new hashes.

The smoke runner clears other `MEMORIES_*` switches (except a caller-supplied
`MEMORIES_DISC`) and uses isolated settings files, a fixed headless dump clock,
no gamepad and no audio device. It still requires the private disc image and
the native build prerequisites described above.

### Save states

F1, F2 and F4 pick those slots (shown in the window title), while slot 3 is
available from File; **F5 saves and F7 loads**. Slots
are `tmp/pc/states/slot<N>.state` (`MEMORIES_STATE_DIR` moves them), about
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

### Memory cards

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
