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
    double c = cell[y][x] > r->dark ? (cell[y][x] - r->dark) / (SATURATE * (r->shade[y] - r->dark)) : 0;
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

/* The picture of `character` for a cell into atlas slot `slot`; 0 when no
 * font sets it or the cell holds no letter. */
static int render(int slot, const unsigned char cell[CELL][CELL], int large, uint32_t character, const Retail *r)
{
    static unsigned char cover[CELL * MAX_FACTOR][CELL * MAX_FACTOR];
    static unsigned short distance[CELL * MAX_FACTOR][CELL * MAX_FACTOR];
    int f = factor, cells_high = large ? 16 : 12, width = CELL * f, height = cells_high * f;
    int x, y, i, n = 0, main = 0, pass, upper = character < 128 && (isupper((int)character) || isdigit((int)character));
    int lower = character < 128 && islower((int)character);
    uint8_t *origin =
        atlas + (size_t)(slot / SLOTS_ACROSS) * CELL * f * side + (size_t)(slot % SLOTS_ACROSS) * CELL * f;
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
        int row = at < 0 ? 0 : (int)at, next = row + 1 < cells_high ? row + 1 : row;
        double t = at < 0 ? 0 : at - row;
        shade = r->shade[row] + (r->shade[next] - r->shade[row]) * t;
        shade = shade * BRIGHT + (1 - BRIGHT) * (r->body[row] + (r->body[next] - r->body[row]) * t);
        for (x = 0; x < width; x++) {
            /* Coverage on the run from the outline's index to the shade;
             * too little to rise above it is outline, as the cells'
             * faintest edges are. */
            int index = r->dark + (int)(cover[y][x] * (shade - r->dark) / 255 + 0.5);
            if (index > r->dark) origin[(size_t)y * side + x] = (uint8_t)index;
            else if (distance[y][x] <= 3u * (unsigned)f) origin[(size_t)y * side + x] = (uint8_t)r->outline;
        }
    }
    y = (slot / SLOTS_ACROSS) * CELL * f;
    if (y < first_dirty || last_dirty < first_dirty) first_dirty = y;
    if (y + CELL * f - 1 > last_dirty) last_dirty = y + CELL * f - 1;
    return 1;
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
        int slot = entry->slot >= 0 ? entry->slot : slots_used < SLOT_COUNT ? slots_used : -1;
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
