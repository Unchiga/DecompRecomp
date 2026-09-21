#ifndef MEMORIES_PC_INLINE_C_NATIVE_H
#define MEMORIES_PC_INLINE_C_NATIVE_H
/* Native replacements for the DMPSX inline-assembly macros in psyq/inline_c.h.
 * Add a macro here when a game unit needs it; an absent macro fails the build
 * instead of silently doing nothing. Command words are the originals. */
#include "gte.h"

#define gte_ldv0(r0) (Memories_GteLoad(0, (r0)), Memories_GteLoad(1, (const char *)(r0) + 4))
#define gte_ldv1(r0) (Memories_GteLoad(2, (r0)), Memories_GteLoad(3, (const char *)(r0) + 4))
#define gte_ldv2(r0) (Memories_GteLoad(4, (r0)), Memories_GteLoad(5, (const char *)(r0) + 4))
#define gte_ldrgb(r0) Memories_GteLoad(6, (r0))
/* The original writes SXY0, SXY2, then SXY1; these are plain registers. */
#define gte_ldsxy3(r0, r1, r2) (Memories_GteWriteData(12, (uint32_t)(r0)), \
    Memories_GteWriteData(14, (uint32_t)(r2)), Memories_GteWriteData(13, (uint32_t)(r1)))

#define gte_rtps() ((void)Memories_GteCommand(0x0180001))
#define gte_rtpt() ((void)Memories_GteCommand(0x0280030))
#define gte_nclip() ((void)Memories_GteCommand(0x1400006))
#define gte_avsz3() ((void)Memories_GteCommand(0x158002d))
#define gte_avsz4() ((void)Memories_GteCommand(0x168002e))
#define gte_ncds() ((void)Memories_GteCommand(0x0e80413))
#define gte_ncdt() ((void)Memories_GteCommand(0x0f80416))

#define gte_stsxy(r0) Memories_GteStore(14, (r0))
#define gte_stsxy3(r0, r1, r2) (Memories_GteStore(12, (r0)), Memories_GteStore(13, (r1)), \
    Memories_GteStore(14, (r2)))
#define gte_strgb(r0) Memories_GteStore(22, (r0))
#define gte_stopz(r0) Memories_GteStore(24, (r0))
#define gte_stotz(r0) Memories_GteStore(7, (r0))
#define gte_stszotz(r0) Memories_GteStoreWord( \
    (uint32_t)((int32_t)Memories_GteReadData(19) >> 2), (r0))
#define gte_stflg(r0) Memories_GteStoreWord(Memories_GteReadControl(31), (r0))
#endif
