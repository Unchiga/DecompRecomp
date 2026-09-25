/* PGXP (pgxp.h): the vertices projected in the last two frames, in a hash
 * table keyed by their screen word, probed a little way; an entry older
 * than that is free again. */
#include "pgxp.h"
#include <math.h>

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

void Pgxp_NextFrame(void)
{
    frame++;
}
