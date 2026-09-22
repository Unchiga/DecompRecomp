# Controls menu: design and implementation workflow

Status: implemented on 2026-09-21; see [Controls](controls-menu.md) for actual
behavior, validation and remaining hardware/manual checks. The original design
and implementation sequence follow. Based on the working tree inspected on
2026-09-21. Follow the stages in order; each stage must compile
and meet its acceptance checks before the next stage starts.

## Window redesign (2026-09-21, later the same day)

A second user pass asked for a window that is easy to understand, polished and
complete. Three decisions supersede the revision below: the PlayStation pad
picture now appears on **both** tabs (it shows destinations, not hardware, so it
serves keyboard bindings too and the keyboard list no longer stretches to fill
the window); the 16 rows are listed in reading order under group headings
(D-pad, face buttons, shoulders and triggers, stick clicks, system) while the
stored order stays the wire-bit order; and the window is resizable with a
minimum size instead of fixed. See [Controls](controls-menu.md) for the
resulting behaviour.

## User design revision (2026-09-21)

The user explicitly removed the visual keyboard from scope. Keyboard controls
used a straightforward full-width binding list (superseded above: the pad
picture now sits beside it; there is still no keyboard picture). Keep the controller diagram.
The controller dropdown selects actual connected PC devices. The diagram always
shows a PS1 controller; remove the icon-style selector and brand-specific diagrams.
Its highlights represent mapped PS1 actions, not fixed physical button positions.
Use the user-supplied controller artwork with a transparent background; it
replaces the previous code-drawn controller outline. Keep L3/R3 in the binding
list because the supplied digital controller has no sticks.
Menu navigation uses mouse and keyboard only. Controller input is used only for
rebinding and live previews within this window.

This revision supersedes references to icon styles, brand-specific diagrams and
keyboard diagrams in the original design
below, including the preview and acceptance requirements.

## Intended result

Add **Game > Controls…** to the PC port. Open a separate native Controls window,
using the existing menu palette, text renderer, and scaling. Players can change
keyboard and controller bindings, choose which connected controller drives each
PlayStation port, and see diagrams highlight the physical input and resulting
PlayStation button. Changes persist across launches and apply without restarting.

Defaults preserve current gameplay input. Keyboard and mouse belong to Player 1;
controller assignment supports Player 1 and Player 2. “Choose controller” means
choosing an actual connected device. A separate icon-style choice changes only
its visual labels.

This work belongs in `src/pc/`; leave retail game logic, matching sources, guest
pad protocols, and save-state formats unchanged. Mouse rebinding, macros, chords,
rumble, and analog PS1 output are outside this first implementation. Existing
mouse mappings remain functional when the Controls window is closed.

## Read these files first

| File | Relevant existing behavior |
| --- | --- |
| `src/pc/platform/menu.c`, `menu.h` | Game menu, action dispatch, `MenuCanvas`, text and menu scale |
| `src/pc/platform/mods_window.c`, `mods_window.h` | Shared native-window UI and shared drawing/hit-test layout |
| `src/pc/platform/sdl.c` | SDL3 keyboard map, controller discovery/polling, native windows, focus and shortcuts |
| `src/pc/platform/x11.c` | Legacy keyboard map, native-window event routing, cached pad output |
| `src/pc/platform/gamepad_evdev.c` | X11 controller backend, fixed two-device assignment, axes and hotplug |
| `src/pc/platform/platform.h`, `platform_common.c` | Signal-safe pad contract, paused event pump, scripted input |
| `src/pc/platform/settings.c`, `settings.h` | Integer settings, file override, error handling |
| `tools/pc/build_game32.py` | Backend selection; shared platform `.c` files are discovered by glob |
| `CMakeLists.txt`, `tests/pc/mods_window_test.c` | Standalone host test registration and UI stubs |
| `tests/pc/mods_context_test.c`, `tools/pc/test_mods_context.sh` | SDL secondary-window OpenGL regression harness |
| `notes/mods-window.md`, `notes/pc-build.md` | Window conventions, native builds and current input defaults |

