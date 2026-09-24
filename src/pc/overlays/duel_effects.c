#include "pc/compat/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/duel_effect_request.h"
#include "pc/guest/mips.h"
#include "pc/debug/log.h"

extern unsigned char D_8009B261;
extern unsigned Memories_PresentedFrames(void);

/* The common WA overlay dispatcher at 0x801462B0, the entry of the bank the
 * game's loader places at 0x80146000 with every duel package. It holds all
 * the field and card effects (fusion, battle damage, destruction, magic,
 * trap, ritual, terrain, and the rest: ids 0-23) as retail MIPS code with
 * no C source, so the default runs the original routine through the
 * interpreter, which bridges its resident SDK and game calls to the native
 * port. That keeps each effect's own timing, colours and particles.
 *
 * MEMORIES_DUEL_EFFECTS=native restores the bring-up behaviour: fusion (1),
 * battle damage (2) and destruction (3) interpreted, everything else
 * reported complete on its first update. An effect the interpreter cannot
 * run is reported once and completed the same way from then on. */
void Memories_DuelEffectControl(short id, short state, int buffer, DuelEffectRequest *request)
{
    static int native = -1;
    static unsigned failed[8];
    uint32_t args[4], result;
    unsigned index = (unsigned)(unsigned short)id;

    if (native < 0) {
        const char *mode = getenv("MEMORIES_DUEL_EFFECTS");
        native = mode && strcmp(mode, "native") == 0;
    }
    LOG(LOG_DUEL_EFFECTS, "id=%d state=%d buffer=%08x payload=%d,%d,%d damage=%d",
        id, state, (unsigned)buffer, request->field_00, request->field_02, request->field_04, request->field_12);
    if ((native && !(id >= 1 && id <= 3)) || index >= 256 || (failed[index >> 5] & (1u << (index & 31)))) {
        D_8009B261 = 1;
        return;
    }
    args[0] = index;
    args[1] = (uint32_t)(int)state;
    args[2] = (uint32_t)buffer;
    args[3] = (uint32_t)(uintptr_t)request;
    if (Memories_MipsTry(0x801462B0u, args, 4, &result)) {
        fprintf(stderr, "memories-pc: duel effect %d cannot run; completing it at once from now on\n", id);
        failed[index >> 5] |= 1u << (index & 31);
        D_8009B261 = 1;
    }
}
