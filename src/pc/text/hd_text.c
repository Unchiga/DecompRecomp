/* HD text (hd_text.h).
 *
 * The retail font is anti-aliased in its indices, which the text palettes
 * run from black to the text's colour: the dark outline is the lowest (1,
 * with 2 and 3 in the large font), and above it an index is how bright the
 * texel is. The letters are shaded too, brightest at the top of the cell.
 * An HD picture is made the same way at `factor` pixels per texel, from the
 * character set in the font glyphs.c sets added characters in:
 *
 * - measured once from the page's letters: the font's lines (baseline,
 *   x-height, capitals, ascenders, descenders), its stems and bars, its
 *   outline and each row's shading;
 * - a letter or digit set to those lines, so a kind of letter is as tall
 *   as the others and a small letter is never a capital's height; anything
 *   else to the height its cell's glyph has;
 * - across, where the cell's glyph stands, as wide (a little wider than the
 *   font's proportions at most, and a bare stem like an l keeps them);
 * - its stems and bars made as heavy as the retail font's;
 * - each pixel's coverage onto the run of indices from the outline's to
 *   the row's shading, and the outline's index in a band a texel wide
 *   round it.
 *
 * Everything else is index 0, which the palettes make transparent, as in the
 * cells. So the same letter stands in the same place in the same colours,
 * finer. */
#include "hd_text.h"
#include "glyphs.h"
#include "pc/platform/settings.h"
#include "pc/render/soft_gpu.h"
#include "pc/cards/art.h"
#include "pc/cards/cards.h"
#include "pc/cards/tables.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_BBOX_H
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define CELL 16                              /* texels a side of an atlas cell */
#define SLOTS_ACROSS 32                      /* the atlas: 32 x 32 cells */
#define SLOT_COUNT (SLOTS_ACROSS * SLOTS_ACROSS)
/* The atlas's last rows of cells hold card titles (96 x 14 texels, five
 * to a row of cells), the rows above them the duel's numbers and labels
 * (HUD_*); the glyphs have the rest. */
#define TITLE_ROWS 4
#define HUD_ROWS 4
#define TITLE_TOP (SLOTS_ACROSS - TITLE_ROWS)  /* the first row of titles, in cells */
#define HUD_TOP (TITLE_TOP - HUD_ROWS)
#define GLYPH_SLOTS (HUD_TOP * SLOTS_ACROSS)
#define TITLES_ACROSS (SLOTS_ACROSS * CELL / CARD_TITLE_WIDTH)
#define TITLE_PLACES (TITLES_ACROSS * TITLE_ROWS)
#define TITLE_UPLOADS 16
#define TABLE_SIZE 4096                      /* a power of two, well above SLOT_COUNT */
#define MAX_FACTOR 8
/* A retail texel at this share of its row's brightest is taken as wholly
 * covered: the large font's strokes are shaded across as well as down, so
 * the middle of a stroke is often well below the brightest. */
#define SATURATE 0.7
/* A covered pixel's index: this share of the way from the row's average
 * stroke to its brightest. The brightest alone makes the letters paler than
 * the cells, whose strokes are mostly edge. */
#define BRIGHT 0.5

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

/* Where the game put card titles in VRAM (func_800289BC), and the words it
 * put there; and the titles drawn in the atlas. */
typedef struct {
    int card, x, y;
    uint32_t sum;
} TitleUpload;
typedef struct {
    int card;       /* 0 for a free place */
    uint32_t name;  /* a hash of the name it was drawn from */
    unsigned used;  /* when it was last asked for */
} Title;
static TitleUpload title_uploads[TITLE_UPLOADS];
static unsigned title_upload_next, title_clock;
static Title titles[TITLE_PLACES];

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

/* What the retail font is like at a size, measured once for its page from
 * its letters and digits (one cell is too little to go by) and kept for the
 * added glyphs, which follow it. Rows and columns are in texels. */
typedef struct {
    int page;                           /* -1 before it is measured */
    int outline;                        /* the index round the letters (commonest next to nothing) */
    int dark;                           /* the highest index mostly next to nothing: no coverage */
    double shade[CELL];                 /* each row's brightest index */
    double body[CELL];                  /* and its average over the letters' strokes */
    double base, cap, x_height, ascender, descender; /* the rows of those edges */
    double stem, bar;                   /* texels across a stem, down a bar */
    /* A sheet of the duel's digits (the HUD section below) has no shading
     * down its cells: its indices are a ramp from the outline (ramp[0]) to
     * the fill, level[] each index's step on it (0 off it). The font's
     * pages have none (ramp_n 0). */
    int ramp_n;
    unsigned char ramp[8];
    unsigned char level[256];
} Retail;

static Retail retail[2] = {{.page = -1}, {.page = -1}};

/* The same of a font, in pixels up from the baseline at FONT_SIZE. */
#define FONT_SIZE 64
typedef struct {
    void *face;
    int ok;
    double cap, x_height, ascender, descender, stem, bar;
} Font;

static Font fonts[8];

/* A letter's edges to a fraction of a texel. */
typedef struct {
    double left, right, top, bottom;
} Box;

static int by_value(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *values, int count)
{
    if (!count) return 0;
    qsort(values, (size_t)count, sizeof(values[0]), by_value);
    return count & 1 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2;
}

/* How much of a texel the letter covers: the outline's indices are none,
 * half the row's brightest (the letters are shaded down the cell, and the
 * large font across its strokes too) and up all of it, and between a
 * share. */
static double coverage(const Retail *r, const unsigned char cell[CELL][CELL], int x, int y)
{
    double c;
    if (r->ramp_n) return r->level[cell[y][x]] / (double)(r->ramp_n - 1);
    c = cell[y][x] > r->dark ? (cell[y][x] - r->dark) / (SATURATE * (r->shade[y] - r->dark)) : 0;
    return c > 1 ? 1 : c;
}

/* Where a cell's letter is: an edge row or column is covered as far into
 * it as the letter reaches. Returns 0 for no letter. */
static int extents(const Retail *r, const unsigned char cell[CELL][CELL], int cells_high, Box *box)
{
    double rows[CELL] = {0}, columns[CELL] = {0};
    int x, y, top = -1, bottom = -1, left = -1, right = -1;
    for (y = 0; y < cells_high; y++) {
        for (x = 0; x < CELL; x++) {
            double c = coverage(r, cell, x, y);
            if (c > rows[y]) rows[y] = c;
            if (c > columns[x]) columns[x] = c;
        }
    }
    for (y = 0; y < cells_high; y++) {
        if (rows[y] <= 0) continue;
        if (top < 0) top = y;
        bottom = y;
    }
    for (x = 0; x < CELL; x++) {
        if (columns[x] <= 0) continue;
        if (left < 0) left = x;
        right = x;
    }
    if (top < 0) return 0;
    box->top = top + 1 - rows[top];
    box->bottom = bottom + rows[bottom];
    box->left = left + 1 - columns[left];
    box->right = right + columns[right];
    return 1;
}

/* The widths of the runs of coverage across the rows (or down the
 * columns), each the sum of its coverage, onto `list`. */
static void add_runs(const float *cover, int width, int height, int down, double *list, int *count, int limit)
{
    int i, j, across = down ? width : height, along = down ? height : width;
    for (i = 0; i < across; i++) {
        double run = 0;
        for (j = 0; j <= along; j++) {
            float c = j < along ? (down ? cover[j * width + i] : cover[i * width + j]) : 0;
            if (c > 0) {
                run += c;
            } else if (run > 0) {
                if (*count < limit) list[(*count)++] = run;
                run = 0;
            }
        }
    }
}

enum { RUN_LIMIT = 16384 };
static double run_list[RUN_LIMIT];

/* Letters to measure by: flat bottoms, flat tops, x-height tops,
 * ascenders, descenders, stems across, bars down. */
static const char *const measured_by[] = {"ABDEFHIKLMNPRTXZ", "BDEFHIKLMNPRTZ", "uvwxz", "bdhkl", "pq",
                                          "HILTUdhilnpqu", "EFHLTZ"};