The inspected workspace already contains uncommitted Mods-window and settings
changes. Preserve them. Re-read the working files before editing; do not restore
them from HEAD. Some prose in `notes/pc-build.md` describes only evdev, whereas
the SDL backend implements its own SDL3 controllers. Source is authoritative.

## Screen and interaction design

Logical starting size: 900 × 650 at UI scale 1. Clamp to the usable desktop,
provide scrolling for mappings, and keep footer actions visible. Scale drawing
and hit-testing together, as the Mods window does. One Controls window at a time;
opening again raises the existing window.

```text
+-----------------------------------------------------------------------+
| CONTROLS                                                              |
| Player [1 v]    Controller [Xbox Wireless Controller · USB · #1     v] |
| Status: Connected       Icons [Automatic v]                           |
| [Keyboard] [Controller]                                               |
|                                                                       |
| +------------------------------+ +----------------------------------+ |
| | Physical keyboard/controller | | PlayStation button | Binding     | |
| | diagram                      | | Up                 | Up arrow    | |
| |                              | | Down               | Down arrow  | |
| | Selected control outlined    | | ...                               | |
| | Held control filled          | | Cross              | X           | |
| |                              | | Circle             | S           | |
| | PS1 output indicator strip   | | ...                               | |
| +------------------------------+ +----------------------------------+ |
| Selected: Cross     [Rebind] [Clear]                                  |
| Press a key to bind Cross. Escape cancels.                             |
| [Restore tab defaults]                  [Cancel] [Apply] [OK]          |
+-----------------------------------------------------------------------+
```

- Player selector changes the edited port. Keyboard tab is disabled for Player 2
  with “Keyboard controls Player 1.” Keep its controller mappings editable.
- Device dropdown lists **Automatic**, **None**, every discovered supported pad,
  and a saved explicit selection even when disconnected. Discover more than two
  devices; a third device must be selectable. Show names, transport when known,
  and a session suffix for identical names. Show “Assigned to Player 2” etc.
- Automatic is the default: preserve a connected assignment; fill free automatic
  ports in discovery order, reserving explicitly selected devices first. An
  explicit disconnected selection stays disconnected instead of silently taking
  another pad. None disables only that port's controller. Keyboard still works
  on Player 1. One device cannot occupy both ports; reject a conflicting choice
  with a message explaining which port must release it.
- Icon options: Automatic, Xbox, PlayStation, Nintendo, Generic. Store per
  controller profile. Automatic uses reliable backend metadata, otherwise Generic.
  Nintendo lettering must reflect physical positions. Changing style never
  changes bindings. The destination labels always use PlayStation names.
- Each table row is a PS1 button. Keyboard has one binding per row; controller
  has two slots per row so D-pad and left-stick defaults can coexist. Blank
  slots show “Unbound.” Clicking a slot selects it; double-clicking a slot or PS1
  diagram button starts the 10-second capture timer. Rebind or Enter also starts capture.
- Diagram clicks select the row/slot using that physical input; an unmapped
  physical control shows its name without changing a binding. Table selection
  highlights its physical binding. Text labels accompany colors and shapes.
- Keyboard diagram includes a full standard key layout (navigation and numpad
  groups included). Label the captured key even if the diagram lacks its region.
  Controller diagram includes face buttons, shoulders, triggers, D-pad, sticks,
  stick clicks, Start and Select. Extra buttons appear as labeled auxiliary
  indicators. Draw shapes in code using `MenuCanvas`; no image downloads or new
  GUI toolkit. Draw PS1 symbols geometrically: the current text renderer is ASCII.
- Preview shows raw pressed inputs plus destination PS1 buttons computed from
  the draft bindings. It must not drive the running game.
- Tab/Shift+Tab move focus; arrows move within lists/dropdowns; Enter activates;
  Escape cancels capture, otherwise closes through the dirty-state flow. Mouse
  can reach every operation. Controllers do not navigate this menu. Their
  buttons remain available for rebinding and live previews; use Escape or mouse
  Cancel to cancel capture.
