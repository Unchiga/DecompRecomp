/* Game > Card drops: more than one card for a won duel, and the results
 * screen's pages that list them (drops.h, notes/card-drops.md).
 *
 * The static recomp's MODS > CARD DROPS did this from outside the game:
 * hooks on the guest's roll, award and page functions, a stream written
 * into free guest memory and a card-name table entry pointed at it, and a
 * host-drawn "New!" sprite. Here the game's own result code asks this
 * module (DuelScene_UpdateResultRewards, Duel_ShowResultPage), and the
 * pages are ordinary strings for the game's text box. */
#include "drops.h"
#include "cards.h"
#include "pc/platform/settings.h"
#include "pc/rng.h"
#include "pc/text/glyphs.h"
#include "game/card_constants.h"
#include "game/duel_rewards.h"
#include "game/save_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void CardDrops_Begin(void)
{
    memset(&gCardDrops, 0, sizeof(gCardDrops));
    gCardDrops.page = -1;
}

/* Copies of `id` in the running save's deck and trunk. */
static int owned(int id)
{
    int count = *Cards_ChestSlot(gDuel_awPlayerDeck, id), i;
    for (i = 0; i < DECK_SIZE; i++) count += gDuel_awPlayerDeck[i] == id;
    return count;
}

/* Several cards are dealt in the community drop mod's order, so a seed
 * gives the same cards here as on its patched discs (the static recomp
 * matched it too): one draw the mod throws away, then for each card six
 * more and the roll, 1 + 7N draws in all. The last roll is the game's own
 * card for SPOILS. One card is the console's single roll. */
int CardDrops_Roll(int pool)
{
    int wanted = Settings_Get(SET_CARD_DROPS), i;
    CardDrops_Begin();
    if (wanted <= 1) return Duel_SelectCardDrop(pool);
    if (wanted > CARD_DROPS_MAX) wanted = CARD_DROPS_MAX;
    Memories_Rand();
    for (i = 1; i < wanted; i++) {
        int burn, card;
        for (burn = 0; burn < 6; burn++) Memories_Rand();
        card = Duel_SelectCardDrop(pool);
        if (!Cards_Valid(card)) continue; /* an empty table */
        gCardDrops.fresh[gCardDrops.count] = owned(card) == 0;
        gCardDrops.cards[gCardDrops.count++] = (u16)card;
    }
    for (i = 0; i < 6; i++) Memories_Rand();
    return Duel_SelectCardDrop(pool);
}

void CardDrops_Award(void)
{
    int i, count = gCardDrops.count;
    /* Once: nothing is left to award if this is reached again. */
    gCardDrops.count = 0;
    for (i = 0; i < count; i++) Duel_AwardCard(gCardDrops.cards[i]);
}

/* --- the pages ---------------------------------------------------------- */

typedef struct {
    int id, copies, fresh;
} Row;

/* The chest puts new cards first; so do the pages, each part by number. */
static int by_row(const void *left, const void *right)
{
    const Row *a = left, *b = right;
    if (a->fresh != b->fresh) return b->fresh - a->fresh;
    return a->id - b->id;
}

/* One row per card, with how many copies were dealt. */
static int rows(Row *out)
{
    int n = 0, i, k;
    for (i = 0; i < gCardDrops.count; i++) {
        for (k = 0; k < n && out[k].id != gCardDrops.cards[i]; k++) {}
        if (k == n) {
            out[n].id = gCardDrops.cards[i];
            out[n].copies = 0;
            out[n++].fresh = gCardDrops.fresh[i];
        }
        out[k].copies++;
    }
    qsort(out, (size_t)n, sizeof(*out), by_row);
    return n;
}

static int added_pages(void)
{
    Row all[CARD_DROPS_MAX];
    return (rows(all) + CARD_DROPS_PER_PAGE - 1) / CARD_DROPS_PER_PAGE;
}

int CardDrops_TurnPage(int page, int step)
{
    int order[CARD_DROPS_FIRST_PAGE + CARD_DROPS_MAX], count = 0, added = added_pages(), i, at = 0;
    order[count++] = 0;
    for (i = 0; i < added; i++) order[count++] = CARD_DROPS_FIRST_PAGE + i;
    order[count++] = 1;
    order[count++] = 2;
    for (i = 0; i < count; i++) {
        if (order[i] == page) at = i;
    }
    return order[(at + step + count) % count];
}

/* The page is written into gCardDrops.text; what does not fit is left off. */
typedef struct {
    u8 *at, *end;
    int glyphs; /* the text box has room for 255 */
} Out;

/* The text box holds 255 glyphs. A page keeps under GLYPH_BUDGET, with
 * room kept for the heading and each row's number, count and NEW. */
#define GLYPH_BUDGET 240
#define HEADING_GLYPHS 24
#define ROW_GLYPHS 11

static void put(Out *out, int byte)
{
    if (out->at < out->end) *out->at++ = (u8)byte;
}

static void command(Out *out, int op, int operand)
{
    put(out, 0xF8);
    put(out, op);
    put(out, operand);
}

