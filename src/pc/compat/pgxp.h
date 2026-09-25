#ifndef MEMORIES_PC_PGXP_H
#define MEMORIES_PC_PGXP_H
/* PGXP, precise geometry (Video > Precise geometry, `pgxp`). The GTE's
 * perspective transform rounds a vertex to a whole console pixel and keeps
 * no depth with it, which makes polygons wobble as they move and their
 * textures bend. rtp() (gte.c) records, for each vertex it projects, where
 * it really falls on the screen and its depth, keyed by the screen word the
 * game stores for it (x | y << 16). The game copies that word about by many
 * roads before it reaches a packet, so the key is the value, not an address.
 * DrawOTag (libgpu.c) looks every word of the frame it collects up, and the
 * words that are a projected vertex carry the precise values on to the
 * OpenGL picture (soft_gpu.h, gl_picture.c), which draws polygons with them.
 * A word two vertices of a frame round to with different precise values is
 * left as it is. Nothing the game or the software GPU sees changes. */
#include <stdint.h>

/* Set by DrawOTag, from the setting: rtp() records while it is nonzero. */
extern int Pgxp_Active;

/* A frame word that is a projected vertex: where it is in the frame, and
 * its precise screen position and depth. */
typedef struct PgxpVertex {
    uint32_t index;
    float x, y, w;
} PgxpVertex;

void Pgxp_Project(uint32_t word, double x, double y, double w);
/* Returns 0 when the word is no vertex projected this frame or the last,
 * or is two of them. */
int Pgxp_Find(uint32_t word, float *x, float *y, float *w);
/* The projections so far were collected: those of the next frame follow. */
void Pgxp_NextFrame(void);
#endif
