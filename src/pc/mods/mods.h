#ifndef MEMORIES_PC_MODS_H
#define MEMORIES_PC_MODS_H
/* The mod system: what the port itself calls. Mods are directories, not
 * built-in code -- see modapi.h for what one contains and notes/modding.md
 * for how one is written. They are found beside the executable (`mods/`,
 * where the release's own mods live) and in the player's mods directory
 * (paths.h), which is where a player installs someone else's. Whether a mod
 * is applied is a setting, `mod.<id>`, saved with the rest of them. */

#define MODS_MAX 64

/* Find every mod and apply the ones the settings say are applied. Safe to
 * call again (settings reload): the directories are only scanned once. */
void Mods_Load(void);
void Mods_Shutdown(void);

int Mods_Count(void);
const char *Mods_Id(int mod);
const char *Mods_Name(int mod);
/* Empty while all is well, else why this mod did not load. */
const char *Mods_Status(int mod);
/* Whether the player has this mod applied. A mod that wants a restart reads
 * back as applied as soon as the player applies it, because that is what the
 * settings now say; it is the next launch that puts it in place. */
int Mods_Enabled(int mod);
/* Whether changing this mod requires a fresh game process. */
int Mods_RequiresRestart(int mod);
/* Apply or remove a mod: loads its code the first time it is applied,
 * turns its data overrides on or off, and records the setting. The caller
 * saves the settings (the mods window reverts the change if that fails). */
void Mods_SetEnabled(int mod, int enabled);

/* Called once the game's frame is on its way to the GPU (libgpu's GsDrawOt),
 * which is where an extra pass can draw over the finished picture. */
void Mods_DrawFrame(void);
/* Drop everything cached from the running game: a resumed save state is
 * another game. */
void Mods_Reset(void);

/* Data overrides, from the drive model (libds.c): replace the 2048 bytes of
 * user data a sector delivers. Interrupt context, so this reads its tables
 * and copies; nothing is allocated or opened here. Returns nonzero when the
 * sector was changed. */
int Mods_DiscSector(int lba, void *user_data);

#endif
