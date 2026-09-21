/* The model polygon drivers: native forms of the hand-written GTE assembly at
 * 0x800612C0-0x80066E60. They are HMD primitive drivers. GsSortUnit calls one
 * per primitive block with the driver scratch area; the driver walks the
 * block's polygons, projects and lights them on the GTE, links GPU packets
 * into the ordering table and returns the next primitive.
 *
 * The thirty-two routines are one algorithm under five switches, which is
 * how they are written here. Read from the assembly:
 *
 *   shape      triangle or quad; flat (one normal, NCCS) or Gouraud (a normal
 *              per vertex, NCCT, plus NCCS for a quad's fourth)
 *   cull       the 0x0020xxxx types drop back faces (NCLIP <= 0); the
 *              0x0030xxxx types are double-sided. Both drop a polygon whose
 *              projection raised the GTE error flag
 *   window     the 0x02xx types carry a texture-window command as the first
 *              word of each polygon; the packet is wrapped in it and a reset
 *   translucent the second bank (0x80063F90 on) forces semi-transparency and
 *              chooses the blend mode from D_8009AFE5 and the first polygon
 *
 * Polygon records are halfwords: uv0, clut, uv1, tpage, uv2, then
 *   flat triangle     pad, normal, v0, v1, v2
 *   Gouraud triangle  pad, (normal, vertex) x 3
 *   flat quad         pad, uv3, normal, v0, v1, v2, v3
 *   Gouraud quad      n0, uv3, v0, (normal, vertex) x 3
 * Vertices and normals are 8-byte SVECTORs in the sections the scratch area
 * points at.
 *
 * Lighting mode D_8009AFE4: 0 takes colours from the cache at D_8009AFE0
 * (one word per normal), 1 only fills that cache (nothing is drawn, nothing
 * is culled), anything else lights every polygon and, when a cache is set,
 * refreshes it.
 *
 * A translucent template (bit 25 of the colour word) with D_8009AFDC set draws
 * each polygon a second time, flat in the template colour, with that CLUT and
 * blend mode 2.
 *
 * Shared-vertex models (0x0120xxxx / 0x0130xxxx, 0x80067354 on) are done in
 * two steps. The pre-pass at 0x80067220 projects a run of vertices once into
 * eight-byte results (screen xy; IR0 << 16 | SZ, or -1 when the projection
 * failed) and, unless the lighting mode is 0, lights a run of normals into
 * eight-byte colour slots (the second word of a slot when a colour cache is
 * set). The polygon drivers of that bank then only pick results up: a polygon
 * touching a failed vertex is dropped, culling is NCLIP on the stored screen
 * points (a quad is kept if either of its triangles faces the viewer), and
 * nothing is drawn in lighting mode 1.
 *
 * The last twelve (0x80069E44 on) draw the same records as outlines: a
 * draw-mode word and one closed polyline per triangle, two per quad. */
#include "types.h"
#include "pc/compat/gte.h"
#include <stddef.h>
#include <stdint.h>

extern u32 D_8009AFAC, D_8009AFB0, D_8009AFB4, D_8009AFB8; /* templates: FT3, FT4, GT3, GT4 */
extern u32 D_8009AFBC, D_8009AFC0, D_8009AFC4, D_8009AFC8; /* second-pass templates of the translucent bank */
extern u32 D_8009AFD8, D_8009AFDC, D_8009AFE0;             /* tpage bits, second-pass CLUT, colour cache */
extern s8 D_8009AFE4, D_8009AFE5;                          /* lighting mode, translucent blend override */
extern u32 D_800FE240;                                     /* LIBGS packet cursor */

enum { RTPS = 0x0180001, RTPT = 0x0280030, NCLIP = 0x1400006, AVSZ3 = 0x158002d, AVSZ4 = 0x168002e,
       NCCS = 0x108041b, NCCT = 0x118043f };

