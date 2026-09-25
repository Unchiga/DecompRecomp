#ifndef MEMORIES_PC_PGXP_H
#define MEMORIES_PC_PGXP_H
/* PGXP, precise geometry (Video > Precise geometry, `pgxp`). The GTE's
 * perspective transform rounds a vertex to a whole console pixel and keeps
 * no depth with it, which makes polygons wobble as they move and their
 * textures bend. rtp() (gte.c) keeps where each vertex it projects really
 * falls on the screen and its depth beside its SXY entry
 * (Memories_GtePrecise).
 *
 * The game's model drawing (the HMD polygon drivers,
 * src/pc/overrides/model_polygon_drivers.c: the duel field and every
 * monster) is ours, so it hands those values on where it writes each vertex
 * word into a packet, keyed by that word's address (Pgxp_StoreAt). DrawOTag
 * (libgpu.c) collects every word of the frame with its address, and the
 * words that are a vertex written so carry the precise values on to the
 * OpenGL picture (soft_gpu.h, gl_picture.c), which draws polygons with them.
 *
 * Drawing we do not follow is looked up by value instead: rtp() also
 * records each vertex keyed by its screen word (x | y << 16), and a frame
 * word equal to one is taken as that vertex (Pgxp_Find). A word two
 * vertices of a frame round to with different precise values is left as it
 * is. Nothing the game or the software GPU sees changes. */
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
/* `word` was written at `address` (physical, as packets link) as a vertex
 * whose precise x, y and depth are `xyw`; NULL when it has none, which
 * drops what was kept for the address. */
void Pgxp_StoreAt(uint32_t address, uint32_t word, const float *xyw);
/* 1 when `word` at `address` was stored this frame or the last with precise
 * values, -1 when stored without (it stays at whole pixels: not to be
 * looked up by value either), 0 when not stored. */
int Pgxp_FindAt(uint32_t address, uint32_t word, float *x, float *y, float *w);
/* The game's own code storing an SXY entry (gte_stsxy), with its precise
 * values or NULL. The next addPrim tags the primitive's words that hold
 * them (Pgxp_AddPrim, from the game units' addPrim: pgxp_game.h). */
void Pgxp_Stored(uint32_t word, const float *xyw);
void Pgxp_AddPrim(const void *packet);
/* The projections so far were collected: those of the next frame follow. */
void Pgxp_NextFrame(void);

/* Declared here rather than in gte.h and packets.h, which mods include: a
 * mod's object is part of what a save state is checked against. */
#include "pc/render/packets.h"
/* The precise screen position and depth of SXY0-2 (`slot` 0-2) when an
 * RTPS/RTPT put that entry there with PGXP on; 0 when not (gte.c). */
int Memories_GtePrecise(unsigned slot, float *x, float *y, float *w);
/* Memories_GpuCollect, and each word's physical address into `addresses`
 * (packets.c). */
MemoriesGpuResult Memories_GpuCollectAt(MemoriesMemory *memory, uint32_t head, uint32_t *words, uint32_t *addresses,
                                        size_t capacity, size_t hop_limit, size_t *count);
#endif
