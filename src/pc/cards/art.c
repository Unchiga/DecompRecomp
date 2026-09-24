/* Custom artwork for the cards mods add (cards.h, notes/more-cards.md).
 *
 * A card's art record on the disc (7 sectors in WA_MRG.MRG, read by
 * func_80029164) is, in its first 0x3060 bytes:
 *
 *   +0x0000  102 x 96 art, one byte per pixel      (indices 1-255)
 *   +0x2640  its CLUT, 256 BGR555 entries          (entry 0 unused, 0x8000)
 *   +0x2840  the title plate, 96 x 14 at 4 bits    (0 clear, 1 darkest ink to 7 faintest)
 *   +0x2AE0  40 x 32 thumbnail, one byte per pixel (indices 1-63)
 *   +0x2FE0  its CLUT, 64 BGR555 entries
 *
 * and the thumbnail block (+0x2AE0, 0x580 bytes) is also the card's own sector
 * the duel reads for the hand and field. This file makes those bytes from a
 * PNG: cropped to the shape, averaged down to the size, and reduced to the
 * colours by median cut. The plate is the card's name set in Times (the
 * system's; the retail plates' own face is not available as a font), or a
 * PNG the mod gives.
 *
 * The plate is drawn subtractively over the card's gold frame through a
 * fixed 16-entry CLUT (index 1 a light grey, 7 a dark one), so index 1 takes
 * the most away: it is the darkest ink, and 7 barely shows. The retail
 * plates are authored that way, stems at 1 with faint 6 and 7 fringes. The
 * measurements, and the settings below that make a legible plate (Times
 * regular at 13 pixels, baseline under row 11, whole-pixel advances, hard
 * coverage steps), are the YuGiOhForbiddenMemoriesRecomp project's
 * (src/psx_card_packs.c, render_title), found against window captures. */
#include "pc/compat/fs.h"
#include "cards.h"
#include "art.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include "pc/compat/font.h"
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#else
#include <fontconfig/fontconfig.h>
#endif

typedef struct { unsigned char r, g, b; } Rgb;

/* --- images ---------------------------------------------------------- */

/* The PNG as RGB over black or, with `ink`, as ink coverage in all three
 * channels: dark and opaque is full ink, so a strip drawn in black on white
 * and one drawn on a transparent background read the same. */
static Rgb *load_png_as(const char *path, int *width, int *height, int ink)
{
    png_image image;
    FILE *file;
    unsigned char *rgba;
    Rgb *out;
    size_t i, count;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (!png_image_begin_read_from_stdio(&image, file)) { fclose(file); return NULL; }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba || !png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
        free(rgba);
        fclose(file);
        png_image_free(&image);
        return NULL;
    }
    fclose(file);
    count = (size_t)image.width * image.height;
    out = malloc(count * sizeof(*out));
    for (i = 0; out && i < count; i++) {
        unsigned a = rgba[i * 4 + 3];
        if (ink) {
            unsigned luma = (rgba[i * 4] * 3u + rgba[i * 4 + 1] * 6u + rgba[i * 4 + 2]) / 10u;
            out[i].r = out[i].g = out[i].b = (unsigned char)((255 - luma) * a / 255);
            continue;
        }
        out[i].r = (unsigned char)(rgba[i * 4] * a / 255);
        out[i].g = (unsigned char)(rgba[i * 4 + 1] * a / 255);
        out[i].b = (unsigned char)(rgba[i * 4 + 2] * a / 255);
    }
    *width = (int)image.width;
    *height = (int)image.height;
    free(rgba);
    png_image_free(&image);
    return out;
}

static Rgb *load_png(const char *path, int *width, int *height)
{
    return load_png_as(path, width, height, 0);
}

/* `w` x `h` from the middle of the image at that shape, each pixel the
 * average of the source pixels under it. */
