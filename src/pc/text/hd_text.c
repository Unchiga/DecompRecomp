/* HD text (hd_text.h).
 *
 * The retail font is anti-aliased in its indices: 1 is the dark outline, and
 * 2 up to the letter's brightest index is how much of a texel the letter
 * covers, which the text palettes turn into its colour. An HD picture is
 * made the same way at `factor` pixels per texel: the character set in the
 * font glyphs.c sets added characters in, fitted to the box the cell's
 * letter fills (its texels at least half covered) and made as heavy as the
 * retail font's strokes, each pixel's coverage put on the cell's run of indices up to its
 * body's average, and
 * index 1 in a band a texel wide round it. Everything else is index 0, which
 * the palettes make transparent, as in the cells. So the same letter stands
 * in the same place in the same colours, finer. */
#include "hd_text.h"
#include "glyphs.h"
#include "pc/platform/settings.h"
#include "pc/render/soft_gpu.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <stdlib.h>
#include <string.h>

#define CELL 16                              /* texels a side of an atlas cell */
#define SLOTS_ACROSS 32                      /* the atlas: 32 x 32 cells */
#define SLOT_COUNT (SLOTS_ACROSS * SLOTS_ACROSS)
#define TABLE_SIZE 4096                      /* a power of two, well above SLOT_COUNT */
#define MAX_FACTOR 8

typedef struct {
    uint32_t key;   /* bank, page, size, u, v; 0 for a free place */
    uint32_t sum;   /* the cell's pixels when its picture was made */
    int slot;       /* its place in the atlas, kept once given; -1 for none */
    int drawn;      /* the place holds the cell's picture now */
} Entry;

static Entry entries[TABLE_SIZE];
static int entry_count, slots_used, factor, side, first_dirty, last_dirty = -1;
static uint8_t *atlas;
static unsigned generation;

int HdText_Enabled(void)
{
    return Settings_Get(SET_HD_TEXT) != 0;
}

/* The cell's 4-bit indices, and a sum of them. */
static uint32_t read_cell(const uint16_t *words, int page_x, int page_y, int large, int u, int v,
                          unsigned char cell[CELL][CELL])
{
    int width = large ? 16 : 8, height = large ? 16 : 12, x, y;
    uint32_t sum = 2166136261u;
    memset(cell, 0, CELL * CELL);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int tu = u + x;
            uint16_t word = words[((page_y + v + y) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH +
                                  ((page_x + tu / 4) & (SOFT_GPU_WIDTH - 1))];
            cell[y][x] = (unsigned char)((word >> ((tu & 3) * 4)) & 15);
            sum = (sum ^ cell[y][x]) * 16777619u;
        }
    }
    return sum;
}

/* A letter's stroke: the most common length of its runs of set pixels
 * across, or down if those are shorter (a bar's runs across are long, a
 * stem's down). */
static int stroke(const unsigned char *pixels, int width, int height, int pitch, int threshold)
{
    enum { LIMIT = CELL * MAX_FACTOR * 2 };
    int across[LIMIT + 1] = {0}, down[LIMIT + 1] = {0}, x, y, run, best_across = 0, best_down = 0;
    for (y = 0; y < height; y++) {
        for (x = 0, run = 0; x <= width; x++) {
            if (x < width && pixels[y * pitch + x] >= threshold) {
                run++;
            } else if (run) {
                across[run < LIMIT ? run : LIMIT]++;
                run = 0;
            }
        }
    }
    for (x = 0; x < width; x++) {
        for (y = 0, run = 0; y <= height; y++) {
            if (y < height && pixels[y * pitch + x] >= threshold) {
                run++;
            } else if (run) {
                down[run < LIMIT ? run : LIMIT]++;
                run = 0;
            }
        }
    }
    for (x = 1; x <= LIMIT; x++) {
        if (across[x] > across[best_across]) best_across = x;
        if (down[x] > down[best_down]) best_down = x;
    }
    return best_across && (!best_down || best_across < best_down) ? best_across : best_down;
}

