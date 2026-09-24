#define _POSIX_C_SOURCE 200809L
/* Deck slots (src/pc/saves/deck_slots.c): when a kept deck can replace the
 * current one, what the trunk holds afterwards, and the slot file. */
#include "pc/saves/deck_slots.h"
#include "pc/compat/posix.h"
#include "scratch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define KNOWN_END 800 /* ids 1..799 exist in this fake game */
#define MOD_CARD 750  /* the one card a "mod" added, with an identity */

static unsigned char trunk_bytes[KNOWN_END];

static unsigned char *trunk(void *context, int id)
{
    (void)context;
    return id >= 1 && id < KNOWN_END ? &trunk_bytes[id] : NULL;
}

static const char *identity(int id) { return id == MOD_CARD ? "more-cards:Test Dragon" : NULL; }
static int find(const char *name) { return strcmp(name, "more-cards:Test Dragon") == 0 ? MOD_CARD : 0; }

/* Thirteen ids three times from `first`, then one more. */
static void fill(unsigned short cards[DECK_SLOT_CARDS], int first)
{
    int i;
    for (i = 0; i < 39; i++) cards[i] = (unsigned short)(first + i / 3);
    cards[39] = (unsigned short)(first + 13);
}

static int total(const unsigned short deck[DECK_SLOT_CARDS])
{
    int id, sum = DECK_SLOT_CARDS;
    (void)deck;
    for (id = 1; id < KNOWN_END; id++) sum += trunk_bytes[id];
    return sum;
}

static void test_use(void)
{
    unsigned short deck[DECK_SLOT_CARDS];
    DeckSlot want = {1, {0}};
    int id, card, count, before;
    memset(trunk_bytes, 3, sizeof(trunk_bytes));
    fill(deck, 1);
    fill(want.cards, 100);
    before = total(deck);
    assert(DeckSlots_Check(deck, &want, trunk, NULL, &card, &count) == DECK_OK);
    DeckSlots_Use(deck, &want, trunk, NULL);
    assert(memcmp(deck, want.cards, sizeof(deck)) == 0);
    /* The old forty are back, the new forty are out, nothing else moved. */
    for (id = 1; id <= 13; id++) assert(trunk_bytes[id] == 6);
    assert(trunk_bytes[14] == 4);
    for (id = 100; id <= 112; id++) assert(trunk_bytes[id] == 0);
    assert(trunk_bytes[113] == 2);
    assert(trunk_bytes[200] == 3);
    assert(total(deck) == before);
    assert(DeckSlots_Same(&want, deck));
}

static void test_shared_cards(void)
{
    /* Cards in both decks count from the current deck, not only the trunk. */
    unsigned short deck[DECK_SLOT_CARDS];
    DeckSlot want = {1, {0}};
    int card, count;
    memset(trunk_bytes, 0, sizeof(trunk_bytes));
    fill(deck, 1);
    memcpy(want.cards, deck, sizeof(deck));
    want.cards[39] = 1; /* a fourth id 1 is not allowed ... */
    assert(DeckSlots_Check(deck, &want, trunk, NULL, &card, &count) == DECK_INVALID && card == 1 && count == 4);
    want.cards[39] = 2; /* ... the same forty in another order is fine */
    want.cards[5] = 14;
    assert(DeckSlots_Check(deck, &want, trunk, NULL, &card, &count) == DECK_OK);
}

static void test_refusals(void)
{
    unsigned short deck[DECK_SLOT_CARDS];
    DeckSlot want = {1, {0}}, empty = {0, {0}};
    int card, count;
    fill(deck, 1);
    fill(want.cards, 100);
    memset(trunk_bytes, 3, sizeof(trunk_bytes));
    assert(DeckSlots_Check(deck, &empty, trunk, NULL, &card, &count) == DECK_EMPTY);
    trunk_bytes[104] = 1;
    assert(DeckSlots_Check(deck, &want, trunk, NULL, &card, &count) == DECK_MISSING);
    assert(card == 104 && count == 2);
    trunk_bytes[104] = 3;
    /* Three of id 1 back in a trunk that holds 249 would make 252. */
    trunk_bytes[1] = 249;
    assert(DeckSlots_Check(deck, &want, trunk, NULL, &card, &count) == DECK_TRUNK_FULL);
    assert(card == 1 && count == 2);
    trunk_bytes[1] = 3;
    want.cards[0] = KNOWN_END + 5; /* a card this game does not have */
    assert(DeckSlots_Check(deck, &want, trunk, NULL, &card, &count) == DECK_INVALID);
    want.cards[0] = 0;
    assert(DeckSlots_Check(deck, &want, trunk, NULL, &card, &count) == DECK_INVALID);
    /* A refused check leaves nothing changed: DeckSlots_Use is not called. */
    assert(trunk_bytes[100] == 3);
}

static void test_file(void)
{
    char dir[SCRATCH_MAX], path[SCRATCH_MAX + 32];
    DeckSlot slots[DECK_SLOT_COUNT], back[DECK_SLOT_COUNT];
    FILE *file;
    int skipped;
    scratch_template(dir, sizeof(dir), "memories-decks");
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/decks.txt", dir);
    assert(DeckSlots_Read(path, back, find, &skipped) == 0 && skipped == 0); /* no file yet */
    memset(slots, 0, sizeof(slots));
    slots[0].used = 1;
    fill(slots[0].cards, 1);
    slots[4].used = 1;
    fill(slots[4].cards, 700);
    slots[4].cards[7] = MOD_CARD;
    assert(DeckSlots_Write(path, slots, identity, "test") == 0);
    assert(DeckSlots_Read(path, back, find, &skipped) == 2 && skipped == 0);
    assert(memcmp(back, slots, sizeof(slots)) == 0);
    /* A mod card is kept by identity; without the mod it no longer reads. */
    assert(DeckSlots_Read(path, back, NULL, &skipped) == 1 && skipped == 1 && !back[4].used);
    /* Hand edits: comments, spaces, a short line, a slot out of range. */
    file = fopen(path, "a");
    assert(file);
    fputs("# a comment\n\n  2 :  1,2,3\n11: 1\n", file);
    fclose(file);
    assert(DeckSlots_Read(path, back, find, &skipped) == 2 && skipped == 2);
    assert(!back[1].used);
    remove(path);
    rmdir(dir);
}

int main(void)
{
    test_use();
    test_shared_cards();
    test_refusals();
    test_file();
    puts("deck slots: ok");
    return 0;
}
