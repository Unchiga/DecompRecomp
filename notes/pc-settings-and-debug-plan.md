# PC port: settings, display, clock, audio buses and debug tooling plan

This is an implementation plan written for an agent that will do the work in
small, verifiable steps. It covers: a settings module, fullscreen, borderless,
integer scaling and other display options, vsync and frame-rate control,
separate music / sound-effect / movie volumes, and a much more robust debug
layer (logging, crash reports, sampling profiler, HUD, debug menu, tests).

Read this whole file once before starting. Then work phase by phase, in
order. Each task lists the files to touch, the exact interfaces to add, and an
acceptance check. Do not start the next task until the acceptance check for
the current one passes.

## 0. Rules for the implementing agent

1. **Never edit `src/game/`, `src/overlays/` or `src/psyq/`.** Those are the
   byte-matching decompilation. Everything in this plan lives under `src/pc/`,
   `tools/pc/`, `tests/pc/`, `notes/` and `tools/pc/build_game32.py`.
   `make match` must keep producing the retail hash; you never need to run it
   for this work, but you must not break it.
2. **Build and run after every task.** `./build-pc.sh` builds;
   `./build-pc.sh run` plays. `python3 tools/pc/build_game32.py` is the build
   driver; it picks up new `src/pc/**/*.c` files automatically by glob (see
   `NATIVE` in that script), except backend files, which are listed in
   `BACKENDS`.
3. **The SDL3 backend (`src/pc/platform/sdl.c`) is the primary target.** The
   X11 backend (`x11.c`) stays buildable and must accept every new
   `Platform_*` call, but it may answer "unsupported" (return `-1`, do
   nothing). Never leave an undefined symbol for the X11 link.
4. **Signal safety.** A 1 kHz `SIGALRM` on the main thread is the game's
   interrupt (`platform_common.c: on_alarm`). Anything reachable from
   `on_alarm`, `on_tick`, `on_vblank`, `run_tick`, `run_vblank`,
   `Memories_DiscService`, `Memories_MdecService` or the game's sequencer
   callback must be async-signal-safe: no `malloc`, no `stdio`, no locks, no
   SDL calls. Every thread a backend creates must be created with all signals
   blocked (`block_signals` / `restore_signals` in `sdl.c`). Follow that
   pattern exactly for any new thread.
5. **Settings are not part of save states.** `Spu_SetOutputVolume` is
   deliberately outside `Spu_State`; keep every new user setting outside the
   `*_State` chunks too. If you add a native static that the *game* depends
   on (clock state that affects game-visible counters, for instance), it needs
   a field in that subsystem's `*_State` function.
6. **Headless must keep working.** `MEMORIES_HEADLESS=1` skips the window and
   audio; settings must still load and apply (they already do through
   `Menu_LoadSettings` being called before the headless early return in
   `Platform_Open`).
7. **Testing the menu**: use `MEMORIES_SDL_SCRIPT` (documented at the bottom
   of `sdl.c`) to push clicks and keys at given frames. Do not try XTest or
   `XSendEvent`; synthetic X input does not reach SDL correctly.
8. **Testing without a display**: `MEMORIES_HEADLESS=1 MEMORIES_INPUT=...
   MEMORIES_DUMP_FRAME=N MEMORIES_DUMP_PATH=...` renders frame N to a PPM and
   exits. Use this for regression tests.
9. Keep the existing code style: C11, 4-space indent, `/* */` comments, one
   explanatory comment per non-obvious block, no new dependencies beyond what
   is already linked (SDL3, FreeType, fontconfig, libm, pthread).
10. When a task says "document", add or edit a section in `notes/pc-build.md`
    and keep the tables in this file (section 9) current.
11. Commit after each task with a message naming the task number, for example
    `pc: task 1.3 integer scaling and aspect modes`.

## 1. Where things are today (facts, with locations)

| Concern | Where | What it does now |
|---|---|---|
| Window, events, present | `src/pc/platform/sdl.c` | Window is exactly `picture_w*scale` by `picture_h*scale + Menu_Height()`. `resize()` recreates textures and calls `SDL_SetWindowSize`. `show()` draws the picture at `(0, Menu_Height())` scaled by `scale`, then the menu overlay, then presents. `SDL_SetRenderVSync(renderer, 0)` in `Platform_Open`. Not resizable. |
| Scale | `Platform_Scale` / `Platform_SetScale` in `sdl.c` and `x11.c`, declared in `menu.h` | 1 to 8; applied at the next `Platform_Present` through `pending_scale`. |
| Frame clock | `src/pc/platform/platform_common.c: on_alarm` | Runs `tick_handler(now)` every ms and `vblank_handler()` every 16683 us of real time. `Platform_WaitVBlank` naps 0.5 ms until `vblank_count` changes. |
| VSync (game side) | `src/pc/sdk/libetc.c: Memories_VSync` | Mode 0 presents, then waits for the next VBlank. Has a `MEMORIES_TRACE_FRAMES` timing report. Calls `exit(0)` when `Platform_ShouldQuit`. |
| Present | `src/pc/sdk/libgpu.c: Memories_PresentDisplay` | Flushes drawing, bumps `frames_presented`, calls `Platform_Frame`, handles `MEMORIES_DUMP_FRAME`, calls `Platform_Present` with the VRAM rectangle. |
| Audio mix | `src/pc/audio/spu.c: Spu_Mix` | 24 voices + CD/XA ring, master volume, one port-owned output gain ramped over 36 ms. Called by the SDL audio thread (`feed` in `sdl.c`) or the silent thread. |
| Voice ownership | `src/game/sound_voice_constants.h` | `SD_VOICE_SLOT_FIRST_VOICE` = 20, `SD_VOICE_SLOT_COUNT` = 4: **sound effects use voices 20 to 23**, the sequencer (music) uses the rest. Movie / streamed audio is the CD/XA path (`Spu_CdWrite`). |
| Settings | `src/pc/platform/menu.c: Menu_LoadSettings`, `save_settings` | `saves/settings.txt` (`MEMORIES_SETTINGS` overrides the path), keys `volume`, `3d_monsters`, `hand_camera`, `scale`. Env overrides `MEMORIES_VOLUME`, `MEMORIES_SCALE`, `MEMORIES_MODS_MONSTERS`. |
| Menu bar | `src/pc/platform/menu.c` | Software-drawn bar: File, Audio (one slider), View (1x to 4x radios), Mods (checks), Debug (one action). `Item` array capacity is 8 per menu. Slider code is hard-wired to the single `volume` variable. Keyboard: F10 opens, arrows, Enter, Esc. |
| Save states | `src/pc/guest/state.c` | F1 to F4 slot, F5 save, F7 load, `Memories_StateRequest(what, slot)`. |
| Cheats | `src/pc/debug/cheats.c` | Give N of every card (menu and `MEMORIES_DEBUG_CHEST`). |
| Stubs | `src/pc/guest/image.c: Memories_Unimplemented` | Exits 70 at the first unported routine; `MEMORIES_STUB_TRACE=1` logs and continues. |
| Tracing | `getenv("MEMORIES_TRACE_*")` in ~12 files | Each site caches a `static int trace = -1` and uses `fprintf(stderr, ...)`. Some sites (`libds.c` disc trace) run inside the signal handler, which is not safe. |
| Tests | `tests/pc/*.c`, `CMakeLists.txt` | Unit tests for the portable adapters only (core, packets, libgs_ot, gte, soft_gpu). No test runs the game. |