static void measure_retail(Retail *r, const uint16_t *words, int page_x, int page_y, int large)
{
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char cell[CELL][CELL];
    float cover[CELL * CELL];
    double *edges[5] = {&r->base, &r->cap, &r->x_height, &r->ascender, &r->descender};
    int i, k, x, y, u, v, cells_high = large ? 16 : 12, count;
    int border[16] = {0}, all[16] = {0}, rows[CELL][16] = {{0}};
    for (y = 0; y < CELL; y++) r->shade[y] = 0;
    for (i = 0; letters[i]; i++) {
        if (!Glyphs_RetailCell((unsigned char)letters[i], large, &u, &v)) continue;
        read_cell(words, page_x, page_y, large, u, v, cell);
        for (y = 0; y < cells_high; y++) {
            for (x = 0; x < CELL; x++) {
                if (cell[y][x] > r->shade[y]) r->shade[y] = cell[y][x];
                all[cell[y][x]]++;
                rows[y][cell[y][x]]++;
                /* The outline: the indices next to nothing. */
                if (cell[y][x] && ((x > 0 && !cell[y][x - 1]) || (x + 1 < CELL && !cell[y][x + 1]) ||
                                   (y > 0 && !cell[y - 1][x]) || (y + 1 < CELL && !cell[y + 1][x]))) {
                    border[cell[y][x]]++;
                }
            }
        }
    }
    for (r->outline = r->dark = 1, i = 2; i < 5; i++) {
        if (border[i] > border[r->outline]) r->outline = i;
        if (border[i] * 2 > all[i] && r->dark == i - 1) r->dark = i;
    }
    for (y = 0; y < CELL; y++) {
        int total = 0, texels = 0;
        for (i = r->dark + 1; i < 16; i++) total += rows[y][i] * i, texels += rows[y][i];
        r->body[y] = texels ? (double)total / texels : 0;
    }
    /* Rows no letter reaches take their neighbours' shading. */
    for (y = 1; y < CELL; y++) {
        if (r->shade[y] < 3) r->shade[y] = r->shade[y - 1], r->body[y] = r->body[y - 1];
    }
    for (y = CELL - 2; y >= 0; y--) {
        if (r->shade[y] < 3) {
            r->shade[y] = r->shade[y + 1] >= 3 ? r->shade[y + 1] : 15;
            r->body[y] = r->body[y + 1] >= 3 ? r->body[y + 1] : 15;
        }
    }
    for (k = 0; k < 7; k++) {
        count = 0;
        for (i = 0; measured_by[k][i]; i++) {
            Box box;
            if (!Glyphs_RetailCell((unsigned char)measured_by[k][i], large, &u, &v)) continue;
            read_cell(words, page_x, page_y, large, u, v, cell);
            if (k >= 5) {
                for (y = 0; y < CELL; y++) {
                    for (x = 0; x < CELL; x++) cover[y * CELL + x] = y < cells_high ? (float)coverage(r, cell, x, y) : 0;
                }
                add_runs(cover, CELL, CELL, k == 6, run_list, &count, RUN_LIMIT);
            } else if (extents(r, cell, cells_high, &box)) {
                run_list[count++] = k == 0 || k == 4 ? box.bottom : box.top;
            }
        }
        if (k < 5) *edges[k] = median(run_list, count);
        else if (k == 5) r->stem = median(run_list, count);
        else r->bar = median(run_list, count);
    }
    /* A page with no letters on it yet (blank, or half loaded) is measured
     * again next time. */
    r->page = r->cap < r->base && r->x_height < r->base && r->stem > 0 ? page_x / 64 + page_y / 256 * 16 : -1;
}

