#ifndef MEMORIES_PC_MODS_H
#define MEMORIES_PC_MODS_H
/* Optional extras under the Game > Mods window: things the port can do that
 * the console could not, kept strictly outside the game's own code paths so
 * that turning one off leaves retail behaviour exactly as it was. */

enum { MODS_FIELD_MODELS, MODS_HAND_CAMERA, MODS_COUNT };

const char *Mods_Name(int mod);
int Mods_Enabled(int mod);
/* Whether changing this mod requires a fresh game process. */
int Mods_RequiresRestart(int mod);
void Mods_SetEnabled(int mod, int enabled);

/* Called once the game's frame is on its way to the GPU (libgpu's GsDrawOt),
 * which is where an extra pass can draw over the finished picture. */
void Mods_DrawFrame(void);
/* Drop everything cached from the running game: a resumed save state is
 * another game. */
void Mods_Reset(void);
/* Hand camera (hand_camera.c): once a frame from Mods_DrawFrame. */
void HandCamera_Frame(void);
#endif