typedef struct Driver {
    int quad, gouraud, cull, window, translucent, shared;
} Driver;

static void load_vector(unsigned slot, const u8 *vectors, unsigned index)
{
    Memories_GteLoad(slot * 2, vectors + index * 8);
    Memories_GteLoad(slot * 2 + 1, vectors + index * 8 + 4);
}

static u32 *draw(u32 *scratch, const Driver *kind)
{
    static u32 *const templates[2][2] = {{&D_8009AFAC, &D_8009AFB4}, {&D_8009AFB0, &D_8009AFB8}};
    static u32 *const second_templates[2][2] = {{&D_8009AFBC, &D_8009AFC4}, {&D_8009AFC0, &D_8009AFC8}};
    const u32 *primitive = (const u32 *)(uintptr_t)scratch[0];
    const u32 *table = (const u32 *)(uintptr_t)scratch[1];
    const unsigned shift = scratch[2] & 31;
    u32 *packet = (u32 *)(uintptr_t)scratch[4];
    const u8 *polygon = (const u8 *)(uintptr_t)scratch[5] + primitive[1] * 4;
    const u8 *vertices = (const u8 *)(uintptr_t)scratch[6], *normals = (const u8 *)(uintptr_t)scratch[7];
    u32 *entries = (u32 *)(uintptr_t)table[1];
    unsigned count = primitive[0] >> 16;
    const int corners = kind->quad ? 4 : 3, lit = kind->gouraud ? corners : 1;
    const unsigned stride = (kind->quad ? (kind->gouraud ? 0x1c : 0x18) : (kind->gouraud ? 0x18 : 0x14)) +
                            (kind->window ? 4u : 0u);
    u32 colour = *templates[kind->quad][kind->gouraud], second_colour = colour;
    u32 page_bits = D_8009AFD8, second_clut = D_8009AFDC, *cache = (u32 *)(uintptr_t)D_8009AFE0;
    const int mode = D_8009AFE4;
    const u32 *shared_colours = NULL;
    if (count && kind->translucent) {
        const u32 *first = (const u32 *)(polygon + (kind->window ? 4 : 0));
        colour |= 0x02000000u;
        second_colour = colour;
        if (D_8009AFE5 != 0 || (first[1] & 0x00600000u) != 0) {
            if (D_8009AFE5 == 0) {
                page_bits = first[1] & 0x00600000u;
            }
            second_clut = 0;
        } else if (second_clut != 0) {
            second_clut = (first[0] >> 16) << 16;
            second_colour = *second_templates[kind->quad][kind->gouraud];
            page_bits = 0x00200000u;
        } else {
            page_bits = 0;
        }
    }
    Memories_GteWriteData(6, colour);
    if (kind->shared) {
        shared_colours = (const u32 *)((uintptr_t)scratch[9] + (cache ? 4u : 0u));
        if (mode == 1) {
            count = 0;
        }
    }
    for (; count; count--, polygon += stride) {
        const u8 *record = polygon + (kind->window ? 4 : 0);
        const u16 *half = (const u16 *)record;
        const u32 *words = (const u32 *)record;
        unsigned vertex[4], normal[4], i, packet_words, at;
        u32 uv[4], shade[4], screen[4], *entry, *out;
        if (kind->gouraud) {
            for (i = 0; i < (unsigned)corners; i++) {
                normal[i] = half[kind->quad && i == 0 ? 5 : 6 + i * 2];
                vertex[i] = half[7 + i * 2];
            }
        } else {
            normal[0] = half[kind->quad ? 7 : 6];
            for (i = 0; i < (unsigned)corners; i++) {
                vertex[i] = half[(kind->quad ? 8 : 7) + i];
            }
        }
        uv[0] = words[0];
        uv[1] = words[1];
        uv[2] = words[2];
        uv[3] = words[3];
        if (kind->shared) {
            int failed = 0, facing;
            for (i = 0; i < (unsigned)corners; i++) {
                const u32 *result = (const u32 *)(normals + vertex[i] * 8); /* scratch[7]: projected vertices */
                failed |= result[1] == 0xffffffffu;
                screen[i] = result[0];
                Memories_GteWriteData((kind->quad ? 16 : 17) + i, result[1]);
            }
            if (failed) {
                continue;
            }
            if (kind->cull) {
                for (i = 0; i < 3; i++) {
                    Memories_GteWriteData(12 + i, screen[i]);
                }
                Memories_GteCommand(NCLIP);
                facing = (s32)Memories_GteReadData(24) > 0;
                if (!facing && kind->quad) {
                    Memories_GteWriteData(12, screen[3]);
                    Memories_GteCommand(NCLIP);
                    facing = (s32)Memories_GteReadData(24) <= 0;
                }
                if (!facing) {
                    continue;
                }
            }
            for (i = 0; i < (unsigned)corners; i++) {
                shade[i] = shared_colours[normal[i] * 2];
            }
            shade[0] |= colour & 0xff000000u;
            Memories_GteCommand(kind->quad ? AVSZ4 : AVSZ3);
            goto emit;
        }
        for (i = 0; i < 3; i++) {
            load_vector(i, vertices, vertex[i]);
        }
        Memories_GteCommand(RTPT);
        if (mode != 1) {
            if ((s32)Memories_GteReadControl(31) < 0) {
                continue;
            }
            if (kind->cull) {
                Memories_GteCommand(NCLIP);
                if ((s32)Memories_GteReadData(24) <= 0) {
                    continue;
                }
            }
        }
        for (i = 0; i < 3; i++) {
            screen[i] = Memories_GteReadData(12 + i);
        }
        if (kind->quad) {
            load_vector(0, vertices, vertex[3]);
            Memories_GteCommand(RTPS);
            if (mode != 1 && (s32)Memories_GteReadControl(31) < 0) {
                continue;
            }
            screen[3] = Memories_GteReadData(14);
        }
        Memories_GteCommand(kind->quad ? AVSZ4 : AVSZ3);
        if (mode != 0) {
            if (!kind->gouraud || kind->quad) {
                unsigned single = kind->gouraud ? 3 : 0;
                load_vector(0, normals, normal[single]);
                Memories_GteCommand(NCCS);
                shade[single] = Memories_GteReadData(22);
                if (cache) {
                    cache[normal[single]] = shade[single];
                }
            }
            if (kind->gouraud) {
                for (i = 0; i < 3; i++) {
                    load_vector(i, normals, normal[i]);
                }
                Memories_GteCommand(NCCT);
                for (i = 0; i < 3; i++) {
                    shade[i] = Memories_GteReadData(20 + i);
                    if (cache) {
                        cache[normal[i]] = shade[i];
                    }
                }
            }
            if (mode == 1) {
                continue;
            }
        } else {
            for (i = 0; i < (unsigned)lit; i++) {
                shade[i] = cache ? cache[normal[i]] : colour;
            }
            shade[0] = (shade[0] & 0x00ffffffu) | (colour & 0xff000000u);
        }
    emit:
        entry = entries + ((Memories_GteReadData(7) >> shift));
        uv[1] |= page_bits;
        if (kind->translucent) {
            uv[1] = (words[1] & 0xff9fffffu) | page_bits;
        }
        packet_words = (unsigned)corners * 2 + (unsigned)lit + (kind->window ? 2u : 0u);
        for (at = 0; at < 2; at++) {
            out = packet + 1;
            if (kind->window) {
                *out++ = *(const u32 *)polygon;
            }
            for (i = 0; i < (unsigned)corners; i++) {
                if (i < (unsigned)lit) {
                    *out++ = shade[i];
                }
                *out++ = screen[i];
                *out++ = uv[i];
            }
            if (kind->window) {
                *out++ = 0xe2000000u;
            }
            packet[0] = (*entry & 0x00ffffffu) | ((u32)packet_words << 24);
            *entry = (u32)(uintptr_t)packet & 0x00ffffffu;
            packet += packet_words + 1;
            /* The translucent second pass: flat template colour, its CLUT, blend mode 2. */
            if (at == 1 || !(colour & 0x02000000u) || !second_clut) {
                break;
            }
            uv[0] = (uv[0] & 0xffffu) | second_clut;
            uv[1] = (uv[1] & 0xff9fffffu) | 0x00400000u;
            for (i = 0; i < (unsigned)lit; i++) {
                shade[i] = second_colour;
            }
        }
    }
    D_800FE240 = (u32)(uintptr_t)packet;
    return (u32 *)(uintptr_t)(scratch[0] + 8);
}

