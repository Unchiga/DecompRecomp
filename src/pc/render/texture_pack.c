#include "texture_pack.h"
#include "texture_dump.h"
#include "soft_gpu.h"
#include "pc/mods/json.h"
#include "pc/compat/signal.h"
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
    uint16_t *pixels; /* resampled to words*per_word x rows, 15-bit | 0x8000, 0 = transparent; NULL until first use */
    unsigned char *image; /* the PNG itself, RGBA, for the scaled picture */
    int image_width, image_height;
    int failed;
    int absolute; /* offset and clut_offset are the disc's (resolve) */
} Entry;

static Entry *entries;
static int entry_count, resolved; /* offsets are absolute on the disc, entries sorted */
/* An upload can arrive from the interrupt tick (a LoadImage in the disc
 * callback), where reading a PNG or the disc's directory is not safe: paint
 * only notes what it needs, and TexturePack_Service does it between frames. */
static volatile int wanted_resolve, wanted_images;
static uint16_t *entry_of; /* per VRAM word: entry index + 1 painted there, 0 none */
static uint32_t *place_of; /* per VRAM word: row << 16 | word within that entry */
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
 * end up adjacent, whatever packs they came from and in whatever order. */
static int compare(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->offset != y->offset) return x->offset < y->offset ? -1 : 1;
    if (x->words != y->words) return x->words < y->words ? -1 : 1;
    if (x->rows != y->rows) return x->rows < y->rows ? -1 : 1;
    if (x->stride != y->stride) return x->stride < y->stride ? -1 : 1;
    if (x->bpp != y->bpp) return x->bpp < y->bpp ? -1 : 1;
    return x->clut_offset < y->clut_offset ? -1 : x->clut_offset > y->clut_offset;
}

/* The PNG, as the texture's own grid of 15-bit colours: each texel takes
 * the average of the image pixels that fall on it (a pack image is any
 * size), alpha below half is the transparent colour. */
static int load_pixels(Entry *entry)
{
    png_image image;
    unsigned char *rgba;
    char path[1200];
    int width = entry->words * per_word(entry->bpp), height = entry->rows, x, y;
    if (entry->pixels || entry->failed) return entry->pixels != NULL;
    entry->wanted = 0;
    snprintf(path, sizeof(path), "%s", entry->file);
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path)) {
        entry->failed = 1;
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        png_image_free(&image);
        entry->failed = 1;
        return 0;
    }
    entry->pixels = calloc((size_t)width * height, sizeof(uint16_t));
    if (!entry->pixels) {
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
                entry->pixels[y * width + x] = (uint16_t)(colour | 0x8000);
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
    resolved = 1;
    fprintf(stderr, "memories-pc: texture packs: %d images\n", entry_count);
    return 1;
}

/* After an upload: every word of it that a pack image covers gets the
 * image's texels in the shadow. */
static void paint(int x, int y, int w, int h)
{
    int i, j;
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
}

int TexturePack_Load(const char *from)
{
    char path[1200], error[256];
    JsonDocument *manifest;
    const JsonValue *list, *item;
    int count, before = entry_count;
    Entry *more;
    /* Packs add up: each enabled mod's joins the entries already loaded. */
    snprintf(path, sizeof(path), "%s/manifest.json", from);
    manifest = Json_ParseFile(path, error, sizeof(error));
    if (!manifest) {
        fprintf(stderr, "memories-pc: texture pack %s: %s\n", path, error);
        return 0;
    }
    list = Json_Root(manifest);
    count = Json_Count(list);
    more = realloc(entries, (size_t)(entry_count + (count ? count : 1)) * sizeof(*entries));
    if (more) {
        entries = more;
        memset(entries + entry_count, 0, (size_t)(count ? count : 1) * sizeof(*entries));
    }
    /* Walked in one pass: a pack can list tens of thousands of images. */
    for (item = more ? Json_At(list, 0) : NULL; item; item = Json_Next(item)) {
        const JsonValue *rows = Json_Member(item, "row_offsets"), *row;
        const char *file = Json_String(Json_Member(item, "file"), NULL);
        const char *archive = Json_String(Json_Member(item, "archive"), NULL);
        Entry *entry = &entries[entry_count];
        double offset, clut_offset, words, rows_count, bpp, stride, crop_left, crop_width;
        if (!file || !archive || strlen(archive) >= sizeof(entry->archive)) continue; /* not addressed on the disc */
        if (entry_count >= 65535) break; /* entry_of holds index + 1 in 16 bits */
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
            fprintf(stderr, "memories-pc: texture pack %s: %s has measures out of range; skipped\n", from, file);
            continue;
        }
        crop_width = Json_Number(Json_Member(item, "width"), words * per_word((int)bpp) - crop_left);
        if (!(crop_width >= 1 && crop_left + crop_width <= words * per_word((int)bpp))) {
            fprintf(stderr, "memories-pc: texture pack %s: %s has measures out of range; skipped\n", from, file);
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
        if (rows && Json_Count(rows) == entry->rows) {
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
            continue;
        }
        entry->file = malloc(strlen(from) + strlen(file) + 2); /* the whole path: packs from several directories add up */
        if (entry->file) sprintf(entry->file, "%s/%s", from, file);
        entry_count++;
    }
    Json_Free(manifest);
    if (entry_count == before) {
        fprintf(stderr, "memories-pc: texture pack %s: no image is addressed on the disc\n", from);
        if (!entry_count) free_entries();
        return 0;
    }
    if (!TextureDump_EnableShadow()) {
        free_entries();
        return 0;
    }
    if (!entry_of) entry_of = calloc((size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(*entry_of));
    if (!place_of) place_of = calloc((size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(*place_of));
    if (!entry_of || !place_of) {
        free_entries();
        return 0;
    }
    TextureDump_Paint = paint;
    TextureDump_Prepare = prepare;
    TextureDump_Sample = sample;
    TextureDump_Forget = forget;
    TextureDump_Follow = follow;
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
    int i, painted = 0;
    if (!entries || (!wanted_resolve && !wanted_images)) return;
    sigemptyset(&held);
    sigaddset(&held, SIGALRM);
    sigprocmask(SIG_BLOCK, &held, &previous);
    if (wanted_resolve) {
        wanted_resolve = 0;
        int got = resolve();
        if (got > 0) painted = 1;        /* uploads since the load were skipped */
        else if (got < 0) wanted_resolve = 1; /* no disc yet: again next frame */
    }
    if (wanted_images && resolved) {
        wanted_images = 0;
        for (i = 0; i < entry_count; i++) {
            if (entries[i].wanted && load_pixels(&entries[i])) painted = 1;
        }
    }
    if (painted) paint(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
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
    if (TextureDump_Shadow) {
        memset(TextureDump_Shadow, 0, (size_t)TEXTURE_SHADOW_WIDTH * SOFT_GPU_HEIGHT * sizeof(*TextureDump_Shadow));
    }
    if (entry_of) memset(entry_of, 0, (size_t)SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT * sizeof(*entry_of));
    free_entries();
    resolved = 0;
    wanted_resolve = wanted_images = 0;
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
