#ifndef MEMORIES_PC_GL_PICTURE_H
#define MEMORIES_PC_GL_PICTURE_H
#include <stdint.h>

/* The internal resolution drawn by OpenGL (SDL backend only). The software
 * GPU records what it did to VRAM (soft_gpu.h, SoftGpuRecorder); at present
 * the record is replayed into a picture of the whole of VRAM at scale x
 * scale pixels per word, kept in a framebuffer. VRAM itself stays the
 * console's. Needs a context of OpenGL 3.0 or later, current on the calling
 * thread; every call below is made with that context current. */

/* Once the window's context exists. Returns 1 when the pass is available,
 * and from then on the software GPU records for it instead of drawing its
 * own picture. 0 (GL too old, MEMORIES_GL_PICTURE=0) leaves the software
 * picture in place. */
int GlPicture_Init(void);
/* Replays what was recorded since the last call; the framebuffer's texture
 * is then the picture. Returns 0 when the pass is off or the scale is 1. */
int GlPicture_Replay(void);
/* The picture's texture (RGBA, picture_w x picture_h texels; texel row 0
 * is the top of VRAM), after Replay. */
unsigned GlPicture_Texture(int *picture_w, int *picture_h);
int GlPicture_Scale(void);
/* The record is half the arena: frames went unshown (a raised game speed)
 * and it should be replayed before it overflows, which would draw the next
 * frame from VRAM at 1x. */
int GlPicture_Behind(void);
/* Pixels x,y,w,h of the picture as 0x00RRGGBB, after a Replay. Returns 0
 * when the pass is off. */
int GlPicture_Read(int x, int y, int w, int h, uint32_t *out);
/* Widescreen: the widened picture of the display area x,y,w,h (words), when
 * the pass drew a target for it (soft_gpu.h, SoftGpu_WideFrame), after a
 * Replay: its texture, picture_w x picture_h texels, row 0 the area's top,
 * the widened area filling it across. Sides nothing drew since the last
 * call are made black, as the software GPU makes them. 0 when there is none. */
unsigned GlPicture_WideTexture(int x, int y, int w, int h, int *picture_w, int *picture_h);
/* The same picture's first h rows as 0x00RRGGBB into `out` (picture_w x
 * h * scale), nothing changed. Returns 0 when there is none. */
int GlPicture_ReadWide(int x, int y, int w, int h, uint32_t *out);
#endif
