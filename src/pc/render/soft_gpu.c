#include "soft_gpu.h"
#include "texture_dump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Vertex {
    int x, y, r, g, b, u, v;
} Vertex;

static uint16_t vram[SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT];
static struct {
    int clip_x1, clip_y1, clip_x2, clip_y2; /* inclusive */
    int offset_x, offset_y;
    int page_x, page_y, blend, depth, dither;
    int window_mask_x, window_mask_y, window_x, window_y;
    int mask_set, mask_check;
    int clut_x, clut_y;
} gpu;
/* This primitive may sample the texture pack's shadow. Not in `gpu`: that
 * struct is a save state's, and its layout is fixed. */
static int shadow_on;

/* Where texels and palettes are read: VRAM, or one of the banks below. Set by
 * every texture-page word, so it is never stale and never part of a state. */
static uint16_t *texture_source;

static uint16_t *banks[SOFT_GPU_BANKS];

/* Where plot writes: VRAM, or a widescreen target while a primitive is drawn
 * a second time into it. Dithering keeps VRAM's phase in a shifted target. */
static uint16_t *target = vram;
static int dither_shift;

/* Widescreen targets. A full-screen drawing area (the game's double
 * buffers) gets a VRAM-shaped companion in which the area is `margin` wider
 * on each side: every primitive drawn to the area is drawn again there,
 * shifted right by the margin and clipped to the wider box, so the geometry
 * the GTE projects left and right of the 4:3 frame, which the hardware clips,
 * has somewhere to land. VRAM itself is never widened, so nothing the game
 * reads back changes.
 * Fills, loads and copies into the area are mirrored into its centre. */
#define WIDE_TARGETS 4
typedef struct WideTarget {
    int x1, y1, x2, y2, margin;
    int drawn;        /* primitives since this target was last presented */
    unsigned stamp;   /* last use, for reuse of the oldest slot */
    uint16_t *pixels;
} WideTarget;
static WideTarget wide[WIDE_TARGETS];
static int wide_on;
static unsigned wide_clock;
/* The scaled picture (SoftGpu_SetScale): scale x scale pixels per word. */
static uint32_t *picture;
static int scale = 1, scale_shift; /* scale is 1, 2, 4 or 8: the picture wraps with masks and divides with shifts */
#define PICTURE_WIDTH (SOFT_GPU_WIDTH << scale_shift)
#define PICTURE_HEIGHT (SOFT_GPU_HEIGHT << scale_shift)
static inline __attribute__((always_inline)) uint16_t *pixel(int x, int y);
static int owns(const Vertex *a, const Vertex *b);

uint16_t *SoftGpu_Bank(int bank)
{
    if (bank <= 0 || bank >= SOFT_GPU_BANKS) {
        return NULL;
    }
    if (!banks[bank]) {
        banks[bank] = calloc(SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT, sizeof(uint16_t));
    }
    return banks[bank];
}

static inline __attribute__((always_inline)) uint32_t expand(uint16_t c)
{
    uint32_t r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

static inline __attribute__((always_inline)) uint32_t *picture_pixel(int hx, int hy)
{
    return &picture[((size_t)(hy & (PICTURE_HEIGHT - 1)) << (10 + scale_shift)) + (size_t)(hx & (PICTURE_WIDTH - 1))];
}

/* The words x..x+w-1, y..y+h-1 of VRAM, copied into the picture. */
static void picture_from_words(int x, int y, int w, int h)
{
    int i, j, sx, sy;
    if (!picture) return;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint32_t colour = expand(*pixel(x + i, y + j));
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) *picture_pixel((x + i) * scale + sx, (y + j) * scale + sy) = colour;
            }
        }
    }
}