/* The texels a cell's letter covers at least half of (index 2 and up is
 * coverage up to its brightest), into `half`; returns the brightest index,
 * under 2 for no letter. */
static int half_covered(const unsigned char cell[CELL][CELL], int cells_high, unsigned char half[CELL][CELL])
{
    int x, y, brightest = 1;
    for (y = 0; y < cells_high; y++) {
        for (x = 0; x < CELL; x++) {
            if (cell[y][x] > brightest) brightest = cell[y][x];
        }
    }
    memset(half, 0, CELL * CELL);
    for (y = 0; y < cells_high; y++) {
        for (x = 0; x < CELL; x++) half[y][x] = cell[y][x] >= 2 && 2 * (cell[y][x] - 1) >= brightest - 1;
    }
    return brightest;
}

/* The glyph's outline at `pixels` high, made `bolder` pixels heavier, as a
 * bitmap in the face's slot. */
static int set(FT_Face face, uint32_t character, int pixels, double bolder)
{
    if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pixels) ||
        FT_Load_Char(face, character, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) ||
        face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        return 0;
    }
    if (bolder > 0) FT_Outline_Embolden(&face->glyph->outline, (FT_Pos)(bolder * 64.0));
    return FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == 0 && face->glyph->bitmap.rows &&
           face->glyph->bitmap.width;
}

/* The picture of `character` for a cell into atlas slot `slot`; 0 when no
 * font sets it or the cell holds no letter. */