/* A new line `dy` pixels down (up when negative), at x 0. */
static void line(Out *out, int dy)
{
    while (dy < -128 || dy > 127) {
        int step = dy < 0 ? -128 : 127;
        command(out, 0x01, step & 0xFF);
        dy -= step;
    }
    command(out, 0x01, dy & 0xFF);
}

static void at_x(Out *out, int x)
{
    put(out, 0xF8);
    put(out, 0x06);
    put(out, x & 0xFF);
    put(out, (x >> 8) & 0xFF);
}

static void glyph(Out *out, int code)
{
    if (code < 0 || out->end - out->at < 3) return;
    if (code >= 0xF0) put(out, 0xF0 + (code >> 8));
    put(out, code & 0xFF);
    out->glyphs++;
}

/* ASCII in the game's letters; a space is a step, not a glyph. */
static void words(Out *out, const char *text)
{
    for (; *text; text++) {
        if (*text == ' ') command(out, 0x02, 8);
        else glyph(out, Glyphs_Code((unsigned char)*text));
    }
}

static int name_length(const u8 *name)
{
    int n = 0;
    while (name && *name < 0xF6) {
        name += *name >= 0xF0 ? 2 : 1;
        n++;
    }
    return n;
}

/* At most `room` glyphs of the name, ending in "..." when cut. */
static void name(Out *out, int id, int room)
{
    const u8 *text = Cards_NameCodes(id);
    int length = name_length(text), keep = length, i;
    if (length > room) keep = room > 3 ? room - 3 : 0;
    for (i = 0; i < keep; i++) {
        if (*text >= 0xF0) {
            glyph(out, ((text[0] - 0xF0) << 8) | text[1]);
            text += 2;
        } else {
            glyph(out, *text++);
        }
    }
    if (keep < length) words(out, keep ? "..." : "");
}

#define ROW_TOP 0x10   /* the SPECIAL ARTS page's first plate, */
#define ROW_STEP 0x18  /* and the step to the next */
#define HEADER_TOP 0x08
#define WIDTH 0x108    /* where the plates end: SPECIAL ARTS' COM column */
#define NEW_X (WIDTH - 3 * 8)
#define COUNT_END (NEW_X - 8)

enum { WHITE = 0x00, GOLD = 0x01, BLUE = 0x02 };

void CardDrops_ComposePage(int page)
{
    Row all[CARD_DROPS_MAX];
    Out out = {gCardDrops.text, gCardDrops.text + sizeof(gCardDrops.text) - 1, 0};
    int count = rows(all), pages = (count + CARD_DROPS_PER_PAGE - 1) / CARD_DROPS_PER_PAGE;
    int first = (page - CARD_DROPS_FIRST_PAGE) * CARD_DROPS_PER_PAGE, r, y = ROW_TOP;
    int last = count < first + CARD_DROPS_PER_PAGE ? count : first + CARD_DROPS_PER_PAGE;
    char buffer[32];
    gCardDrops.page = (s16)page;
    /* Rows first, as the game's own pages are written, then the heading. */
    command(&out, 0x04, 2);
    line(&out, ROW_TOP);
    for (r = first; r < last; r++) {
        const Row *row = &all[r];
        int budget = GLYPH_BUDGET - HEADING_GLYPHS - out.glyphs - ROW_GLYPHS * (last - r);
        int digits = snprintf(buffer, sizeof(buffer), "%03d", row->id);
        int name_x = (digits + 1) * 8, end = WIDTH, room;
        if (r > first) {
            line(&out, ROW_STEP);
            y += ROW_STEP;
        }
        command(&out, 0x0A, BLUE);
        words(&out, buffer);
        if (row->copies > 1) end = COUNT_END - 8 * (snprintf(buffer, sizeof(buffer), "x%d", row->copies) + 1);
        else if (row->fresh) end = NEW_X - 8;
        room = (end - name_x) / 8;
        if (room > budget) room = budget;
        at_x(&out, name_x);
        command(&out, 0x0A, WHITE);
        name(&out, row->id, room);
        if (row->copies > 1) {
            at_x(&out, COUNT_END - 8 * (int)strlen(buffer));
            words(&out, buffer);
        }
        if (row->fresh) {
            at_x(&out, NEW_X);
            command(&out, 0x0A, GOLD);
            words(&out, "NEW");
        }
    }
    /* The heading above the first plate, in the small letters, as SPECIAL
     * ARTS: how many cards past the first, and which page of how many. */
    line(&out, HEADER_TOP - y);
    command(&out, 0x0A, WHITE);
    command(&out, 0x04, 1);
    at_x(&out, -8);
    snprintf(buffer, sizeof(buffer), "%d MORE CARD%s", gCardDrops.count, gCardDrops.count == 1 ? "" : "S");
    words(&out, buffer);
    if (pages > 1) {
        snprintf(buffer, sizeof(buffer), "PAGE %d OF %d", page - CARD_DROPS_FIRST_PAGE + 1, pages);
        at_x(&out, WIDTH - 8 * (int)strlen(buffer));
        words(&out, buffer);
    }
    *out.at = 0xFF;
}

const unsigned char *CardDrops_Text(int id)
{
    return id == CARD_DROPS_TEXT_ID && gCardDrops.page >= CARD_DROPS_FIRST_PAGE ? gCardDrops.text : NULL;
}