- Apply saves and activates the draft, keeping the window open. OK does the same
  then closes. Cancel discards edits since the last Apply. Closing a dirty window
  offers Apply / Discard / Keep editing. Restore defaults affects only the current
  tab/profile draft and asks for confirmation. Clear affects the selected slot.
- A failed save leaves the old active configuration and file intact; keep the
  draft and show a retryable error. No restart is required.

## Binding and device rules

Use the following destination bit values and defaults, shared by both backends:

| PS1 button | Bit | Keyboard | Controller source |
| --- | --- | --- | --- |
| Select | `0x0001` | Right Shift | Back / View |
| L3 | `0x0002` | T | Left stick click |
| R3 | `0x0004` | Y | Right stick click |
| Start | `0x0008` | Enter | Start / Menu |
| Up | `0x0010` | Up | D-pad up; left stick negative Y |
| Right | `0x0020` | Right | D-pad right; left stick positive X |
| Down | `0x0040` | Down | D-pad down; left stick positive Y |
| Left | `0x0080` | Left | D-pad left; left stick negative X |
| L2 | `0x0100` | E | Left trigger |
| R2 | `0x0200` | R | Right trigger |
| L1 | `0x0400` | Q | Left shoulder |
| R1 | `0x0800` | W | Right shoulder |
| Triangle | `0x1000` | A | North face button |
| Circle | `0x2000` | S | East face button |
| Cross | `0x4000` | X | South face button |
| Square | `0x8000` | Z | West face button |

Implement explicit source kinds: unbound, keyboard key, controller button,
signed stick axis, trigger, and hat direction. Normalize backend inputs into
project-owned tokens. Prefer stable physical keyboard positions; SDL scancodes
and X11 XKB physical key names need adapters to the same tokens. Never serialize
raw SDL enum numbers, X keycodes, device-array indices, or `/dev/input/eventN`.
Display the current layout's key name; unsupported keys get a clear error.

Preserve existing axis activation thresholds initially: SDL sticks and triggers
at one third, evdev sticks at half and triggers at one third of their ranges.
Normalize asymmetric/signed ranges correctly. Add a lower release threshold
(80% of activation threshold) for hysteresis. Capture requires neutral first,
then a fresh threshold crossing; drifting/held controls cannot bind on entry.
Rebinding a direction replaces that slot, with no hidden hard-coded stick mapping
left active. Right-stick directions may also be bound to digital destinations.

Audit actual shortcuts in both backends. Reserve the union of existing host
shortcuts (Escape, Tab, F1–F5, F7, F10–F12, P, M, period) and modifier keys that
would make system/menu shortcuts ambiguous, except Right Shift's existing Select
binding. Reject modifier chords; Alt+Enter retains fullscreen behavior outside
the Controls window. Centralize the reserved-key policy; explain rejection in
the window. Do not accidentally reserve normal Enter or navigation arrows.

Binding conflicts are local to a keyboard profile or a controller profile, not
across devices. If a source is already bound, offer **Move binding** or **Cancel**.
Move clears its previous slot; no silent overwrite or one-source/many-action
mapping. Multiple distinct sources may activate the same destination. Compute
output from the complete held-source state so releasing one source does not
release a destination another source is still holding.

Device identity uses available GUID/vendor/product plus serial or stable physical
path, with a separate transient session ID. A GUID alone does not distinguish
two identical pads. Persist profiles and explicit selections by a bounded,
escaped stable key. When two devices cannot be distinguished after restart,
show “Select device again” and retain profiles; never claim persistent identity
from a discovery-order suffix. Transport changes may require selection again.

## Architecture to implement

Keep rendering, configuration, and device I/O separate. These are proposed new
modules, not APIs that already exist:

| Module | Responsibility |
| --- | --- |
| `controls.h`, `controls.c` | PS1 action table, normalized sources, defaults, profiles, draft validation, mapping evaluation, capture state |
| `controls_config.h`, `controls_config.c` | Versioned load/save with atomic replacement and bounded parsing |
| `controls_window.h`, `controls_window.c` | Layout, drawing, focus, edits, conflict/dirty prompts, preview |
| `sdl.c` | SDL normalized keyboard/device adapter, Controls native window and event routing |
| `x11.c`, `gamepad_evdev.c` | XKB/evdev adapters, Controls native window and device enumeration |
| `platform.h`, `menu.c` | `Platform_OpenControls()` entry and Game menu action |

