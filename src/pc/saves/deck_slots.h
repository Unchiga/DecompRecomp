#ifndef MEMORIES_PC_DECK_SLOTS_H
#define MEMORIES_PC_DECK_SLOTS_H
/* Deck slots: decks kept beside a save, to switch between in one step.
 *
 * The game has one deck, the 40 card ids at the start of the save state, and
 * a trunk that counts only the cards outside it (Build Deck moves a card from
 * one to the other). Using a kept deck therefore puts the current forty back
 * in the trunk and takes the kept forty out, which needs every kept card in
 * the trunk or the current deck, and no trunk count past the 250 a byte of it
 * holds (the game's own return to a full trunk drops the card). This file is
 * those rules and the slot file; deck_menu.c is the screen and the game side.
 *
 * The slot file is text, one line per used slot:
 *     3: 1,1,1,20,..., more-cards:Blue Dragon Knight
 * the slot number, then the forty cards: a retail id, or the identity of a
 * card a mod adds (its ids change with the mods applied; cards.h). */

#define DECK_SLOT_COUNT 10
#define DECK_SLOT_CARDS 40
#define DECK_SLOT_COPIES_MAX 3
#define DECK_SLOT_TRUNK_MAX 250

typedef struct {
    int used;
    unsigned short cards[DECK_SLOT_CARDS];
} DeckSlot;

/* The trunk: the byte holding how many of `id` it has, or NULL for an id the
 * running game does not have. */
typedef unsigned char *(*DeckTrunkFn)(void *context, int id);
/* A mod card's identity from its id (NULL for a retail card), and back (0
 * when no applied mod has it). */
typedef const char *(*DeckIdentityFn)(int id);
typedef int (*DeckFindFn)(const char *identity);

enum {
    DECK_OK,
    DECK_EMPTY,          /* the slot holds no deck */
    DECK_INVALID,        /* not forty known cards, or a card more than three times */
    DECK_MISSING,        /* a card is in neither the trunk nor the current deck */
    DECK_TRUNK_FULL      /* a card of the current deck would not fit back in the trunk */
};

/* Read and write the slot file. Reading clears `slots` first and returns
 * the number of used slots (0 for a missing file); a line that does not read
 * as forty cards leaves its slot empty and is counted in *skipped. Writing
 * returns 0 on success. */
int DeckSlots_Read(const char *path, DeckSlot slots[DECK_SLOT_COUNT], DeckFindFn find, int *skipped);
int DeckSlots_Write(const char *path, const DeckSlot slots[DECK_SLOT_COUNT], DeckIdentityFn identity,
                    const char *comment);

/* Whether `want` can replace `current` with the trunk as it is. On
 * DECK_MISSING or DECK_TRUNK_FULL, *card is the first card at fault and
 * *count how many copies are short or over. */
int DeckSlots_Check(const unsigned short current[DECK_SLOT_CARDS], const DeckSlot *want, DeckTrunkFn trunk,
                    void *context, int *card, int *count);
/* Replace the deck after DeckSlots_Check said DECK_OK: the current cards go
 * back to the trunk, the kept ones come out of it, in the kept order. */
void DeckSlots_Use(unsigned short current[DECK_SLOT_CARDS], const DeckSlot *want, DeckTrunkFn trunk, void *context);

/* Whether a slot holds the same forty cards as `deck`, in any order. */
int DeckSlots_Same(const DeckSlot *slot, const unsigned short deck[DECK_SLOT_CARDS]);
#endif
