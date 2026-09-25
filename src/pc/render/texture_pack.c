#include "pc/compat/fs.h"
#include "texture_pack.h"
#include "texture_dump.h"
#include "soft_gpu.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/compat/signal.h"
#include "pc/sdk/disc.h"
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Entry {
    uint32_t offset, clut_offset, stride; /* disc bytes once resolved; stride in words, 0 with row_offsets */
    char archive[32];                     /* the archive the offsets are relative to until then */
    int32_t *row_offsets;
    int words, rows, bpp, crop_left, crop_width, clut_entries;
    char *file;
    volatile int wanted; /* an upload needs this image: TexturePack_Service reads it */
    uint16_t *pixels; /* resampled to words*per_word x rows, shadow cells (texture_dump.h); NULL until first use */
    unsigned char *image; /* the PNG itself, RGBA, for the scaled picture */
    int image_width, image_height;
    int failed;
    int absolute; /* offset and clut_offset are the disc's (resolve) */
    unsigned rank; /* the pack's place in the mods' load order: a later pack's reading of the same words wins */
    int position;  /* and the entry's in its manifest, so the order never rests on qsort's */
} Entry;

static Entry *entries;
static int entry_count, resolved; /* offsets are absolute on the disc, entries sorted */
/* An upload can arrive from the interrupt tick (a LoadImage in the disc
 * callback), where reading a PNG or the disc's directory is not safe: paint
 * only notes what it needs, and TexturePack_Service does it between frames. */
static volatile int wanted_resolve, wanted_images, wanted_rediscover;
static uint16_t *entry_of; /* per VRAM word: entry index + 1 painted there, 0 none */
static uint32_t *place_of; /* per VRAM word: row << 16 | word within that entry */
/* The uploads that asked for an image not read yet: once it is, only they
 * are painted again. Painting all of VRAM takes longer than a frame, and
 * the frame it lands in is shown late (a card's art is read as the card is
 * turned over). Past the last slot, all of VRAM. */
#define WAITING_AREAS 32
typedef struct Area {
    int x, y, w, h;
} Area;
static Area waiting[WAITING_AREAS];
static volatile int waiting_count;
static unsigned generation, map_generation; /* of the entries, of the maps (texture_pack.h) */
/* prepare's pick for the primitive: the head entry (index) whose words it
 * samples and the sibling read with its depth and palette. */
static int chosen_head = -1, chosen = -1;

static int per_word(int bpp) { return bpp == 4 ? 4 : bpp == 8 ? 2 : 1; }

/* Readings of the same words: entries of one geometry, differing in depth
 * or palette (a sheet the game draws with several palettes). Sorted by
 * offset they are adjacent; the first is the head, the one the maps name. */
static int sibling(const Entry *a, const Entry *b)
{
    return a->offset == b->offset && a->words == b->words && a->rows == b->rows && a->stride == b->stride &&
           !a->row_offsets && !b->row_offsets;
}

static int head_of(int index)
{
    while (index > 0 && sibling(&entries[index - 1], &entries[index])) index--;
    return index;
}

/* The disc byte offset of an archive named as the extractor names it
 * ("WA_MRG.MRG"): -1 if the disc has no such file, -2 while there is no
 * disc to ask yet (mods are applied before it is opened). */
static long archive_start(const char *name)
{
    static struct { char name[32]; long start; } known[8];
    static int count;
    char path[64];
    int i, lba, found;
    unsigned size;
    for (i = 0; i < count; i++) {
        if (strcmp(known[i].name, name) == 0) return known[i].start;
    }
    snprintf(path, sizeof(path), "\\DATA\\%s;1", name);
    found = TextureDump_DiscFile(path, &lba, &size);
    if (found == -2) return -2;
    if (found != 0 || lba < 0) return -1;
    if (count < 8 && strlen(name) < sizeof(known[0].name)) {
        strcpy(known[count].name, name);
        known[count].start = (long)lba * 2048;
        count++;
    }
    return (long)lba * 2048;
}

/* By offset, then geometry, depth and palette: readings of the same words
 * end up adjacent, whatever packs they came from and in whatever order.
 * The same reading from two packs goes the later pack first, since the first
 * of a run is the one drawn (head_of, prepare); last, the manifest's own
 * order. No two entries compare equal, so glibc's qsort and the Windows
 * CRT's put them in the same order. */
static int compare(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->offset != y->offset) return x->offset < y->offset ? -1 : 1;
    if (x->words != y->words) return x->words < y->words ? -1 : 1;
    if (x->rows != y->rows) return x->rows < y->rows ? -1 : 1;
    if (x->stride != y->stride) return x->stride < y->stride ? -1 : 1;
    if (x->bpp != y->bpp) return x->bpp < y->bpp ? -1 : 1;
    if (x->clut_offset != y->clut_offset) return x->clut_offset < y->clut_offset ? -1 : 1;
    if (x->rank != y->rank) return x->rank > y->rank ? -1 : 1;
    return x->position < y->position ? -1 : x->position > y->position;
}