## 2. Design decisions (do not re-decide these)

- **One settings module** (`src/pc/platform/settings.c/.h`) owns the file, the
  env overrides and the defaults. `menu.c`, `sdl.c`, `spu.c` and the debug
  code read and write through it. It has no SDL or X11 dependency so a unit
  test can link it alone.
- **Layout is computed from the window's output size**, not the other way
  round. The window becomes resizable; the picture is placed inside the area
  under the menu bar according to the scaling mode. Changing the "scale"
  setting resizes the window in windowed mode; in fullscreen it is ignored.
- **A virtual clock** replaces the real-time arithmetic in `on_alarm`. The
  tick and VBlank handlers see virtual microseconds that advance at
  `rate / 100` of real time. Pause is rate 0; frame-step advances exactly one
  VBlank; turbo is a high rate; "uncapped" fires VBlanks from the wait loop.
  The sequencer, disc service and VBlank all follow the same clock, so music
  tempo, disc timing and frame rate stay in step, as they do in emulators.
- **VSync on** means SDL presents with vsync *and* the VBlank clock is
  phase-locked to the presents when the display refresh is within 1 Hz of
  59.94. Otherwise the timer stays the clock and vsync only removes tearing.
- **Three audio buses** in the mixer: music (voices 0 to 19), sound effects
  (voices 20 to 23), stream (CD/XA). Each has its own 0 to 100 gain, ramped
  like the existing output gain. The existing output gain becomes "master".
- **One logging module** (`src/pc/debug/log.c/.h`) with named channels, a
  signal-safe ring buffer, timestamps and frame numbers, an optional log
  file, and a HUD that can show the tail. Every existing `MEMORIES_TRACE_*`
  site is converted to it, and the old variable names keep working.
- **Crash handling, profiling and the watchdog** live in `src/pc/debug/`
  and read the build's own symbol table
  (`tmp/pc/game32/symbols/<buildid>.txt`) at startup so that a signal handler
  can name functions without allocating.

## 3. Phase 0: settings module and menu generalization

### Task 0.1: `settings.c/.h`

Create `src/pc/platform/settings.h`:

```c
#ifndef MEMORIES_PC_SETTINGS_H
#define MEMORIES_PC_SETTINGS_H
/* The port's stored settings (saves/settings.txt; MEMORIES_SETTINGS names
 * another file). Every key has a default, a range and an environment
 * override MEMORIES_<KEY IN UPPER CASE> (legacy names in the table below
 * are also honoured). Settings are never part of save states. */
typedef enum {
    SET_MASTER_VOLUME,   /* 0-100, legacy key "volume", env MEMORIES_VOLUME */
    SET_MUSIC_VOLUME,    /* 0-100 */
    SET_SFX_VOLUME,      /* 0-100 */
    SET_STREAM_VOLUME,   /* 0-100: CD/XA movie audio */
    SET_SCALE,           /* 1-8 windowed integer scale, env MEMORIES_SCALE */
    SET_FULLSCREEN,      /* 0 windowed, 1 borderless fullscreen (desktop), 2 exclusive */
    SET_BORDERLESS,      /* 0/1: windowed mode without decorations */
    SET_SCALING,         /* 0 integer, 1 fit (aspect kept), 2 stretch */
    SET_ASPECT,          /* 0 4:3, 1 square pixels */
    SET_FILTER,          /* 0 nearest, 1 linear */
    SET_VSYNC,           /* 0 off, 1 on */
    SET_SPEED,           /* clock rate in percent, 25-400; 100 normal */
    SET_SHOW_MENU_FULLSCREEN, /* 0 hidden until the pointer touches the top, 1 always */
    SET_PAUSE_ON_FOCUS_LOSS,  /* 0/1 */
    SET_MUTE_ON_FOCUS_LOSS,   /* 0/1 */
    SET_HIDE_CURSOR,     /* 0/1: hide the pointer over the picture after 2 s */
    SET_SHOW_HUD,        /* 0 off, 1 fps, 2 full stats */
    SET_MOD_3D_MONSTERS, /* legacy key "3d_monsters", env MEMORIES_MODS_MONSTERS */
    SET_MOD_HAND_CAMERA, /* legacy key "hand_camera" */
    SET_COUNT
} SettingId;

void Settings_Load(void);              /* file, then environment overrides */
void Settings_Save(void);              /* rewrites the whole file */
int Settings_Get(SettingId id);
void Settings_Set(SettingId id, int value); /* clamps; does not save */
const char *Settings_Key(SettingId id);      /* the file key, e.g. "music_volume" */
int Settings_Min(SettingId id);
int Settings_Max(SettingId id);
/* Subscribers: called after Settings_Set changes a value, on the calling
 * thread (always the main thread). At most 8. */
void Settings_Observe(void (*changed)(SettingId id, int value));
#endif
```

Implement `settings.c` with a static table `{key, legacy_key, env, legacy_env,
def, min, max}` per id. Rules:

- File lines are `key=value`. Unknown keys are kept in memory and written
  back unchanged (so older/newer builds do not destroy each other's keys).
- Legacy keys (`volume`, `3d_monsters`, `hand_camera`, `scale`) are read as
  their new ids and written under the new key **and** the legacy key for
  one release; add a `// TODO remove legacy` comment.
- Environment: for each id, `MEMORIES_` + upper-cased key, then the legacy
  env name. Env wins over the file, and env-set values are not saved back.
- `Settings_Set` clamps, and if the value changed calls observers.
- `Settings_Save` mirrors `save_settings` in `menu.c`: `mkdir("saves")`
  unless `MEMORIES_SETTINGS` is set; write through a temp file then rename.

Wire it in:

- `Menu_LoadSettings` becomes a thin wrapper: `Settings_Load()`, then apply:
  `Spu_SetOutputVolume(Settings_Get(SET_MASTER_VOLUME))`, `Platform_SetScale`,
  `Mods_SetEnabled(...)` for the two mods. Keep the name so both backends
  keep calling it.
- Replace `menu.c`'s `volume` variable, `settings_path`, `save_settings` and
  the sscanf loop with settings calls.

Acceptance: `tests/pc/settings_test.c` (add to `CMakeLists.txt` next to the
other `pc_*` tests) that sets `MEMORIES_SETTINGS` to a temp file, writes
`volume=40\nmusic_volume=70\nunknown=7\n`, calls `Settings_Load`, asserts the
values and the default for a missing key, sets one, saves, and asserts the
file still contains `unknown=7`. Then `./build-pc.sh run` with an existing
`saves/settings.txt` from before must start with the same volume and scale.

### Task 0.2: generalize the menu

In `menu.c`:

- Raise `Item items[8]` to `items[16]`.
- Add to `Item`: `SettingId setting` (for `ITEM_SLIDER`, `ITEM_CHECK`,
  `ITEM_RADIO` items that mirror a setting; `-1` when not) and `int value`
  (the radio's value). Replace `item_state` with: check → `Settings_Get(setting)`;
  radio → `Settings_Get(setting) == value`; mods keep their existing path.
- Sliders: replace `volume` with `Settings_Get(item->setting)`; `set_volume`
  becomes `set_slider(item, value)` which calls `Settings_Set`; the wheel and
  left/right keys act on the hot slider (`hot_item`) instead of testing
  `open_menu == MENU_AUDIO`. `Settings_Save()` on release, as now.
- Add `ITEM_DISABLED` flag (a bit in a new `int flags` field): drawn dim,
  skipped by `step_item`, ignored by `activate`. Backends set it for
  unsupported features via a new `Menu_SetItemEnabled(id, enabled)`.
- Add `MENU_KEY_TAB`/`MENU_KEY_F11` etc. only when a task needs them.
- Keep the `Menu_Height()` contract: it is called by the backends for layout.

Acceptance: `MEMORIES_SDL_SCRIPT="120:click:60:13,180:click:120:40"` (click
Audio, then the slider track) changes `master_volume` in the settings file.
Keyboard: F10, Right, Down, Right five times, Esc changes it by +25.

## 4. Phase 1: display

All of this is in `sdl.c` unless stated. Add to `platform.h` (and stub in
`x11.c`):

```c
/* Display settings; the backend reads them from Settings_* and applies at
 * the next present. Platform_ApplyDisplaySettings is called by the menu
 * after a change; backends that cannot honour a setting ignore it. */
void Platform_ApplyDisplaySettings(void);
/* 1 if the backend can do fullscreen/borderless/resizing (SDL), else 0. */
int Platform_HasWindowModes(void);
```

### Task 1.1: resizable window and a layout function

- Create the window with `SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY`.
- Add `static struct { int win_w, win_h; SDL_FRect dst; } layout;` and a
  `static void relayout(void)` that reads `SDL_GetRenderOutputSize(renderer,
  &w, &h)`, subtracts the menu bar height (`menu_visible ? Menu_Height() : 0`,
  see task 1.5), and computes `dst` from `SET_SCALING` and `SET_ASPECT`:
  - **Integer**: `k = max(1, min((w) / pw, (h - menu) / ph))` where
    `pw, ph` are the picture's *display* size (see aspect below). `dst.w =
    pw*k`, `dst.h = ph*k`, centered.
  - **Fit**: `s = min(w / pw, (h - menu) / ph)` as float, centered.
  - **Stretch**: fill the whole area.
  - **Aspect 4:3**: `ph = picture_h`, `pw = picture_h * 4 / 3` (320x240 is
    unchanged; a 256-wide mode is widened). **Square pixels**: `pw =
    picture_w`.
- `show()` uses `layout.dst`. The overlay texture is sized to the render
  output size (not `w*scale`); recreate it when the output size changes.
  `SDL_SetWindowSize` is now only called from `Platform_SetScale` (windowed
  mode) to `pw*scale` by `ph*scale + Menu_Height()`.
- Handle `SDL_EVENT_WINDOW_RESIZED` and `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`
  by calling `relayout()` and forcing a redraw.
- Mouse coordinates: run every mouse event through
  `SDL_ConvertEventToRenderCoordinates(renderer, &event)` before `translate`
  so high-DPI windows keep the menu hit-tests right.
- Black bars are the renderer clear colour; keep it black.

Acceptance: drag the window corner; the picture stays pixel-perfect at the
largest integer scale that fits, centered, menu bar on top. `View > 3x`
still sets the window to 960x746.

### Task 1.2: View menu rebuild

Replace the four radios with (ids are new enum values in `menu.c`):

```
View
  Window scale   > 1x 2x 3x 4x 5x 6x   (radios on SET_SCALE)
  ----
  Fullscreen           F11   (check on SET_FULLSCREEN != 0)
  Borderless window          (check on SET_BORDERLESS)
  ----
  Integer scaling            (radio SET_SCALING=0)
  Fit to window              (radio SET_SCALING=1)
  Stretch                    (radio SET_SCALING=2)
  ----
  4:3 aspect                 (radio SET_ASPECT=0)
  Square pixels              (radio SET_ASPECT=1)
  ----
  Smooth filtering           (check on SET_FILTER)
  VSync                      (check on SET_VSYNC)   [wired in phase 2]
```

The menu has no submenus; the six scale radios go in the same dropdown
(16 items is enough). `activate` for any View item calls `Settings_Set`,
`Settings_Save`, then `Platform_ApplyDisplaySettings()`.

### Task 1.3: fullscreen and borderless

In `Platform_ApplyDisplaySettings` (SDL):

- `SET_FULLSCREEN == 1`: `SDL_SetWindowFullscreenMode(window, NULL)` then
  `SDL_SetWindowFullscreen(window, true)` (borderless desktop fullscreen).
- `SET_FULLSCREEN == 2`: pick the closest mode to `pw*scale x ph*scale` at the
  display's current refresh with `SDL_GetClosestFullscreenDisplayMode`, set
  it, then fullscreen true. If none, fall back to mode 1 and log a warning.
- `SET_FULLSCREEN == 0`: `SDL_SetWindowFullscreen(window, false)`, then
  `SDL_SetWindowBordered(window, !SET_BORDERLESS)`, then restore the
  windowed size from the scale setting.
- `SET_FILTER`: `SDL_SetTextureScaleMode(picture, filter ? SDL_SCALEMODE_LINEAR :
  SDL_SCALEMODE_NEAREST)`. Linear with integer scaling is pointless; the menu
  may still allow it.
- After any of these, `relayout()` and `show()`.
- Startup: apply once in `Platform_Open` after the renderer exists, so
  `fullscreen=1` in the file starts fullscreen.
- Hotkeys in `pump`: **F11** and **Alt+Enter** toggle `SET_FULLSCREEN`
  between 0 and 1 (save the setting). **Esc** in fullscreen leaves
  fullscreen instead of quitting; a second Esc quits (keep the current
  behaviour in windowed mode).
- Remember the windowed position: on `SDL_EVENT_WINDOW_MOVED` while
  windowed, store into two new hidden settings `window_x`, `window_y`
  (add ids `SET_WINDOW_X`, `SET_WINDOW_Y`, range -16384..16384, default
  `SDL_WINDOWPOS_CENTERED` sentinel -1); apply at creation.

Acceptance: F11 goes fullscreen with black bars and a crisp integer-scaled
picture; F11 again returns to the previous window size and position; the
`fullscreen=` line in the file tracks it. Borderless check removes
decorations without changing size.

### Task 1.4: cursor and focus behaviour

- `SET_HIDE_CURSOR`: `SDL_HideCursor()` after 2 s without motion while the
  pointer is over the picture; `SDL_ShowCursor()` on motion or when it is
  over the menu bar. Track with a frame counter in `Platform_Frame`.
- `SET_PAUSE_ON_FOCUS_LOSS`: on `SDL_EVENT_WINDOW_FOCUS_LOST` call
  `Platform_SetClockRate(0)` (phase 2), on `FOCUS_GAINED` restore the
  previous rate. Until phase 2 exists, leave this item disabled.
- `SET_MUTE_ON_FOCUS_LOSS`: `Spu_SetOutputVolume(0)` on loss, restore on
  gain. Do not write the setting.

### Task 1.5: menu bar in fullscreen

- `menu_visible` is 1 in windowed mode. In fullscreen it is
  `SET_SHOW_MENU_FULLSCREEN || pointer_y < Menu_Height() || open_menu >= 0
  || menu_reveal_frames > 0`, where `menu_reveal_frames` is set to 120 when
  the pointer enters the top strip or F10 is pressed.
- The overlay is transparent where the bar would be when hidden; `relayout`
  gives the picture the full height. `Menu_Draw` must not paint when the bar
  is hidden: add `Menu_SetVisible(int)` and have `Menu_Draw`/`Menu_Bounds`
  return an empty rectangle when hidden.
- Menu bar clicks in fullscreen behave as in windowed mode.

Acceptance: in fullscreen the bar is hidden; moving the pointer to the top
shows it; F10 opens File; Esc closes it and the bar fades after ~2 s.

### Task 1.6: screenshots

- **F12** (and File > Screenshot): save the *picture* (not the menu) as
  `saves/screenshots/<yyyy-mm-dd-hhmmss>-<frame>.bmp` using
  `SDL_CreateSurfaceFrom(picture_w, picture_h, SDL_PIXELFORMAT_XRGB8888,
  picture_pixels, picture_w * 4)` and `SDL_SaveBMP`. Shift+F12 saves the
  window as displayed via `SDL_RenderReadPixels`. Log the path.
- X11 backend: write a PPM the way `MEMORIES_DUMP_FRAME` does.
- `MEMORIES_SCREENSHOT_DIR` overrides the directory.

## 5. Phase 2: clock, vsync and speed

All in `platform_common.c` unless stated. New `platform.h` declarations:

```c
/* The virtual clock the interrupt handlers run on. Rate is a percentage of
 * real time: 100 normal, 0 paused, up to 800 turbo. -1 is uncapped: a
 * VBlank is delivered as soon as the game waits for one. Main thread. */
void Platform_SetClockRate(int percent);
int Platform_ClockRate(void);
/* While paused: deliver exactly one more VBlank. */
void Platform_StepFrame(void);
/* Vsync: the backend calls this after a vsynced present with the real time
 * of the flip, so the VBlank clock can lock its phase to the display. */
void Platform_NotifyPresent(uint64_t real_now_us, int vsynced);
/* The VBlank period in virtual microseconds (16683 by default). */
void Platform_SetVBlankPeriod(unsigned us);
```

### Task 2.1: virtual clock

Rewrite `on_alarm`:

```c
static volatile int rate = 100;          /* percent; 0 paused; -1 uncapped */
static uint64_t real_prev, virtual_now, next_vblank;
static volatile unsigned vblank_period = 16683;
static volatile int step_pending;

static void advance(uint64_t real_now)
{
    uint64_t elapsed = real_prev ? real_now - real_prev : 0;
    real_prev = real_now;
    if (elapsed > 100000) elapsed = 0;          /* stalled: do not replay */
    if (rate > 0) virtual_now += elapsed * (uint64_t)rate / 100;
    if (rate == -1) virtual_now += elapsed;      /* uncapped: real time for ticks */
    if (tick_handler) tick_handler(virtual_now);
    if (!next_vblank) next_vblank = virtual_now;
    while (virtual_now >= next_vblank) {
        next_vblank += vblank_period;
        if (virtual_now > next_vblank + 4 * vblank_period) next_vblank = virtual_now;
        deliver_vblank();
    }
    if (step_pending) { step_pending = 0; deliver_vblank(); }
}
```

`deliver_vblank` bumps `vblank_count` and calls `vblank_handler`. `on_alarm`
becomes `advance(now_us())`. Everything the handlers receive is now virtual
time; `run_tick` in `libetc.c` already works on whatever it is given.

- `Platform_SetClockRate` just stores `rate` (a single aligned int is safe to
  share with the handler). `Platform_StepFrame` sets `step_pending`.
- `Platform_WaitVBlank`: when `rate == -1`, instead of napping, block
  `SIGALRM` (`sigprocmask`), call `advance(now_us())` with `virtual_now`
  forced forward by one `vblank_period` so a VBlank is delivered now, then
  unblock. This runs the handlers on the main thread outside a signal, which
  is allowed (same thread, between game instructions).
- When `rate == 0` the game sits in `Platform_WaitVBlank`; the nap is fine.
- **Audio and speed**: the SPU mixes in real time regardless; at 200 % the
  sequencer issues notes twice as fast, so music plays at double tempo, as in
  emulators. Nothing to do.
- **Presents and speed**: in `Memories_VSync` mode 0, when `rate > 100 ||
  rate == -1`, skip `Memories_PresentDisplay`'s `Platform_Present` unless at
  least 16 ms of real time passed since the last present (frame skipping).
  Put that check in `libgpu.c: Memories_PresentDisplay` with a
  `Platform_ClockRate()` query. Never skip the `Platform_Frame` call (input
  scripts count presented frames, and `Cheats_Frame` runs there).

Acceptance: `MEMORIES_TRACE_FRAMES=1 ./build-pc.sh run` reports
`~59.94 VBlanks/s` at rate 100 and `~119.9` after choosing 200 % from the
Debug menu (task 5.2); title music tempo doubles; pause freezes the picture
and the music while sound already keyed keeps decaying; frame step advances
one frame per press.

### Task 2.2: vsync

In `sdl.c`:

- `Platform_ApplyDisplaySettings` calls `SDL_SetRenderVSync(renderer,
  SET_VSYNC ? 1 : 0)`.
- After `SDL_RenderPresent` in `show()`, when vsync is on, call
  `Platform_NotifyPresent(now_us(), 1)`.
- Read the display refresh: `SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window))->refresh_rate`
  (float). Re-read on `SDL_EVENT_WINDOW_DISPLAY_CHANGED`.

In `platform_common.c: Platform_NotifyPresent`:

- If `vsynced` and the refresh is within 59.0 to 61.0 Hz: set
  `vblank_period = 1e6 / refresh` and set `next_vblank = virtual_now +
  vblank_period - 1500` (a VBlank 1.5 ms before the next expected flip so
  the game's frame is ready when the present blocks). Because these fields
  are also written by the handler, block `SIGALRM` around the update.
- Otherwise leave the clock alone. Vsync then only prevents tearing.
- Document the Wayland note already in `Platform_Open` (x11 preferred
  because Wayland presents block); with vsync on that difference disappears,
  so also add: when `SET_VSYNC` is 1, do not set the `x11,wayland` hint.

Acceptance: with a 60 Hz display, `MEMORIES_TRACE_FRAMES` reports 0 of 120
missed VBlanks with vsync on over a minute of the title screen; with a 144 Hz
display the report stays at 59.94 VBlanks/s and no tearing is visible.

### Task 2.3: hotkeys and settings for the clock

- **Tab** held: turbo (`rate = 400`), release restores `SET_SPEED`.
- **P**: toggle pause (rate 0 / `SET_SPEED`).
- **.** (period): frame step while paused.
- `SET_SPEED` is applied at startup and when changed from the Debug menu.
- `MEMORIES_SPEED=percent` env override comes for free from the settings
  module; `MEMORIES_SPEED=-1` should map to uncapped (allow -1 in the range).
- Show the state in the window title suffix, e.g. `[paused]`, `[200%]`,
  reusing the title code in `pump` that shows the state slot.

## 6. Phase 3: audio buses

### Task 3.1: mixer buses

In `spu.h`:

```c
typedef enum { SPU_BUS_MUSIC, SPU_BUS_SFX, SPU_BUS_STREAM, SPU_BUS_COUNT } SpuBus;
/* Per-bus port gains, 0-100, ramped like the output volume. Music is the
 * sequencer's voices 0-19, SFX the four dedicated voices 20-23
 * (SD_VOICE_SLOT_FIRST_VOICE in src/game/sound_voice_constants.h), stream
 * the CD/XA input. Main thread sets, audio thread reads. */