static const int8_t dither_matrix[4][4] = {
    {-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

const uint16_t *SoftGpu_Vram(void)
{
    return vram;
}

void SoftGpu_Reset(void)
{
    memset(&gpu, 0, sizeof(gpu));
    texture_source = vram;
    TextureDump_Init();
    gpu.clip_x2 = SOFT_GPU_WIDTH - 1;
    gpu.clip_y2 = SOFT_GPU_HEIGHT - 1;
}

static inline __attribute__((always_inline)) uint16_t *pixel(int x, int y)
{
    return &target[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

/* VRAM itself, whatever `target` is. Loads, stores, copies and fills always
 * mean VRAM, and they can arrive from the interrupt tick (a LoadImage in a
 * callback) while a primitive is being drawn a second time into a widescreen
 * target: through pixel() they landed there instead, and VRAM missed the
 * upload (the password screen's letters, differently on every run). */
static inline __attribute__((always_inline)) uint16_t *vram_pixel(int x, int y)
{
    return &vram[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

int SoftGpu_SetScale(int wanted)
{
    uint32_t *made = NULL;
    if (wanted != 1 && wanted != 2 && wanted != 4 && wanted != 8) return 0;
    if (wanted == scale) return 1;
    if (wanted > 1) {
        made = calloc((size_t)SOFT_GPU_WIDTH * wanted * SOFT_GPU_HEIGHT * wanted, sizeof(*made));
        if (!made) return 0;
    }
    free(picture);
    picture = made;
    scale = wanted;
    scale_shift = wanted == 8 ? 3 : wanted == 4 ? 2 : wanted == 2 ? 1 : 0;
    SoftGpu_PictureFromVram();
    return 1;
}

int SoftGpu_Scale(void) { return scale; }

const uint32_t *SoftGpu_Picture(void) { return picture; }

void SoftGpu_PictureFromVram(void)
{
    if (picture) picture_from_words(0, 0, SOFT_GPU_WIDTH, SOFT_GPU_HEIGHT);
}

/* The same word in whichever bank the current texture page names. */
static inline __attribute__((always_inline)) uint16_t sample(int x, int y)
{
    return texture_source[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

/* Copies what VRAM now holds in x,y,w,h into the centre of every
 * widescreen target it overlaps. A fill spanning a target's whole width
 * also fills its sides, as a cleared screen is cleared edge to edge. */
static void wide_mirror(int x, int y, int w, int h, int fill, uint16_t colour)
{
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        const WideTarget *wt = &wide[t];
        int x1 = x > wt->x1 ? x : wt->x1, x2 = x + w - 1 < wt->x2 ? x + w - 1 : wt->x2;
        int y1 = y > wt->y1 ? y : wt->y1, y2 = y + h - 1 < wt->y2 ? y + h - 1 : wt->y2;
        int row, column;
        if (!wt->pixels || x1 > x2 || y1 > y2) {
            continue;
        }
        for (row = y1; row <= y2; row++) {
            uint16_t *out = wt->pixels + row * SOFT_GPU_WIDTH;
            memcpy(out + x1 + wt->margin, vram + row * SOFT_GPU_WIDTH + x1, (size_t)(x2 - x1 + 1) * 2);
            if (fill && x <= wt->x1 && x + w - 1 >= wt->x2) {
                for (column = 0; column < wt->margin; column++) {
                    out[wt->x1 + column] = colour;
                    out[wt->x2 + wt->margin + 1 + column] = colour;
                }
            }
        }
    }
}

/* The target for the current drawing area, made on first use; NULL when
 * widescreen is off or the area is not a full screen. */
static WideTarget *wide_target(void)
{
    int w = gpu.clip_x2 - gpu.clip_x1 + 1, h = gpu.clip_y2 - gpu.clip_y1 + 1, margin, t, oldest = 0;
    WideTarget *wt;
    if (!wide_on || w < 256 || h < 192) {
        return NULL;
    }
    for (t = 0; t < WIDE_TARGETS; t++) {
        wt = &wide[t];
        if (wt->pixels && wt->x1 == gpu.clip_x1 && wt->y1 == gpu.clip_y1 && wt->x2 == gpu.clip_x2 &&
            wt->y2 == gpu.clip_y2) {
            wt->stamp = ++wide_clock;
            return wt;
        }
        if (wide[t].stamp < wide[oldest].stamp) {
            oldest = t;
        }
    }
    margin = (w / 6 + 1) & ~1; /* w * 4/3 in all, rounded to even */
    if (gpu.clip_x2 + 2 * margin >= SOFT_GPU_WIDTH) {
        return NULL;
    }
    wt = &wide[oldest];
    if (!wt->pixels && !(wt->pixels = malloc(sizeof(vram)))) {
        return NULL;
    }
    memset(wt->pixels, 0, sizeof(vram));
    wt->x1 = gpu.clip_x1;
    wt->y1 = gpu.clip_y1;
    wt->x2 = gpu.clip_x2;
    wt->y2 = gpu.clip_y2;
    wt->margin = margin;
    wt->drawn = 0;
    wt->stamp = ++wide_clock;
    /* Start from what VRAM holds, so a target made mid-frame is not black. */
    wide_mirror(wt->x1, wt->y1, w, h, 0, 0);
    return wt;
}

void SoftGpu_SetWidescreen(int on)
{
    int t;
    if (on == wide_on) {
        return;
    }
    wide_on = on;
    for (t = 0; t < WIDE_TARGETS; t++) {
        free(wide[t].pixels);
        memset(&wide[t], 0, sizeof(wide[t]));
    }
}

int SoftGpu_WideFrameView(int x, int y, int w, int h, const uint16_t **pixels, int *out_x, int *out_w)
{
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        const WideTarget *wt = &wide[t];
        if (wt->pixels && wt->x1 == x && wt->y1 == y && wt->x2 - wt->x1 + 1 == w && wt->y2 - wt->y1 + 1 >= h) {
            *pixels = wt->pixels;
            *out_x = x;
            *out_w = w + 2 * wt->margin;
            return 1;
        }
    }
    return 0;
}

int SoftGpu_WideFrame(int x, int y, int w, int h, const uint16_t **pixels, int *out_x, int *out_w)
{
    int t;
    for (t = 0; t < WIDE_TARGETS; t++) {
        WideTarget *wt = &wide[t];
        if (wt->pixels && wt->x1 == x && wt->y1 == y && wt->x2 - wt->x1 + 1 == w && wt->y2 - wt->y1 + 1 >= h) {
            if (!wt->drawn) {
                /* Nothing was drawn since it was last shown (a movie, or a
                 * still loaded straight into VRAM): the sides are stale. */
                int row;
                for (row = y; row < y + h; row++) {
                    uint16_t *out = wt->pixels + (row & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH;
                    memset(out + wt->x1, 0, (size_t)wt->margin * 2);
                    memset(out + wt->x2 + wt->margin + 1, 0, (size_t)wt->margin * 2);
                }
            }
            wt->drawn = 0;
            *pixels = wt->pixels;
            *out_x = x;
            *out_w = w + 2 * wt->margin;
            return 1;
        }
    }
    return 0;
}

void SoftGpu_Load(int x, int y, int w, int h, const uint16_t *pixels)
{
    int i, j;
    if (TextureDump_Tags) TextureDump_Loaded(x, y, w, h, pixels);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint16_t *target = vram_pixel(x + i, y + j);
            if (!gpu.mask_check || !(*target & 0x8000)) {
                *target = (uint16_t)(pixels[j * w + i] | (gpu.mask_set ? 0x8000 : 0));
            }
        }
    }
    wide_mirror(x, y, w, h, 0, 0);
    picture_from_words(x, y, w, h);
}

void SoftGpu_Store(int x, int y, int w, int h, uint16_t *pixels)
{
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            pixels[j * w + i] = *vram_pixel(x + i, y + j);
        }
    }
}

void SoftGpu_Move(int sx, int sy, int dx, int dy, int w, int h)
{
    int i, j;
    if (TextureDump_Tags) TextureDump_Moved(sx, sy, dx, dy, w, h);
    for (j = 0; j < h; j++) {
        /* Overlapping copies read each row forwards, as the hardware does. */
        for (i = 0; i < w; i++) {
            uint16_t *target = vram_pixel(dx + i, dy + j);
            if (!gpu.mask_check || !(*target & 0x8000)) {
                *target = (uint16_t)(*vram_pixel(sx + i, sy + j) | (gpu.mask_set ? 0x8000 : 0));
            }
        }
    }
    wide_mirror(dx, dy, w, h, 0, 0);
    if (picture) {
        /* The picture's own pixels move, row by row forwards like the words. */
        int hw = w * scale, hh = h * scale, hsx = sx * scale, hsy = sy * scale, hdx = dx * scale, hdy = dy * scale;
        if (!gpu.mask_check) {
            for (j = 0; j < hh; j++) {
                for (i = 0; i < hw; i++) *picture_pixel(hdx + i, hdy + j) = *picture_pixel(hsx + i, hsy + j);
            }
        } else {
            /* A word the mask kept was not written: its pixels stay too. */
            for (j = 0; j < h; j++) {
                for (i = 0; i < w; i++) {
                    int px, py;
                    if (*pixel(dx + i, dy + j) != (uint16_t)(*pixel(sx + i, sy + j) | 0x8000)) continue;
                    for (py = 0; py < scale; py++) {
                        for (px = 0; px < scale; px++) {
                            *picture_pixel(hdx + i * scale + px, hdy + j * scale + py) =
                                *picture_pixel(hsx + i * scale + px, hsy + j * scale + py);
                        }
                    }
                }
            }
        }
    }
}

static uint16_t pack(uint32_t rgb24)
{
    return (uint16_t)(((rgb24 >> 3) & 0x1f) | (((rgb24 >> 11) & 0x1f) << 5) |
                      (((rgb24 >> 19) & 0x1f) << 10));
}

void SoftGpu_Fill(int x, int y, int w, int h, uint32_t rgb24)
{
    int i, j;
    uint16_t colour = pack(rgb24);
    if (TextureDump_Tags) TextureDump_Cleared(x, y, w, h);
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            *vram_pixel(x + i, y + j) = colour;
        }
    }
    wide_mirror(x, y, w, h, 1, colour);
    picture_from_words(x, y, w, h);
}

static inline __attribute__((always_inline)) uint16_t texel(int u, int v)
{
    int x, y;
    u = (u & ~(gpu.window_mask_x * 8)) | ((gpu.window_x & gpu.window_mask_x) * 8);
    v = (v & ~(gpu.window_mask_y * 8)) | ((gpu.window_y & gpu.window_mask_y) * 8);
    u &= 0xff;
    v &= 0xff;
    y = gpu.page_y + v;
    if (shadow_on) {
        /* A replaced texel: the pack's colour, 0 for one painted transparent. */
        uint16_t cell = gpu.depth == 0 ? *TextureDump_Cell(gpu.page_x + u / 4, y, u & 3)
                        : gpu.depth == 1 ? *TextureDump_Cell(gpu.page_x + u / 2, y, (u & 1) * 2)
                                         : *TextureDump_Cell(gpu.page_x + u, y, 0);
        if (cell) return cell & 0x7fff;
    }
    if (gpu.depth == 0) {
        x = gpu.page_x + u / 4;
        return sample(gpu.clut_x + ((sample(x, y) >> ((u & 3) * 4)) & 0xf), gpu.clut_y);
    }
    if (gpu.depth == 1) {
        x = gpu.page_x + u / 2;
        return sample(gpu.clut_x + ((sample(x, y) >> ((u & 1) * 8)) & 0xff), gpu.clut_y);
    }
    return sample(gpu.page_x + u, y);
}

static inline __attribute__((always_inline)) int clamp8(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

/* The provenance tag of the word plot is about to draw, or NULL when it draws
 * into a widescreen target (whose words have no tags) or tracing is off.
 * Outside plot, whose local target hides the global one. */
static inline __attribute__((always_inline)) uint32_t *drawn_tag(const uint16_t *word)
{
    return TextureDump_Tags && target == vram ? &TextureDump_Tags[word - vram] : NULL;
}

/* flags: 1 raw texture, 2 semi-transparent, 4 textured, 8 dither-eligible */
static inline __attribute__((always_inline)) void plot(int x, int y, int r, int g, int b, int u, int v, int flags)
{
    uint16_t *target, source;
    uint32_t *tag;
    int semi = flags & 2;
    if (x < gpu.clip_x1 || x > gpu.clip_x2 || y < gpu.clip_y1 || y > gpu.clip_y2) {
        return;
    }
    target = pixel(x, y);
    if (gpu.mask_check && (*target & 0x8000)) {
        return;
    }
    if ((tag = drawn_tag(target)) != NULL) { /* drawn, not from the disc */
        *tag = 0;
        if (TextureDump_Shadow) memset(TextureDump_Shadow + (tag - TextureDump_Tags) * 4, 0, 4 * sizeof(uint16_t));
    }
    if (flags & 4) {
        source = texel(u, v);
        if (!source) {
            return;
        }
        semi = semi && (source & 0x8000);
        if (flags & 1) {
            r = (source & 0x1f) << 3;
            g = ((source >> 5) & 0x1f) << 3;
            b = ((source >> 10) & 0x1f) << 3;
        } else {
            r = ((source & 0x1f) * r) >> 4;
            g = (((source >> 5) & 0x1f) * g) >> 4;
            b = (((source >> 10) & 0x1f) * b) >> 4;
        }
    } else {
        source = 0;
    }
    if ((flags & 8) && gpu.dither) {
        int offset = dither_matrix[y & 3][(x - dither_shift) & 3];
        r += offset;
        g += offset;
        b += offset;
    }
    r = clamp8(r) >> 3;
    g = clamp8(g) >> 3;
    b = clamp8(b) >> 3;
    if (semi) {
        int br = *target & 0x1f, bg = (*target >> 5) & 0x1f, bb = (*target >> 10) & 0x1f;
        switch (gpu.blend) {
        case 0: r = (br + r) >> 1; g = (bg + g) >> 1; b = (bb + b) >> 1; break;
        case 1: r += br; g += bg; b += bb; break;
        case 2: r = br - r; g = bg - g; b = bb - b; break;
        default: r = br + (r >> 2); g = bg + (g >> 2); b = bb + (b >> 2); break;
        }
        r = r < 0 ? 0 : r > 31 ? 31 : r;
        g = g < 0 ? 0 : g > 31 ? 31 : g;
        b = b < 0 ? 0 : b > 31 ? 31 : b;
    }
    *target = (uint16_t)(r | (g << 5) | (b << 10) | (source & 0x8000) |
                         (gpu.mask_set ? 0x8000 : 0));
}

/* --- the scaled picture's own pass ------------------------------------
 * The same primitives, drawn again at scale x scale pixels per word into
 * the picture: positions and clipping scaled, texture coordinates carried
 * with a fraction so that a pack's image is sampled at its own resolution,
 * VRAM's own texels otherwise. No dithering; the mask bit is VRAM's. */

/* A texel for the picture: the pack's image if it replaces it, else the
 * word in VRAM through the palette. u and v in 16.16 texels. */
static inline __attribute__((always_inline)) int picture_texel(int u, int v, uint32_t *rgb)
{
    uint16_t word;
    if (shadow_on && TextureDump_Sample) {
        int got = TextureDump_Sample(gpu.page_x, gpu.page_y, gpu.depth, u, v, rgb);
        if (got == 1) return 1;
        if (got == 2) return 0;
    }
    word = texel(u >> 16, v >> 16);
    if (!word) return 0;
    *rgb = expand(word) | ((uint32_t)(word & 0x8000) << 16); /* bit 31: semi-transparent texel */
    return 1;
}

static inline __attribute__((always_inline)) void picture_plot_in(int hx, int hy, int r, int g, int b, int u, int v,
                                                                  int flags)
{
    uint32_t *target, rgb = 0;
    int semi = flags & 2;
    if (gpu.mask_check && (*pixel(hx >> scale_shift, hy >> scale_shift) & 0x8000)) return;
    target = picture_pixel(hx, hy);
    if (flags & 4) {
        if (!picture_texel(u, v, &rgb)) return;
        semi = semi && (rgb & 0x80000000u);
        if (flags & 1) {
            r = (int)((rgb >> 16) & 0xff);
            g = (int)((rgb >> 8) & 0xff);
            b = (int)(rgb & 0xff);
        } else {
            r = (int)(((rgb >> 16) & 0xff) * r) >> 7;
            g = (int)(((rgb >> 8) & 0xff) * g) >> 7;
            b = (int)((rgb & 0xff) * b) >> 7;
        }
    }
    r = clamp8(r);
    g = clamp8(g);
    b = clamp8(b);
    if (semi) {
        int br = (int)((*target >> 16) & 0xff), bg = (int)((*target >> 8) & 0xff), bb = (int)(*target & 0xff);
        switch (gpu.blend) {
        case 0: r = (br + r) >> 1; g = (bg + g) >> 1; b = (bb + b) >> 1; break;
        case 1: r += br; g += bg; b += bb; break;
        case 2: r = br - r; g = bg - g; b = bb - b; break;
        default: r = br + (r >> 2); g = bg + (g >> 2); b = bb + (b >> 2); break;
        }
        r = clamp8(r);
        g = clamp8(g);
        b = clamp8(b);
    }
    *target = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* The same, for a caller that has not clipped (rectangles and lines). */
static inline __attribute__((always_inline)) void picture_plot(int hx, int hy, int r, int g, int b, int u, int v,
                                                               int flags)
{
    if (hx < gpu.clip_x1 * scale || hx >= (gpu.clip_x2 + 1) * scale || hy < gpu.clip_y1 * scale ||
        hy >= (gpu.clip_y2 + 1) * scale) {
        return;
    }
    picture_plot_in(hx, hy, r, g, b, u, v, flags);
}

static int64_t picture_edge(const Vertex *a, const Vertex *b, int x, int y)
{
    return (int64_t)(b->x - a->x) * scale * (y - a->y * scale) - (int64_t)(b->y - a->y) * scale * (x - a->x * scale);
}

static void picture_triangle(Vertex a, Vertex b, Vertex c, int flags)
{
    int min_x, max_x, min_y, max_y, bias0, bias1, bias2;
    int64_t area = (int64_t)(b.x - a.x) * (c.y - a.y) - (int64_t)(b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) {
        Vertex swap = b;
        b = c;
        c = swap;
        area = -area;
    }
    min_x = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    max_x = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    min_y = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    max_y = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (max_x - min_x > 1023 || max_y - min_y > 511) return;
    if (min_x < gpu.clip_x1) min_x = gpu.clip_x1;
    if (min_y < gpu.clip_y1) min_y = gpu.clip_y1;
    if (max_x > gpu.clip_x2) max_x = gpu.clip_x2;
    if (max_y > gpu.clip_y2) max_y = gpu.clip_y2;
    bias0 = owns(&b, &c) ? 0 : -1;
    bias1 = owns(&c, &a) ? 0 : -1;
    bias2 = owns(&a, &b) ? 0 : -1;
    {
        /* Edge functions on picture pixels, attributes in 12.20 from vertex
         * a, stepped per picture pixel (a word is `scale` steps). */
        enum { FRACTION = 20, BIAS = 1 << 12 };
        const int32_t ex0 = (b.y - c.y), ey0 = (c.x - b.x), ex1 = (c.y - a.y), ey1 = (a.x - c.x);
        const int32_t ex2 = (a.y - b.y), ey2 = (b.x - a.x);
        const int hx0 = min_x * scale, hx1 = (max_x + 1) * scale - 1, hy0 = min_y * scale, hy1 = (max_y + 1) * scale - 1;
        /* Everything fits 32 bits: an edge function is at most 8192 * 4096,
         * an attribute 255 << 20 plus its steps; the machine is 32-bit. */
        int32_t row0 = (int32_t)(-picture_edge(&c, &b, hx0, hy0)) + bias0 * scale;
        int32_t row1 = (int32_t)(-picture_edge(&a, &c, hx0, hy0)) + bias1 * scale;
        int32_t row2 = (int32_t)(-picture_edge(&b, &a, hx0, hy0)) + bias2 * scale;
        const int32_t sx0 = ex0 * scale, sx1 = ex1 * scale, sx2 = ex2 * scale; /* per picture pixel */
        const int32_t sy0 = ey0 * scale, sy1 = ey1 * scale, sy2 = ey2 * scale;
        const int values[5][3] = {{a.r, b.r, c.r}, {a.g, b.g, c.g}, {a.b, b.b, c.b}, {a.u, b.u, c.u}, {a.v, b.v, c.v}};
        int32_t step_x[5], step_y[5], row[5];
        int k, hy, hx;
        for (k = 0; k < 5; k++) {
            int64_t nx = (int64_t)ex0 * values[k][0] + (int64_t)ex1 * values[k][1] + (int64_t)ex2 * values[k][2];
            int64_t ny = (int64_t)ey0 * values[k][0] + (int64_t)ey1 * values[k][1] + (int64_t)ey2 * values[k][2];
            step_x[k] = (int32_t)((nx * (1 << FRACTION) + (nx < 0 ? -area / 2 : area / 2)) / area / scale);
            step_y[k] = (int32_t)((ny * (1 << FRACTION) + (ny < 0 ? -area / 2 : area / 2)) / area / scale);
            row[k] = (int32_t)(values[k][0] * (1 << FRACTION) + BIAS + (int64_t)step_x[k] * (hx0 - a.x * scale) +
                               (int64_t)step_y[k] * (hy0 - a.y * scale));
        }
        for (hy = hy0; hy <= hy1; hy++) {
            int32_t w0 = row0, w1 = row1, w2 = row2;
            int32_t r = row[0], g = row[1], blue = row[2], u = row[3], v = row[4];
            for (hx = hx0; hx <= hx1; hx++) {
                if ((w0 | w1 | w2) >= 0) {
                    picture_plot_in(hx, hy, r >> FRACTION, g >> FRACTION, blue >> FRACTION, u >> (FRACTION - 16),
                                    v >> (FRACTION - 16), flags);
                }
                w0 += sx0;
                w1 += sx1;
                w2 += sx2;
                r += step_x[0];
                g += step_x[1];
                blue += step_x[2];
                u += step_x[3];
                v += step_x[4];
            }
            row0 += sy0;
            row1 += sy1;
            row2 += sy2;
            for (k = 0; k < 5; k++) row[k] += step_y[k];
        }
    }
}

static int64_t edge(const Vertex *a, const Vertex *b, int x, int y)
{
    return (int64_t)(b->x - a->x) * (y - a->y) - (int64_t)(b->y - a->y) * (x - a->x);
}

/* Top-left rule: an edge owns its pixels when it is a top or a left edge. */
static int owns(const Vertex *a, const Vertex *b)
{
    int dx = b->x - a->x, dy = b->y - a->y;
    return dy < 0 || (dy == 0 && dx > 0);
}

static inline __attribute__((always_inline)) void triangle_with(Vertex a, Vertex b, Vertex c, const int flags)
{
    int min_x, max_x, min_y, max_y, x, y, bias0, bias1, bias2;
    int64_t area = edge(&a, &b, c.x, c.y);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        Vertex swap = b;
        b = c;
        c = swap;
        area = -area;
    }
    min_x = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    max_x = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    min_y = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    max_y = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (max_x - min_x > 1023 || max_y - min_y > 511) {
        return;
    }
    if (min_x < gpu.clip_x1) { min_x = gpu.clip_x1; }
    if (min_y < gpu.clip_y1) { min_y = gpu.clip_y1; }
    if (max_x > gpu.clip_x2) { max_x = gpu.clip_x2; }
    if (max_y > gpu.clip_y2) { max_y = gpu.clip_y2; }
    /* With this winding, inside is edge >= 0; shared edges go to one side. */
    bias0 = owns(&b, &c) ? 0 : -1;
    bias1 = owns(&c, &a) ? 0 : -1;
    bias2 = owns(&a, &b) ? 0 : -1;
    {
        /* The edge functions are affine and, within the extent limit above,
         * fit 32 bits, so they are stepped rather than re-evaluated.
         * Attributes are stepped in 12.20 fixed point from vertex a, where
         * they are exact. The bias exceeds the accumulated rounding error
         * (under 2^-10 over the largest extent), so integer-valued samples
         * (1:1 texture mapping) never land a hair below their integer. */
        enum { FRACTION = 20, BIAS = 1 << 12 };
        const int32_t dx0 = b.y - c.y, dy0 = c.x - b.x, dx1 = c.y - a.y, dy1 = a.x - c.x;
        const int32_t dx2 = a.y - b.y, dy2 = b.x - a.x;
        int32_t row0 = (int32_t)-edge(&c, &b, min_x, min_y) + bias0;
        int32_t row1 = (int32_t)-edge(&a, &c, min_x, min_y) + bias1;
        int32_t row2 = (int32_t)-edge(&b, &a, min_x, min_y) + bias2;
        const int values[5][3] = {{a.r, b.r, c.r}, {a.g, b.g, c.g}, {a.b, b.b, c.b}, {a.u, b.u, c.u}, {a.v, b.v, c.v}};
        int32_t step_x[5], step_y[5], row[5];
        int k;
        for (k = 0; k < 5; k++) {
            int64_t nx = (int64_t)dx0 * values[k][0] + (int64_t)dx1 * values[k][1] + (int64_t)dx2 * values[k][2];
            int64_t ny = (int64_t)dy0 * values[k][0] + (int64_t)dy1 * values[k][1] + (int64_t)dy2 * values[k][2];
            step_x[k] = (int32_t)((nx * (1 << FRACTION) + (nx < 0 ? -area / 2 : area / 2)) / area);
            step_y[k] = (int32_t)((ny * (1 << FRACTION) + (ny < 0 ? -area / 2 : area / 2)) / area);
            row[k] = (int32_t)(values[k][0] * (1 << FRACTION) + BIAS + (int64_t)step_x[k] * (min_x - a.x) +
                               (int64_t)step_y[k] * (min_y - a.y));
        }
        for (y = min_y; y <= max_y; y++) {
            int32_t w0 = row0, w1 = row1, w2 = row2;
            int32_t r = row[0], g = row[1], blue = row[2], u = row[3], v = row[4];
            for (x = min_x; x <= max_x; x++) {
                if ((w0 | w1 | w2) >= 0) {
                    plot(x, y, r >> FRACTION, g >> FRACTION, blue >> FRACTION, u >> FRACTION, v >> FRACTION, flags);
                }
                w0 += dx0;
                w1 += dx1;
                w2 += dx2;
                r += step_x[0];
                g += step_x[1];
                blue += step_x[2];
                u += step_x[3];
                v += step_x[4];
            }
            row0 += dy0;
            row1 += dy1;
            row2 += dy2;
            for (k = 0; k < 5; k++) {
                row[k] += step_y[k];
            }
        }
    }
}

/* One copy of the loop per flag combination, so the per-pixel tests on
 * texturing, blending and dithering are resolved at compile time. */
static void triangle(Vertex a, Vertex b, Vertex c, int flags)
{
    switch (flags & 15) {
#define CASE(n) case n: triangle_with(a, b, c, n); break;
    CASE(0) CASE(1) CASE(2) CASE(3) CASE(4) CASE(5) CASE(6) CASE(7)
    CASE(8) CASE(9) CASE(10) CASE(11) CASE(12) CASE(13) CASE(14) CASE(15)
#undef CASE
    }
}

static void line(Vertex a, Vertex b, int flags)
{
    int dx = b.x - a.x, dy = b.y - a.y;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    int i;
    if ((dx < 0 ? -dx : dx) > 1023 || (dy < 0 ? -dy : dy) > 511) {
        return;
    }
    if (picture) { /* first: see the polygon's note */
        int hsteps = steps * scale, sx, sy;
        for (i = 0; i <= hsteps; i++) {
            int n = hsteps ? hsteps : 1;
            int hx = a.x * scale + dx * scale * i / n, hy = a.y * scale + dy * scale * i / n;
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) {
                    picture_plot(hx + sx, hy + sy, a.r + (b.r - a.r) * i / n, a.g + (b.g - a.g) * i / n,
                                 a.b + (b.b - a.b) * i / n, 0, 0, flags);
                }
            }
        }
    }
    for (i = 0; i <= steps; i++) {
        int n = steps ? steps : 1;
        plot(a.x + dx * i / n, a.y + dy * i / n, a.r + (b.r - a.r) * i / n,
             a.g + (b.g - a.g) * i / n, a.b + (b.b - a.b) * i / n, 0, 0, flags);
    }
}