static int render(int slot, const unsigned char cell[CELL][CELL], int large, uint32_t character, double weight)
{
    static unsigned char cover[CELL * MAX_FACTOR][CELL * MAX_FACTOR];
    static unsigned short distance[CELL * MAX_FACTOR][CELL * MAX_FACTOR];
    static unsigned char half[CELL][CELL];
    int f = factor, cells_high = large ? 16 : 12, width = (large ? 16 : 8) * f, height = cells_high * f;
    int x, y, top = CELL, bottom = -1, left = CELL, right = -1, brightest = 1, body, total = 0, count = 0;
    int box_w, box_h, fit_w, x0, y0, render_size, measured, rows, columns, pitch;
    uint8_t *origin =
        atlas + (size_t)(slot / SLOTS_ACROSS) * CELL * f * side + (size_t)(slot % SLOTS_ACROSS) * CELL * f;
    FT_Face face = (FT_Face)Glyphs_Face(character);
    double scale;
    if (!face) return 0;
    brightest = half_covered(cell, cells_high, half);
    if (brightest < 2) return 0;
    for (y = 0; y < cells_high; y++) {
        for (x = 0; x < CELL; x++) {
            if (!half[y][x]) continue;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
            if (x < left) left = x;
            if (x > right) right = x;
            total += cell[y][x];
            count++;
        }
    }
    /* A covered pixel takes the index the letter's body has on average (its
     * texels are not all the brightest: the palettes shade them). */
    body = (total + count / 2) / count;
    box_w = (right - left + 1) * f;
    box_h = (bottom - top + 1) * f;
    /* Set once plainly at twice the height, to measure; then heavier to
     * match the cell's strokes, allowing for the squeeze across an M in the
     * 8x12 font takes. */
    render_size = (large ? 14 : 10) * f * 2;
    if (!set(face, character, render_size, 0)) return 0;
    rows = (int)face->glyph->bitmap.rows;
    columns = (int)face->glyph->bitmap.width;
    scale = (double)box_h / rows;
    if (columns * scale > box_w) scale = (double)box_w / columns;
    measured = stroke(face->glyph->bitmap.buffer, columns, rows, face->glyph->bitmap.pitch, 128);
    if (measured) {
        /* The font's strokes (the cell's own where the font's are not
         * known), and half a texel for the edges beyond. */
        double wanted = ((weight > 0 ? weight : (double)stroke(&half[0][0], CELL, cells_high, CELL, 1)) + 0.5) * f;
        double bolder = (wanted - measured * scale) / scale;
        if (bolder > 0 && !set(face, character, render_size, bolder)) return 0;
    }
    /* Into the box: its height, its width at most, centred across it; each
     * pixel the share of it the letter covers. */
    rows = (int)face->glyph->bitmap.rows;
    columns = (int)face->glyph->bitmap.width;
    pitch = face->glyph->bitmap.pitch;
    fit_w = (int)((double)columns * box_h / rows + 0.5);
    if (fit_w > box_w || fit_w < 1) fit_w = box_w;
    x0 = left * f + (box_w - fit_w) / 2;
    y0 = top * f;
    memset(cover, 0, sizeof(cover));
    for (y = 0; y < box_h; y++) {
        int sy0 = y * rows / box_h, sy1 = (y + 1) * rows / box_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (x = 0; x < fit_w; x++) {
            int sx0 = x * columns / fit_w, sx1 = (x + 1) * columns / fit_w, sx, sy;
            unsigned sum = 0, n = 0;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (sy = sy0; sy < sy1; sy++) {
                for (sx = sx0; sx < sx1; sx++) {
                    sum += face->glyph->bitmap.buffer[sy * pitch + sx];
                    n++;
                }
            }
            cover[y0 + y][x0 + x] = (unsigned char)(sum / n);
        }
    }
    /* The outline: pixels within a texel of the letter's half-covered ones
     * (chamfer distance, 3 across and 4 diagonally). */
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            unsigned d = cover[y][x] >= 128 ? 0 : 0xFFFF;
            if (x > 0 && distance[y][x - 1] + 3u < d) d = distance[y][x - 1] + 3u;
            if (y > 0 && distance[y - 1][x] + 3u < d) d = distance[y - 1][x] + 3u;
            if (x > 0 && y > 0 && distance[y - 1][x - 1] + 4u < d) d = distance[y - 1][x - 1] + 4u;
            if (x + 1 < width && y > 0 && distance[y - 1][x + 1] + 4u < d) d = distance[y - 1][x + 1] + 4u;
            distance[y][x] = (unsigned short)d;
        }
    }
    for (y = height - 1; y >= 0; y--) {
        for (x = width - 1; x >= 0; x--) {
            unsigned d = distance[y][x];
            if (x + 1 < width && distance[y][x + 1] + 3u < d) d = distance[y][x + 1] + 3u;
            if (y + 1 < height && distance[y + 1][x] + 3u < d) d = distance[y + 1][x] + 3u;
            if (x + 1 < width && y + 1 < height && distance[y + 1][x + 1] + 4u < d) d = distance[y + 1][x + 1] + 4u;
            if (x > 0 && y + 1 < height && distance[y + 1][x - 1] + 4u < d) d = distance[y + 1][x - 1] + 4u;
            distance[y][x] = (unsigned short)d;
        }
    }
    for (y = 0; y < CELL * f; y++) memset(origin + (size_t)y * side, 0, (size_t)CELL * f);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            /* Coverage on the run 2..body; too little to make index 2 is
             * outline, as the cells' faintest edges are. */
            int index = 1 + (cover[y][x] * (body - 1) + 127) / 255;
            if (index >= 2) origin[(size_t)y * side + x] = (uint8_t)index;
            else if (distance[y][x] <= 3u * (unsigned)f) origin[(size_t)y * side + x] = 1;
        }
    }
    y = (slot / SLOTS_ACROSS) * CELL * f;
    if (y < first_dirty || last_dirty < first_dirty) first_dirty = y;
    if (y + CELL * f - 1 > last_dirty) last_dirty = y + CELL * f - 1;
    return 1;
}

/* The retail font's strokes in texels for a size, the mean of its letters'
 * and digits' (one letter's own is too few runs to go by): measured once
 * for a font page, and kept for the added glyphs, which follow it. */
static double font_weight[2];
static int font_weight_page[2] = {-1, -1};