#define DRIVER_KIND(address, quad, gouraud, cull, window, translucent, shared) \
    u32 *func_##address(u32 *scratch) \
    { \
        static const Driver kind = {quad, gouraud, cull, window, translucent, shared}; \
        return draw(scratch, &kind); \
    }
#define DRIVER(address, quad, gouraud, cull, window, translucent) \
    DRIVER_KIND(address, quad, gouraud, cull, window, translucent, 0)
#define SHARED(address, quad, cull, window, translucent) DRIVER_KIND(address, quad, 1, cull, window, translucent, 1)

/*      address   quad gouraud cull window translucent      HMD type */
DRIVER(800612C0, 0, 0, 1, 0, 0) /* 0x00200009 */
DRIVER(8006151C, 0, 1, 1, 0, 0) /* 0x0020000D */
DRIVER(800617E0, 1, 0, 1, 0, 0) /* 0x00200011 */
DRIVER(80061A84, 1, 1, 1, 0, 0) /* 0x00200015 */
DRIVER(80061DDC, 0, 0, 1, 1, 0) /* 0x00200209 */
DRIVER(80062058, 0, 1, 1, 1, 0) /* 0x0020020D */
DRIVER(8006233C, 1, 0, 1, 1, 0) /* 0x00200211 */
DRIVER(80062600, 1, 1, 1, 1, 0) /* 0x00200215 */
DRIVER(80062978, 0, 0, 0, 0, 0) /* 0x00300009 */
DRIVER(80062BC0, 0, 1, 0, 0, 0) /* 0x0030000D */
DRIVER(80062E70, 1, 0, 0, 0, 0) /* 0x00300011 */
DRIVER(80063100, 1, 1, 0, 0, 0) /* 0x00300015 */
DRIVER(80063444, 0, 0, 0, 1, 0) /* 0x00300209 */
DRIVER(800636AC, 0, 1, 0, 1, 0) /* 0x0030020D */
DRIVER(8006397C, 1, 0, 0, 1, 0) /* 0x00300211 */
DRIVER(80063C2C, 1, 1, 0, 1, 0) /* 0x00300215 */
DRIVER(80063F90, 0, 0, 1, 0, 1)
DRIVER(80064248, 0, 1, 1, 0, 1)
DRIVER(80064568, 1, 0, 1, 0, 1)
DRIVER(80064868, 1, 1, 1, 0, 1)
DRIVER(80064C1C, 0, 0, 1, 1, 1)
DRIVER(80064EF4, 0, 1, 1, 1, 1)
DRIVER(80065234, 1, 0, 1, 1, 1)
DRIVER(80065554, 1, 1, 1, 1, 1)
DRIVER(80065928, 0, 0, 0, 0, 1)
DRIVER(80065BCC, 0, 1, 0, 0, 1)
DRIVER(80065ED8, 1, 0, 0, 0, 1)
DRIVER(800661C4, 1, 1, 0, 0, 1)
DRIVER(80066564, 0, 0, 0, 1, 1)
DRIVER(80066828, 0, 1, 0, 1, 1)
DRIVER(80066B54, 1, 0, 0, 1, 1)
DRIVER(80066E60, 1, 1, 0, 1, 1)