Use backend-independent structs for `ControlSource`, `ControllerDevice`,
`ControllerSnapshot`, `ControlsConfig`, and `ControlsDraft`. Keep all allocation,
device operations, mapping, and persistence on the main thread. Draft owns a
copy; it must not share mutable mapping arrays with active config.

Keep `Platform_Pad`, `Gamepad_Bits`, and connection reads signal-safe cached-word
reads as required by `platform.h`. Publish output and assignment transitions
under the existing signal-blocking discipline. Never traverse a device list or
call SDL, file I/O, allocation, or UI code from a pad getter.

Continue OR-ing keyboard, mouse, controller and `MEMORIES_INPUT` for Player 1
outside the Controls window. While it is open, suppress live gameplay input on
both ports, including mouse and polled controllers. Scripted input remains
unchanged for automation. Keep raw preview input separate. Clear held output on
open, close, focus loss, profile apply, device switch and disconnect. Require
release before live input can reactivate after closing; closing with Enter or a
face button must not activate the game. Handle source releases even when a menu
consumes their corresponding events.

Poll devices, hotplug, capture, and preview from the idle event pump as well as
normal frames. Use monotonic time for discovery cadence and a 10-second capture
timeout, not game-frame count. This must work at speed 0 and during focus-loss
pause; avoid processing the same edge twice. Game-window focus handlers must not
overwrite the stored clock rate repeatedly when switching among auxiliary windows.

Capture state machine:

```text
Idle -> WaitForNeutral -> Listening -> candidate -> conflict? -> DraftUpdated
                          |                         |
                          +-> Escape/timeout        +-> Move / Cancel
                              /focus loss/disconnect -> Idle (old slot retained)
```

Ignore repeats and the activation event. WaitForNeutral also times out. Accept
controller input only from the selected device; disconnect cancels capture.
When Automatic resolves to no device or None is selected, controller capture is
disabled with an explanation. An explicit disconnected profile stays editable
for clearing/defaults, but capture requires reconnection.

Use a separate versioned `controls.txt` rather than stretching integer-only
`Settings_*` to strings. Default is `saves/controls.txt`; `MEMORIES_CONTROLS` sets
an explicit path. Otherwise place it beside `MEMORIES_SETTINGS` when that path
is set, so isolated settings runs do not touch the user's default controls.
Document the actual grammar in stage 2 before writing the parser. Use readable
tokens such as `key.x`, `button.south`, `axis.left_y.negative`, and `unbound`.
Store keyboard mappings, per-device controller mappings/icon style, and port
selection modes/identities. Bound file size, profile count, identity length and
token length. Missing file uses defaults; malformed supported-version config
falls back as a whole with a diagnostic. Unknown future versions are not
overwritten by Apply: show an error. Write a temporary file in the destination
directory, check write/flush/close, then rename; failure preserves active config.
Do not store controls in guest saves or save states.

## Ordered implementation tasks

Each task ends with a brief report: files changed, checks run and results,
remaining failures, and the next task. Keep a progress checklist in the handoff
when sessions change. Do not replace unimplemented behavior with success stubs.

### 0. Record baseline

Read the files above and run `git status --short`. Record existing edits and
available SDL/X11 build prerequisites. Run existing `pc_settings` and
`pc_mods_window` tests. Record pre-existing failures separately.

Done when the implementer can identify both backend event pumps, controller
polls, pad getters, menu dispatch, and native build source discovery.

### 1. Shared mapping model

Create `controls.h/.c`, defaults, normalized source tokens, held-state evaluator,
reserved-key policy, validation, and capture state machine. Use synthetic device
snapshots; leave backend maps connected until adapter stages are ready. Add
`tests/pc/controls_test.c` and CTest `pc_controls`.