/* The PNG, as the texture's own grid of 15-bit colours: each texel takes
 * the average of the image pixels that fall on it (a pack image is any
 * size), alpha below half is the transparent colour. */
static int load_pixels(Entry *entry)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    char path[1200];
    int width = entry->words * per_word(entry->bpp), height = entry->rows, x, y;
    if (entry->pixels || entry->failed) return entry->pixels != NULL;
    entry->wanted = 0;
    snprintf(path, sizeof(path), "%s", entry->file);
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file || !png_image_begin_read_from_stdio(&image, file)) {
        fprintf(stderr, "memories-pc: texture pack: %s cannot be read: %s\n", path, image.message);
        if (file) fclose(file);
        entry->failed = 1;
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        fprintf(stderr, "memories-pc: texture pack: %s cannot be read: %s\n", path, rgba ? image.message : "out of memory");
        free(rgba);
        fclose(file);
        png_image_free(&image);
        entry->failed = 1;
        return 0;
    }
    fclose(file);
    entry->pixels = calloc((size_t)width * height, sizeof(uint16_t));
    if (!entry->pixels) {
        fprintf(stderr, "memories-pc: texture pack: %s: out of memory\n", path);
        free(rgba);
        png_image_free(&image);
        entry->failed = 1;
        return 0;
    }
    for (y = 0; y < height; y++) {
        int sy0 = (int)((long long)y * image.height / height), sy1 = (int)((long long)(y + 1) * image.height / height);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (x = entry->crop_left; x < entry->crop_left + entry->crop_width && x < width; x++) {
            int px = x - entry->crop_left;
            int sx0 = (int)((long long)px * image.width / entry->crop_width);
            int sx1 = (int)((long long)(px + 1) * image.width / entry->crop_width);
            unsigned long r = 0, g = 0, b = 0, a = 0, n = 0;
            int sx, sy;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (sy = sy0; sy < sy1 && sy < (int)image.height; sy++) {
                for (sx = sx0; sx < sx1 && sx < (int)image.width; sx++) {
                    const unsigned char *p = rgba + ((size_t)sy * image.width + sx) * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
                }
            }
            if (n && a / n >= 128) {
                uint16_t colour = (uint16_t)(((r / n) >> 3) | (((g / n) >> 3) << 5) | (((b / n) >> 3) << 10));
                /* Black stays opaque: 0x8000 alone is transparent. */
                entry->pixels[y * width + x] = colour ? (uint16_t)(colour | 0x8000) : TEXTURE_SHADOW_BLACK;
            } else {
                entry->pixels[y * width + x] = 0x8000; /* painted transparent: replaced, by nothing */
            }
        }
    }
    entry->image = rgba;
    entry->image_width = (int)image.width;
    entry->image_height = (int)image.height;
    png_image_free(&image);
    return 1;
}

/* The pack's image at its own resolution for the scaled picture: u and v
 * are 16.16 texels within the page. 0 not replaced, 1 a colour, 2 painted
 * transparent. */
static int sample(int page_x, int page_y, int depth, int u, int v, uint32_t *rgb)
{
    int per = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    int tu = (u >> 16) & 0xff, tv = (v >> 16) & 0xff;
    int vx = (page_x + tu / per) & (SOFT_GPU_WIDTH - 1), vy = (page_y + tv) & (SOFT_GPU_HEIGHT - 1);
    size_t at = (size_t)vy * SOFT_GPU_WIDTH + vx;
    uint16_t index = entry_of[at];
    const Entry *entry;
    const unsigned char *p;
    int64_t px, py;
    int row, word, texel_x;
    if (!index || index - 1 != chosen_head) return 0; /* prepare's pick: its words, its reading */
    if (!*TextureDump_Cell(vx, vy, (tu % per) * (4 / per))) return 0; /* drawn over since */
    entry = &entries[chosen];
    if (!entry->image || per != per_word(entry->bpp)) return 0;
    row = (int)(place_of[at] >> 16);
    word = (int)(place_of[at] & 0xffff);
    /* The texel within the image, with the fraction the picture carries. */
    texel_x = word * per + (tu % per) - entry->crop_left;
    if (texel_x < 0 || texel_x >= entry->crop_width) return 0;
    px = ((int64_t)texel_x * 65536 + (u & 0xffff)) * entry->image_width / ((int64_t)entry->crop_width * 65536);
    py = ((int64_t)row * 65536 + (v & 0xffff)) * entry->image_height / ((int64_t)entry->rows * 65536);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= entry->image_width) px = entry->image_width - 1;
    if (py >= entry->image_height) py = entry->image_height - 1;
    p = entry->image + ((size_t)py * entry->image_width + (size_t)px) * 4;
    if (p[3] < 128) return 2;
    *rgb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return 1;
}

/* The entry holding disc byte `offset`, and where in it: the row and the
 * word within the row. -1 if none. */
