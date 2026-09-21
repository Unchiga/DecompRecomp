#include "profile.h"
#include "symbols.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct ProfileRange {
    uintptr_t first, last;
    uint32_t *samples;
    size_t count;
} ProfileRange;

typedef struct ProfileEntry {
    uintptr_t address;
    uint32_t count;
} ProfileEntry;

static ProfileRange ranges[2];
static const char *output_path;

static int compare_entries(const void *left, const void *right)
{
    const ProfileEntry *a = left, *b = right;
    return a->count < b->count ? 1 : a->count > b->count ? -1 :
           a->address < b->address ? -1 : a->address > b->address;
}

static void write_profile(void)
{
    ProfileEntry *entries;
    size_t used = 0, room = ranges[0].count + ranges[1].count, r, i;
    FILE *file;
    sigset_t set, previous;
    if (!output_path || !(entries = malloc(room * sizeof(*entries)))) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    for (r = 0; r < 2; r++) {
        for (i = 0; i < ranges[r].count; i++) {
            if (ranges[r].samples[i]) {
                entries[used].address = ranges[r].first + i * 16;
                entries[used++].count = ranges[r].samples[i];
            }
        }
    }
    qsort(entries, used, sizeof(*entries), compare_entries);
    file = fopen(output_path, "w");
    if (file) {
        for (i = 0; i < used; i++) {
            uintptr_t offset = 0;
            const char *name = Symbols_Lookup(entries[i].address, &offset);
            fprintf(file, "%08lx %u %s+0x%lx\n", (unsigned long)entries[i].address, entries[i].count,
                    name ? name : "?", (unsigned long)offset);
        }
        fclose(file);
    }
    free(entries);
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void Profile_Init(void)
{
    extern char __start_game_text[], __stop_game_text[], __executable_start[], etext[];
    output_path = getenv("MEMORIES_PROFILE");
    if (!output_path || !*output_path) return;
    ranges[0].first = (uintptr_t)__start_game_text;
    ranges[0].last = (uintptr_t)__stop_game_text;
    ranges[1].first = (uintptr_t)__executable_start;
    ranges[1].last = (uintptr_t)etext;
    ranges[0].count = (ranges[0].last - ranges[0].first + 15) / 16;
    ranges[1].count = (ranges[1].last - ranges[1].first + 15) / 16;
    ranges[0].samples = calloc(ranges[0].count, sizeof(uint32_t));
    ranges[1].samples = calloc(ranges[1].count, sizeof(uint32_t));
    if (!ranges[0].samples || !ranges[1].samples) {
        free(ranges[0].samples);
        free(ranges[1].samples);
        ranges[0].samples = ranges[1].samples = NULL;
        return;
    }
    atexit(write_profile);
}

void Profile_Sample(uintptr_t address)
{
    int r;
    for (r = 0; r < 2; r++) {
        if (ranges[r].samples && address >= ranges[r].first && address < ranges[r].last) {
            ranges[r].samples[(address - ranges[r].first) / 16]++;
            return;
        }
    }
}

void Profile_Flush(void) { write_profile(); }
