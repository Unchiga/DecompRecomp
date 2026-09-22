# Controls

Open **Game > Controls...**. The header holds the window title, the Player 1 /
Player 2 segmented control, the Keyboard and Controller tabs and an "Unsaved
changes" marker. Below it the Controller row holds the device list and a state
badge (Connected, Not connected, Switched off, or "Choose again after a
restart" for two identical pads). Choose Player 1 or Player 2, then choose an
actual controller from that list. Automatic fills free ports while preserving
connected assignments; None disables that port's controller. Keyboard and mouse
continue to belong to Player 1. A connected third controller can be selected.

Double-click a binding cell or a PS1 diagram button to start the 10-second rebind timer.
You can also select a binding cell and choose Rebind (or press Enter). Release held inputs,
then press the new key, controller button, D-pad direction, trigger or stick
direction. Escape cancels; capture also cancels on focus loss, disconnection or
a ten-second timeout. Repeated keyboard events do not create bindings. Existing
host shortcuts and modifier chords are reserved. Right Shift alone remains
available for Select. If the input belongs to another action, choose Move binding
or Cancel. Clear unbinds the selected slot. Controller rows have two slots so
D-pad and stick directions can both control the same PS1 direction.

Both tabs show the PlayStation pad picture beside the binding table. The
picture always shows a PS1 controller using the supplied cyan outline artwork
on a transparent background, regardless of the connected hardware; its buttons
highlight the mapped PlayStation output, and the panel header names whatever is
held. Clicking a button in the picture selects that PS1 action, and
double-clicking it starts a rebind. Stick clicks (L3/R3) have no place on the
artwork and are edited in the list. The Controller dropdown lists the actual
connected PC controllers by name; it chooses the input device, not a diagram
style.

The table lists the 16 destinations in reading order under group headings
(D-pad, Face buttons, Shoulders and triggers, Stick clicks, System), with
column headings above them; the keyboard tab has one binding column, the
controller tab a main and an alternate column. That order is presentation only:
the stored file, the destination indices and the wire bits keep the order of
`Controls_Actions`. Below the table the selected binding is named in full, with
Clear and Rebind beside it, then a hint line (or the capture countdown) and the
message line.

The window is resizable, with a minimum size of 560x440. As it shrinks, the
table scrolls, then the pad picture drops out, and under 560 logical rows of
height the hint line goes too (the capture countdown moves to the message line); the menu scale drops towards 1 rather than clipping the
layout. Tab/Shift+Tab move focus, arrows navigate rows and lists, Enter
activates, Delete clears the focused slot, and Page Up/Page Down scroll.
The pointer highlights whatever it is over. Menu navigation uses mouse and
keyboard only. Controller inputs are used for binding capture and live previews;
they cannot move focus, activate menu buttons or close the window.

Apply saves and activates the draft. OK also closes. Cancel discards changes
since the last Apply. Closing a dirty window offers Apply, Discard and Keep
editing. Restore keyboard/controller defaults changes only the current
tab/profile after confirmation. Apply carries an accent outline while the draft differs
from the saved configuration and drops to a flat, disabled face when there is
nothing to save (the same disabled face Clear uses on an empty slot), and
the dialogs (unsaved changes, restore defaults, input already in use) dim the
window behind them. A save failure leaves both the active mapping and existing file
unchanged. Live gameplay input is suppressed while this window is open, and held
inputs must be released before gameplay resumes. Scripted `MEMORIES_INPUT` still
works. Capture and device discovery work while the game is paused.

Explicit device selections remain disconnected if the selected device is absent;
they do not silently take another controller. Stable Linux device identities use
vendor/product/bus plus serial or physical path. SDL and evdev share these
identities where SDL supplies an accessible evdev path. Devices without a stable
identity can be selected for the current session; the UI warns that selection
must be repeated after restart. Transport or USB-port changes can also require
reselection. Previous profiles remain in the file. Discovery supports 32 devices
and storage supports 16 device profiles. Physical hardware/driver-specific naming
and reconnect behavior should still be checked with the user's controllers.

## Storage

Controls live outside guest saves and save states. The default file is
`saves/controls.txt`. `MEMORIES_CONTROLS` overrides it; otherwise an explicit
`MEMORIES_SETTINGS` places `controls.txt` beside that settings file. Apply writes
a temporary file in the same directory, checks flush/close and atomically renames
it. Missing files use defaults. Invalid version-1 files fall back to defaults
with a diagnostic. Unsupported versions are preserved and cannot be overwritten
by Apply.

Version 1 is ASCII with newline-terminated records:

```text
controls 1 <device-profile-count>
port <0|1> <mode> <icon-style> <hex-identity-or-dash>
device <profile-index> <icon-style> <hex-identity>
bind <map-index> <slot-index> <source-token>
```

Both port records are required. Mode 0 means None, 1 Automatic, 2 explicit.
The legacy icon-style field (0–4) is retained for file compatibility and ignored
by the fixed PS1 diagram. Identities
are hex-encoded bytes; `-` is the empty identity. Profile indices start at zero.
Map 0 is the keyboard, maps 1 and 2 are per-port controller defaults, and maps
3 onward are stored device profiles. Each map has exactly 32 binding records:
slot index = PS1 bit index × 2 + binding slot. The keyboard's second slots are
always `unbound`. An entire default file is produced by Apply; the example above
is a grammar, not a complete usable configuration.

Source examples: `key.x`, `key.rshift`, `button.south`, `button.l-shoulder`,
`axis.l-stick_y_-`, `axis.r-stick_x_+`, `trigger.r-trigger`, `hat.dpad_up`,
`unbound`. Tokens use canonical physical names, independently of the current
keyboard layout's display labels. No SDL enum values, X keycodes or event-node
numbers are persisted. Duplicate records/sources, invalid ranges, missing records,
excessive lines and long identities are rejected. The existing default mapping
is listed in [pc-build.md](pc-build.md).

## Implementation and verification

The window redesign of 2026-09-21 kept every rule above and changed the
presentation: the menu.c palette so the game menu, Mods and Controls match,
grouped table rows with column headings, tabs and a segmented player control,
pointer highlighting, a device state badge, a capture countdown, dimmed
dialogs, and Delete/Page Up/Page Down. `ControlsWindow_MinSize` feeds the
backends' minimum window size, and `ControlsWindow_Locate` returns where the
last draw put a widget, so tests address controls by the
`CONTROLS_UI_*` ids in `controls_window.h` instead of by pixel. Both backends
create the window resizable (SDL `SDL_WINDOW_RESIZABLE` plus
`SDL_SetWindowMinimumSize`; X11 `PMinSize` with `StructureNotifyMask`) and
rebuild the canvas and texture/XImage on resize, keeping the old pair when the
new one cannot be made.

Shared modules are `controls` (mapping/capture), `controls_config` (persistence),
`controls_runtime` (device registry, assignments, published input),
`controls_window` (drawing and interaction), and `controls_linux` (Linux device
identity). SDL and X11 own their native windows and normalize input into the
shared types. Gamepad/keyboard getters read cached words; mapping, I/O, allocation
and UI work run on the main thread. Secondary SDL renderers restore the game GL
context before returning. The controller PNG is embedded in the executable and
alpha-blended by `controls_art`; see [artwork and edit prompt](../src/pc/assets/README.md).
The artwork has no analog sticks, so L3/R3 are edited in the binding list.

The initial partial implementation had out-of-bounds axis/trigger indexing, a
button mask too narrow for its enum, colliding source identities, and incomplete
neutral capture. These were corrected before backend integration.

Validation on 2026-09-21 (re-run after the window redesign):

- `tmp/pc-controls`: all 13 CTests passed, including mapping, config, runtime,
  window, evdev, existing settings/Mods, and portable game core tests.
- `tmp/pc-controls-sanitize`: the five controls CTests passed with AddressSanitizer
  and UndefinedBehaviorSanitizer.
- Native 32-bit SDL (`./build-pc.sh`) and X11 (`--backend x11`) builds succeeded.
- `tools/pc/test_controls_backend.sh` passed using real SDL3 virtual controllers
  and the offscreen OpenGL driver: three devices, explicit third-device selection,
  buttons/sticks/triggers, removal, suppression/release gating, actual secondary
  window event dispatch/rebinding, and repeated Controls/Mods GL context checks.
- `tools/pc/test_mods_context.sh` passed.
- Native SDL headless screenshot smoke fixtures (main-menu cursor, options and
  title) matched their existing hashes; fixture hashes were not changed.
- `tools/pc/preview_controls.sh` renders review images under `tmp/pc/controls-*.ppm`
  using the actual menu text renderer: keyboard and controller tabs, the device
  list, capture, both dialogs, a disconnected controller, the minimum window
  size, menu scale 2 and the bitmap fallback font. It drives the window through
  its own key API, so the pictures survive layout changes. All were inspected.
- The window CTest also covers reading-order navigation, Delete, pointer
  highlighting, wheel clamping and the minimum-size layout; the SDL backend test
  covers a resize down to the minimum size.

The evdev test uses synthetic reads/ioctls, including `SYN_DROPPED`, range
normalization, paused polling and EOF removal. No physical controller was used
for this change. X11 native compilation and its shared UI/evdev tests are covered;
interactive X11 window-manager behavior remains a manual check.

Re-run the five controls tests with:

```sh
cmake -S . -B tmp/pc-controls -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/pc-controls
ctest --test-dir tmp/pc-controls --output-on-failure -R '^pc_controls'
tools/pc/test_controls_backend.sh
tools/pc/preview_controls.sh
```