void Spu_SetBusVolume(SpuBus bus, int percent);
int Spu_GetBusVolume(SpuBus bus);
```

In `Spu_Mix`:

- Keep three `gain[bus]` walkers next to the existing `gain` (same
  `GAIN_STEP` ramp).
- Accumulate voices into `music_l/r` (v < 20) and `sfx_l/r` (v >= 20)
  instead of one `left/right`; apply the bus gain to each sum, then add,
  then apply master volume as now. The CD/XA contribution gets the stream
  gain before being added.
- Do **not** include the game's own master volume (`master_left/right`) or
  the per-voice volumes in the bus split; those stay as they are.
- Voice-to-bus mapping is a constant `#define SPU_SFX_FIRST_VOICE 20`; include
  `game/sound_voice_constants.h` and `_Static_assert` it equals
  `SD_VOICE_SLOT_FIRST_VOICE`.

Verification before trusting the split (do this first, record the result in
`notes/pc-build.md`): run `MEMORIES_TRACE_SPU=1 ./build-pc.sh run`, reach the
title screen (music) and move the cursor (sound effects). Every `key on` line
during music alone must have a voice mask with no bits 20 to 23 set; every
cursor blip must set only bits 20 to 23. If the sequencer ever keys 20 to 23,
stop and report; the fallback design is to tag voices at key-on by the game
routine that keyed them (`SpuSetKeyOnWithAttr` is the SFX path, `SpuSetKey`
the sequencer path), which needs a `bus_of_voice[24]` array written in
`libspu.c`.