static void resample(const Rgb *source, int sw, int sh, Rgb *out, int w, int h)
{
    double cw = sw, ch = sh, x0, y0;
    int x, y;
    if (cw * h > ch * w) cw = ch * w / h; else ch = cw * h / w;
    x0 = (sw - cw) / 2;
    y0 = (sh - ch) / 2;
    for (y = 0; y < h; y++) {
        int top = (int)(y0 + ch * y / h), bottom = (int)(y0 + ch * (y + 1) / h);
        if (bottom <= top) bottom = top + 1;
        for (x = 0; x < w; x++) {
            int left = (int)(x0 + cw * x / w), right = (int)(x0 + cw * (x + 1) / w), sx, sy;
            unsigned long r = 0, g = 0, b = 0, n = 0;
            if (right <= left) right = left + 1;
            for (sy = top; sy < bottom && sy < sh; sy++) {
                for (sx = left; sx < right && sx < sw; sx++) {
                    const Rgb *p = &source[(size_t)sy * sw + sx];
                    r += p->r; g += p->g; b += p->b; n++;
                }
            }
            if (!n) n = 1;
            out[y * w + x].r = (unsigned char)(r / n);
            out[y * w + x].g = (unsigned char)(g / n);
            out[y * w + x].b = (unsigned char)(b / n);
        }
    }
}

/* --- colours --------------------------------------------------------- */

typedef struct { int first, count; } Box;

static unsigned short to555(int r, int g, int b)
{
    unsigned short c = (unsigned short)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    return c ? c : 0x8000;   /* 0 is the transparent colour; this is black */
}

static int channel(const Rgb *c, int axis) { return axis == 0 ? c->r : axis == 1 ? c->g : c->b; }
static int sort_axis;
static int by_axis(const void *a, const void *b) { return channel(a, sort_axis) - channel(b, sort_axis); }

/* Median cut of the pixels to `colours` entries, written to `clut` from
 * entry 1 on, and each pixel's entry to `indices`. */
static void quantize(const Rgb *pixels, int count, int colours, unsigned short *clut, unsigned char *indices)
{
    Rgb *sorted = malloc((size_t)count * sizeof(*sorted)), palette[256];
    Box boxes[256];
    int box_count = 1, i, k;
    if (!sorted) return;
    memcpy(sorted, pixels, (size_t)count * sizeof(*sorted));
    boxes[0].first = 0;
    boxes[0].count = count;
    while (box_count < colours) {
        int best = -1, best_range = 0, axis = 0;
        for (i = 0; i < box_count; i++) {
            int a, low[3] = {255, 255, 255}, high[3] = {0, 0, 0};
            if (boxes[i].count < 2) continue;
            for (k = boxes[i].first; k < boxes[i].first + boxes[i].count; k++) {
                for (a = 0; a < 3; a++) {
                    int v = channel(&sorted[k], a);
                    if (v < low[a]) low[a] = v;
                    if (v > high[a]) high[a] = v;
                }
            }
            for (a = 0; a < 3; a++) {
                if (high[a] - low[a] > best_range) { best_range = high[a] - low[a]; best = i; axis = a; }
            }
        }
        if (best < 0 || best_range < 4) break;   /* no box worth splitting */
        sort_axis = axis;
        qsort(sorted + boxes[best].first, (size_t)boxes[best].count, sizeof(*sorted), by_axis);
        boxes[box_count].first = boxes[best].first + boxes[best].count / 2;
        boxes[box_count].count = boxes[best].count - boxes[best].count / 2;
        boxes[best].count /= 2;
        box_count++;
    }
    for (i = 0; i < box_count; i++) {
        unsigned long r = 0, g = 0, b = 0, n = (unsigned long)boxes[i].count;
        for (k = boxes[i].first; k < boxes[i].first + boxes[i].count; k++) {
            r += sorted[k].r; g += sorted[k].g; b += sorted[k].b;
        }
        if (!n) n = 1;
        palette[i].r = (unsigned char)(r / n);
        palette[i].g = (unsigned char)(g / n);
        palette[i].b = (unsigned char)(b / n);
        clut[i + 1] = to555(palette[i].r, palette[i].g, palette[i].b);
    }
    for (; i < colours; i++) clut[i + 1] = 0x8000;
    clut[0] = 0x8000;
    for (k = 0; k < count; k++) {
        int best = 0;
        long best_distance = -1;
        for (i = 0; i < box_count; i++) {
            long dr = pixels[k].r - palette[i].r, dg = pixels[k].g - palette[i].g, db = pixels[k].b - palette[i].b;
            long distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (best_distance < 0 || distance < best_distance) { best_distance = distance; best = i; }
        }
        indices[k] = (unsigned char)(best + 1);
    }
    free(sorted);
}

