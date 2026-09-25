#ifndef MEMORIES_PC_RENDER_PRESENT_PASS_H
#define MEMORIES_PC_RENDER_PRESENT_PASS_H
/* The present pass: shader effects on the game picture as the window shows
 * it (the OpenGL presenter in sdl.c), under the menu and the HUD. With every
 * effect at its default the pass is not used at all, so the picture is drawn
 * exactly as before by the fixed-function quad. present_pass.c lists the
 * effects. */

/* Whether any effect is on (the settings are read every frame). Called
 * for every present, so that the flash reduction knows when it was off. */
int PresentPass_Wanted(void);
/* Around the picture's quad: Begin binds the effect program for a picture
 * source_h texels high that the quad samples from the OpenGL texture
 * between coordinates (s0, t0) and (s1, t1), bound on the active unit or
 * not yet; End goes back to fixed function. Begin returns 0 (and End is not
 * needed) when the program cannot be built; the quad then draws plainly. */
int PresentPass_Begin(unsigned texture, int source_h, float s0, float t0, float s1, float t1);
void PresentPass_End(void);
#endif