static int locate(uint32_t offset, int *row, int *word)
{
    int low = 0, high = entry_count, i;
    while (low < high) {
        int middle = (low + high) / 2;
        if (entries[middle].offset <= offset) low = middle + 1;
        else high = middle;
    }
    /* Entries overlap (the same texture drawn as sub-rectangles), so look
     * back through those starting at or before the offset. */
    for (i = low - 1; i >= 0 && i >= low - 64; i--) {
        const Entry *entry = &entries[i];
        uint32_t delta = offset - entry->offset;
        if (entry->row_offsets) {
            int r;
            for (r = 0; r < entry->rows; r++) {
                int32_t start = entry->row_offsets[r];
                if ((int32_t)delta >= start && (int32_t)delta < start + entry->words * 2) {
                    *row = r;
                    *word = (int)(((int32_t)delta - start) / 2);
                    return i;
                }
            }
        } else if (entry->stride) {
            uint32_t r = delta / (entry->stride * 2), c = (delta % (entry->stride * 2)) / 2;
            if (r < (uint32_t)entry->rows && c < (uint32_t)entry->words) {
                *row = (int)r;
                *word = (int)c;
                return i;
            }
        }
    }
    return -1;
}

/* Recall: the first bytes on the disc of every image and palette the pack
 * replaces, read once the entries are resolved and sorted by them. The
 * disc layer traces an upload through its ring of recent reads
 * (texture_dump.c); an upload it cannot trace is known by these instead:
 * the duel's card thumbnails come from a table filled when the duel
 * starts, whose sectors have left the ring after a long duel (3D models
 * read since) or were never in it (a state loaded). Only images whose
 * rows follow each other on the disc: an upload is one block. Heads that
 * places with other bytes share (a thumbnail's border row, a palette two
 * archives hold) are told apart by a hash of their whole block. */
#define RECALL_BYTES 32
typedef struct {
    unsigned char head[RECALL_BYTES]; /* first: compare_recalled */
    uint32_t disc;
    uint32_t bytes;  /* the block's size */
    uint32_t hash;   /* of the whole block, where `shared` */
    int shared;
} Recalled;
static Recalled *recalled;
static int recalled_count;

static int compare_recalled(const void *a, const void *b) { return memcmp(a, b, RECALL_BYTES); }

/* 1 with the bytes at a disc offset, 0 when unreadable or one value
 * throughout (a fill would be found anywhere). */
static int read_head(uint32_t offset, unsigned char *out)
{
    unsigned char sectors[2 * 2048];
    int at = (int)(offset % 2048), count = at + RECALL_BYTES > 2048 ? 2 : 1, i;
    if (Memories_DiscReadSectors((int)(offset / 2048), count, sectors) != count) return 0;
    memcpy(out, sectors + at, RECALL_BYTES);
    for (i = 1; i < RECALL_BYTES && out[i] == out[0]; i++) {}
    return i < RECALL_BYTES;
}

static uint32_t hash_bytes(const void *data, size_t size)
{
    const unsigned char *at = data;
    uint32_t hash = 2166136261u;
    while (size--) hash = (hash ^ *at++) * 16777619u;
    return hash;
}

/* The hash of a disc block, 0 unreadable. */
static uint32_t hash_disc(uint32_t offset, uint32_t bytes)
{
    size_t first = offset % 2048;
    int sectors = (int)((first + bytes + 2047) / 2048);
    uint32_t hash = 0;
    unsigned char *data = malloc((size_t)sectors * 2048);
    if (!data) return 0;
    if (Memories_DiscReadSectors((int)(offset / 2048), sectors, data) == sectors) hash = hash_bytes(data + first, bytes);
    free(data);
    return hash;
}

static void recall_heads(void)
{
    int i, n = 0;
    free(recalled);
    recalled_count = 0;
    recalled = malloc(sizeof(*recalled) * (size_t)entry_count * 2 + 1);
    if (!recalled) return;
    for (i = 0; i < entry_count; i++) {
        const Entry *entry = &entries[i];
        int whole = !entry->row_offsets && (entry->stride == (uint32_t)entry->words || entry->rows == 1);
        if (whole && entry->words * entry->rows * 2 >= RECALL_BYTES && (i == 0 || entry->offset != entries[i - 1].offset) &&
            read_head(entry->offset, recalled[n].head)) {
            recalled[n].disc = entry->offset;
            recalled[n++].bytes = (uint32_t)(entry->words * entry->rows * 2);
        }
        if (entry->clut_entries * 2 >= RECALL_BYTES && read_head(entry->clut_offset, recalled[n].head)) {
            recalled[n].disc = entry->clut_offset;
            recalled[n++].bytes = (uint32_t)(entry->clut_entries * 2);
        }
    }
    qsort(recalled, (size_t)n, sizeof(*recalled), compare_recalled);
    for (i = 0; i < n;) {
        int j, k, shared = 0;
        for (j = i + 1; j < n && !compare_recalled(&recalled[i], &recalled[j]); j++)
            if (recalled[j].disc != recalled[i].disc) shared = 1;
        for (k = i; k < j; k++) {
            recalled[k].shared = shared;
            recalled[k].hash = shared ? hash_disc(recalled[k].disc, recalled[k].bytes) : 0;
        }
        i = j;
    }
    recalled_count = n;
}

