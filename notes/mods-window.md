# Mods window

Game > Mods opens a separate window. Available mods are on the left; applied
mods are on the right, with apply/remove arrow buttons in the gutter between
them. Select a row and press an arrow, or drag a row into the other list (the
target list gets an accent outline and a label follows the pointer). Rows that
need a restart carry a dim "restart" tag. Up/Down walk both lists in the order
shown, Right applies and Left removes the selected mod, Enter toggles it, and
Escape closes the window (or cancels a pending restart warning). The footer
holds a status line and Close; while a restart warning is pending it shows the
warning in amber with Cancel and Apply and restart instead.

Layout lives in layout() in src/pc/platform/mods_window.c and is shared by
drawing and hit-testing; everything scales with Menu_Scale and uses the menu
bar's palette. The window is fixed-size: it is as wide as the lists or the
longest footer message row, whichever is wider. To preview the drawing without
the game, tmp/modsprev/harness.c links menu.c with stubs and writes a PPM.

The window lists whatever the mod system found: every directory with a
`mod.json` in `mods/` beside the executable and in the player's user
directory (`notes/modding.md`). A mod that failed to load carries an "error"
tag instead of "restart"; selecting it and trying to apply it puts the reason
in the footer, and removing it still works.

Changes are saved to the normal settings file, as `mod.<id>`. The 3D Monsters
and Hand camera mods apply immediately. Mods_RequiresRestart identifies mods
that need a fresh process, which is what the manifest's `restart` says and
what a mod with data overrides gets by default. Such changes first show an unsaved-progress warning;
Apply and restart saves settings and relaunches the executable with its original
arguments. Cancel leaves the mod unchanged. Restart does not auto-load a state
specified by MEMORIES_LOAD_STATE. A failed settings write cancels the change;
a failed relaunch displays an error and leaves the saved choice for next launch.

The shared drawing/input code is in src/pc/platform/mods_window.c; SDL and
X11 own their respective native windows and route their events separately
from game input. Nothing here knows which mods exist: the window asks
Mods_Count, Mods_Name, Mods_Status, Mods_Enabled and Mods_RequiresRestart,
and applies a change with Mods_SetEnabled followed by Settings_Save.

Run the pc_mods_window and pc_settings CTest tests for interaction, restart
confirmation/cancellation, and settings persistence coverage. Restart is stubbed
in the interaction test so it can verify requests without replacing the test process.

Run `tools/pc/test_mods_context.sh` after building the local 32-bit SDL library
to exercise real OpenGL context ownership across repeated mods-window creation,
redraw, and destruction. It uses SDL's offscreen driver by default. The game
context and its texture-upload stride must survive every secondary-window
operation; leaving SDL_Render's context current can make the next game upload
read beyond the source frame buffer.