/*      address   quad cull window translucent      HMD type */
SHARED(80067354, 0, 1, 0, 0) /* 0x0120000D */
SHARED(8006759C, 1, 1, 0, 0) /* 0x01200015 */
SHARED(80067858, 0, 1, 1, 0) /* 0x0120020D */
SHARED(80067ABC, 1, 1, 1, 0) /* 0x01200215 */
SHARED(80067D94, 0, 0, 0, 0) /* 0x0130000D */
SHARED(80067FD0, 1, 0, 0, 0) /* 0x01300015 */
SHARED(8006825C, 0, 0, 1, 0) /* 0x0130020D */
SHARED(800684B4, 1, 0, 1, 0) /* 0x01300215 */
SHARED(8006875C, 0, 1, 0, 1)
SHARED(80068A00, 1, 1, 0, 1)
SHARED(80068D18, 0, 1, 1, 1)
SHARED(80068FD8, 1, 1, 1, 1)
SHARED(8006930C, 0, 0, 0, 1)
SHARED(800695A4, 1, 0, 0, 1)
SHARED(8006988C, 0, 0, 1, 1)
SHARED(80069B40, 1, 0, 1, 1)

/* The shared-vertex pre-pass (see the top of the file). Its primitive is
 * seven words: header, vertex count, source and result offsets, normal count,
 * source and result offsets (offsets in eight-byte units). */
