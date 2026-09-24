/* Deck slots: the rules and the slot file. See deck_slots.h. */
#include "deck_slots.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/compat/posix.h"

/* How many of each card two decks hold: at most eighty different ids. */
typedef struct {
    int id[2 * DECK_SLOT_CARDS];
    int count[2][2 * DECK_SLOT_CARDS];
    int n;
} Tally;

static int tally_index(Tally *tally, int id)
{
    int i;
    for (i = 0; i < tally->n; i++) {
        if (tally->id[i] == id) return i;
    }
    tally->id[tally->n] = id;
    tally->count[0][tally->n] = tally->count[1][tally->n] = 0;
    return tally->n++;
}

static void count_decks(Tally *tally, const unsigned short current[DECK_SLOT_CARDS],
                        const unsigned short want[DECK_SLOT_CARDS])
{
    int i;
    tally->n = 0;
    for (i = 0; i < DECK_SLOT_CARDS; i++) {
        if (current[i]) tally->count[0][tally_index(tally, current[i])]++;
        tally->count[1][tally_index(tally, want[i])]++;
    }
}

int DeckSlots_Check(const unsigned short current[DECK_SLOT_CARDS], const DeckSlot *want, DeckTrunkFn trunk,
                    void *context, int *card, int *count)
{
    Tally tally;
    int i;
    *card = *count = 0;
    if (!want->used) return DECK_EMPTY;
    for (i = 0; i < DECK_SLOT_CARDS; i++) {
        if (!want->cards[i] || !trunk(context, want->cards[i])) {
            *card = want->cards[i];
            return DECK_INVALID;
        }
    }
    count_decks(&tally, current, want->cards);
    for (i = 0; i < tally.n; i++) {
        if (tally.count[1][i] > DECK_SLOT_COPIES_MAX) {
            *card = tally.id[i];
            *count = tally.count[1][i];
            return DECK_INVALID;
        }
    }
    for (i = 0; i < tally.n; i++) {
        unsigned char *held = trunk(context, tally.id[i]);
        int have = (held ? *held : 0) + tally.count[0][i];
        if (tally.count[1][i] > have) {
            *card = tally.id[i];
            *count = tally.count[1][i] - have;
            return DECK_MISSING;
        }
    }
    for (i = 0; i < tally.n; i++) {
        unsigned char *held = trunk(context, tally.id[i]);
        int after = (held ? *held : 0) + tally.count[0][i] - tally.count[1][i];
        /* A card of the current deck whose mod is gone has no trunk to go to. */
        if (!held && after > 0) after = DECK_SLOT_TRUNK_MAX + after;
        if (after > DECK_SLOT_TRUNK_MAX) {
            *card = tally.id[i];
            *count = after - DECK_SLOT_TRUNK_MAX;
            return DECK_TRUNK_FULL;
        }
    }
    return DECK_OK;
}

void DeckSlots_Use(unsigned short current[DECK_SLOT_CARDS], const DeckSlot *want, DeckTrunkFn trunk, void *context)
{
    Tally tally;
    int i;
    count_decks(&tally, current, want->cards);
    for (i = 0; i < tally.n; i++) {
        unsigned char *held = trunk(context, tally.id[i]);
        if (held) *held = (unsigned char)(*held + tally.count[0][i] - tally.count[1][i]);
    }
    memcpy(current, want->cards, sizeof(want->cards));
}

int DeckSlots_Same(const DeckSlot *slot, const unsigned short deck[DECK_SLOT_CARDS])
{
    Tally tally;
    int i;
    if (!slot->used) return 0;
    count_decks(&tally, deck, slot->cards);
    for (i = 0; i < tally.n; i++) {
        if (tally.count[0][i] != tally.count[1][i]) return 0;
    }
    return 1;
}

static char *trim(char *text)
{
    char *end;
    while (isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = 0;
    return text;
}

/* "3: 1,1,20,...": the slot's forty cards, or -1. */
static int read_line(char *line, DeckSlot slots[DECK_SLOT_COUNT], DeckFindFn find)
{
    char *field, *end;
    long slot = strtol(line, &end, 10);
    DeckSlot kept = {1, {0}};
    int n = 0;
    if (end == line) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != ':' || slot < 1 || slot > DECK_SLOT_COUNT) return -1;
    for (field = strtok(end + 1, ","); field; field = strtok(NULL, ",")) {
        char *text = trim(field);
        long id = strtol(text, &end, 10);
        if (n == DECK_SLOT_CARDS) return -1;
        if (*text && !*end) {
            if (id < 1 || id > 0xffff) return -1;
        } else {
            id = find ? find(text) : 0;
            if (id < 1) return -1;
        }
        kept.cards[n++] = (unsigned short)id;
    }
    if (n != DECK_SLOT_CARDS) return -1;
    slots[slot - 1] = kept;
    return 0;
}

int DeckSlots_Read(const char *path, DeckSlot slots[DECK_SLOT_COUNT], DeckFindFn find, int *skipped)
{
    char line[4096];
    FILE *file;
    int slot, used = 0;
    memset(slots, 0, sizeof(DeckSlot) * DECK_SLOT_COUNT);
    *skipped = 0;
    file = fopen(path, "r");
    if (!file) return 0;
    while (fgets(line, sizeof(line), file)) {
        char *text = trim(line);
        if (!*text || *text == '#') continue;
        if (read_line(text, slots, find)) ++*skipped;
    }
    fclose(file);
    for (slot = 0; slot < DECK_SLOT_COUNT; slot++) used += slots[slot].used;
    return used;
}

int DeckSlots_Write(const char *path, const DeckSlot slots[DECK_SLOT_COUNT], DeckIdentityFn identity,
                    const char *comment)
{
    char partial[1100];
    FILE *file;
    int slot, i, failed;
    snprintf(partial, sizeof(partial), "%s.partial", path);
    file = fopen(partial, "w");
    if (!file) return -1;
    if (comment) fprintf(file, "# %s\n", comment);
    fprintf(file, "# One line per slot: its number, then the forty cards (a retail id, or a mod card's identity).\n");
    for (slot = 0; slot < DECK_SLOT_COUNT; slot++) {
        if (!slots[slot].used) continue;
        fprintf(file, "%d:", slot + 1);
        for (i = 0; i < DECK_SLOT_CARDS; i++) {
            const char *name = identity ? identity(slots[slot].cards[i]) : NULL;
            if (name) fprintf(file, "%s %s", i ? "," : "", name);
            else fprintf(file, "%s %d", i ? "," : "", slots[slot].cards[i]);
        }
        fputc('\n', file);
    }
    failed = ferror(file) != 0;
    if (fclose(file) != 0) failed = 1;
    if (failed || rename(partial, path) != 0) {
        remove(partial);
        return -1;
    }
    return 0;
}