Done when tests verify all 16 default bits, two sources holding one destination,
clear/move conflicts, reserved keys, neutral gate, repeat rejection, axis signs,
trigger ranges, hysteresis, timeout, and cancellation retaining the old binding.

### 2. Persistence and draft transaction

Create `controls_config.h/.c`. Write the version-1 grammar and a sample file in
`notes/controls-menu.md`. Implement defaults, strict bounded load, round-trip
save, active/draft separation, and save-then-publish behavior. Add CTest
`pc_controls_config`; use temporary directories and explicit paths in tests.

Done when restart round-trips mappings, selections and icon style; missing/bad
files behave as specified; unsupported versions are protected; write/rename
failures preserve old bytes and active state; Cancel never persists edits.

### 3. SDL3 device registry and mapping adapter

Replace the two-entry discovery model with a registry of all supported connected
pads, separate from two port assignments. Reuse the vendored SDL3 headers and
APIs already in use; do not copy SDL2 examples. Normalize keyboard, buttons and
axes; use the shared evaluator instead of the fixed maps. Implement identity,
automatic/explicit/None selection, duplicate-port rejection, disconnect clearing
and periodic reconciliation while paused. Preserve `MEMORIES_NO_GAMEPAD`.

Done when synthetic adapter tests select the third of three pads, keep two ports
independent, reconnect an explicit device, reject duplicate assignments, explain
ambiguous identity, preserve keyboard with None, and retain scripted input.
Build the SDL native executable before moving on.

### 4. Shared Controls window and diagrams

Create `controls_window.h/.c`, using backend stubs for devices and persistence in
tests. Implement the screen, all slots, scrolling, diagrams, fixed navigation,
preview, capture messages, conflict prompt and Apply/Cancel/OK flows. Extend
menu navigation events with Tab/Shift+Tab if needed; raw capture events must use
their own normalized type because `MenuKey` collapses most keys to OTHER.

Add CTest `pc_controls_window` and a display-free preview harness that emits
PPM images with deterministic fake pads and input. Use one layout definition
for drawing and hit-testing. Cover scale 1 and 2 and a small usable desktop.

Done when every action is reachable with mouse and keyboard, diagrams correspond
to the selected bindings, all icon styles keep physical positions correct, long
device names clip safely, and the preview harness renders keyboard, controller,
capture, conflict, no-device and disconnected states. Inspect the actual images.

### 5. SDL window integration and input ownership

Add Game > Controls and `Platform_OpenControls`. Route secondary-window events
before gameplay shortcuts; process global hotplug even when a secondary window
is focused. Capture raw input before translating it into menu navigation.
Integrate polling into paused pumps and suppress gameplay while editing.
Reuse the Mods window's resource cleanup and explicitly restore the game OpenGL
context after creating, drawing or destroying a secondary renderer.

Extend the real SDL context harness for Controls. Test opening Mods and Controls
together, closing each, and returning to gameplay. Closing Controls must not
quit the app. Add missing stubs to existing tests that link menu/platform APIs.

Done when capture cannot save/load state, quit, pause, change speed or toggle
fullscreen; held controls cannot leak to gameplay; preview/hotplug work while
paused; and game rendering survives repeated window creation and destruction.

### 6. X11 and evdev parity

Add X11 Controls window ownership and XKB key normalization. Refactor evdev
discovery into the same registry/assignment model and normalize raw events.
Retain each backend's initial thresholds. Handle removal/EOF, permissions,
held-state initialization, and `SYN_DROPPED` by resynchronizing state rather
than leaving stuck buttons. Use monotonic rescanning even with both ports full.

Done when X11 builds and supports the same menu, selection, capture and storage
flows; portable bindings load between backends; inaccessible devices show a
useful status; and synthetic evdev disconnect/resynchronization tests pass.
Do not declare complete with an SDL-only implementation.

### 7. Acceptance and player documentation

Run the commands below. Complete the manual matrix, update
`notes/controls-menu.md` with actual behavior/format and `notes/pc-build.md` with
the entry point and defaults. Note which physical controllers were tested and
which checks used synthetic devices. Capture final UI images for review.