static void put_clut(unsigned char *out, const unsigned short *clut, int entries)
{
    int i;
    for (i = 0; i < entries; i++) {
        out[i * 2] = (unsigned char)(clut[i] & 0xFF);
        out[i * 2 + 1] = (unsigned char)(clut[i] >> 8);
    }
}

static int image_into(const char *path, unsigned char *record, int thumbnail_only, char *why, size_t why_size)
{
    int width, height;
    Rgb *source = load_png(path, &width, &height), *art;
    unsigned short clut[256];
    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    art = malloc(CARD_ART_WIDTH * CARD_ART_HEIGHT * sizeof(*art));
    if (!art) { free(source); return 0; }
    if (!thumbnail_only) {
        resample(source, width, height, art, CARD_ART_WIDTH, CARD_ART_HEIGHT);
        quantize(art, CARD_ART_WIDTH * CARD_ART_HEIGHT, 255, clut, record + CARD_ART_PIXELS);
        put_clut(record + CARD_ART_CLUT, clut, 256);
    }
    resample(source, width, height, art, CARD_THUMB_WIDTH, CARD_THUMB_HEIGHT);
    quantize(art, CARD_THUMB_WIDTH * CARD_THUMB_HEIGHT, 63, clut, record + CARD_THUMB_PIXELS);
    put_clut(record + CARD_THUMB_CLUT, clut, 64);
    free(art);
    free(source);
    return 1;
}

int CardArt_FromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    return image_into(path, record, 0, why, why_size);
}

int CardArt_ThumbnailFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    return image_into(path, record, 1, why, why_size);
}

/* --- the title plate --------------------------------------------------- */

static void put_ink(unsigned char *plate, int x, int y, int ink)
{
    unsigned char *byte;
    if (x < 0 || x >= CARD_TITLE_WIDTH || y < 0 || y >= CARD_TITLE_HEIGHT) return;
    byte = plate + y * (CARD_TITLE_WIDTH / 2) + x / 2;
    if (x & 1) *byte = (unsigned char)((*byte & 0x0F) | (ink << 4));
    else *byte = (unsigned char)((*byte & 0xF0) | ink);
}

/* Coverage (0-255) to the plate's inks: full coverage is 1, the darkest;
 * an edge 3; a faint halo 6, as the retail plates carry; else clear. */
static int ink_of(int coverage)
{
    return coverage >= 150 ? 1 : coverage >= 96 ? 3 : coverage >= 40 ? 6 : 0;
}

int CardArt_TitleFromImage(const char *path, unsigned char *record, char *why, size_t why_size)
{
    int width, height, x, y;
    Rgb *source = load_png_as(path, &width, &height, 1), small[CARD_TITLE_WIDTH * CARD_TITLE_HEIGHT];
    if (!source) {
        snprintf(why, why_size, "%s is not a PNG it could read", path);
        return 0;
    }
    resample(source, width, height, small, CARD_TITLE_WIDTH, CARD_TITLE_HEIGHT);
    memset(record + CARD_TITLE_PIXELS, 0, CARD_TITLE_BYTES);
    for (y = 0; y < CARD_TITLE_HEIGHT; y++) {
        for (x = 0; x < CARD_TITLE_WIDTH; x++) {
            put_ink(record + CARD_TITLE_PIXELS, x, y, ink_of(small[y * CARD_TITLE_WIDTH + x].r));
        }
    }
    free(source);
    return 1;
}

static FT_Library library;
static FT_Face face;
static int face_tried;

static const char *serif_file(void)
{
#ifdef _WIN32
    return Win32_SerifFontPath();
#else
    static char path[1024];
    FcPattern *pattern, *match;
    FcResult result;
    FcChar8 *file = NULL;
    if (!FcInit()) return NULL;
    pattern = FcNameParse((const FcChar8 *)"Times:regular");
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    path[0] = '\0';
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
        snprintf(path, sizeof(path), "%s", (const char *)file);
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    return path[0] ? path : NULL;
#endif
}

