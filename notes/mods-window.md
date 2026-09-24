# Mod manager

**Game > Mods** opens a resizable library. Search names, IDs or authors; cycle
All / Enabled / Disabled / Issues to filter the list. The list and the detail
pane scroll independently. The window fits the desktop, and adapts its local
text scale when resized. SDL and X11 use the same drawing and input code.

Select a mod to see its description, author, version, source directory, current
runtime status and native-code/content classification. The Settings tab renders
the mod's declared integer sliders, toggles, choices and pad-button bindings.
Restore defaults resets the selected mod's staged settings. Compatibility lists
requirements, ordering constraints, declared conflicts and potential data or
texture overlap. Lower Order values load first, subject to dependencies.

Changes are staged until **Apply changes**. Restart-only changes share one
confirmation; restarting discards unsaved game progress. Close/Escape asks before
discarding staged edits; the title-bar close button asks once, and a second
click discards. Only settings edited in the window are written; an untouched
mod's order keeps following its manifest `priority`. A failed restart keeps the saved preferences for the
next launch. Active, inactive, pending, warning and error states are distinct.
Changing a load-order value requires a restart. New directories are discovered
on the next launch; native objects stay resident until exit.
**Open mods folder** shows the folder new mods go in (`MEMORIES_MODS_DIR`, else
the user `mods` folder, created if missing) in the system file manager.

The profile field accepts a name (letters, digits, spaces, `_`, `-`). Save writes
the currently applied preferences to `mod-profiles/<name>.txt` in the user
directory. Load stages that profile for inspection; it does not change the
running game. Profiles include declared option values and load-order settings.
Apply edits before saving a profile. Profiles do not copy the mods themselves.

Keyboard: Up/Down select a mod; Left/Right disable/enable; Enter toggles; Tab
focuses search, then the profile name, then the list. Escape leaves text entry,
cancels a confirmation, or closes the window. The mouse wheel scrolls whichever
pane is under the pointer; integer sliders support dragging.

Implementation: `src/pc/platform/mods_window.c`, `src/pc/mods/manager.c`, and
window ownership in `sdl.c` / `x11.c`. `pc_mods_window` exercises real settings
and manifests with a fake renderer/restart. `tools/pc/test_mods_context.sh`
checks actual SDL/OpenGL context ownership during secondary-window operations.