### Task 3.2: Audio menu

```
Audio
  Master     [slider SET_MASTER_VOLUME]
  Music      [slider SET_MUSIC_VOLUME]
  Sound FX   [slider SET_SFX_VOLUME]
  Movies     [slider SET_STREAM_VOLUME]
  ----
  Mute all          M   (check; toggles a runtime mute, not a setting)
  Mute on focus loss    (check SET_MUTE_ON_FOCUS_LOSS)
```

`Menu_LoadSettings` applies the three bus volumes on startup; the settings
observer (task 0.1) applies them on change so the slider is live while
dragging.

Acceptance: title screen: music slider to 0 silences the tune while cursor
blips remain; sound FX slider to 0 does the reverse; intro movie: only the
Movies slider changes its soundtrack. `MEMORIES_DUMP_AUDIO` output with music
at 0 during the title has near-zero RMS.

### Task 3.3: audio health stats

Expose from `sdl.c` for the HUD: `SDL_GetAudioStreamQueued(stream)` in
frames, and count underruns (`feed` asked for more than the mixer could
deliver is not possible here, so count when queued drops to 0 at entry to
`feed`). Add `Platform_AudioStats(int *queued_frames, unsigned *underruns)`
to `platform.h` (X11/ALSA: return zeros).

## 7. Phase 4: debug foundations

