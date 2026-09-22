#ifndef MEMORIES_PC_PATHS_H
#define MEMORIES_PC_PATHS_H
/* Where the port's files live. Two roots:
 *
 *  - the program directory, which holds the executable and the mods shipped
 *    with the release; nothing is ever written there;
 *  - the user directory, which holds everything the player owns: settings,
 *    controls, memory cards, save states, screenshots, their own mods and
 *    whatever a mod stores. On Windows that is
 *    Documents\My Games\YFM Re-Decomp; elsewhere $XDG_DATA_HOME/YFM Re-Decomp
 *    (~/.local/share/YFM Re-Decomp). MEMORIES_USER_DIR names another.
 */
#include <stddef.h>

const char *Paths_UserDir(void);
const char *Paths_ProgramDir(void);
/* Join a relative path onto a root, creating the directories above it in the
 * user root's case. Both return 0 on success, -1 if it would not fit. */
int Paths_User(char *out, size_t size, const char *relative);
int Paths_Program(char *out, size_t size, const char *relative);
/* Create a directory and every directory above it. 0 on success. */
int Paths_MakeDirs(const char *path);
/* Nonzero when a relative path stays inside its directory: not empty, not
 * absolute, no "." or ".." component, no backslashes or drive letters. What
 * a mod is allowed to name (Mods_OpenAsset, Mods_OpenData). */
int Paths_Contained(const char *relative);
/* Carry what older builds left in ./saves into the user directory. Copies a
 * file only when the destination is missing, so it is safe to call always. */
void Paths_MigrateLegacySaves(void);

#endif