/* The name as the retail plates set theirs: Times at 13 pixels, the
 * baseline under row 11, from column 3, each glyph on a whole pixel so stems
 * fill whole columns; a name wider than 90 pixels is squeezed into columns
 * 3 to 93, as the long retail names are. */
int CardArt_TitleFromName(const char *name, unsigned char *record)
{
    enum { WIDE = 512, BASELINE = 11, LEFT = 3, ROOM = 90 };
    static unsigned char line[CARD_TITLE_HEIGHT][WIDE];
    int pen = LEFT, x, y, ink_low = WIDE, ink_high = -1, previous = 0;
    const char *c;
    if (!face_tried) {
        const char *file = serif_file();
        face_tried = 1;
        if (!file || FT_Init_FreeType(&library) || FT_New_Face(library, file, 0, &face) ||
            FT_Set_Pixel_Sizes(face, 0, 13)) {
            face = NULL;
        }
    }
    if (!face) return 0;
    memset(line, 0, sizeof(line));
    for (c = name; *c; c++) {
        FT_UInt index = FT_Get_Char_Index(face, (FT_ULong)(unsigned char)*c);
        FT_Bitmap *bitmap;
        if (previous && index && FT_HAS_KERNING(face)) {
            FT_Vector kern;
            if (!FT_Get_Kerning(face, previous, index, FT_KERNING_DEFAULT, &kern)) pen += (int)((kern.x + 32) >> 6);
        }
        previous = index;
        if (FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_NO_HINTING)) continue;
        bitmap = &face->glyph->bitmap;
        for (y = 0; y < (int)bitmap->rows; y++) {
            int ty = BASELINE - face->glyph->bitmap_top + y;
            if (ty < 0 || ty >= CARD_TITLE_HEIGHT) continue;
            for (x = 0; x < (int)bitmap->width; x++) {
                int tx = pen + face->glyph->bitmap_left + x;
                unsigned char v = bitmap->buffer[y * bitmap->pitch + x];
                if (tx < 0 || tx >= WIDE || !v) continue;
                if (v > line[ty][tx]) line[ty][tx] = v;
                if (tx < ink_low) ink_low = tx;
                if (tx > ink_high) ink_high = tx;
            }
        }
        pen += (int)((face->glyph->advance.x + 32) >> 6);
        if (pen >= WIDE) break;
    }
    memset(record + CARD_TITLE_PIXELS, 0, CARD_TITLE_BYTES);
    if (ink_high < ink_low) return 1;
    if (ink_high - ink_low + 1 <= ROOM) {
        for (y = 0; y < CARD_TITLE_HEIGHT; y++) {
            for (x = 0; x < CARD_TITLE_WIDTH; x++) put_ink(record + CARD_TITLE_PIXELS, x, y, ink_of(line[y][x]));
        }
    } else {
        /* Squeezed with a linear filter, then brought back up to full ink:
         * averaging thins every stem, and thin stems are faint ones. */
        static float squeezed[CARD_TITLE_HEIGHT][ROOM];
        float step = (float)(ink_high - ink_low + 1) / ROOM, peak = 1.0f;
        for (y = 0; y < CARD_TITLE_HEIGHT; y++) {
            for (x = 0; x < ROOM; x++) {
                float at = ink_low + (x + 0.5f) * step - 0.5f, t;
                int i0 = (int)at, i1;
                if (at < 0) at = 0;
                i0 = (int)at;
                t = at - i0;
                i1 = i0 + 1 < WIDE ? i0 + 1 : i0;
                squeezed[y][x] = line[y][i0] * (1.0f - t) + line[y][i1] * t;
                if (squeezed[y][x] > peak) peak = squeezed[y][x];
            }
        }
        for (y = 0; y < CARD_TITLE_HEIGHT; y++) {
            for (x = 0; x < ROOM; x++) {
                put_ink(record + CARD_TITLE_PIXELS, LEFT + x, y, ink_of((int)(squeezed[y][x] * 255.0f / peak + 0.5f)));
            }
        }
    }
    return 1;
}
