#include "soft_gpu.h"
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

/* Where texels and palettes are read: VRAM, or one of the banks below. Set by
 * every texture-page word, so it is never stale and never part of a state. */
static uint16_t *texture_source;

static uint16_t *banks[SOFT_GPU_BANKS];

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
    gpu.clip_x2 = SOFT_GPU_WIDTH - 1;
    gpu.clip_y2 = SOFT_GPU_HEIGHT - 1;
}

static inline __attribute__((always_inline)) uint16_t *pixel(int x, int y)
{
    return &vram[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

/* The same word in whichever bank the current texture page names. */
static inline __attribute__((always_inline)) uint16_t sample(int x, int y)
{
    return texture_source[(y & (SOFT_GPU_HEIGHT - 1)) * SOFT_GPU_WIDTH + (x & (SOFT_GPU_WIDTH - 1))];
}

void SoftGpu_Load(int x, int y, int w, int h, const uint16_t *pixels)
{
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint16_t *target = pixel(x + i, y + j);
            if (!gpu.mask_check || !(*target & 0x8000)) {
                *target = (uint16_t)(pixels[j * w + i] | (gpu.mask_set ? 0x8000 : 0));
            }
        }
    }
}

void SoftGpu_Store(int x, int y, int w, int h, uint16_t *pixels)
{
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            pixels[j * w + i] = *pixel(x + i, y + j);
        }
    }
}

void SoftGpu_Move(int sx, int sy, int dx, int dy, int w, int h)
{
    int i, j;
    for (j = 0; j < h; j++) {
        /* Overlapping copies read each row forwards, as the hardware does. */
        for (i = 0; i < w; i++) {
            uint16_t *target = pixel(dx + i, dy + j);
            if (!gpu.mask_check || !(*target & 0x8000)) {
                *target = (uint16_t)(*pixel(sx + i, sy + j) | (gpu.mask_set ? 0x8000 : 0));
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
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            *pixel(x + i, y + j) = colour;
        }
    }
}

static inline __attribute__((always_inline)) uint16_t texel(int u, int v)
{
    int x, y;
    u = (u & ~(gpu.window_mask_x * 8)) | ((gpu.window_x & gpu.window_mask_x) * 8);
    v = (v & ~(gpu.window_mask_y * 8)) | ((gpu.window_y & gpu.window_mask_y) * 8);
    u &= 0xff;
    v &= 0xff;
    y = gpu.page_y + v;
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

/* flags: 1 raw texture, 2 semi-transparent, 4 textured, 8 dither-eligible */
static inline __attribute__((always_inline)) void plot(int x, int y, int r, int g, int b, int u, int v, int flags)
{
    uint16_t *target, source;
    int semi = flags & 2;
    if (x < gpu.clip_x1 || x > gpu.clip_x2 || y < gpu.clip_y1 || y > gpu.clip_y2) {
        return;
    }
    target = pixel(x, y);
    if (gpu.mask_check && (*target & 0x8000)) {
        return;
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
        int offset = dither_matrix[y & 3][x & 3];
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
        if (command >= 0x20 && command < 0x40) {
            used = polygon(words + at, count - at);
        } else if (command >= 0x40 && command < 0x60) {
            used = lines(words + at, count - at);
        } else if (command >= 0x60 && command < 0x80) {
            used = rectangle(words + at, count - at);
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