### Task 4.1: logging module

Create `src/pc/debug/log.h`:

```c
typedef enum {
    LOG_FRAMES, LOG_DISC, LOG_SPU, LOG_INPUT, LOG_MENU, LOG_MEMCARD, LOG_MODS,
    LOG_MODEL, LOG_DUEL_EFFECTS, LOG_MIPS_PRINTF, LOG_STUB, LOG_STATE, LOG_CLOCK,
    LOG_WINDOW, LOG_AUDIO, LOG_COUNT
} LogChannel;
/* MEMORIES_TRACE="disc,spu" or "all" enables channels; each legacy
 * MEMORIES_TRACE_<NAME>=1 still enables its channel. MEMORIES_LOG=path
 * also writes every line to that file. Lines carry the presented frame and
 * the VBlank count. */
void Log_Init(void);
int Log_Enabled(LogChannel channel);              /* cheap: a bit test */
void Log_Enable(LogChannel channel, int on);      /* runtime toggles (Debug menu) */
const char *Log_ChannelName(LogChannel channel);
/* Main thread and worker threads: formats and writes immediately. */
void Log_Printf(LogChannel channel, const char *format, ...);
/* Signal-handler safe: copies up to 6 integer arguments and a literal
 * format string into a lock-free ring; the main thread formats them at
 * the next Log_Drain (called from Platform_Frame). */
void Log_Signal(LogChannel channel, const char *literal_format, long a, long b, long c, long d, long e, long f);
void Log_Drain(void);
/* The last N formatted lines, for the HUD and the crash report. */
int Log_Tail(int n, const char **lines);
#define LOG(channel, ...) do { if (Log_Enabled(channel)) Log_Printf(channel, __VA_ARGS__); } while (0)
```

- The ring for `Log_Signal` is a fixed array of 256 records with an atomic
  head index; overflow increments a `dropped` counter reported at drain.
- `Log_Tail` keeps the last 64 lines in a static circular array of
  fixed-size strings (no malloc after init).
- Every line: `[frame 1234 vb 1240 disc] lba 5000 -> ...`.
- Convert every existing `getenv("MEMORIES_TRACE_*")` site (see the census
  in section 9) to `LOG(...)`. Sites reached from the signal handler
  (`libds.c` disc trace, anything in `run_tick`/`run_vblank`) must use
  `Log_Signal`. `MEMORIES_TRACE_FRAMES` in `libetc.c` keeps its statistics
  but prints through `LOG(LOG_FRAMES, ...)`.
- `Log_Init` is called first thing in `main()`; `Log_Drain` from
  `Platform_Frame` in both backends.

Acceptance: `MEMORIES_TRACE=disc,frames MEMORIES_LOG=tmp/pc/run.log
./build-pc.sh run` produces the same information the legacy variables did,
with prefixes, in the file and on stderr; `MEMORIES_TRACE_DISC=1` alone still
works. Add `tests/pc/log_test.c`: enable a channel, push 300 `Log_Signal`
records, drain, assert 256 lines plus a dropped notice.

### Task 4.2: symbol table at runtime

Create `src/pc/debug/symbols.c/.h`:

```c
/* The build's own symbol table (tmp/pc/game32/symbols/<buildid>.txt, the
 * file state.c already reads) loaded once at startup into a sorted array,
 * so a signal handler can name an address without allocating. */
int Symbols_Load(void);
/* Name and offset for an address, or NULL. Async-signal-safe. */
const char *Symbols_Lookup(uintptr_t address, uintptr_t *offset);
```

`state.c` already locates the table beside the executable (see
`build_id` and the `symbols/` lookup around line 430); reuse that path
logic (factor a `Memories_SymbolTablePath(char *out, size_t size)` out of
`state.c`).

### Task 4.3: crash reporter

Create `src/pc/debug/crash.c`. In `main()` after `Log_Init` and
`Symbols_Load`, install `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` and `SIGABRT`
handlers with `SA_SIGINFO | SA_ONSTACK` on an alternate stack
(`sigaltstack`, 64 KiB; the game's stack is mapped at a fixed address and may
be what overflowed). The handler, using only `write(2)`, `snprintf` into a
static buffer, and `Symbols_Lookup`:

1. Prints the signal, the fault address (`si_addr`), and classifies it:
   guest RAM (`0x80000000`+2 MiB, `0xA0000000` mirror, physical mirror,
   scratchpad `0x1F800000`), game section (`game_text` 0x01000000 ...,
   from `FIXED_SECTIONS`), game stack (`0x70000000`), or native.
2. Prints `EIP`, `ESP`, `EBP` from `ucontext_t.uc_mcontext.gregs[REG_EIP]`
   etc. (i386), with `Symbols_Lookup` for `EIP`.
