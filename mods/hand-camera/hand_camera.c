/* The Hand camera mod (mod.json beside this file; notes/modding.md): while
 * the hand is up (scene state 4, the human's hand
 * actions), where the console leaves the shoulder buttons unused, L1 and R1
 * turn the duel camera around the mat and L3 and R3 zoom it (L2 and R2 are
 * the game's own top-down look at the opponent's field). Both move the
 * view state's own numbers (D_800F2848: angle, a 0x1000-unit turn, and
 * field_00, the distance) and re-apply ViewState_ApplyOrbit, which the game
 * itself only does while one of its own camera tweens runs; the mat, the
 * card sprites and the 3D Monsters mod all follow.
 *
 * The view stays where it was put. Nothing eases it back on release, and
 * nothing resets it when the hand closes: every camera move the game makes
 * from there (placing a card, the turn switch, an attack) is a tween from
 * the current view to an absolute target, so the game's own moves carry the
 * camera back to its usual places smoothly. */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "game/view_state.h"
#include "game/view_state_orbit.h"
#include "pc/mods/modapi.h"
#include <stdint.h>
#include <stdlib.h>

extern ViewState D_800F2848;
extern u16 gDuel_wSceneStateFlags;
extern void (*D_800E9DB0[4])(void); /* the frame service callbacks */
extern void Duel_DrawFieldCards(void);

#define HAND_STATE 4
#define STATE_MASK 0xF
#define TURN 0x1000
#define TURN_STEP 20   /* units a frame while a button is held: a turn in 3.4 s */
#define ZOOM_STEP 6    /* distance units a frame; the duel's own view is 600 away */
#define ZOOM_NEAR 200
#define ZOOM_FAR 1400

static const MemoriesModHost *host;

static int hand_up(void)
{
    return D_800E9DB0[3] == Duel_DrawFieldCards && (gDuel_wSceneStateFlags & STATE_MASK) == HAND_STATE;
}

static void frame(void)
{
    uint16_t pad = host->pad(host, 0);
    int turn = (pad & 0x0800 ? 1 : 0) - (pad & 0x0400 ? 1 : 0); /* R1 - L1 */
    int zoom = (pad & 0x0004 ? 1 : 0) - (pad & 0x0002 ? 1 : 0); /* R3 - L3: out - in */
    int distance;
    if (!hand_up() || (!turn && !zoom)) {
        return;
    }
    D_800F2848.angle = (s16)(((D_800F2848.angle + turn * TURN_STEP) % TURN + TURN) % TURN);
    distance = D_800F2848.field_00 + zoom * ZOOM_STEP;
    D_800F2848.field_00 = (s16)(distance < ZOOM_NEAR ? ZOOM_NEAR : distance > ZOOM_FAR ? ZOOM_FAR : distance);
    ViewState_ApplyOrbit();
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    host = from;
    mod->api = MEMORIES_MOD_API;
    mod->frame = frame;
    return 1;
}