static double retail_weight(const uint16_t *vram, int page_x, int page_y, int large)
{
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char cell[CELL][CELL], half[CELL][CELL];
    int i, u, v, total = 0, count = 0, cells_high = large ? 16 : 12;
    for (i = 0; letters[i]; i++) {
        int width;
        if (!Glyphs_RetailCell((unsigned char)letters[i], large, &u, &v)) continue;
        read_cell(vram, page_x, page_y, large, u, v, cell);
        if (half_covered((const unsigned char (*)[CELL])cell, cells_high, half) < 2) continue;
        width = stroke(&half[0][0], CELL, cells_high, CELL, 1);
        if (width) {
            total += width;
            count++;
        }
    }
    return count ? (double)total / count : 0;
}

/* A new atlas for another factor: every picture is made again. */
static int make_atlas(int wanted)
{
    free(atlas);
    side = SLOTS_ACROSS * CELL * wanted;
    atlas = calloc((size_t)side * (size_t)side, 1);
    memset(entries, 0, sizeof(entries));
    entry_count = slots_used = 0;
    generation++;
    if (!atlas) {
        factor = side = 0;
        return 0;
    }
    factor = wanted;
    first_dirty = 0;
    last_dirty = side - 1;
    return 1;
}

int HdText_Cell(int bank, int page_x, int page_y, int large, int u, int v, int wanted, int *atlas_u, int *atlas_v)
{
    const uint16_t *words = bank ? SoftGpu_BankPixels(bank) : SoftGpu_Vram();
    unsigned char cell[CELL][CELL];
    uint32_t key, sum;
    unsigned at;
    Entry *entry;
    if (wanted < 2 || wanted > MAX_FACTOR || !words || (bank && bank != GLYPHS_BANK)) return 0;
    if (wanted != factor && !make_atlas(wanted)) return 0;
    key = 0x8000000u | (uint32_t)bank << 22 | (uint32_t)(page_x / 64) << 18 | (uint32_t)(page_y / 256) << 17 |
          (uint32_t)(large != 0) << 16 | (uint32_t)(u & 255) << 8 | (uint32_t)(v & 255);
    sum = read_cell(words, page_x, page_y, large, u, v, cell);
    if (!bank && font_weight_page[large != 0] != page_x / 64 + page_y / 256 * 16) {
        font_weight_page[large != 0] = page_x / 64 + page_y / 256 * 16;
        font_weight[large != 0] = retail_weight(words, page_x, page_y, large);
    }
    for (at = (key * 2654435761u) >> 20;; at = (at + 1) & (TABLE_SIZE - 1)) {
        entry = &entries[at & (TABLE_SIZE - 1)];
        if (entry->key == key || !entry->key) break;
    }
    if (!entry->key) {
        if (entry_count >= TABLE_SIZE / 2) return 0;
        entry_count++;
        entry->key = key;
        entry->slot = -1;
        entry->drawn = 0;
        entry->sum = ~sum;
    }
    if (entry->sum != sum) {
        /* New, or the cell holds something else now (an added glyph made
         * since, another font loaded). */
        uint32_t character = Glyphs_CellCharacter(bank != 0, page_x / 64, large, u, v);
        int slot = entry->slot >= 0 ? entry->slot : slots_used < SLOT_COUNT ? slots_used : -1;
        entry->sum = sum;
        entry->drawn = character && slot >= 0 &&
                       render(slot, (const unsigned char (*)[CELL])cell, large, character, font_weight[large != 0]);
        if (entry->drawn && entry->slot < 0) {
            /* The place stays the cell's when it later holds no letter, to
             * be drawn over when it holds one again. */
            entry->slot = slot;
            slots_used++;
        }
    }
    if (!entry->drawn) return 0;
    *atlas_u = (entry->slot % SLOTS_ACROSS) * CELL;
    *atlas_v = (entry->slot / SLOTS_ACROSS) * CELL;
    return 1;
}

const uint8_t *HdText_Atlas(int *size, int *first, int *last, unsigned *atlas_generation)
{
    *size = side;
    *first = first_dirty;
    *last = last_dirty;
    *atlas_generation = generation;
    first_dirty = side;
    last_dirty = -1;
    return atlas;
}