3. Walks the frame chain from `EBP` (game units are `-O0` so frame pointers
   exist; native is `-O2` — add `-fno-omit-frame-pointer` to `NATIVE_CFLAGS`
   in `build_game32.py`), printing up to 32 return addresses with names.
   Validate each `EBP` is within the game stack or the main stack range
   before dereferencing.
4. Prints the presented frame, VBlank count, clock rate, the last state slot
   loaded, and `Log_Tail(32)`.
5. Writes the same text to `tmp/pc/crash-<pid>.txt` (open with `open(2)`).
6. Re-raises with the default handler so the exit status is unchanged.

Also: `MEMORIES_STUB_TRACE` mode should collect a per-stub call count and
print a sorted summary at exit (`atexit` in `image.c`); add
`MEMORIES_STUB_BREAK=name` which raises `SIGTRAP` when that stub is reached,
for use under gdb.

Acceptance: `MEMORIES_CRASH_TEST=1` (an env check in `Platform_Frame` that
dereferences a null pointer at frame 60) produces a report naming
`Platform_Frame` and the game function that called `VSync`, and the file
exists.

### Task 4.4: sampling profiler and watchdog

In `platform_common.c`, switch `on_alarm` to `SA_SIGINFO` so it gets the
`ucontext_t` and can read the interrupted `EIP`. With `MEMORIES_PROFILE=path`:

- Keep a static histogram `uint32_t samples[N]` bucketed by 16 bytes over
  the game text and native text ranges (compute the ranges at startup from
  the symbol table; about 1 MiB of text -> 64 K buckets). Increment in the
  handler; that is signal-safe.
- At exit, write `path` as lines `address count symbol+offset` sorted by
  count. Add `tools/pc/profile_report.py` that prints the top 40 with
  percentages and an inclusive-by-symbol table.

Watchdog: if `Memories_VSync` has not been entered for 5 s of real time while
`rate != 0`, the handler sets a flag; `Platform_Frame` cannot run (the game
is stuck), so the alarm handler itself, once, writes a stack sample (the
same frame walk as the crash reporter, from the interrupted context) to
stderr and to `tmp/pc/hang-<pid>.txt`, prefixed `memories-pc: no VSync for 5 s`.
`MEMORIES_WATCHDOG=0` disables it; `MEMORIES_WATCHDOG=seconds` changes it.

Acceptance: profile of 30 s at the title screen names `SoftGpu_Gp0` /
rasterizer functions and `Spu_Mix` near the top; a deliberate infinite loop
behind `MEMORIES_HANG_TEST=1` in `Platform_Frame` yields the hang report.

### Task 4.5: assertion and invariant checks (cheap, always on)

- `libgpu.c: DrawOTag` already exits 70 on a bad list; route it through the
  crash reporter's text dump (call a `Crash_ReportSoft("DrawOTag", ...)`
  that prints frame, stack and log tail, then exits 70).
- `Memories_Unimplemented` (non-trace mode) does the same before `_exit(70)`.
- `Memories_StatePoint` load failures print the reason and now also
  `Log_Tail`.

## 8. Phase 5: debug surface

### Task 5.1: HUD

Create `src/pc/debug/hud.c/.h` drawing onto the menu overlay canvas after
`Menu_Draw` (call `Hud_Draw(&canvas)` from `Platform_Present` in both
backends; the HUD needs `menu.c`'s text routines, so export
`Menu_DrawText(MenuCanvas *, int x, int y, const char *, uint32_t colour)` and
`Menu_TextWidth`). Levels (`SET_SHOW_HUD`, toggled by **F3**):

1. `fps`: `59.9 fps` top-right, plus `[paused]`/`[200%]`.
2. `full`: a translucent panel with: frame / VBlank counts; game and
   present µs per frame and the max (from the `MEMORIES_TRACE_FRAMES`
   statistics in `libetc.c`, which must be moved into an always-updated
   struct `FrameStats` exposed by a `Memories_FrameStats()` getter, cheap
   enough to keep on always); missed VBlanks per 120; DrawOTag words and µs;
   audio queued frames and underruns; disc head LBA and bytes/s (from
   `libds.c`); active SPU voices (`Spu_KeyStatus != 0`) per bus; clock rate;
   state slot; last 8 log lines.