## Verification commands

Run from the repository root. These are handoff commands, not claims that the
new targets already exist. The CMake host suite does not build the native game.

```sh
cmake -S . -B tmp/pc-controls -DCMAKE_BUILD_TYPE=Debug
cmake --build tmp/pc-controls
ctest --test-dir tmp/pc-controls --output-on-failure -R '^pc_(controls|controls_config|controls_window|settings|mods_window)$'

python3 tools/pc/build_game32.py --backend sdl --build tmp/pc/controls-sdl
python3 tools/pc/build_game32.py --backend x11 --build tmp/pc/controls-x11
tools/pc/test_mods_context.sh
```

Add the new adapter/context tests to CMake or a documented harness command in
their implementation stage and run them too. Native builds require the existing
32-bit dependencies and game artifacts described in `notes/pc-build.md`. Report
missing prerequisites explicitly; never turn a skipped build into a passing one.

| Scenario | Required result |
| --- | --- |
| Fresh config | Existing keyboard/controller mappings still play correctly |
| Rebind Cross on keyboard and controller | New inputs activate Cross; replaced inputs do not |
| Two held sources, release one | Destination stays held until both are released |
| D-pad plus left-stick slots | Either works; replacing one leaves the other intact |
| Three controllers connected | Any device can be assigned to either port |
| Explicit selection unplug/replug | Immediate neutral output; profile retained; correct reconnect |
| Identical pad names, reversed enumeration | Distinguishable by stable identity or explicit reselection |
| None / no devices / disabled gamepad support | Keyboard works; menu explains controller state |
| Capture input already held or drifting | No immediate accidental binding |
| Capture reserved shortcut or input on another device | No host action and no incorrect assignment |
| Open while a gameplay key is held; release in menu | No stuck input on return |
| Pause or focus-loss pause | Capture, timeout, preview and hotplug remain responsive |
| Apply, Cancel, restart, save failure | Correct draft/active/file separation |
| Dirty close and restoring defaults | Explicit choice; no silent loss of edits |
| Switch icon styles | Labels change; physical mapping and PS1 output stay fixed |
| No font available, small window, long names | Readable fallback; reachable footer and rows |
| Repeated Controls/Mods open/draw/close | No broken game GL context or leaked native resources |
| Headless scripted run | Existing scripted pad behavior remains intact |

## Copy/paste instruction for the implementing model

> Implement `notes/controls-menu-workflow.md` in order. Start by reading the
> working tree and its referenced source files. Preserve existing uncommitted
> changes. This document specifies a native C Controls window, keyboard and
> controller rebinding with diagrams, actual device selection for two ports,
> persistence, and SDL3/X11 parity. Complete each stage's checks before advancing.
> Keep changes scoped to the PC platform, tests and documentation. Use shared
> mapping logic and synthetic test inputs; do not alter guest gameplay or
> save-state formats. Keep a completed-stage checklist and report exact test
> results and blockers. Do not call the feature complete until stage 7 passes;
> label unavailable hardware checks honestly.

## Implementation checklist (2026-09-21)

- [x] 0: Baseline recorded; existing settings/Mods tests passed.
- [x] 1: Shared mapping model repaired and verified.
- [x] 2: Versioned persistence and draft transaction.
- [x] 3: SDL device registry and adapter, including third-device selection.
- [x] 4: Shared Controls UI and code-drawn diagrams.
- [x] 5: SDL window integration, event routing and GL regression checks.
- [x] 6: X11 integration and evdev adapter; build and synthetic adapter tests.
- [x] 7: Automated acceptance, native screenshot smoke checks and documentation.
- [ ] Physical controller/transport checks and interactive X11 WM acceptance.

Implementation detail: anonymous devices use a session identity so they can be
chosen immediately but must be selected again after restart. Binding file tokens
use the documented canonical names in `controls-menu.md`. Input and display
logic fit compact windows by reducing UI scale and hiding the diagram when
necessary; mappings and footer actions remain available.