static void set_colour(Vertex *vertex, uint32_t word)
{
    vertex->r = word & 0xff;
    vertex->g = (word >> 8) & 0xff;
    vertex->b = (word >> 16) & 0xff;
}

static void set_position(Vertex *vertex, uint32_t word)
{
    vertex->x = (((int32_t)(word << 21)) >> 21) + gpu.offset_x;
    vertex->y = (((int32_t)(word << 5)) >> 21) + gpu.offset_y;
}

static void set_page(uint32_t value)
{
    int bank = (int)((value >> 11) & (SOFT_GPU_BANKS - 1));
    texture_source = bank && banks[bank] ? banks[bank] : vram;
    gpu.page_x = (value & 0xf) * 64;
    gpu.page_y = ((value >> 4) & 1) * 256;
    gpu.blend = (value >> 5) & 3;
    gpu.depth = (value >> 7) & 3;
    if (gpu.depth == 3) {
        gpu.depth = 2;
    }
}

static size_t polygon(const uint32_t *words, size_t count)
{
    uint32_t command = words[0] >> 24;
    int quad = command & 8, textured = command & 4, shaded = command & 0x10;
    int vertices = quad ? 4 : 3, i;
    size_t need = (size_t)vertices * (1 + (textured ? 1 : 0)) + (shaded ? (size_t)vertices : 1);
    size_t at = 0;
    int flags = (command & 3) | (textured ? 4 : 0);
    Vertex v[4];
    if (count < need) {
        return 0;
    }
    if (shaded || (textured && !(command & 1))) {
        flags |= 8;
    }
    memset(v, 0, sizeof(v));
    for (i = 0; i < vertices; i++) {
        if (i == 0 || shaded) {
            set_colour(&v[i], words[at++]);
        } else {
            v[i].r = v[0].r; v[i].g = v[0].g; v[i].b = v[0].b;
        }
        set_position(&v[i], words[at++]);
        if (textured) {
            uint32_t word = words[at++];
            v[i].u = word & 0xff;
            v[i].v = (word >> 8) & 0xff;
            if (i == 0) {
                gpu.clut_x = ((word >> 16) & 0x3f) * 16;
                gpu.clut_y = (word >> 22) & 0x1ff;
            } else if (i == 1) {
                set_page(word >> 16);
            }
        }
    }
    shadow_on = textured && TextureDump_Prepare && texture_source == vram &&
                TextureDump_Prepare(gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y, v[0].u, v[0].v);
    if (textured && TextureDump_Enabled) {
        /* The texels the primitive covers: a quad's far edge is exclusive. */
        int u0 = v[0].u, u1 = v[0].u, v0 = v[0].v, v1 = v[0].v;
        for (i = 1; i < vertices; i++) {
            if (v[i].u < u0) u0 = v[i].u;
            if (v[i].u > u1) u1 = v[i].u;
            if (v[i].v < v0) v0 = v[i].v;
            if (v[i].v > v1) v1 = v[i].v;
        }
        TextureDump_Primitive(texture_source, gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y, u0, v0,
                              u1 > u0 ? u1 - 1 : u1, v1 > v0 ? v1 - 1 : v1);
    }
    /* The picture's pass first: it reads the mask bits VRAM has before this
     * primitive, as the primitive's own pass does. */
    if (picture) {
        picture_triangle(v[0], v[1], v[2], flags);
        if (quad) picture_triangle(v[1], v[2], v[3], flags);
    }
    triangle(v[0], v[1], v[2], flags);
    if (quad) {
        triangle(v[1], v[2], v[3], flags);
    }
    return need;
}