/* The first recalled head an upload starts with, -1 none. */
static int recalled_first(const uint16_t *pixels)
{
    int low = 0, high = recalled_count;
    while (low < high) {
        int middle = (low + high) / 2;
        if (memcmp(pixels, recalled[middle].head, RECALL_BYTES) > 0) low = middle + 1;
        else high = middle;
    }
    return low < recalled_count && !memcmp(pixels, recalled[low].head, RECALL_BYTES) ? low : -1;
}

/* The disc offset of an upload's first byte, 0 unknown. Any context. */
static uint32_t recall(const uint16_t *pixels, size_t words)
{
    uint32_t found = 0;
    int i;
    if (words * 2 < RECALL_BYTES || (i = recalled_first(pixels)) < 0) return 0;
    if (!recalled[i].shared) return recalled[i].disc;
    /* Only the one place whose whole block the upload holds; two with the
     * same bytes throughout would each be a guess. */
    for (; i < recalled_count && !memcmp(pixels, recalled[i].head, RECALL_BYTES); i++) {
        if (recalled[i].bytes > words * 2 || !recalled[i].hash ||
            hash_bytes(pixels, recalled[i].bytes) != recalled[i].hash || recalled[i].disc == found)
            continue;
        if (found) return 0;
        found = recalled[i].disc;
    }
    return found;
}

/* The disc is open after the mods are applied, so the archives' places are
 * looked up on first use; an archive the disc lacks drops its images.
 * 1 resolved, 0 nothing left to resolve, -1 no disc yet. */
static int resolve(void)
{
    int i, kept = 0;
    if (resolved) return 1;
    for (i = 0; i < entry_count; i++) {
        if (!entries[i].absolute && archive_start(entries[i].archive) == -2) return -1; /* no disc yet: next time */
    }
    for (i = 0; i < entry_count; i++) {
        Entry *entry = &entries[i];
        long base = entry->absolute ? 0 : archive_start(entry->archive);
        if (base < 0) {
            free(entry->file);
            free(entry->row_offsets);
            free(entry->pixels);
            free(entry->image);
            continue;
        }
        entry->offset += (uint32_t)base;
        if (entry->clut_entries) entry->clut_offset += (uint32_t)base;
        entry->absolute = 1;
        entries[kept++] = *entry;
    }
    entry_count = kept;
    generation++; /* the entries' order changes: their indexes with it */
    if (!entry_count) {
        fprintf(stderr, "memories-pc: texture packs: none of their archives is on the disc\n");
        resolved = 1; /* nothing to paint, and nothing to ask again every frame */
        return 0;
    }
    qsort(entries, (size_t)entry_count, sizeof(*entries), compare);
    recall_heads();
    resolved = 1;
    fprintf(stderr, "memories-pc: texture packs: %d images\n", entry_count);
    return 1;
}

static void wait_for_image(int x, int y, int w, int h)
{
    int i;
    for (i = 0; i < waiting_count && i < WAITING_AREAS; i++) {
        if (waiting[i].x == x && waiting[i].y == y && waiting[i].w == w && waiting[i].h == h) return;
    }
    if (waiting_count < WAITING_AREAS) {
        waiting[waiting_count].x = x;
        waiting[waiting_count].y = y;
        waiting[waiting_count].w = w;
        waiting[waiting_count].h = h;
    }
    if (waiting_count <= WAITING_AREAS) waiting_count++;
}

/* After an upload: every word of it that a pack image covers gets the
 * image's texels in the shadow. */
static void paint(int x, int y, int w, int h)
{
    int i, j, asked = 0;
    if (!resolved) {
        wanted_resolve = 1;
        return;
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            int vx = (x + i) & (SOFT_GPU_WIDTH - 1), vy = (y + j) & (SOFT_GPU_HEIGHT - 1), row, word, index, k, per;
            size_t at = (size_t)vy * SOFT_GPU_WIDTH + vx;
            uint32_t tag = TextureDump_Tags[at], place;
            uint16_t was = entry_of[at];
            Entry *entry;
            entry_of[at] = 0;
            if (!tag || (index = locate(tag - 1, &row, &word)) < 0) {
                if (was) map_generation++;
                continue;
            }
            index = head_of(index);
            entry = &entries[index];
            if (!entry->pixels) {
                if (!entry->failed) {
                    entry->wanted = 1;
                    wanted_images = 1;
                    asked = 1;
                }
                if (was) map_generation++;
                continue;
            }
            per = per_word(entry->bpp);
            place = ((uint32_t)row << 16) | (uint32_t)word;
            if (was != index + 1 || place_of[at] != place) map_generation++;
            entry_of[at] = (uint16_t)(index + 1);
            place_of[at] = place;
            for (k = 0; k < per; k++) {
                uint16_t colour = entry->pixels[row * entry->words * per + word * per + k];
                int sub = k * (4 / per), s;
                for (s = 0; s < 4 / per; s++) *TextureDump_Cell(vx, vy, sub + s) = colour;
            }
        }
    }
    if (asked) wait_for_image(x, y, w, h);
}

