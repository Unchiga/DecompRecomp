/* PGXP (pgxp.h): the vertices projected in the last two frames, in a hash
 * table keyed by their screen word, probed a little way; an entry older
 * than that is free again. */
#include "pgxp.h"
#include <math.h>
#include <string.h>

#define TABLE_SIZE 32768 /* a power of two */
#define PROBES 32

typedef struct {
    uint32_t word;
    unsigned frame; /* 0: never used */
    int ambiguous;
    float x, y, w;
} Entry;

static Entry table[TABLE_SIZE];
static unsigned frame = 2;
int Pgxp_Active;

static unsigned first(uint32_t word)
{
    return (word * 2654435761u) >> 17; /* 15 bits */
}

static int fresh(const Entry *entry)
{
    return entry->frame + 1 >= frame;
}

void Pgxp_Project(uint32_t word, double x, double y, double w)
{
    unsigned at = first(word), n;
    Entry *free_entry = 0;
    for (n = 0; n < PROBES; n++, at = (at + 1) & (TABLE_SIZE - 1)) {
        Entry *entry = &table[at];
        if (!fresh(entry)) {
            if (!free_entry) free_entry = entry;
            continue;
        }
        if (entry->word != word) continue;
        if (entry->frame != frame) {
            /* Last frame's: this frame's replaces it. */
            entry->frame = frame;
            entry->ambiguous = 0;
        } else if (fabs(entry->x - x) > 1.0 / 64 || fabs(entry->y - y) > 1.0 / 64 || fabs(entry->w - w) > 1.0 / 64) {
            entry->ambiguous = 1; /* two vertices round to this word */
            return;
        }
        entry->x = (float)x;
        entry->y = (float)y;
        entry->w = (float)w;
        return;
    }
    if (!free_entry) return; /* crowded: this one stays at whole pixels */
    free_entry->word = word;
    free_entry->frame = frame;
    free_entry->ambiguous = 0;
    free_entry->x = (float)x;
    free_entry->y = (float)y;
    free_entry->w = (float)w;
}

int Pgxp_Find(uint32_t word, float *x, float *y, float *w)
{
    unsigned at = first(word), n;
    for (n = 0; n < PROBES; n++, at = (at + 1) & (TABLE_SIZE - 1)) {
        const Entry *entry = &table[at];
        if (!fresh(entry) || entry->word != word) continue;
        if (entry->ambiguous) return 0;
        *x = entry->x;
        *y = entry->y;
        *w = entry->w;
        return 1;
    }
    return 0;
}

/* By address: where the game's own drawing code wrote a vertex word, with
 * the word written, so nothing is guessed. Last write wins. */
typedef struct {
    uint32_t address, word;
    unsigned frame; /* 0: free */
    int known;
    float x, y, w;
} Placed;

static Placed placed[TABLE_SIZE];

static int placed_fresh(const Placed *entry)
{
    return entry->frame && entry->frame + 1 >= frame;
}

static void forget_at(uint32_t address)
{
    unsigned at = first(address), n;
    for (n = 0; n < PROBES; n++, at = (at + 1) & (TABLE_SIZE - 1)) {
        if (placed[at].address == address) placed[at].frame = 0;
    }
}

void Pgxp_StoreAt(uint32_t address, uint32_t word, const float *xyw)
{
    unsigned at = first(address), n;
    Placed *free_entry = 0;
    for (n = 0; n < PROBES; n++, at = (at + 1) & (TABLE_SIZE - 1)) {
        Placed *entry = &placed[at];
        if (!placed_fresh(entry)) {
            if (!free_entry) free_entry = entry;
            continue;
        }
        if (entry->address != address) continue;
        free_entry = entry;
        break;
    }
    if (!free_entry) return; /* crowded: left to Pgxp_Find */
    free_entry->address = address;
    free_entry->word = word;
    free_entry->frame = frame;
    free_entry->known = xyw != 0;
    if (xyw) {
        free_entry->x = xyw[0];
        free_entry->y = xyw[1];
        free_entry->w = xyw[2];
    }
}

int Pgxp_FindAt(uint32_t address, uint32_t word, float *x, float *y, float *w)
{
    unsigned at = first(address), n;
    for (n = 0; n < PROBES; n++, at = (at + 1) & (TABLE_SIZE - 1)) {
        const Placed *entry = &placed[at];
        if (!placed_fresh(entry) || entry->address != address) continue;
        if (entry->word != word) return 0; /* rewritten since */
        if (!entry->known) return -1;
        *x = entry->x;
        *y = entry->y;
        *w = entry->w;
        return 1;
    }
    return 0;
}

/* The game's own stores of projected vertices (gte_stsxy: Memories_GteStore)
 * since its last addPrim; the primitive it adds is built from them. */
#define STORED 16

typedef struct {
    uint32_t word;
    int known;
    float xyw[3];
} Stored;

static Stored stored[STORED];
static unsigned stored_count;

void Pgxp_Stored(uint32_t word, const float *xyw)
{
    Stored *entry;
    if (stored_count == STORED) {
        memmove(stored, stored + 1, sizeof(stored[0]) * (STORED - 1));
        stored_count--;
    }
    entry = &stored[stored_count++];
    entry->word = word;
    entry->known = xyw != 0;
    if (xyw) memcpy(entry->xyw, xyw, sizeof(entry->xyw));
}

void Pgxp_AddPrim(const void *packet)
{
    const uint32_t *words = (const uint32_t *)packet;
    uint32_t base = (uint32_t)(uintptr_t)packet & 0x00ffffffu; /* physical, as packets link */
    unsigned length, i, k;
    if (!Pgxp_Active) {
        stored_count = 0;
        return;
    }
    length = words[0] >> 24;
    for (i = 1; i <= length; i++) {
        int found = -1, clash = 0;
        /* Packet buffers are reused for projected and unprojected drawing.
         * Even an unchanged word is no longer the old address's vertex. */
        forget_at(base + i * 4);
        for (k = stored_count; k-- > 0;) {
            if (stored[k].word != words[i]) continue;
            if (found < 0) {
                found = (int)k;
            } else if (stored[k].known != stored[found].known ||
                       memcmp(stored[k].xyw, stored[found].xyw, sizeof(stored[k].xyw)) != 0) {
                clash = 1; /* two of this primitive's vertices round to one word */
            }
        }
        if (found < 0) continue;
        Pgxp_StoreAt(base + i * 4, words[i], stored[found].known && !clash ? stored[found].xyw : 0);
    }
    stored_count = 0;
}

void Pgxp_NextFrame(void)
{
    frame++;
    stored_count = 0;
}
