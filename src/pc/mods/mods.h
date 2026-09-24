#ifndef MEMORIES_PC_MODS_H
#define MEMORIES_PC_MODS_H
/* The mod system: what the port itself calls. Mods are directories, not
 * built-in code -- see modapi.h for what one contains and notes/modding.md
 * for how one is written. They are found beside the executable (`mods/`,
 * where the release's own mods live) and in the player's mods directory
 * (paths.h), which is where a player installs someone else's. Whether a mod
 * is applied is a setting, `mod.<id>`, saved with the rest of them. */

#define MODS_MAX 256
#include <stddef.h>
#include "mod_types.h"

/* Find every mod and apply the ones the settings say are applied. Safe to
 * call again (settings reload): the directories are only scanned once. */
void Mods_Load(void);
/* The texture pack loader a "textures" mod goes through (src/pc/render/texture_pack.h);
 * without one, such a mod notes that this build has no texture packs. */
void Mods_SetTexturePack(int (*load)(const char *directory), void (*unload)(void));
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

/* The "cards" array of every applied mod, with the directory its images are
 * named from, in the order the mods were found, for src/pc/cards (json.h
 * reads them). A data mod needs no code for them. */
struct JsonValue;
void Mods_VisitCards(void (*visit)(const char *id, const char *directory, const struct JsonValue *cards, void *context),
                     void *context);
/* One of a mod's settings (`mod.<id>.<key>`, or MEMORIES_MOD_<ID>_<KEY> for
 * the run), as a code mod's host->setting reads it. */
int Mods_Setting(const char *id, const char *key, int fallback);
/* Say why a mod is not quite what it asked for: on stderr and beside it in
 * the Mods window. */
void Mods_Note(const char *id, const char *format, ...);

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

/* Manager metadata and configuration. Borrowed strings live until exit. */
const struct JsonValue *Mods_Manifest(int mod);
const char *Mods_Metadata(int mod, const char *key);
const char *Mods_Directory(int mod);
const char *Mods_Origin(int mod);
int Mods_Active(int mod);
int Mods_Failed(int mod);
int Mods_ConflictText(int mod, char *out, size_t size);
int Mods_OptionCount(int mod);
const struct JsonValue *Mods_Option(int mod, int option);
int Mods_OptionValue(int mod, int option);
int Mods_OptionValid(int mod, int option, int value);
int Mods_OptionSet(int mod, int option, int value);
/* Validate the whole proposed set, before saving/changing anything. */
int Mods_CheckManifest(int mod, char *error, size_t size);
int Mods_Compatible(int mod, const int *enabled, char *error, size_t size);
int Mods_Order(const int *enabled, int *order, char *error, size_t size);
int Mods_ProfileValue(const char *name, const char *key, int fallback);
int Mods_Validate(const int *enabled, char *error, size_t size);
int Mods_Apply(const int *enabled, char *error, size_t size);
int Mods_ProfileSave(const char *name);
int Mods_ProfileRead(const char *name, int *enabled);
void Mods_SetCardSignature(unsigned signature);
unsigned Mods_CardSignature(void);
void Mods_SetCardResolver(int (*resolve)(const char *));
void Mods_Dispatch(MemoriesModEvent *event);
int Mods_Notify(unsigned type, int a, int b, int c);
unsigned Mods_Sequence(int mod);
int Mods_RuntimeOption(int mod, int option);
unsigned Mods_CodeHash(int mod);
unsigned Mods_Signature(void);
int Mods_DamageLife(int side, int life, int damage, int kind);
#endif