u32 *func_80067220(u32 *scratch)
{
    const u32 *primitive = (const u32 *)(uintptr_t)scratch[0];
    const int mode = D_8009AFE4;
    unsigned count;
    Memories_GteWriteData(6, D_8009AFB4 & 0x00ffffffu);
    if ((count = primitive[1]) != 0) {
        const u8 *source = (const u8 *)(uintptr_t)scratch[6] + primitive[2] * 8;
        u32 *result = (u32 *)((uintptr_t)scratch[7] + primitive[3] * 8);
        for (; count; count--, source += 8, result += 2) {
            Memories_GteLoad(0, source);
            Memories_GteLoad(1, source + 4);
            Memories_GteCommand(RTPS);
            result[1] = 0xffffffffu;
            if (mode == 1 || (s32)Memories_GteReadControl(31) >= 0) {
                result[0] = Memories_GteReadData(14);
                result[1] = (Memories_GteReadData(8) << 16) | Memories_GteReadData(19);
            }
        }
    }
    if (mode != 0 && (count = primitive[4]) != 0) {
        const u8 *source = (const u8 *)(uintptr_t)scratch[8] + primitive[5] * 8;
        u32 *result = (u32 *)((uintptr_t)scratch[9] + primitive[6] * 8 + (D_8009AFE0 ? 4u : 0u));
        for (; count; count--, source += 8, result += 2) {
            Memories_GteLoad(0, source);
            Memories_GteLoad(1, source + 4);
            Memories_GteCommand(NCCS);
            result[0] = Memories_GteReadData(22);
        }
    }
    return (u32 *)(uintptr_t)(scratch[0] + 0x1c);
}

/* Outlines. `layout` is the polygon record being outlined (its vertex
 * indices sit where the polygon drivers find them). */
extern u32 D_8009AFCC, D_8009AFD0, D_8009AFD4; /* quad and triangle polyline words, draw-mode word */

