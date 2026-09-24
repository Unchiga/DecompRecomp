#define _POSIX_C_SOURCE 200809L
#include "pc/platform/title_jump.h"
#include "pc/platform/menu.h"
#include "pc/guest/state.h"
#include "pc/compat/posix.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int enabled, saving, jumps;

void Menu_SetItemEnabled(int id, int value)
{
    assert(id == MENU_ITEM_TITLE);
    enabled = value;
}
int SaveMenu_Active(void) { return saving; }
void TitleJump_Execute(void)
{
    assert(!enabled);
    jumps++;
    /* Input remains live inside the actual disc wait and fade. */
    TitleJump_Request();
    TitleJump_Frame(30);
}

struct MemoriesState { int loading, present, active; };
int Memories_StateLoading(const MemoriesState *state) { return state->loading; }
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    assert(!strcmp(tag, "title-jump") && count == 1 && fields[0].size == sizeof(int));
    if (state->loading) {
        if (!state->present) return 0;
        memcpy(fields[0].data, &state->active, sizeof(int));
        return 1;
    }
    state->present = 1;
    memcpy(&state->active, fields[0].data, sizeof(int));
    return 0;
}

int main(void)
{
    MemoriesState title = {0}, game = {0}, old = {1, 0, 0};
    assert(!setenv("MEMORIES_TITLE_AT", "10,20,30", 1));

    /* Requests at the title, including scheduled ones, must not fire later. */
    TitleJump_SetActive(0);
    TitleJump_Request();
    TitleJump_Frame(10);
    TitleJump_State(&title);
    TitleJump_Poll();
    assert(enabled && jumps == 0);
    TitleJump_State(&game);

    /* Scheduled requests only execute at a safe poll, after saves close. */
    TitleJump_Frame(20);
    assert(jumps == 0);
    saving = 1;
    TitleJump_Poll();
    TitleJump_Poll();
    assert(enabled && jumps == 0);
    saving = 0;
    TitleJump_Poll();
    assert(!enabled && jumps == 1);
    TitleJump_Poll(); /* next game: clicks during the jump were discarded */
    assert(enabled && jumps == 1);

    /* A retail game-over/debug exit enters the frontend too. Clear a
     * queued request and disable input even when our jump did not run. */
    TitleJump_Request();
    TitleJump_SetActive(0);
    assert(!enabled);
    TitleJump_Request();
    TitleJump_Poll();
    assert(enabled && jumps == 1);

    /* Loading a title state while a jump waits on a save discards the
     * request and disables the item immediately. */
    saving = 1;
    TitleJump_Request();
    TitleJump_Poll();
    title.loading = 1;
    TitleJump_State(&title);
    assert(!enabled);
    saving = 0;
    TitleJump_Request();
    TitleJump_Poll();
    assert(jumps == 1);

    /* Loading gameplay at the title enables the item immediately, without
     * waiting for a runner to finish its nested loop. */
    TitleJump_SetActive(0);
    game.loading = 1;
    TitleJump_State(&game);
    assert(enabled);
    TitleJump_Request();
    TitleJump_Poll();
    assert(jumps == 2);

    /* A snapshot never replays an old UI request, nor retains a newer one. */
    TitleJump_Poll();
    TitleJump_Request();
    game.loading = 0;
    TitleJump_State(&game);
    game.loading = 1;
    TitleJump_State(&game);
    TitleJump_Poll();
    assert(jumps == 2);

    /* Older states have no chunk; fail closed until the next safe poll. */
    TitleJump_Request();
    TitleJump_State(&old);
    assert(!enabled);
    TitleJump_Poll();
    assert(enabled && jumps == 2);
    puts("title jump: ok");
    return 0;
}