/* The character's outline at FONT_SIZE in the face's slot. */
static int load(FT_Face face, uint32_t character)
{
    return FT_Set_Pixel_Sizes(face, 0, FONT_SIZE) == 0 &&
           FT_Load_Char(face, character, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0 &&
           face->glyph->format == FT_GLYPH_FORMAT_OUTLINE && face->glyph->outline.n_points > 0;
}

/* The character loaded at FONT_SIZE, outline or none (a space): its
 * advance in pixels, or -1 when the face cannot load it. */
static double advance(FT_Face face, uint32_t character)
{
    if (FT_Set_Pixel_Sizes(face, 0, FONT_SIZE) ||
        FT_Load_Char(face, character, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
        return -1;
    }
    return face->glyph->advance.x / 64.0;
}

static const Font *measure_font(FT_Face face)
{
    static int next;
    enum { SIDE = FONT_SIZE * 2 };
    static float cover[SIDE * SIDE];
    double *edges[5];
    Font *font;
    int i, k, x, y, count;
    for (i = 0; i < (int)(sizeof(fonts) / sizeof(fonts[0])); i++) {
        if (fonts[i].face == face) return fonts[i].ok ? &fonts[i] : NULL;
    }
    font = &fonts[next];
    next = (next + 1) % (int)(sizeof(fonts) / sizeof(fonts[0]));
    memset(font, 0, sizeof(*font));
    font->face = face;
    edges[0] = NULL;
    edges[1] = &font->cap;
    edges[2] = &font->x_height;
    edges[3] = &font->ascender;
    edges[4] = &font->descender;
    for (k = 1; k < 7; k++) {
        count = 0;
        for (i = 0; measured_by[k][i]; i++) {
            FT_BBox bbox;
            FT_Bitmap *bitmap;
            if (!load(face, (unsigned char)measured_by[k][i])) continue;
            if (k < 5) {
                FT_Outline_Get_BBox(&face->glyph->outline, &bbox);
                run_list[count++] = (k == 4 ? bbox.yMin : bbox.yMax) / 64.0;
                continue;
            }
            if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) continue;
            bitmap = &face->glyph->bitmap;
            if ((int)bitmap->width > SIDE || (int)bitmap->rows > SIDE) continue;
            for (y = 0; y < (int)bitmap->rows; y++) {
                for (x = 0; x < (int)bitmap->width; x++) {
                    cover[y * (int)bitmap->width + x] = bitmap->buffer[y * bitmap->pitch + x] / 255.0f;
                }
            }
            add_runs(cover, (int)bitmap->width, (int)bitmap->rows, k == 6, run_list, &count, RUN_LIMIT);
        }
        if (!count) return NULL;
        if (k < 5) *edges[k] = median(run_list, count);
        else if (k == 5) font->stem = median(run_list, count);
        else font->bar = median(run_list, count);
    }
    font->ok = font->cap > font->x_height && font->x_height > 0 && font->ascender > 0 && font->descender < 0 &&
               font->stem > 0 && font->bar > 0;
    return font->ok ? font : NULL;
}

/* Piecewise-linear through the points (from ascending), carried on past
 * the ends. */
static double through(const double *from, const double *to, int count, double value)
{
    int i = 0;
    while (i + 2 < count && value > from[i + 1]) i++;
    return to[i] + (value - from[i]) * (to[i + 1] - to[i]) / (from[i + 1] - from[i]);
}

/* A rectangle (in pixels) filled into the coverage, its edges shared. */
static void fill(unsigned char cover[CELL * MAX_FACTOR][CELL * MAX_FACTOR], double x0, double y0, double x1,
                 double y1, int width, int height)
{
    int x, y;
    for (y = (int)y0; y < height && y < y1; y++) {
        double h = (y + 1 < y1 ? y + 1 : y1) - (y > y0 ? y : y0);
        if (y < 0 || h <= 0) continue;
        for (x = (int)x0; x < width && x < x1; x++) {
            double w = (x + 1 < x1 ? x + 1 : x1) - (x > x0 ? x : x0), c;
            if (x < 0 || w <= 0) continue;
            c = cover[y][x] + w * h * 255;
            cover[y][x] = (unsigned char)(c > 255 ? 255 : c);
        }
    }
}

/* Rows y0 to y1 (atlas pixels) of the atlas have changed. */
static void changed(int y0, int y1)
{
    if (y0 < first_dirty || last_dirty < first_dirty) first_dirty = y0;
    if (y1 > last_dirty) last_dirty = y1;
}

/* The picture of `character` for a cell `cells_high` texels high into the
 * atlas at cell (at_column, at_row) of CELL texels; 0 when no font sets it or the
 * cell holds no letter. */
static int render_at(int at_column, int at_row, const unsigned char cell[CELL][CELL], int cells_high, uint32_t character,
                     const Retail *r)
{
    static unsigned char cover[CELL * MAX_FACTOR][CELL * MAX_FACTOR];
    static unsigned short distance[CELL * MAX_FACTOR][CELL * MAX_FACTOR];
    int f = factor, width = CELL * f, height = cells_high * f;
    int x, y, i, n = 0, main = 0, pass, upper = character < 128 && (isupper((int)character) || isdigit((int)character));
    int lower = character < 128 && islower((int)character);
    uint8_t *origin = atlas + (size_t)at_row * CELL * f * side + (size_t)at_column * CELL * f;
    FT_Face face = (FT_Face)Glyphs_Face(character);
    const Font *font = face ? measure_font(face) : NULL;
    double from[5], to[5], tops[5], room, sx, sv, ex = 0, ey = 0, centre_f, centre_r, stem_f, width_f;
    FT_BBox bbox;
    FT_Outline *outline;
    FT_Bitmap bitmap;
    Box box;
    if (!font || !extents(r, cell, cells_high, &box) || !load(face, character)) return 0;
    outline = &face->glyph->outline;
    FT_Outline_Get_BBox(outline, &bbox);
    width_f = (bbox.xMax - bbox.xMin) / 64.0;
    centre_f = (bbox.xMax + bbox.xMin) / 128.0;
    centre_r = (box.left + box.right) / 2;
    if (upper || lower) {
        /* The lines, up the font; tops[] says which are tops of strokes.
         * A descender or ascender line either font lacks (or has out of
         * order) is left out. */
        double top_f = lower ? font->x_height : font->cap, top_r = lower ? r->x_height : r->cap;
        if (font->descender < -1 && r->descender > r->base + 0.25) {
            from[n] = font->descender, to[n] = r->descender, tops[n++] = 0;
        }
        main = n;
        from[n] = 0, to[n] = r->base, tops[n++] = 0;
        from[n] = top_f, to[n] = top_r, tops[n++] = 1;
        if (font->ascender > top_f + 1 && r->ascender < top_r - 0.25) {
            from[n] = font->ascender, to[n] = r->ascender, tops[n++] = 1;
        }
    } else {
        from[n] = bbox.yMin / 64.0, to[n] = box.bottom, tops[n++] = 0;
        from[n] = bbox.yMax / 64.0, to[n] = box.top, tops[n++] = 1;
    }
    if (from[main + 1] <= from[main] || to[main + 1] >= to[main]) return 0;
    /* Heavier (or lighter) by ex across and ey down puts half of each
     * beyond every edge: the lines move in by that much, and the width
     * allows for it. Twice, as the scale moves with them. */
    for (pass = 0; pass < 2; pass++) {
        /* The scale from the baseline to the x-height or the capitals (the
         * foot to the head of anything else). */
        sv = (to[main] - to[main + 1] - ey) / (from[main + 1] - from[main]);
        ey = r->bar - font->bar * sv;
        if (ey < -font->bar * sv / 2) ey = -font->bar * sv / 2;
    }
    for (i = 0; i < n; i++) to[i] += tops[i] ? ey / 2 : -ey / 2;
    if (sv <= 0 || to[main + 1] >= to[main]) return 0;
    /* As wide as the cell's glyph, stems and all, but no more than a
     * quarter wider than the font's own proportions; a bare stem, like an
     * l, keeps them, narrowed only to fit. */
    stem_f = font->stem < width_f ? font->stem : width_f;
    room = box.right - box.left;
    if (width_f - stem_f > width_f / 4) {
        sx = (room - r->stem) / (width_f - stem_f);
        if (sx > sv * 1.25) sx = sv * 1.25;
        if (sx <= 0) sx = room / width_f;
    } else {
        sx = sv;
        if (width_f * sx + r->stem - stem_f * sx > room) sx = room / width_f;
    }
    ex = r->stem - stem_f * sx;
    if (ex < -stem_f * sx / 2) ex = -stem_f * sx / 2;
    /* Into the cell's pixels: across about the centre, down along the
     * lines (the outline's y is up from the picture's foot). */
    for (i = 0; i < outline->n_points; i++) {
        double px = outline->points[i].x / 64.0, py = outline->points[i].y / 64.0;
        double column = centre_r + (px - centre_f) * sx, row = through(from, to, n, py);
        outline->points[i].x = (FT_Pos)(column * f * 64);
        outline->points[i].y = (FT_Pos)((cells_high - row) * f * 64);
    }
    FT_Outline_EmboldenXY(outline, (FT_Pos)(ex * f * 64), (FT_Pos)(ey * f * 64));
    memset(cover, 0, sizeof(cover));
    memset(&bitmap, 0, sizeof(bitmap));
    bitmap.rows = (unsigned)height;
    bitmap.width = (unsigned)width;
    bitmap.pitch = CELL * MAX_FACTOR;
    bitmap.buffer = &cover[0][0];
    bitmap.num_grays = 256;
    bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
    if (FT_Outline_Get_Bitmap(face->glyph->library, outline, &bitmap)) return 0;
    if (character == 'I' && width_f < font->stem * 1.8 && box.right - box.left > r->stem * 1.5) {
        /* The retail I has serifs, which tell it from an l; a font's bare
         * stem gets them. */
        fill(cover, box.left * f, box.top * f, box.right * f, (box.top + r->bar) * f, width, height);
        fill(cover, box.left * f, (box.bottom - r->bar) * f, box.right * f, box.bottom * f, width, height);
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
        /* The row's shading, between the texel rows' either side. */
        double at = (y + 0.5) / f - 0.5, shade;
        int line = at < 0 ? 0 : (int)at, next = line + 1 < cells_high ? line + 1 : line;
        double t = at < 0 ? 0 : at - line;
        shade = r->shade[line] + (r->shade[next] - r->shade[line]) * t;
        shade = shade * BRIGHT + (1 - BRIGHT) * (r->body[line] + (r->body[next] - r->body[line]) * t);
        if (r->ramp_n) {
            /* A digit sheet: the nearest step of its ramp, the outline
             * round it as for the text. */
            for (x = 0; x < width; x++) {
                int step = (int)(cover[y][x] * (r->ramp_n - 1) / 255.0 + 0.5);
                if (step > 0) origin[(size_t)y * side + x] = r->ramp[step];
                else if (distance[y][x] <= 3u * (unsigned)f) origin[(size_t)y * side + x] = r->ramp[0];
            }
            continue;
        }
        for (x = 0; x < width; x++) {
            /* Coverage on the run from the outline's index to the shade;
             * too little to rise above it is outline, as the cells'
             * faintest edges are. */
            int index = r->dark + (int)(cover[y][x] * (shade - r->dark) / 255 + 0.5);
            if (index > r->dark) origin[(size_t)y * side + x] = (uint8_t)index;
            else if (distance[y][x] <= 3u * (unsigned)f) origin[(size_t)y * side + x] = (uint8_t)r->outline;
        }
    }
    changed(at_row * CELL * f, (at_row + 1) * CELL * f - 1);
    return 1;
}

static int render(int slot, const unsigned char cell[CELL][CELL], int large, uint32_t character, const Retail *r)
{
    return render_at(slot % SLOTS_ACROSS, slot / SLOTS_ACROSS, cell, large ? 16 : 12, character, r);
}

/* A new atlas for another factor: every picture is made again. */
static int make_atlas(int wanted)
{
    free(atlas);
    side = SLOTS_ACROSS * CELL * wanted;
    atlas = calloc((size_t)side * (size_t)side, 1);
    memset(entries, 0, sizeof(entries));
    memset(titles, 0, sizeof(titles));
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
    int seen = 1;
    Entry *entry;
    if (wanted < 2 || wanted > MAX_FACTOR || !words || (bank && bank != GLYPHS_BANK)) return 0;
    if (wanted != factor && !make_atlas(wanted)) return 0;
    key = 0x8000000u | (uint32_t)bank << 22 | (uint32_t)(page_x / 64) << 18 | (uint32_t)(page_y / 256) << 17 |
          (uint32_t)(large != 0) << 16 | (uint32_t)(u & 255) << 8 | (uint32_t)(v & 255);
    sum = read_cell(words, page_x, page_y, large, u, v, cell);
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
        seen = 0;
    }
    /* Measured again when a retail cell changes too: another font loaded
     * on the page. */
    if (!bank && (retail[large != 0].page != page_x / 64 + page_y / 256 * 16 || (seen && entry->sum != sum))) {
        measure_retail(&retail[large != 0], words, page_x, page_y, large);
    }
    /* Added glyphs follow the retail font, so they wait until it is
     * measured. */
    if (retail[large != 0].page < 0) return 0;
    if (entry->sum != sum) {
        /* New, or the cell holds something else now (an added glyph made
         * since, another font loaded). */
        uint32_t character = Glyphs_CellCharacter(bank != 0, page_x / 64, large, u, v);
        int slot = entry->slot >= 0 ? entry->slot : slots_used < GLYPH_SLOTS ? slots_used : -1;
        entry->sum = sum;
        entry->drawn = character && slot >= 0 &&
                       render(slot, (const unsigned char (*)[CELL])cell, large, character, &retail[large != 0]);
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

/* --- card titles ------------------------------------------------------ */

static uint32_t title_sum(const uint16_t *words, int x, int y)
{
    uint32_t sum = 2166136261u;
    int i, j;
    for (j = 0; j < CARD_TITLE_HEIGHT; j++) {
        for (i = 0; i < CARD_TITLE_WIDTH / 4; i++) {
            sum = (sum ^ words[((y + j) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((x + i) & (SOFT_GPU_WIDTH - 1))]) *
                  16777619u;
        }
    }
    return sum;
}

void HdText_TitleUploaded(int card, int x, int y)
{
    const uint16_t *words = SoftGpu_Vram();
    TitleUpload *upload = NULL;
    unsigned i;
    if (!words) return;
    for (i = 0; i < TITLE_UPLOADS; i++) {
        if (title_uploads[i].card && title_uploads[i].x == x && title_uploads[i].y == y) upload = &title_uploads[i];
    }
    if (!upload) upload = &title_uploads[title_upload_next++ % TITLE_UPLOADS];
    upload->x = x;
    upload->y = y;
    upload->sum = title_sum(words, x, y);
    upload->card = card;
}

int HdText_Title(int page_x, int page_y, int u, int v, int wanted, int *atlas_u, int *atlas_v, int *title_u,
                 int *title_v)
{
    const uint16_t *words = SoftGpu_Vram();
    int vx = page_x + (u & 255) / 4, vy = page_y + (v & 255), i, place = -1, oldest = 0;
    const TitleUpload *upload = NULL;
    char name[128];
    uint32_t hash = 2166136261u;
    const char *c;
    if (wanted < 2 || wanted > MAX_FACTOR || !words) return 0;
    for (i = 0; i < TITLE_UPLOADS; i++) {
        const TitleUpload *at = &title_uploads[i];
        if (at->card && vx >= at->x && vx < at->x + CARD_TITLE_WIDTH / 4 && vy >= at->y &&
            vy < at->y + CARD_TITLE_HEIGHT) {
            upload = at;
        }
    }
    /* Words written over since are not a title any more. */
    if (!upload || title_sum(words, upload->x, upload->y) != upload->sum) return 0;
    if (!Cards_NameUtf8(upload->card, name, sizeof(name))) return 0;
    for (c = name; *c; c++) hash = (hash ^ (unsigned char)*c) * 16777619u;
    if (wanted != factor && !make_atlas(wanted)) return 0;
    title_clock++;
    for (i = 0; i < TITLE_PLACES; i++) {
        if (titles[i].card == upload->card && titles[i].name == hash) place = i;
        if (titles[i].used < titles[oldest].used) oldest = i;
    }
    if (place < 0) {
        /* The place asked for longest ago: few titles show at once. */
        int x0 = (oldest % TITLES_ACROSS) * CARD_TITLE_WIDTH * factor;
        int y0 = (TITLE_TOP + oldest / TITLES_ACROSS) * CELL * factor, y;
        for (y = 0; y < CELL * factor; y++) memset(atlas + (size_t)(y0 + y) * side + x0, 0, (size_t)CARD_TITLE_WIDTH * factor);
        titles[oldest].card = 0;
        if (!CardArt_TitlePicture(name, factor, atlas + (size_t)y0 * side + x0, side)) return 0;
        if (y0 < first_dirty || last_dirty < first_dirty) first_dirty = y0;
        if (y0 + CELL * factor - 1 > last_dirty) last_dirty = y0 + CELL * factor - 1;
        titles[oldest].card = upload->card;
        titles[oldest].name = hash;
        place = oldest;
    }
    titles[place].used = title_clock;
    *atlas_u = (place % TITLES_ACROSS) * CARD_TITLE_WIDTH;
    *atlas_v = (TITLE_TOP + place / TITLES_ACROSS) * CELL;
    *title_u = (upload->x - page_x) * 4;
    *title_v = upload->y - page_y;
    return 1;
}

/* --- the duel's numbers and labels -------------------------------------- */

/* A texel of a 4-bit (depth 0) or 8-bit (1) page. */
static int texel(const uint16_t *words, int depth, int page_x, int page_y, int u, int v)
{
    uint16_t word = words[((page_y + v) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH +
                          ((page_x + (depth ? u / 2 : u / 4)) & (SOFT_GPU_WIDTH - 1))];
    return depth ? (word >> ((u & 1) * 8)) & 255 : (word >> ((u & 3) * 4)) & 15;
}

/* A palette entry as 0-255 red, green, blue. */
static void colour(const uint16_t *words, int clut_x, int clut_y, int index, double rgb[3])
{
    uint16_t word = words[(clut_y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + ((clut_x + index) & (SOFT_GPU_WIDTH - 1))];
    rgb[0] = (word & 31) * 8.0;
    rgb[1] = ((word >> 5) & 31) * 8.0;
    rgb[2] = ((word >> 10) & 31) * 8.0;
}

/* The ramp from index `from` to index `to` through the indices between
 * them: those of `used` whose colours lie on the way (near the line from
 * one colour to the other, in order along it). A colour off the way, like
 * the purple the duel's digits have in a few corners, is left out. */
static int make_ramp(const uint16_t *words, int clut_x, int clut_y, int from, int to, const int *used, int count,
                     unsigned char *ramp, int room)
{
    double a[3], b[3], ab[3], length2, along[256];
    int i, j, k, n = 1, middle[256], middles = 0;
    colour(words, clut_x, clut_y, from, a);
    colour(words, clut_x, clut_y, to, b);
    for (k = 0; k < 3; k++) ab[k] = b[k] - a[k];
    length2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    if (length2 <= 0) return 0;
    for (i = 0; i < count; i++) {
        double c[3], t = 0, off = 0;
        if (used[i] == from || used[i] == to) continue;
        colour(words, clut_x, clut_y, used[i], c);
        for (k = 0; k < 3; k++) t += (c[k] - a[k]) * ab[k];
        t /= length2;
        for (k = 0; k < 3; k++) {
            double d = c[k] - (a[k] + t * ab[k]);
            off += d * d;
        }
        if (t <= 0 || t >= 1 || off > 0.02 * length2) continue;
        along[used[i]] = t;
        /* In order along the way. */
        for (j = middles; j > 0 && along[middle[j - 1]] > t; j--) middle[j] = middle[j - 1];
        middle[j] = used[i];
        middles++;
    }
    ramp[0] = (unsigned char)from;
    for (i = 0; i < middles && n < room - 1; i++) ramp[n++] = (unsigned char)middle[i];
    ramp[n++] = (unsigned char)to;
    return n;
}

/* The sheets the duel draws numbers from: digit d is the cell at
 * u + d * step, v. Each is measured from its own ten digits: the outline
 * (the index round them), the fill (the commonest inside), the ramp between
 * and the lines of their feet and heads, as the font's pages are. */
typedef struct {
    int depth, page_x, page_y, clut_x, clut_y; /* where, and the palette it is measured through */
    int clut_rows;                             /* the palette rows it is drawn through, from clut_y */
    int u, v, step, width, height;             /* the digits' cells */
    int column, row;                           /* the first digit's picture: its cell in the HUD rows */
    uint32_t sum;                              /* the digits' texels when measured */
    unsigned made;                             /* the atlas generation the pictures are of */
    int ok;
    signed char drawn[10];                     /* 1 drawn, -1 none, 0 not yet */
    Retail r;
} Sheet;

/* The duel's sheet is read through six palette rows (the hand, the card
 * bar, the dimmed side), the menus' through one. */

static Sheet sheets[] = {
    /* The life points, the deck counts and the field cards' ATK and DEF
     * (duel_draw_status_numbers.c, duel_card_frame_draw.c): 8-bit. */
    {1, 896, 256, 256, 241, 6, 0, 0x58, 8, 8, 8, 4, 0},
    /* The menus' (duel_card_stat_display.c, the deck builder's list). */
    {0, 704, 0, 656, 250, 1, 0x80, 0x70, 8, 8, 8, 4, 1},
    /* The field cards' larger numbers, 12 x 16 (duel_card_frame_draw.c). */
    {1, 896, 256, 256, 241, 6, 0, 0x70, 12, 12, 16, 4, 3},
    /* A second set of small ones above the first. */
    {1, 896, 256, 256, 241, 6, 8, 0x50, 8, 8, 8, 14, 3},
};

static uint32_t read_digit(const uint16_t *words, const Sheet *s, int d, unsigned char cell[CELL][CELL],
                           uint32_t sum)
{
    int x, y;
    memset(cell, 0, CELL * CELL);
    for (y = 0; y < s->height; y++) {
        for (x = 0; x < s->width; x++) {
            cell[y][x] = (unsigned char)texel(words, s->depth, s->page_x, s->page_y, s->u + d * s->step + x, s->v + y);
            sum = (sum ^ cell[y][x]) * 16777619u;
        }
    }
    return sum;
}

static int measure_sheet(Sheet *s, const uint16_t *words)
{
    unsigned char cell[CELL][CELL];
    int border[256] = {0}, inside[256] = {0}, used[256], count = 0, outline = 0, fill = 0, d, x, y, i;
    double tops[10], bottoms[10];
    float cover[CELL * CELL];
    Retail *r = &s->r;
    memset(r, 0, sizeof(*r));
    for (d = 0; d < 10; d++) {
        read_digit(words, s, d, cell, 0);
        for (y = 0; y < CELL; y++) {
            for (x = 0; x < CELL; x++) {
                int c = cell[y][x];
                if (!c) continue;
                if ((x > 0 && !cell[y][x - 1]) || (x + 1 < CELL && !cell[y][x + 1]) || (y > 0 && !cell[y - 1][x]) ||
                    (y + 1 < CELL && !cell[y + 1][x]) || !x || !y) {
                    border[c]++;
                } else {
                    inside[c]++;
                }
            }
        }
    }
    for (i = 1; i < 256; i++) {
        if (border[i] + inside[i]) used[count++] = i;
        if (border[i] > border[outline]) outline = i;
    }
    for (i = 1; i < 256; i++) {
        if (i != outline && inside[i] > inside[fill]) fill = i;
    }
    if (!outline || !fill) return 0;
    r->ramp_n = make_ramp(words, s->clut_x, s->clut_y, outline, fill, used, count, r->ramp, 8);
    if (r->ramp_n < 2) return 0;
    for (i = 0; i < r->ramp_n; i++) r->level[r->ramp[i]] = (unsigned char)i;
    r->outline = r->dark = outline;
    /* Their feet and heads, and their strokes across and down. */
    for (d = 0; d < 10; d++) {
        Box box;
        read_digit(words, s, d, cell, 0);
        if (!extents(r, cell, s->height, &box)) return 0;
        tops[d] = box.top;
        bottoms[d] = box.bottom;
    }
    r->cap = median(tops, 10);
    r->base = median(bottoms, 10);
    /* No lower-case lines: render_at uses a digit's own. */
    r->x_height = r->ascender = r->cap;
    r->descender = r->base;
    for (i = 0; i < 2; i++) {
        int runs = 0;
        for (d = 0; d < 10; d++) {
            read_digit(words, s, d, cell, 0);
            for (y = 0; y < CELL; y++) {
                for (x = 0; x < CELL; x++) cover[y * CELL + x] = y < s->height ? (float)coverage(r, cell, x, y) : 0;
            }
            add_runs(cover, CELL, CELL, i, run_list, &runs, RUN_LIMIT);
        }
        if (!runs) return 0;
        if (i) r->bar = median(run_list, runs);
        else r->stem = median(run_list, runs);
    }
    return r->cap < r->base && r->stem > 0 && r->bar > 0;
}

/* The life-point panel (duel_init_scene.c): one 64 x 40 sprite of both
 * sides' boxes, their LP, COM and YOU drawn in, from the duel's common
 * page. Its picture is the panel's texels made `factor` times larger with
 * the labels set anew in the font, where the retail panel is (its words
 * hash to PANEL_SUM); any other panel, a mod's, is left as it is. */
#define PANEL_PAGE_X 704
#define PANEL_PAGE_Y 0
#define PANEL_CLUT_X 736
#define PANEL_CLUT_Y 252
#define PANEL_U 128
#define PANEL_V 128
#define PANEL_W 64
#define PANEL_H 40
#define PANEL_SUM 0x14e82665u

/* Each label's box in the panel (first texel to one past the last): the
 * background round the retail letters, and the letters. */
static const struct {
    int x0, y0, x1, y1;
    const char *text;
} panel_labels[] = {
    {16, 3, 29, 8, "LP"},
    {2, 11, 25, 17, "COM"},
    {2, 23, 25, 29, "YOU"},
    {16, 32, 29, 37, "LP"},
};

static unsigned panel_made;
static int panel_ok;

static uint32_t panel_sum(const uint16_t *words)
{
    uint32_t sum = 2166136261u;
    int x, y;
    for (y = 0; y < PANEL_H; y++) {
        for (x = 0; x < PANEL_W / 4; x++) {
            sum = (sum ^ words[((PANEL_PAGE_Y + PANEL_V + y) & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH +
                               ((PANEL_PAGE_X + PANEL_U / 4 + x) & (SOFT_GPU_WIDTH - 1))]) *
                  16777619u;
        }
    }
    return sum;
}

/* Text set anew over a sprite's lettering: `texels` (the sprite's, `pitch`
 * across) are what the retail letters are measured from, the box x0, y0 to
 * x1, y1 (texels, one past the last) is cleared to `ramp[0]` in the
 * picture at `origin` and the text is set there. As high as the retail
 * letters from their baseline (the commonest foot of their columns) to
 * their tops, at most as wide, centred, as heavy (no more than a fifth of
 * their height: a pixel font's two-texel strokes would blot a font's), in
 * the ramp's colours by coverage. `outlined`: ramp[0] is an outline a texel
 * wide round the letters, the rest of the box clear (0). `shadow`: that
 * index a texel right of and below the letters, where they are not.
 * `span`: across the whole box (a texel in from each side) rather than as
 * wide as the retail letters, which still give the height and weight. */
static int set_text(void *face_pointer, const unsigned char *texels, int pitch, int x0, int y0, int x1, int y1,
                    const char *text, const unsigned char *ramp, int n, int outlined, int shadow, int span,
                    uint8_t *origin)
{
    enum { WIDE = 64 * MAX_FACTOR, HIGH = 16 * MAX_FACTOR };
    static unsigned char cover[HIGH][WIDE], one[HIGH][WIDE];
    static unsigned short distance[HIGH][WIDE];
    static float strokes[16 * 64];
    FT_Face face = (FT_Face)face_pointer;
    const Font *font = face ? measure_font(face) : NULL;
    int f = factor, width = (x1 - x0) * f, height = (y1 - y0) * f, x, y, i, runs = 0;
    int left = x1, right = x0, top = y1, feet[16] = {0}, baseline = y0, level[256] = {0};
    double sv, sx, ex, ey, pen, ink_left = 1e9, ink_right = -1e9, stem;
    const char *c;
    if (!font || width > WIDE || height > HIGH || x1 - x0 > 64 || y1 - y0 > 16 || n < 2) return 0;
    for (i = 1; i < n; i++) level[ramp[i]] = i;
    /* Where the letters are, how heavy (runs of them across), and their
     * baseline. */
    memset(strokes, 0, sizeof(strokes));
    for (x = x0; x < x1; x++) {
        int foot = -1;
        for (y = y0; y < y1; y++) {
            int step = level[texels[y * pitch + x]];
            if (!step) continue;
            /* Half the ramp or more is the stroke; less, its edge. */
            strokes[(y - y0) * 64 + (x - x0)] = step * 2 >= n - 1 ? 1.0f : (float)step / (n - 1);
            if (x < left) left = x;
            if (x + 1 > right) right = x + 1;
            if (y < top) top = y;
            foot = y + 1;
        }
        if (foot > y0) feet[foot - y0 - 1]++;
    }
    if (right <= left) return 0;
    if (span) left = x0 + 1, right = x1 - 1;
    for (i = 0, baseline = y0; i < y1 - y0; i++) {
        if (feet[i] && (baseline == y0 || feet[i] > feet[baseline - y0 - 1])) baseline = y0 + i + 1;
    }
    add_runs(strokes, 64, y1 - y0, 0, run_list, &runs, RUN_LIMIT);
    stem = median(run_list, runs);
    if (stem > (baseline - top) * 0.2) stem = (baseline - top) * 0.2;
    /* The text across the font, from the first letter's ink to the last's. */
    for (pen = 0, c = text; *c; c++) {
        FT_BBox bbox;
        double step = advance(face, (unsigned char)*c);
        if (step < 0) return 0;
        if (load(face, (unsigned char)*c)) {
            FT_Outline_Get_BBox(&face->glyph->outline, &bbox);
            if (pen + bbox.xMin / 64.0 < ink_left) ink_left = pen + bbox.xMin / 64.0;
            if (pen + bbox.xMax / 64.0 > ink_right) ink_right = pen + bbox.xMax / 64.0;
        }
        pen += step;
    }
    if (ink_right <= ink_left) return 0;
    /* The capitals as high as the retail letters, the heaviness taken off;
     * as wide as they are, a quarter wider than the font's proportions at
     * most. A narrow word squeezes the font across, which thins its stems:
     * the weight across follows the scale across, the weight down the
     * scale down. Never lighter: a serif face's hairlines would break. */
    ey = stem - font->stem * (baseline - top) / font->cap;
    sv = (baseline - top - (ey > 0 ? ey : 0)) / font->cap;
    ey = stem - font->stem * sv;
    if (ey < 0) ey = 0;
    for (ex = 0, i = 0; i < 2; i++) {
        sx = (right - left - ex) / (ink_right - ink_left);
        if (sx > sv * 1.25) sx = sv * 1.25;
        ex = stem - font->stem * sx;
        if (ex < 0) ex = 0;
    }
    memset(cover, 0, sizeof(cover));
    for (pen = 0, c = text; *c; c++) {
        FT_Outline *outline;
        FT_Bitmap bitmap;
        double start = (left + right) / 2.0 - (ink_right - ink_left) * sx / 2 - ink_left * sx;
        double step = advance(face, (unsigned char)*c);
        if (!load(face, (unsigned char)*c)) {
            pen += step; /* a space */
            continue;
        }
        outline = &face->glyph->outline;
        for (i = 0; i < outline->n_points; i++) {
            double column = start + (pen + outline->points[i].x / 64.0) * sx - x0;
            double row = baseline - ey / 2 - outline->points[i].y / 64.0 * sv - y0;
            outline->points[i].x = (FT_Pos)(column * f * 64);
            outline->points[i].y = (FT_Pos)((y1 - y0 - row) * f * 64);
        }
        FT_Outline_EmboldenXY(outline, (FT_Pos)(ex * f * 64), (FT_Pos)(ey * f * 64));
        memset(one, 0, sizeof(one));
        memset(&bitmap, 0, sizeof(bitmap));
        bitmap.rows = (unsigned)height;
        bitmap.width = (unsigned)width;
        bitmap.pitch = WIDE;
        bitmap.buffer = &one[0][0];
        bitmap.num_grays = 256;
        bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
        if (FT_Outline_Get_Bitmap(face->glyph->library, outline, &bitmap)) return 0;
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                if (one[y][x] > cover[y][x]) cover[y][x] = one[y][x];
            }
        }
        pen += face->glyph->advance.x / 64.0;
    }
    if (outlined) {
        /* The outline: within a texel of the half-covered pixels (chamfer
         * distance, as render_at). */
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
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int step = (int)(cover[y][x] * (n - 1) / 255.0 + 0.5);
            uint8_t index = ramp[step];
            if (!step && outlined) index = distance[y][x] <= 3u * (unsigned)f ? ramp[0] : 0;
            if (!step && shadow && x >= f && y >= f && cover[y - f][x - f] >= 128) index = (uint8_t)shadow;
            if (!step && !outlined && ramp[0]) {
                /* On a background: where the sprite has nothing (the box's
                 * border shows through) it still has nothing. */
                if (!texels[(y0 + y / f) * pitch + x0 + x / f]) index = 0;
            }
            origin[(size_t)(y0 * f + y) * side + x0 * f + x] = index;
        }
    }
    return 1;
}

/* One of the panel's labels, in the font the text is set in: its box's
 * background (the commonest index across the panel in the label's rows:
 * inside the label the letters can be the commoner) to the letters' colour
 * farthest from it. */
static int set_label(const uint16_t *words, uint8_t *origin, unsigned char texels[PANEL_H][PANEL_W], int label)
{
    int x0 = panel_labels[label].x0, y0 = panel_labels[label].y0, x1 = panel_labels[label].x1;
    int y1 = panel_labels[label].y1, counts[16] = {0}, inside[16] = {0}, used[16], n_used = 0, background = 0;
    int far = 0, x, y, i, n;
    unsigned char ramp[8];
    double far_by = -1, bg[3];
    for (y = y0; y < y1; y++) {
        for (x = 0; x < PANEL_W; x++) counts[texels[y][x]]++;
        for (x = x0; x < x1; x++) inside[texels[y][x]]++;
    }
    for (i = 1; i < 16; i++) {
        if (counts[i] > counts[background]) background = i;
    }
    colour(words, PANEL_CLUT_X, PANEL_CLUT_Y, background, bg);
    for (i = 1; i < 16; i++) {
        double rgb[3], by;
        if (!inside[i] || i == background) continue;
        used[n_used++] = i;
        colour(words, PANEL_CLUT_X, PANEL_CLUT_Y, i, rgb);
        by = (rgb[0] - bg[0]) * (rgb[0] - bg[0]) + (rgb[1] - bg[1]) * (rgb[1] - bg[1]) + (rgb[2] - bg[2]) * (rgb[2] - bg[2]);
        if (by > far_by) far_by = by, far = i;
    }
    if (!background || !far) return 0;
    n = make_ramp(words, PANEL_CLUT_X, PANEL_CLUT_Y, background, far, used, n_used, ramp, 8);
    return set_text(Glyphs_Face((unsigned char)panel_labels[label].text[0]), &texels[0][0], PANEL_W, x0, y0, x1, y1,
                    panel_labels[label].text, ramp, n, 0, 0, 0, origin);
}

static int make_panel(const uint16_t *words)
{
    static unsigned char texels[PANEL_H][PANEL_W];
    int f = factor, x, y, i;
    uint8_t *origin = atlas + (size_t)HUD_TOP * CELL * f * side;
    for (y = 0; y < PANEL_H; y++) {
        for (x = 0; x < PANEL_W; x++) {
            texels[y][x] = (unsigned char)texel(words, 0, PANEL_PAGE_X, PANEL_PAGE_Y, PANEL_U + x, PANEL_V + y);
        }
    }
    for (y = 0; y < PANEL_H * f; y++) {
        for (x = 0; x < PANEL_W * f; x++) origin[(size_t)y * side + x] = texels[y / f][x / f];
    }
    changed(HUD_TOP * CELL * f, HUD_TOP * CELL * f + PANEL_H * f - 1);
    for (i = 0; i < (int)(sizeof(panel_labels) / sizeof(panel_labels[0])); i++) {
        if (!set_label(words, origin, texels, i)) return 0;
    }
    return 1;
}

/* Words and numbers set anew where the game has them lettered: each a
 * text over one sprite or several side by side (a name the game cuts in two,
 * MEAD + OW), found by page, palette and rectangle as #47's HD pack recipe
 * lists them. Styles: BOX, letters on the sprite's own background (the
 * ramp from the commonest index to the letters' colour farthest from it);
 * OUTLINE, letters with the index round them next to nothing; CLEAR,
 * letters on nothing (the ramp from 0 to the colour farthest from black),
 * with `shadow` a texel right and down when not 0. The card view's inks
 * are CLEAR: their palette is subtracted, so the farthest is the darkest.
 * A picture is made again when the sprites' texels change (a terrain's own
 * sheet). */
enum { BOX, OUTLINE, CLEAR };
typedef struct {
    int depth, page_x, page_y, clut_x, clut_y, clut_y2; /* palette rows clut_y to clut_y2 */
    const char *text;
    int serif, style, shadow;
    int pieces, rect[2][4];                             /* u, v, w, h of each, left to right */
    int column, row;                                    /* the picture's first cell in the HUD rows */
    uint32_t sum;
    unsigned made;
    signed char drawn;
} Label;

static Label labels[64];
static int label_count;

static void add_label(int depth, int page_x, int page_y, int clut_x, int clut_y, int clut_y2, const char *text,
                      int serif, int style, int shadow, int u, int v, int w, int h, int u2, int v2, int w2, int h2,
                      int column, int row)
{
    Label *l = &labels[label_count++];
    memset(l, 0, sizeof(*l));
    l->depth = depth, l->page_x = page_x, l->page_y = page_y, l->clut_x = clut_x, l->clut_y = clut_y;
    l->clut_y2 = clut_y2, l->text = text, l->serif = serif, l->style = style, l->shadow = shadow;
    l->rect[0][0] = u, l->rect[0][1] = v, l->rect[0][2] = w, l->rect[0][3] = h;
    l->pieces = 1;
    if (w2) {
        l->rect[1][0] = u2, l->rect[1][1] = v2, l->rect[1][2] = w2, l->rect[1][3] = h2;
        l->pieces = 2;
    }
    l->column = column, l->row = row;
}

static void make_labels(void)
{
    static const char *const kinds[] = {"Magic", "Equip", "Trap", "Ritual"};
    static const char digits[] = "0\0" "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9";
    int i;
    if (label_count) return;
    /* The card kinds, beside the duel's digits (duel_card_frame_draw.c), in
     * the hand and on the card bar: the sheet's six palette rows. */
    for (i = 0; i < 4; i++) add_label(1, 896, 256, 256, 241, 246, kinds[i], 1, OUTLINE, 0, i * 32, 96, 32, 16, 0, 0, 0, 0, 4 + 2 * i, 2);
    /* The FIELD box (duel HUD): its word, and the terrains' names. */
    add_label(0, 704, 0, 720, 252, 252, "FIELD", 0, CLEAR, 4, 24, 88, 40, 8, 0, 0, 0, 0, 18, 1);
    add_label(0, 704, 0, 720, 252, 252, "FOREST", 0, BOX, 0, 64, 112, 32, 16, 0, 0, 0, 0, 12, 2);
    add_label(0, 704, 0, 720, 252, 252, "WASTELAND", 0, BOX, 0, 96, 80, 32, 16, 112, 32, 16, 16, 14, 2);
    add_label(0, 704, 0, 720, 252, 252, "MOUNTAIN", 0, BOX, 0, 64, 96, 32, 16, 112, 96, 16, 16, 17, 2);
    add_label(0, 704, 0, 720, 252, 252, "MEADOW", 0, BOX, 0, 64, 80, 32, 16, 112, 48, 16, 16, 20, 2);
    add_label(0, 704, 0, 720, 252, 252, "SEA", 0, BOX, 0, 96, 112, 24, 16, 0, 0, 0, 0, 23, 2);
    add_label(0, 704, 0, 720, 252, 252, "DARK", 0, BOX, 0, 96, 96, 16, 16, 120, 112, 8, 16, 25, 2);
    /* The card view's ATK and DFD, and its digits, in the plates' inks
     * (func_80028B08): row 248 for ATK, 249 for DFD. */
    add_label(0, 960, 256, 496, 248, 249, "ATK", 1, CLEAR, 0, 0, 206, 24, 12, 0, 0, 0, 0, 14, 1);
    add_label(0, 960, 256, 496, 248, 249, "DFD", 1, CLEAR, 0, 0, 218, 24, 12, 0, 0, 0, 0, 16, 1);
    for (i = 0; i < 10; i++) {
        add_label(0, 960, 256, 496, 248, 249, digits + i * 2, 1, CLEAR, 0, 16 + i * 6, 0x90, 6, 13, 0, 0, 0, 0, 14 + i, 0);
        add_label(0, 960, 256, 496, 248, 249, digits + i * 2, 1, CLEAR, 0, 16 + i * 6, 0x10, 6, 13, 0, 0, 0, 0, 21 + i % 10 / 5 * 5 + i % 5, 1 + 2 * (i >= 5));
    }
}

/* A label's sprites side by side, and a hash of them. */
static uint32_t read_label(const uint16_t *words, const Label *l, unsigned char texels[16][64], int *width)
{
    uint32_t sum = 2166136261u;
    int i, x, y, at = 0;
    memset(texels, 0, 16 * 64);
    for (i = 0; i < l->pieces; i++) {
        for (y = 0; y < l->rect[i][3]; y++) {
            for (x = 0; x < l->rect[i][2]; x++) {
                texels[y][at + x] = (unsigned char)texel(words, l->depth, l->page_x, l->page_y, l->rect[i][0] + x,
                                                         l->rect[i][1] + y);
                sum = (sum ^ texels[y][at + x]) * 16777619u;
            }
        }
        at += l->rect[i][2];
    }
    *width = at;
    return sum;
}

static int make_label(const uint16_t *words, const Label *l, unsigned char texels[16][64], int width)
{
    int border[256] = {0}, all[256] = {0}, used[256], count = 0, from = 0, to = 0, x, y, i, n, f = factor;
    int height = l->rect[0][3];
    unsigned char ramp[8];
    double far_by = -1, base[3];
    void *face = l->serif ? CardArt_SerifFace() : NULL;
    uint8_t *origin = atlas + (size_t)(HUD_TOP + l->row) * CELL * f * side + (size_t)l->column * CELL * f;
    if (!face) face = Glyphs_Face((unsigned char)l->text[0]);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int c = texels[y][x];
            if (!c) continue;
            all[c]++;
            if (!x || !y || x + 1 == width || y + 1 == height || !texels[y][x - 1] || !texels[y][x + 1] ||
                !texels[y - 1][x] || !texels[y + 1][x]) {
                border[c]++;
            }
        }
    }
    for (i = 1; i < 256; i++) {
        if (all[i] && i != l->shadow) used[count++] = i;
    }
    if (l->style == OUTLINE) {
        /* The outline is what stands next to nothing, the fill the
         * commonest of the rest. */
        for (i = 1; i < 256; i++) {
            if (border[i] > border[from]) from = i;
        }
        for (i = 1; i < 256; i++) {
            if (i != from && all[i] > all[to]) to = i;
        }
    } else {
        if (l->style == BOX) {
            for (i = 1; i < 256; i++) {
                if (all[i] > all[from]) from = i;
            }
        }
        colour(words, l->clut_x, l->clut_y, from, base);
        for (i = 0; i < count; i++) {
            double rgb[3], by;
            if (used[i] == from) continue;
            colour(words, l->clut_x, l->clut_y, used[i], rgb);
            by = (rgb[0] - base[0]) * (rgb[0] - base[0]) + (rgb[1] - base[1]) * (rgb[1] - base[1]) +
                 (rgb[2] - base[2]) * (rgb[2] - base[2]);
            if (by > far_by) far_by = by, to = used[i];
        }
    }
    if ((l->style != CLEAR && !from) || !to) return 0;
    n = make_ramp(words, l->clut_x, l->clut_y, from, to, used, count, ramp, 8);
    for (y = 0; y < CELL * f; y++) memset(origin + (size_t)y * side, 0, (size_t)(width + 15) / 16 * CELL * f);
    if (!set_text(face, &texels[0][0], 64, 0, 0, width, height, l->text, ramp, n, l->style == OUTLINE, l->shadow, 0,
                  origin)) {
        return 0;
    }
    changed((HUD_TOP + l->row) * CELL * f, (HUD_TOP + l->row + 1) * CELL * f - 1);
    return 1;
}

/* The opponent's name in place of COM (hd_text.h): COM's box, rows 9 to
 * 18 of the panel, made as long as the name needs, leftwards from where it
 * meets the panel (column 25): its left end as the panel has it, then its
 * rows' border and background, the name set over the background as COM
 * is. Its picture is in the HUD rows' last row, cells 0 to 3. */
#define NAME_TOP 9
#define NAME_ROWS 10
#define NAME_JOIN 25
#define NAME_WIDE 64
static int name_duelist, name_width;
static unsigned name_made;

static int make_name(const uint16_t *words, const char *name, int *width)
{
    static unsigned char texels[PANEL_H][PANEL_W], box[16][64];
    int f = factor, x, y, i, n, counts[16] = {0}, background = 0, wide;
    unsigned char ramp[8];
    double pen = 0, sv, bg[3], far_by = -1;
    int far = 0;
    FT_Face face = (FT_Face)Glyphs_Face((unsigned char)name[0]);
    const Font *font = face ? measure_font(face) : NULL;
    uint8_t *origin = atlas + (size_t)(HUD_TOP + 3) * CELL * f * side;
    int used[16], n_used = 0;
    if (!font) return 0;
    for (y = 0; y < PANEL_H; y++) {
        for (x = 0; x < PANEL_W; x++) {
            texels[y][x] = (unsigned char)texel(words, 0, PANEL_PAGE_X, PANEL_PAGE_Y, PANEL_U + x, PANEL_V + y);
        }
    }
    /* COM's colours and its box's background, as set_label finds them. */
    for (y = panel_labels[1].y0; y < panel_labels[1].y1; y++) {
        for (x = 0; x < PANEL_W; x++) counts[texels[y][x]]++;
    }
    for (i = 1; i < 16; i++) {
        if (counts[i] > counts[background]) background = i;
    }
    /* The letters' colour farthest from it. */
    colour(words, PANEL_CLUT_X, PANEL_CLUT_Y, background, bg);
    for (y = panel_labels[1].y0; y < panel_labels[1].y1; y++) {
        for (x = panel_labels[1].x0; x < panel_labels[1].x1; x++) {
            double rgb[3], by;
            int c = texels[y][x];
            if (!c || c == background) continue;
            colour(words, PANEL_CLUT_X, PANEL_CLUT_Y, c, rgb);
            by = (rgb[0] - bg[0]) * (rgb[0] - bg[0]) + (rgb[1] - bg[1]) * (rgb[1] - bg[1]) +
                 (rgb[2] - bg[2]) * (rgb[2] - bg[2]);
            if (by > far_by) far_by = by, far = c;
        }
    }
    for (i = 1; i < 16; i++) {
        if (i != background) used[n_used++] = i;
    }
    if (!far) return 0;
    n = make_ramp(words, PANEL_CLUT_X, PANEL_CLUT_Y, background, far, used, n_used, ramp, 8);
    if (n < 2) return 0;
    /* As long as the name set as high as COM, and a texel each side. */
    for (i = 0; name[i]; i++) {
        double step = advance(face, (unsigned char)name[i]);
        if (step < 0) return 0;
        pen += step;
    }
    sv = (panel_labels[1].y1 - panel_labels[1].y0) / font->cap;
    wide = (int)(pen * sv + 0.999) + 4;
    if (wide < NAME_JOIN) wide = NAME_JOIN;
    if (wide > NAME_WIDE) wide = NAME_WIDE;
    /* The box: its left end (two columns) as the panel has it, then each
     * row's border or background; COM's letters kept at its right, for
     * set_text to measure. */
    memset(box, 0, sizeof(box));
    for (y = 0; y < NAME_ROWS; y++) {
        int row = NAME_TOP + y, label = row >= panel_labels[1].y0 && row < panel_labels[1].y1;
        for (x = 0; x < wide; x++) {
            int from = x < 2 ? x : x >= wide - (NAME_JOIN - 2) ? x - (wide - NAME_JOIN) : 2;
            int c = texels[row][from];
            if (label && x >= 2) c = x >= wide - (NAME_JOIN - 2) ? texels[row][from] : background;
            box[y][x] = (unsigned char)c;
        }
    }
    for (y = 0; y < 16 * f; y++) memset(origin + (size_t)y * side, 0, (size_t)NAME_WIDE * f);
    for (y = 0; y < NAME_ROWS * f; y++) {
        for (x = 0; x < wide * f; x++) origin[(size_t)y * side + x] = box[y / f][x / f];
    }
    if (!set_text(face, &box[0][0], 64, panel_labels[1].x0, panel_labels[1].y0 - NAME_TOP, wide,
                  panel_labels[1].y1 - NAME_TOP, name, ramp, n, 0, 0, 1, origin)) {
        return 0;
    }
    changed((HUD_TOP + 3) * CELL * f, (HUD_TOP + 4) * CELL * f - 1);
    *width = wide;
    return 1;
}

int HdText_NameBox(int wanted, int *atlas_u, int *atlas_v, int *x, int *y, int *width, int *height)
{
    const uint16_t *words = SoftGpu_Vram();
    int duelist = Tables_OpponentId();
    const char *name = Tables_DuelistShortName(duelist);
    if (!name || wanted < 2 || wanted > MAX_FACTOR || !words || panel_sum(words) != PANEL_SUM) return 0;
    if (wanted != factor && !make_atlas(wanted)) return 0;
    if (name_made != generation || name_duelist != duelist) {
        name_made = generation;
        name_duelist = duelist;
        if (!make_name(words, name, &name_width)) name_width = 0;
    }
    if (!name_width) return 0;
    *atlas_u = 0;
    *atlas_v = (HUD_TOP + 3) * CELL;
    *x = NAME_JOIN - name_width;
    *y = NAME_TOP;
    *width = name_width;
    *height = NAME_ROWS;
    return 1;
}

int HdText_NameEnabled(void)
{
    return Settings_Get(SET_OPPONENT_NAME) != 0;
}

int HdText_HudEnabled(void)
{
    return Settings_Get(SET_HD_HUD) != 0;
}

int HdText_Hud(int depth, int page_x, int page_y, int clut_x, int clut_y, int u, int v, int w, int h, int wanted,
               int *atlas_u, int *atlas_v)
{
    const uint16_t *words = SoftGpu_Vram();
    unsigned i;
    if (wanted < 2 || wanted > MAX_FACTOR || !words || w < 1 || h < 1) return 0;
    if (!depth && page_x == PANEL_PAGE_X && page_y == PANEL_PAGE_Y && clut_x == PANEL_CLUT_X &&
        clut_y == PANEL_CLUT_Y && u == PANEL_U && v == PANEL_V && w == PANEL_W && h == PANEL_H) {
        if (panel_sum(words) != PANEL_SUM) return 0;
        if (wanted != factor && !make_atlas(wanted)) return 0;
        if (panel_made != generation) {
            panel_made = generation;
            panel_ok = make_panel(words);
        }
        if (!panel_ok) return 0;
        *atlas_u = 0;
        *atlas_v = HUD_TOP * CELL;
        return 1;
    }
    make_labels();
    for (i = 0; i < (unsigned)label_count; i++) {
        Label *l = &labels[i];
        unsigned char texels[16][64];
        int piece, at = 0, width;
        uint32_t sum;
        if (depth != l->depth || page_x != l->page_x || page_y != l->page_y || clut_x != l->clut_x ||
            clut_y < l->clut_y || clut_y > l->clut_y2) {
            continue;
        }
        for (piece = 0; piece < l->pieces; piece++) {
            if (u == l->rect[piece][0] && v == l->rect[piece][1] && w <= l->rect[piece][2] && h <= l->rect[piece][3]) break;
            at += l->rect[piece][2];
        }
        if (piece == l->pieces) continue;
        if (wanted != factor && !make_atlas(wanted)) return 0;
        sum = read_label(words, l, texels, &width);
        if (sum != l->sum || l->made != generation) {
            l->sum = sum;
            l->made = generation;
            l->drawn = make_label(words, l, texels, width) ? 1 : -1;
        }
        if (l->drawn < 0) return 0;
        *atlas_u = l->column * CELL + at;
        *atlas_v = (HUD_TOP + l->row) * CELL;
        return 1;
    }
    for (i = 0; i < sizeof(sheets) / sizeof(sheets[0]); i++) {
        Sheet *s = &sheets[i];
        unsigned char cell[CELL][CELL];
        uint32_t sum = 2166136261u;
        int d;
        if (depth != s->depth || page_x != s->page_x || page_y != s->page_y || clut_x != s->clut_x ||
            clut_y < s->clut_y || clut_y >= s->clut_y + s->clut_rows || v != s->v || u < s->u) {
            continue;
        }
        d = (u - s->u) / s->step;
        if (d > 9 || u != s->u + d * s->step || w > s->width || h > s->height) return 0;
        if (wanted != factor && !make_atlas(wanted)) return 0;
        for (d = 0; d < 10; d++) sum = read_digit(words, s, d, cell, sum);
        d = (u - s->u) / s->step;
        if (sum != s->sum || s->made != generation) {
            /* Measured again when the sheet changes (another package). */
            s->sum = sum;
            s->made = generation;
            s->ok = measure_sheet(s, words);
            memset(s->drawn, 0, sizeof(s->drawn));
        }
        if (!s->ok) return 0;
        if (!s->drawn[d]) {
            read_digit(words, s, d, cell, 0);
            s->drawn[d] = render_at(s->column + d, HUD_TOP + s->row, (const unsigned char (*)[CELL])cell, s->height,
                                    (uint32_t)('0' + d), &s->r)
                              ? 1
                              : -1;
        }
        if (s->drawn[d] < 0) return 0;
        *atlas_u = (s->column + d) * CELL;
        *atlas_v = (HUD_TOP + s->row) * CELL;
        return 1;
    }
    return 0;
}