static u32 *outline(u32 *scratch, int quad, int gouraud, int window, int shared)
{
    const u32 *primitive = (const u32 *)(uintptr_t)scratch[0];
    const u32 *table = (const u32 *)(uintptr_t)scratch[1];
    const unsigned shift = scratch[2] & 31;
    u32 *packet = (u32 *)(uintptr_t)scratch[4], *entries = (u32 *)(uintptr_t)table[1];
    const u8 *polygon = (const u8 *)(uintptr_t)scratch[5] + primitive[1] * 4;
    const u8 *vertices = (const u8 *)(uintptr_t)scratch[6];
    const u32 *projected = (const u32 *)(uintptr_t)scratch[7];
    const unsigned stride = (quad ? (gouraud ? 0x1c : 0x18) : (gouraud ? 0x18 : 0x14)) + (window ? 4u : 0u);
    const int corners = quad ? 4 : 3;
    const u32 line = quad ? D_8009AFCC : D_8009AFD0;
    unsigned count = (s32)D_8009AFE4 == 1 ? 0 : primitive[0] >> 16;
    for (; count; count--, polygon += stride) {
        const u16 *half = (const u16 *)(polygon + (window ? 4 : 0));
        u32 screen[4], *entry, *out = packet + 1;
        unsigned vertex[4], words;
        int i, failed = 0;
        for (i = 0; i < corners; i++) {
            vertex[i] = gouraud ? half[7 + i * 2] : half[(quad ? 8 : 7) + i];
        }
        if (shared) {
            for (i = 0; i < corners; i++) {
                failed |= projected[vertex[i] * 2 + 1] == 0xffffffffu;
                screen[i] = projected[vertex[i] * 2];
                Memories_GteWriteData((quad ? 16 : 17) + (unsigned)i, projected[vertex[i] * 2 + 1]);
            }
        } else {
            for (i = 0; i < 3; i++) {
                load_vector((unsigned)i, vertices, vertex[i]);
            }
            Memories_GteCommand(RTPT);
            failed = (s32)Memories_GteReadControl(31) < 0;
            for (i = 0; i < 3; i++) {
                screen[i] = Memories_GteReadData(12 + (unsigned)i);
            }
            if (quad && !failed) {
                load_vector(0, vertices, vertex[3]);
                Memories_GteCommand(RTPS);
                failed = (s32)Memories_GteReadControl(31) < 0;
                screen[3] = Memories_GteReadData(14);
            }
        }
        if (failed) {
            continue;
        }
        Memories_GteCommand(quad ? AVSZ4 : AVSZ3);
        *out++ = D_8009AFD4;
        *out++ = line;
        *out++ = screen[0];
        *out++ = screen[1];
        if (quad) {
            *out++ = screen[3];
            *out++ = 0x55555555u;
            *out++ = line;
            *out++ = screen[3];
            *out++ = screen[2];
            *out++ = screen[0];
        } else {
            *out++ = screen[2];
            *out++ = screen[0];
        }
        *out++ = 0x55555555u;
        words = (unsigned)(out - packet) - 1;
        entry = entries + (Memories_GteReadData(7) >> shift);
        packet[0] = (*entry & 0x00ffffffu) | ((u32)words << 24);
        *entry = (u32)(uintptr_t)packet & 0x00ffffffu;
        packet = out;
    }
    D_800FE240 = (u32)(uintptr_t)packet;
    return (u32 *)(uintptr_t)(scratch[0] + 8);
}

#define OUTLINE(address, quad, gouraud, window, shared) \
    u32 *func_##address(u32 *scratch) { return outline(scratch, quad, gouraud, window, shared); }

OUTLINE(80069E44, 0, 0, 0, 0)
OUTLINE(80069F94, 0, 1, 0, 0)
OUTLINE(8006A0E8, 1, 0, 0, 0)
OUTLINE(8006A268, 1, 1, 0, 0)
OUTLINE(8006A3F0, 0, 0, 1, 0)
OUTLINE(8006A540, 0, 1, 1, 0)
OUTLINE(8006A694, 1, 0, 1, 0)
OUTLINE(8006A814, 1, 1, 1, 0)
OUTLINE(8006A99C, 0, 1, 0, 1)
OUTLINE(8006AAFC, 1, 1, 0, 1)
OUTLINE(8006AC88, 0, 1, 1, 1)
OUTLINE(8006ADE8, 1, 1, 1, 1)