static size_t rectangle(const uint32_t *words, size_t count)
{
    static const int sizes[4] = {0, 1, 8, 16};
    uint32_t command = words[0] >> 24;
    int textured = command & 4, kind = (command >> 3) & 3, w, h, i, j;
    size_t need = 2 + (textured ? 1u : 0u) + (kind == 0 ? 1u : 0u), at = 2;
    int flags = (command & 3) | (textured ? 4 : 0);
    Vertex base;
    if (count < need) {
        return 0;
    }
    memset(&base, 0, sizeof(base));
    set_colour(&base, words[0]);
    set_position(&base, words[1]);
    if (textured) {
        base.u = words[at] & 0xff;
        base.v = (words[at] >> 8) & 0xff;
        gpu.clut_x = ((words[at] >> 16) & 0x3f) * 16;
        gpu.clut_y = (words[at] >> 22) & 0x1ff;
        at++;
    }
    w = h = sizes[kind];
    if (kind == 0) {
        w = words[at] & 0x3ff;
        h = (words[at] >> 16) & 0x1ff;
    }
    shadow_on = textured && TextureDump_Prepare && texture_source == vram &&
                TextureDump_Prepare(gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y, base.u, base.v);
    if (textured && TextureDump_Enabled && w && h) {
        TextureDump_Primitive(texture_source, gpu.page_x, gpu.page_y, gpu.depth, gpu.clut_x, gpu.clut_y, base.u,
                              base.v, base.u + w - 1, base.v + h - 1);
    }
    if (picture) { /* first: see the polygon's note */
        int hw = w * scale, hh = h * scale;
        for (j = 0; j < hh; j++) {
            for (i = 0; i < hw; i++) {
                picture_plot(base.x * scale + i, base.y * scale + j, base.r, base.g, base.b,
                             (base.u << 16) + (i << 16) / scale, (base.v << 16) + (j << 16) / scale, flags);
            }
        }
    }
    switch (flags) {
#define CASE(n) case n: \
        for (j = 0; j < h; j++) { \
            for (i = 0; i < w; i++) { \
                plot(base.x + i, base.y + j, base.r, base.g, base.b, base.u + i, base.v + j, n); \
            } \
        } \
        break;
    CASE(0) CASE(1) CASE(2) CASE(3) CASE(4) CASE(5) CASE(6) CASE(7)
#undef CASE
    }
    return need;
}

