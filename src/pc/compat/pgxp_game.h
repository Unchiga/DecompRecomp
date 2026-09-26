#ifndef MEMORIES_PC_PGXP_GAME_H
#define MEMORIES_PC_PGXP_GAME_H
/* PGXP (pgxp.h): addPrim also tags the words of the primitive being added
 * that hold vertices the unit stored with gte_stsxy (Pgxp_AddPrim).
 * build_game32.py puts this before every game unit (-include), so the
 * unit's own #include of psyq/libgpu.h finds it done. Not in a header that
 * mods include: a mod's object, debug info and all, is part of what a save
 * state is checked against. */
#include "psyq/libgte.h" /* libgpu.h uses its types */
#include "psyq/libgpu.h"

void Pgxp_AddPrim(const void *packet);
#undef addPrim
#define addPrim(ot, p) setaddr(p, getaddr(ot)), setaddr(ot, p), Pgxp_AddPrim(p)
#endif