The panel must be repainted only when the HUD is on (the overlay upload
rectangle in `sdl.c` must include the HUD's bounds: add `Hud_Bounds`).

### Task 5.2: Debug menu

```
Debug
  Show HUD               F3   (radio-ish: cycles 0/1/2; use a check for on/off and a second "Full stats" check)
  ----
  Pause                  P    (check, runtime)
  Frame step             .    (action)
  Speed 50%                   (radio SET_SPEED=50)
  Speed 100%                  (radio SET_SPEED=100)
  Speed 200%                  (radio SET_SPEED=200)
  Speed uncapped              (radio SET_SPEED=-1)
  ----
  Trace: frames / disc / spu / input / state   (checks calling Log_Enable)
  ----
  Dump frame (PPM)            (action: write the current VRAM display rect via the MEMORIES_DUMP_FRAME code path, refactored into `Memories_DumpFrame(path, full_vram)`)
  Dump VRAM (PPM)             (action)
  Give 3 of every card        (existing)
```

16 items is the cap; if that is exceeded, move the trace checks to a second
menu titled "Trace". Also add to File: `Slot 1..4` radios (runtime, shown in
the title as now), `Screenshot F12`, `Reload settings`.

### Task 5.3: deterministic smoke test

Add `tools/pc/smoke.py`:

- Runs `tmp/pc/game32/memories-pc` headless with `MEMORIES_INPUT` scripts
  from `tests/pc/smoke/*.json` (`{"name", "input", "frame", "sha256"}`),
  `MEMORIES_DUMP_FRAME` at the given frame, and compares the PPM hash.
- First run with `--record` writes the hashes. Provide three scripts: the
  title screen (frame 900, no input), the main menu with the cursor moved
  once, and Options.
- Also runs each `pc_*` CTest.
- Exit non-zero on any mismatch and print the differing frame's path.

Add a `make check-pc` target that runs it. Document in `notes/pc-build.md`
and `notes/continuous-integration.md`.

Acceptance: `make check-pc` passes twice in a row (determinism) and fails
when the input script is altered.

## 9. Reference tables (keep current)

### Settings keys

| Key | Id | Default | Range | Env |
|---|---|---|---|---|
| `master_volume` (legacy `volume`) | SET_MASTER_VOLUME | 100 | 0-100 | `MEMORIES_MASTER_VOLUME`, `MEMORIES_VOLUME` |
| `music_volume` | SET_MUSIC_VOLUME | 100 | 0-100 | `MEMORIES_MUSIC_VOLUME` |
| `sfx_volume` | SET_SFX_VOLUME | 100 | 0-100 | `MEMORIES_SFX_VOLUME` |
| `stream_volume` | SET_STREAM_VOLUME | 100 | 0-100 | `MEMORIES_STREAM_VOLUME` |
| `scale` | SET_SCALE | 4 | 1-8 | `MEMORIES_SCALE` |
| `fullscreen` | SET_FULLSCREEN | 0 | 0-2 | `MEMORIES_FULLSCREEN` |
| `borderless` | SET_BORDERLESS | 0 | 0-1 | `MEMORIES_BORDERLESS` |
| `scaling` | SET_SCALING | 0 | 0-2 | `MEMORIES_SCALING` |
| `aspect` | SET_ASPECT | 0 | 0-1 | `MEMORIES_ASPECT` |
| `filter` | SET_FILTER | 0 | 0-1 | `MEMORIES_FILTER` |
| `vsync` | SET_VSYNC | 0 | 0-1 | `MEMORIES_VSYNC` |
| `speed` | SET_SPEED | 100 | -1, 25-800 | `MEMORIES_SPEED` |
| `show_menu_fullscreen` | SET_SHOW_MENU_FULLSCREEN | 0 | 0-1 | |
| `pause_on_focus_loss` | SET_PAUSE_ON_FOCUS_LOSS | 0 | 0-1 | |
| `mute_on_focus_loss` | SET_MUTE_ON_FOCUS_LOSS | 0 | 0-1 | |
| `hide_cursor` | SET_HIDE_CURSOR | 1 | 0-1 | |
| `show_hud` | SET_SHOW_HUD | 0 | 0-2 | `MEMORIES_SHOW_HUD` |
| `window_x`, `window_y` | SET_WINDOW_X/Y | -1 | -16384..16384 | |
| `3d_monsters` | SET_MOD_3D_MONSTERS | 0 | 0-1 | `MEMORIES_MODS_MONSTERS` |
| `hand_camera` | SET_MOD_HAND_CAMERA | 1 | 0-1 | |

### Hotkeys (after this plan)

| Key | Action |
|---|---|
| F1-F4 | choose state slot |
| F5 / F7 | save / load state |
| F3 | cycle HUD |
| F10 | open menu bar |
| F11, Alt+Enter | toggle fullscreen |
| F12 / Shift+F12 | screenshot of picture / of window |
| Tab (hold) | turbo |
| P | pause |
| . | frame step |
| M | mute |
| Esc | leave fullscreen, close menu, else quit |

### Existing environment variables to keep working

`MEMORIES_DEBUG_CHEST, MEMORIES_DISC, MEMORIES_DUEL_EFFECTS, MEMORIES_DUMP_AUDIO,
MEMORIES_DUMP_FRAME, MEMORIES_DUMP_PATH, MEMORIES_DUMP_VRAM, MEMORIES_HEADLESS,
MEMORIES_INPUT, MEMORIES_LOAD_STATE, MEMORIES_MODEL_MODULES, MEMORIES_MODS_MONSTERS,
MEMORIES_NO_AUDIO, MEMORIES_NO_GAMEPAD, MEMORIES_NO_SHM, MEMORIES_SAVE_STATE,
MEMORIES_SCALE, MEMORIES_SDL_SCRIPT, MEMORIES_SETTINGS, MEMORIES_STATE_DIR,
MEMORIES_STUB_TRACE, MEMORIES_VOLUME` and every `MEMORIES_TRACE_*`:
`DISC, DUEL_EFFECTS, FRAMES, INPUT, MEMCARD, MENU, MIPS_PRINTF, MODEL_MODULES,
MODS, SPU`.

New: `MEMORIES_TRACE, MEMORIES_LOG, MEMORIES_PROFILE, MEMORIES_WATCHDOG,
MEMORIES_STUB_BREAK, MEMORIES_SCREENSHOT_DIR, MEMORIES_SPEED, MEMORIES_VSYNC,
MEMORIES_FULLSCREEN` and the other settings envs above.

## 10. Pitfalls checklist (read before each task)

- Blocking signals: any `pthread_create`, `SDL_Init`, `SDL_CreateRenderer`,
  `SDL_OpenAudioDeviceStream` must be wrapped in `block_signals` /
  `restore_signals`, or the game's interrupt lands on a driver thread and the
  main thread deadlocks (see the comment above `Platform_StartTimers`).
- `SDL_SetWindowSize` inside `resize()` fights fullscreen; only call it from
  the scale setter in windowed mode.
- The overlay texture must match the render *output* size, not the window
  size, on high-DPI displays.
- `pending_scale` exists because `Platform_SetScale` may be called before the
  window exists (settings load) and from the menu mid-frame; keep applying
  display changes at the next present, never from inside `pump()` while a
  texture is being uploaded.
- The wheel and mouse buttons must not reach the game while a menu is open
  (`Menu_Event` returns 1 to swallow them); keep that when adding items.
- `Memories_VSync` calls `exit(0)` on quit; the crash reporter and profiler
  writers must use `atexit`, and `Memories_Unimplemented` uses `_exit`, so
  flush the profile from the stub path explicitly too.
- Game units are `-O0` and poll globals the VBlank handler updates; the
  virtual clock must still deliver VBlanks from the *main* thread only.
- `Spu_Mix` runs on the audio thread: bus gains must be single-word writes,
  read once per mix call into locals, ramped inside the loop.
- 24-bit display (`rgb24`, movies) has a different `Platform_Present`
  conversion path; test fullscreen and integer scaling during the intro
  movie as well as at 15-bit screens.
- The X11 backend must still link: every new `Platform_*` symbol needs a
  definition there, and `build_game32.py: BACKENDS["x11"]` must keep
  listing the extra files it needs.
- Save states must load across these changes: nothing here goes into a
  `*_State` chunk, and `vblank_count` keeps the same meaning.