static size_t lines(const uint32_t *words, size_t count)
{
    uint32_t command = words[0] >> 24;
    int shaded = command & 0x10, poly = command & 8, flags = (command & 2) | (shaded ? 8 : 0);
    size_t at = 0;
    Vertex previous, next;
    memset(&previous, 0, sizeof(previous));
    if (count < (shaded ? 4u : 3u)) {
        return 0;
    }
    set_colour(&previous, words[at++]);
    set_position(&previous, words[at++]);
    for (;;) {
        if (poly && at < count && (words[at] & 0xf000f000u) == 0x50005000u) {
            return at + 1;
        }
        next = previous;
        if (shaded) {
            if (at >= count) { return 0; }
            set_colour(&next, words[at++]);
        }
        if (at >= count) { return 0; }
        set_position(&next, words[at++]);
        line(previous, next, flags);
        previous = next;
        if (!poly) {
            return at;
        }
        if (at >= count) { return 0; }
    }
}

size_t SoftGpu_Gp0(const uint32_t *words, size_t count)
{
    size_t at = 0;
    while (at < count) {
        uint32_t word = words[at], command = word >> 24;
        size_t used = 1;
        if (command >= 0x20 && command < 0x80) {
            size_t (*draw)(const uint32_t *, size_t) = command < 0x40 ? polygon : command < 0x60 ? lines : rectangle;
            WideTarget *wt;
            used = draw(words + at, count - at);
            if (used && (wt = wide_target()) != NULL) {
                /* Again into the widescreen target, shifted. Polygons and
                 * lines, which is what the GTE projects, are unclipped at the
                 * sides, so the scene carries on past the 4:3 edges. Sprites
                 * keep the 4:3 clip: they are the 2D screens, and those park
                 * what they do not show just past the edge (the deck
                 * builder's column of card numbers), where the console's clip
                 * hides it. The primitive's state words are idempotent. */
                int clip_x1 = gpu.clip_x1, clip_x2 = gpu.clip_x2, offset_x = gpu.offset_x;
                if (command < 0x60) {
                    gpu.clip_x2 += 2 * wt->margin;
                } else {
                    gpu.clip_x1 += wt->margin;
                    gpu.clip_x2 += wt->margin;
                }
                gpu.offset_x += wt->margin;
                target = wt->pixels;
                dither_shift = wt->margin;
                draw(words + at, count - at);
                target = vram;
                dither_shift = 0;
                gpu.clip_x1 = clip_x1;
                gpu.clip_x2 = clip_x2;
                gpu.offset_x = offset_x;
                wt->drawn++;
            }
        } else if (command == 0x02) {
            used = count - at >= 3 ? 3 : 0;
            if (used) {
                SoftGpu_Fill(words[at + 1] & 0x3f0, (words[at + 1] >> 16) & 0x1ff,
                             ((words[at + 2] & 0x3ff) + 15) & ~15, (words[at + 2] >> 16) & 0x1ff, word);
            }
        } else if (command >= 0x80 && command < 0xa0) {
            used = count - at >= 4 ? 4 : 0;
            if (used) {
                SoftGpu_Move(words[at + 1] & 0x3ff, (words[at + 1] >> 16) & 0x1ff,
                             words[at + 2] & 0x3ff, (words[at + 2] >> 16) & 0x1ff,
                             ((words[at + 3] - 1) & 0x3ff) + 1, (((words[at + 3] >> 16) - 1) & 0x1ff) + 1);
            }
        } else if (command >= 0xa0 && command < 0xc0) {
            if (count - at < 3) {
                used = 0;
            } else {
                int w = (int)((words[at + 2] - 1) & 0x3ff) + 1;
                int h = (int)(((words[at + 2] >> 16) - 1) & 0x1ff) + 1;
                size_t data = ((size_t)w * (size_t)h + 1) / 2;
                used = count - at >= 3 + data ? 3 + data : 0;
                if (used) {
                    SoftGpu_Load(words[at + 1] & 0x3ff, (words[at + 1] >> 16) & 0x1ff, w, h,
                                 (const uint16_t *)(words + at + 3));
                }
            }
        } else if (command >= 0xc0 && command < 0xe0) {
            used = count - at >= 3 ? 3 : 0;
        } else if (command == 0xe1) {
            set_page(word);
            gpu.dither = (word >> 9) & 1;
        } else if (command == 0xe2) {
            gpu.window_mask_x = word & 0x1f;
            gpu.window_mask_y = (word >> 5) & 0x1f;
            gpu.window_x = (word >> 10) & 0x1f;
            gpu.window_y = (word >> 15) & 0x1f;
        } else if (command == 0xe3) {
            gpu.clip_x1 = word & 0x3ff;
            gpu.clip_y1 = (word >> 10) & 0x3ff;
        } else if (command == 0xe4) {
            gpu.clip_x2 = word & 0x3ff;
            gpu.clip_y2 = (word >> 10) & 0x3ff;
        } else if (command == 0xe5) {
            gpu.offset_x = ((int32_t)(word << 21)) >> 21;
            gpu.offset_y = ((int32_t)(word << 10)) >> 21;
        } else if (command == 0xe6) {
            gpu.mask_set = word & 1;
            gpu.mask_check = (word >> 1) & 1;
        }
        if (!used) {
            break;
        }
        at += used;
    }
    return at;
}

/* Save states: VRAM (index 0) and the drawing state (index 1). */
void *SoftGpu_StateData(int index, size_t *size)
{
    *size = index ? sizeof(gpu) : sizeof(vram);
    return index ? (void *)&gpu : (void *)vram;
}