/* The maps follow the words as the shadow does (texture_dump.c): cleared
 * with them, and moved with them. */
static void forget(int x, int y, int w, int h)
{
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            size_t at = (size_t)((y + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((x + i) & (SOFT_GPU_WIDTH - 1));
            if (entry_of[at]) {
                entry_of[at] = 0;
                map_generation++;
            }
        }
    }
}

static void follow(int sx, int sy, int dx, int dy, int w, int h)
{
    int i, j;
    /* Words still waiting for an image may be among those moved. */
    if (waiting_count) wait_for_image(dx, dy, w, h);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            size_t from = (size_t)((sy + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((sx + i) & (SOFT_GPU_WIDTH - 1));
            size_t to = (size_t)((dy + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((dx + i) & (SOFT_GPU_WIDTH - 1));
            if (entry_of[to] != entry_of[from] || (entry_of[from] && place_of[to] != place_of[from])) {
                entry_of[to] = entry_of[from];
                place_of[to] = place_of[from];
                map_generation++;
            }
        }
    }
}

/* Once per textured primitive: a pack image applies if the word of a texel
 * it samples was painted from one and the primitive reads it at that
 * image's depth with its palette. Among the readings of the same words the
 * one whose palette this is: 1 when it is the head, whose colours the
 * shadow holds, so the 1x picture shows them too; 2 for another reading,
 * for the scaled picture alone (sample). */
static int prepare(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v)
{
    int per = depth == 0 ? 4 : depth == 1 ? 2 : 1;
    int vx = (page_x + (u & 0xff) / per) & (SOFT_GPU_WIDTH - 1), vy = (page_y + (v & 0xff)) & (SOFT_GPU_HEIGHT - 1);
    uint16_t index = entry_of[vy * SOFT_GPU_WIDTH + vx];
    int bpp = depth == 0 ? 4 : depth == 1 ? 8 : 16, head, i;
    uint32_t clut = 0;
    chosen_head = chosen = -1;
    if (!index) return 0;
    head = index - 1;
    if (entries[head].clut_entries) {
        clut = TextureDump_Tags[(clut_y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (clut_x & (SOFT_GPU_WIDTH - 1))];
    }
    for (i = head; i < entry_count && (i == head || sibling(&entries[head], &entries[i])); i++) {
        Entry *entry = &entries[i];
        if (entry->bpp != bpp) continue;
        if (entry->clut_entries && (!clut || clut - 1 != entry->clut_offset)) continue;
        if (!entry->image) {
            /* Read between frames (TexturePack_Service); the head's colours
             * are not this reading's, so nothing replaces until then. */
            if (!entry->failed && !entry->wanted) {
                entry->wanted = 1;
                wanted_images = 1;
            }
            return 0;
        }
        chosen_head = head;
        chosen = i;
        return i == head ? 1 : 2;
    }
    return 0;
}

static void free_entries(void)
{
    int i;
    for (i = 0; i < entry_count; i++) {
        free(entries[i].file);
        free(entries[i].row_offsets);
        free(entries[i].pixels);
        free(entries[i].image);
    }
    free(entries);
    entries = NULL;
    entry_count = 0;
    free(recalled);
    recalled = NULL;
    recalled_count = 0;
}

/* What was wrong with a pack's entries, kind by kind: how many, and the
 * first one's file, for the one line the Mods window has room for. */
typedef struct {
    int count;
    char first[64];
} Problem;

static void problem(Problem *kind, const char *file)
{
    if (!kind->count++) snprintf(kind->first, sizeof(kind->first), "%s", file ? file : "(no file)");
}

static void describe(char *out, size_t size, const Problem *kind, const char *one, const char *many)
{
    size_t length = strlen(out);
    if (!kind->count || length >= size) return;
    snprintf(out + length, size - length, "%s%d %s (first: %s)", length ? "; " : "", kind->count,
             kind->count == 1 ? one : many, kind->first);
}

/* The images are read when the game first needs them, so at load a file is
 * only checked to be there and to be a PNG: a problem shows as the pack is
 * applied, not when a screen happens to want the picture. */
static int is_png(const char *path)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    unsigned char start[8];
    FILE *file = fopen(path, "rb");
    int same;
    if (!file) return 0;
    same = fread(start, 1, sizeof(start), file) == sizeof(start) && !memcmp(start, signature, sizeof(start));
    fclose(file);
    return same;
}

/* The size of the block an upload of the image or palette starting at a
 * disc offset covers: 0 when the pack has none there. */
static int block_at(uint32_t disc, int *words, int *rows)
{
    int i;
    for (i = 0; i < entry_count; i++) {
        const Entry *entry = &entries[i];
        if (entry->offset == disc && !entry->row_offsets && (entry->stride == (uint32_t)entry->words || entry->rows == 1)) {
            *words = entry->words;
            *rows = entry->rows;
            return 1;
        }
        if (entry->clut_entries && entry->clut_offset == disc) {
            *words = entry->clut_entries;
            *rows = 1;
            return 1;
        }
    }
    return 0;
}

/* Whether VRAM's block at x,y holds the disc's bytes from `disc` on. */
static int holds_disc(const uint16_t *vram, uint32_t disc, int x, int y, int words, int rows)
{
    size_t bytes = (size_t)words * rows * 2, first = disc % 2048;
    int sectors = (int)((first + bytes + 2047) / 2048), j, same = 1;
    unsigned char *data = malloc((size_t)sectors * 2048);
    if (!data) return 0;
    if (Memories_DiscReadSectors((int)(disc / 2048), sectors, data) != sectors) same = 0;
    for (j = 0; same && j < rows; j++)
        same = memcmp(vram + (size_t)(y + j) * SOFT_GPU_WIDTH + x, data + first + (size_t)j * words * 2,
                      (size_t)words * 2) == 0;
    free(data);
    return same;
}

/* After a state load VRAM holds its pictures without their tags (what the
 * disc delivered is not in a state): a block that starts with a recalled
 * head and matches the disc throughout is tagged as its upload was, and
 * painted. Between frames, with the entries resolved. */
static void rediscover(void)
{
    const uint16_t *vram = SoftGpu_Vram();
    int x, y, i, j;
    if (!recalled_count || !vram || !TextureDump_Tags) return;
    for (y = 0; y < SOFT_GPU_HEIGHT; y++) {
        for (x = 0; x + RECALL_BYTES / 2 <= SOFT_GPU_WIDTH; x++) {
            const uint16_t *at = vram + (size_t)y * SOFT_GPU_WIDTH + x;
            uint32_t disc = 0;
            int words = 0, rows = 0, w, r, k, places = 0;
            if (TextureDump_Tags[(size_t)y * SOFT_GPU_WIDTH + x] || (k = recalled_first(at)) < 0) continue;
            /* The one place sharing the head whose bytes the block holds. */
            for (; k < recalled_count && !memcmp(at, recalled[k].head, RECALL_BYTES); k++) {
                if (recalled[k].disc == disc || !block_at(recalled[k].disc, &w, &r) || x + w > SOFT_GPU_WIDTH ||
                    y + r > SOFT_GPU_HEIGHT || !holds_disc(vram, recalled[k].disc, x, y, w, r))
                    continue;
                disc = recalled[k].disc;
                words = w;
                rows = r;
                places++;
            }
            if (places != 1) continue;
            for (j = 0; j < rows; j++)
                for (i = 0; i < words; i++)
                    TextureDump_Tags[(size_t)(y + j) * SOFT_GPU_WIDTH + x + i] = disc + (uint32_t)(j * words + i) * 2 + 1;
            paint(x, y, words, rows);
            x += words - 1;
        }
    }
}

static void restored(void) { wanted_rediscover = 1; }

int TexturePack_Load(const char *from, unsigned rank, int (*part)(const char *setting, void *context), void *context,
                     char *problems, size_t problems_size)
{
    char path[1200], error[256];
    JsonDocument *manifest;
    const JsonValue *list, *item;
    int count, before = entry_count, position = 0, full = 0, off = 0;
    Problem unreadable = {0}, outside = {0}, measures = {0}, unaddressed = {0}, row_count = {0}, undeclared = {0};
    Entry *more;
    if (problems && problems_size) problems[0] = '\0';
    /* Packs add up: each enabled mod's joins the entries already loaded. */
    snprintf(path, sizeof(path), "%s/manifest.json", from);
    manifest = Json_ParseFile(path, error, sizeof(error));
    if (!manifest) {
        fprintf(stderr, "memories-pc: texture pack %s: %s\n", path, error);
        if (problems && problems_size) snprintf(problems, problems_size, "manifest.json %s", error);
        return -1;
    }
    list = Json_Root(manifest);
    count = Json_Count(list);
    more = realloc(entries, (size_t)(entry_count + (count ? count : 1)) * sizeof(*entries));
    if (more) {
        entries = more;
        memset(entries + entry_count, 0, (size_t)(count ? count : 1) * sizeof(*entries));
    }
    /* Walked in one pass: a pack can list tens of thousands of images. */
    for (item = more ? Json_At(list, 0) : NULL; item; item = Json_Next(item), position++) {
        const JsonValue *rows = Json_Member(item, "row_offsets"), *row;
        const char *file = Json_String(Json_Member(item, "file"), NULL);
        const char *archive = Json_String(Json_Member(item, "archive"), NULL);
        const JsonValue *setting = Json_Member(item, "setting");
        Entry *entry = &entries[entry_count];
        double offset, clut_offset, words, rows_count, bpp, stride, crop_left, crop_width;
        if (setting) { /* a part of the pack the mod's settings switch off, or a setting it lacks: then used */
            const char *key = Json_String(setting, "");
            int on = part && *key ? part(key, context) : -1;
            if (!on) {
                off++;
                continue;
            }
            if (on < 0) problem(&undeclared, *key ? key : file);
        }
        if (!file || !archive || strlen(archive) >= sizeof(entry->archive)) { /* not addressed on the disc */
            problem(&unaddressed, file);
            continue;
        }
        if (!Paths_Contained(file)) { /* "../../x.png" would reach past the mod */
            problem(&outside, file);
            continue;
        }
        if (entry_count >= 65535) { /* entry_of holds index + 1 in 16 bits */
            full = 1;
            break;
        }
        offset = Json_Number(Json_Member(item, "offset"), -1);
        words = Json_Number(Json_Member(item, "words"), 0);
        rows_count = Json_Number(Json_Member(item, "rows"), 0);
        bpp = Json_Number(Json_Member(item, "bpp"), 0);
        clut_offset = Json_Number(Json_Member(item, "clut_offset"), 0);
        stride = Json_Number(Json_Member(item, "stride"), words); /* rows contiguous unless said */
        crop_left = Json_Number(Json_Member(item, "crop_left"), 0);
        /* What the game could upload: a texture page at most 1024 words
         * wide, 512 rows, from an offset on a CD. */
        if (!(offset >= 0 && offset < 1e9) || !(clut_offset >= 0 && clut_offset < 1e9) ||
            !(words >= 1 && words <= SOFT_GPU_WIDTH) || !(rows_count >= 1 && rows_count <= SOFT_GPU_HEIGHT) ||
            (bpp != 4 && bpp != 8 && bpp != 16) || !(stride >= 1 && stride <= 1e6) ||
            !(crop_left >= 0 && crop_left < words * per_word((int)bpp))) {
            problem(&measures, file);
            continue;
        }
        crop_width = Json_Number(Json_Member(item, "width"), words * per_word((int)bpp) - crop_left);
        if (!(crop_width >= 1 && crop_left + crop_width <= words * per_word((int)bpp))) {
            problem(&measures, file);
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", from, file);
        if (!is_png(path)) {
            problem(&unreadable, file);
            continue;
        }
        strcpy(entry->archive, archive);
        entry->offset = (uint32_t)offset;
        entry->words = (int)words;
        entry->rows = (int)rows_count;
        entry->bpp = (int)bpp;
        entry->clut_entries = (int)Json_Number(Json_Member(item, "clut_entries"), 0);
        entry->clut_offset = entry->clut_entries ? (uint32_t)clut_offset : 0;
        entry->stride = (uint32_t)stride;
        entry->crop_left = (int)crop_left;
        entry->crop_width = (int)crop_width;
        entry->rank = rank;
        entry->position = position;
        if (rows && Json_TypeOf(rows) != JSON_NULL && Json_Count(rows) != entry->rows) {
            problem(&row_count, file); /* read with the stride instead, which may well be wrong */
        } else if (rows && Json_Count(rows) == entry->rows) {
            int r;
            entry->row_offsets = malloc(sizeof(int32_t) * (size_t)entry->rows);
            for (r = 0, row = Json_At(rows, 0); entry->row_offsets && row; r++, row = Json_Next(row)) {
                entry->row_offsets[r] = (int32_t)Json_Number(row, 0);
            }
            entry->stride = 0;
        }
        if (entry->words <= 0 || entry->rows <= 0 || entry->rows > 512 || (!entry->stride && !entry->row_offsets)) {
            free(entry->row_offsets);
            entry->row_offsets = NULL;
            problem(&measures, file);
            continue;
        }
        entry->file = malloc(strlen(from) + strlen(file) + 2); /* the whole path: packs from several directories add up */
        if (!entry->file) {
            free(entry->row_offsets);
            entry->row_offsets = NULL;
            problem(&unreadable, file);
            continue;
        }
        sprintf(entry->file, "%s/%s", from, file);
        entry_count++;
    }
    Json_Free(manifest);
    if (problems && problems_size) {
        describe(problems, problems_size, &unreadable, "image could not be read", "images could not be read");
        describe(problems, problems_size, &outside, "image is outside the pack", "images are outside the pack");
        describe(problems, problems_size, &measures, "image has measures out of range",
                 "images have measures out of range");
        describe(problems, problems_size, &row_count, "image's row_offsets do not match its rows",
                 "images' row_offsets do not match their rows");
        describe(problems, problems_size, &unaddressed, "image names no file or archive",
                 "images name no file or archive");
        describe(problems, problems_size, &undeclared, "image names a setting the mod does not declare",
                 "images name a setting the mod does not declare");
        if (full) {
            size_t length = strlen(problems);
            if (length < problems_size)
                snprintf(problems + length, problems_size - length, "%sonly 65535 images fit; the rest were left out",
                         length ? "; " : "");
        }
        if (*problems) fprintf(stderr, "memories-pc: texture pack %s: %s\n", from, problems);
    }
    if (entry_count == before) {
        if (!entry_count) free_entries();
        if (off) { /* the player switched every part off: nothing wrong with the pack */
            fprintf(stderr, "memories-pc: texture pack %s: every image is switched off\n", from);
            return 0;
        }
        fprintf(stderr, "memories-pc: texture pack %s: no image is addressed on the disc\n", from);
        return -1;
    }
    if (!TextureDump_EnableShadow()) {
        free_entries();
        return -1;
    }
    if (!entry_of) entry_of = calloc((size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(*entry_of));
    if (!place_of) place_of = calloc((size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(*place_of));
    if (!entry_of || !place_of) {
        free_entries();
        return -1;
    }
    TextureDump_Paint = paint;
    TextureDump_Prepare = prepare;
    TextureDump_Sample = sample;
    TextureDump_Forget = forget;
    TextureDump_Follow = follow;
    TextureDump_Recall = recall;
    TextureDump_Restored = restored;
    /* The disc's directory and the images wait for TexturePack_Service,
     * between frames: a mod is applied while the game starts, before the
     * disc is open, and an upload can ask for an image from the interrupt
     * tick. Until then paint only notes what it needs and sample sees no
     * image. */
    resolved = 0; /* the new entries, and the order, with the others */
    wanted_resolve = 1;
    generation++;
    fprintf(stderr, "memories-pc: texture pack %s: %d images\n", from, entry_count - before);
    return entry_count - before;
}

/* Between frames, on the main thread with the clock held: the disc's
 * directory and the images an upload asked for, then the words they cover. */
void TexturePack_Service(void)
{
    sigset_t held, previous;
    int i, painted = 0, everywhere = 0;
    if (!entries || (!wanted_resolve && !wanted_images && !wanted_rediscover)) return;
    sigemptyset(&held);
    sigaddset(&held, SIGALRM);
    sigprocmask(SIG_BLOCK, &held, &previous);
    if (wanted_resolve) {
        wanted_resolve = 0;
        int got = resolve();
        if (got > 0) everywhere = 1;          /* uploads since the load were skipped */
        else if (got < 0) wanted_resolve = 1; /* no disc yet: again next frame */
    }
    if (wanted_rediscover && resolved) {
        wanted_rediscover = 0;
        rediscover();
    }
    if (wanted_images && resolved) {
        wanted_images = 0;
        /* Another reading of words already painted (prepare) is sampled
         * from its image alone: only the uploads that waited are painted. */
        for (i = 0; i < entry_count; i++) {
            if (entries[i].wanted && load_pixels(&entries[i])) painted = 1;
        }
    }
    if (everywhere || (painted && waiting_count > WAITING_AREAS)) {
        waiting_count = 0;
        paint(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
    } else if (painted) {
        /* paint notes again what is still missing, so the list is read first. */
        int count = waiting_count, k;
        Area areas[WAITING_AREAS];
        memcpy(areas, waiting, sizeof(areas));
        waiting_count = 0;
        for (k = 0; k < count; k++) paint(areas[k].x, areas[k].y, areas[k].w, areas[k].h);
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void TexturePack_Unload(void)
{
    if (!entries) return;
    TextureDump_Paint = NULL;
    TextureDump_Prepare = NULL;
    TextureDump_Sample = NULL;
    TextureDump_Forget = NULL;
    TextureDump_Follow = NULL;
    TextureDump_Recall = NULL;
    TextureDump_Restored = NULL;
    if (TextureDump_Shadow) {
        memset(TextureDump_Shadow, 0, (size_t)TEXTURE_SHADOW_WIDTH * SOFT_GPU_HEIGHT * sizeof(*TextureDump_Shadow));
    }
    if (entry_of) memset(entry_of, 0, (size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * sizeof(*entry_of));
    free_entries();
    resolved = 0;
    wanted_resolve = wanted_images = wanted_rediscover = 0;
    waiting_count = 0;
    generation++;
    map_generation++;
}

int TexturePack_EntryFor(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v)
{
    if (!entries || !entry_of || !TextureDump_Tags || !prepare(page_x, page_y, depth, clut_x, clut_y, u, v)) return 0;
    return chosen + 1;
}

int TexturePack_EntryHead(int entry)
{
    if (entry < 1 || entry > entry_count) return 0;
    return head_of(entry - 1) + 1;
}

int TexturePack_EntryImage(int entry, const unsigned char **rgba, int *width, int *height, int *crop_left,
                           int *crop_width, int *rows, int *texels_per_word)
{
    const Entry *at;
    if (entry < 1 || entry > entry_count || !entries[entry - 1].image) return 0;
    at = &entries[entry - 1];
    *rgba = at->image;
    *width = at->image_width;
    *height = at->image_height;
    *crop_left = at->crop_left;
    *crop_width = at->crop_width;
    *rows = at->rows;
    *texels_per_word = per_word(at->bpp);
    return 1;
}

unsigned TexturePack_Generation(void) { return generation; }
unsigned TexturePack_MapGeneration(void) { return map_generation; }
const uint16_t *TexturePack_EntryMap(void) { return entry_of; }
const uint32_t *TexturePack_PlaceMap(void) { return place_of; }
