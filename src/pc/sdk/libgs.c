/* LIBGS graph/display-buffer management, ported from the resident Psy-Q 4.6
 * routines. State stays in the library's guest globals because game code
 * reads and writes several of them (GsDRAWENV, GsDISPENV) directly. */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "pc/compat/libgs_ot.h"
#include "pc/guest/image.h"
#include "pc/debug/crash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern short D_800FE030[2], D_800FE034[2]; /* display buffer x[2], y[2] */
extern short D_800FE038[2], D_800FE03C[2]; /* offset-mode copies */
extern short D_800FE040[2];                /* GsSetOrign */
extern DRAWENV D_800FE048;                 /* GsDRAWENV */
extern DISPENV D_800FE0A8;                 /* GsDISPENV */
extern short D_800FE0BC, D_800FE0BE;       /* offset applied through the GTE */
extern RECT D_800FE0C0;                    /* CLIP2 */
extern int D_800FE0C8;                     /* frame counter, never zero */
extern short D_800FE0CC, D_800FE0CE;       /* active buffer, GsOFSGPU flag */
extern int D_800FE0D0, D_800FE0D4;         /* width, height */
extern int D_800FE0D8, D_800FE0DC, D_800FE0E0;
extern u8 D_800FE010[0x20];
extern MATRIX D_800FE0E8, D_800FE108, D_800FE168, D_800FE188;
extern u32 D_800FE240;                     /* GsSetWorkBase */
extern void *D_800E9D98[];                 /* current model ordering table */

void *func_80058F10(void)
{
    return D_800E9D98[0];
}


#define IMAGE ((MemoriesMemory *)(uintptr_t)MEMORIES_GUEST_RAM) /* unused token */
/* Four frame tables hold 0x1444 entries; anything longer is a corrupt chain. */
#define SORT_HOP_LIMIT 0x20000u

static void require(MemoriesGsResult result, const char *name)
{
    if (result != MEMORIES_GS_OK) {
        char detail[96];
        snprintf(detail, sizeof(detail), "%s rejected its ordering table (%d)", name, (int)result);
        Crash_ReportFatal("GS", detail);
        exit(70);
    }
}

void GsClearOt(unsigned short offset, unsigned short point, void *ot)
{
    require(Memories_GsClearOt(IMAGE, offset, point, (uint32_t)(uintptr_t)ot), "GsClearOt");
}

void *GsSortOt(void *source, void *destination)
{
    require(Memories_GsSortOt(IMAGE, (uint32_t)(uintptr_t)source,
                              (uint32_t)(uintptr_t)destination, SORT_HOP_LIMIT), "GsSortOt");
    return destination;
}

static void set_draw_buffer_clip(void)
{
    int active = D_800FE0CC;
    D_800FE048.clip.h = D_800FE0C0.h;
    D_800FE048.clip.w = D_800FE0C0.w;
    D_800FE048.clip.x = (short)(D_800FE0C0.x + D_800FE030[active]);
    D_800FE048.clip.y = (short)(D_800FE0C0.y + D_800FE034[active]);
    PutDrawEnv(&D_800FE048);
}

static void set_draw_buffer_offset(void)
{
    int active = D_800FE0CC;
    if (D_800FE0CE) {
        D_800FE0BE = 0;
        D_800FE0BC = 0;
        D_800FE048.ofs[0] = (short)(D_800FE040[0] + D_800FE030[active]);
        D_800FE048.ofs[1] = (short)(D_800FE040[1] + D_800FE034[active]);
        PutDrawEnv(&D_800FE048);
    } else {
        /* The original indexes these with the *inactive* buffer. */
        short x = (short)(D_800FE040[0] + D_800FE030[active ? 0 : 1]);
        short y = (short)(D_800FE040[1] + D_800FE034[active ? 0 : 1]);
        SetGeomOffset(x, y);
        D_800FE0BC = x;
        D_800FE0BE = y;
    }
}

static void variable_init(unsigned short width, unsigned short height)
{
    D_800FE0D0 = width;
    D_800FE0D4 = height;
    memset(&D_800FE168, 0, sizeof(MATRIX));
    D_800FE168.m[0][0] = D_800FE168.m[1][1] = D_800FE168.m[2][2] = 0x1000;
    D_800FE188 = D_800FE168;
    D_800FE0E8 = D_800FE168;
    D_800FE0E8.m[0][0] = D_800FE0E8.m[1][1] = D_800FE0E8.m[2][2] = 0;
    D_800FE108 = D_800FE0E8;
    D_800FE038[0] = D_800FE038[1] = D_800FE03C[0] = D_800FE03C[1] = 0;
    D_800FE040[0] = D_800FE040[1] = 0;
    D_800FE0C0.x = D_800FE0C0.y = 0;
    D_800FE188.m[1][1] = (short)((((int)height << 14) / (int)width) / 3);
    D_800FE010[3] = D_800FE010[0x13] = 3;
    D_800FE010[7] = D_800FE010[0x17] = 2;
    D_800FE0C8 = 1;
    D_800FE0C0.w = (short)width;
    D_800FE0C0.h = (short)height;
}

/* No GPU reset and no environment upload: the movie player switches modes
 * with this and installs the environments itself. */
void GsInitGraph2(unsigned short width, unsigned short height, unsigned short interlace,
                  unsigned short dither, unsigned short vram)
{
    D_800FE048.tpage = 0;
    D_800FE048.dtd = (u8)dither;
    D_800FE048.dfe = 0;
    D_800FE048.isbg = 0;
    D_800FE0A8.disp.w = (short)width;
    D_800FE0A8.disp.h = (short)height;
    D_800FE0A8.isinter = interlace & 1;
    D_800FE0CE = interlace & 4;
    D_800FE0A8.isrgb24 = (u8)vram;
    variable_init(width, height);
}

void GsInitGraph(unsigned short width, unsigned short height, unsigned short interlace,
                 unsigned short dither, unsigned short vram)
{
    ResetGraph(((interlace >> 4) & 3) == 3 ? 3 : 0);
    D_800FE048.ofs[0] = D_800FE048.ofs[1] = 0;
    memset(&D_800FE048.tw, 0, sizeof(D_800FE048.tw));
    D_800FE048.tpage = 0;
    D_800FE048.dtd = (u8)dither;
    D_800FE048.dfe = 0;
    D_800FE048.isbg = 0;
    PutDrawEnv(&D_800FE048);
    memset(&D_800FE0A8, 0, 0x10);
    D_800FE0A8.disp.w = (short)width;
    D_800FE0A8.disp.h = (short)height;
    D_800FE0A8.isinter = interlace & 1;
    D_800FE0CE = interlace & 4;
    D_800FE0A8.isrgb24 = (u8)vram;
    PutDispEnv(&D_800FE0A8);
    InitGeom();
    D_800FE0CC = 0;
    variable_init(width, height);
    set_draw_buffer_clip();
    set_draw_buffer_offset();
}

void GsDefDispBuff(unsigned short x0, unsigned short y0, unsigned short x1, unsigned short y1)
{
    D_800FE030[0] = (short)x0;
    D_800FE030[1] = (short)x1;
    D_800FE034[0] = (short)y0;
    D_800FE034[1] = (short)y1;
    if (D_800FE0CE) {
        D_800FE038[0] = D_800FE038[1] = D_800FE03C[0] = D_800FE03C[1] = 0;
    } else {
        D_800FE038[0] = (short)x0;
        D_800FE038[1] = (short)x1;
        D_800FE03C[0] = (short)y0;
        D_800FE03C[1] = (short)y1;
    }
    set_draw_buffer_clip();
    set_draw_buffer_offset();
}

void GsSwapDispBuff(void)
{
    int active = D_800FE0CC;
    D_800FE0A8.disp.x = D_800FE030[active];
    D_800FE0A8.disp.y = D_800FE034[active];
    PutDispEnv(&D_800FE0A8);
    SetDispMask(1);
    D_800FE0C8++;
    if (D_800FE0C8 == 0) {
        D_800FE0C8 = 1;
    }
    D_800FE0CC = D_800FE0CC == 0;
    set_draw_buffer_clip();
    set_draw_buffer_offset();
}

int GsGetActiveBuff(void)
{
    return D_800FE0CC;
}

void GsSetWorkBase(void *base)
{
    D_800FE240 = (u32)base;
}

void GsSetOrign(int x, int y)
{
    D_800FE040[0] = (short)x;
    D_800FE040[1] = (short)y;
}

void GsInit3D(void)
{
    D_800FE040[0] = (short)(D_800FE0D0 / 2);
    D_800FE040[1] = (short)(D_800FE0D4 / 2);
    set_draw_buffer_offset();
    D_800FE0E0 = 10;
    D_800FE0DC = 0;
    D_800FE0D8 = 0x3fff;
}

void GsSetAmbient(long r, long g, long b)
{
    SetBackColor(r >> 4, g >> 4, b >> 4);
}

void GsSetProjection(long h)
{
    SetGeomScreen(h);
}

void GsSetLsMatrix(MATRIX *matrix)
{
    SetRotMatrix(matrix);
    SetTransMatrix(matrix);
}

/* _make_packet: link `words` payload words at priority `pri`. The entry keeps
 * only the low 24 bits of the packet address, as the original's byte store
 * does; returns the next free packet address. */
static u32 *make_packet(u32 *packet, u32 *ot, unsigned pri, unsigned words)
{
    int index = (int)(pri & 0xffff) - (int)ot[2];
    u32 *entry;
    if (index < 0) {
        fprintf(stderr, "memories-pc: LIBGS packet priority %u is below the table offset %u\n",
                pri & 0xffff, (unsigned)ot[2]);
    }
    entry = (u32 *)(uintptr_t)(ot[1] + (u32)index * 4);
    packet[0] = (*entry & 0x00ffffffu) | ((u32)words << 24);
    *entry = (u32)(uintptr_t)packet & 0x00ffffffu;
    return packet + words + 1;
}

typedef struct BoxFill {
    u32 attribute;
    short x, y;
    u16 w, h;
    u8 r, g, b;
} BoxFill;

void GsSortBoxFill(BoxFill *box, u32 *ot, unsigned short pri)
{
    u32 *packet = (u32 *)(uintptr_t)D_800FE240;
    int attribute = (int)box->attribute;
    u8 *bytes = (u8 *)packet;
    if (attribute < 0) {
        return; /* display off */
    }
    packet[1] = 0xe1000200u | ((u32)(attribute >> 17) & 0x180) | ((u32)(attribute >> 23) & 0x60);
    bytes[8] = box->r;
    bytes[9] = box->g;
    bytes[10] = box->b;
    bytes[11] = (u8)(0x60 | ((attribute >> 29) & 2));
    *(u16 *)(bytes + 12) = (u16)(box->x + D_800FE0BC);
    *(u16 *)(bytes + 14) = (u16)(box->y + D_800FE0BE);
    *(u16 *)(bytes + 16) = box->w;
    *(u16 *)(bytes + 18) = box->h;
    D_800FE240 = (u32)(uintptr_t)make_packet(packet, ot, pri, 4);
}

/* GsLINE and GsGLINE share their first fields; the shaded one carries a
 * second colour. Each becomes a draw-mode word (dither on, the attribute's
 * semi-transparency rate) followed by the line primitive, offset like every
 * LIBGS 2D primitive. Attribute bit 31 hides it. */
typedef struct Line {
    u32 attribute;
    short x0, y0, x1, y1;
    u8 r0, g0, b0, r1, g1, b1;
} Line;

static void sort_line(const Line *line, u32 *ot, unsigned short pri, int shaded)
{
    u32 *packet = (u32 *)(uintptr_t)D_800FE240, *at = packet + 1;
    u32 attribute = line->attribute, code = (shaded ? 0x50u : 0x40u) | (((s32)attribute >> 29) & 2);
    if ((s32)attribute < 0) {
        return;
    }
    *at++ = 0xe1000200u | (((s32)attribute >> 23) & 0x60);
    *at++ = line->r0 | ((u32)line->g0 << 8) | ((u32)line->b0 << 16) | (code << 24);
    *at++ = (u16)(line->x0 + D_800FE0BC) | ((u32)(u16)(line->y0 + D_800FE0BE) << 16);
    if (shaded) {
        *at++ = line->r1 | ((u32)line->g1 << 8) | ((u32)line->b1 << 16);
    }
    *at++ = (u16)(line->x1 + D_800FE0BC) | ((u32)(u16)(line->y1 + D_800FE0BE) << 16);
    D_800FE240 = (u32)(uintptr_t)make_packet(packet, ot, pri, shaded ? 5 : 4);
}

void GsSortLine(Line *line, u32 *ot, unsigned short pri) { sort_line(line, ot, pri, 0); }
void GsSortGLine(Line *line, u32 *ot, unsigned short pri) { sort_line(line, ot, pri, 1); }

typedef struct Sprite {
    u32 attribute;
    short x, y;
    u16 w, h;
    u16 tpage;
    u8 u, v;
    short cx, cy;
    u8 r, g, b;
    short mx, my;
    short scalex, scaley;
    long rotate;
} Sprite;

static u32 sprite_colour(const Sprite *sprite, u32 code)
{
    u32 attribute = sprite->attribute;
    return code | ((attribute >> 5) & 0x02000000u) | ((attribute << 18) & 0x01000000u) |
           ((u32)sprite->b << 16) | ((u32)sprite->g << 8) | sprite->r;
}

static u32 sprite_clut(const Sprite *sprite)
{
    return ((u32)sprite->cy << 22) | (((u32)sprite->cx << 12) & 0x3f0000u);
}

static void sort_plain_sprite(const Sprite *sprite, u32 *ot, unsigned pri, int mx, int my)
{
    u32 *packet = (u32 *)(uintptr_t)D_800FE240;
    u32 attribute = sprite->attribute;
    packet[1] = 0xe1000200u | ((attribute >> 17) & 0x180) | (sprite->tpage & 0x1f) | ((attribute >> 23) & 0x60);
    packet[2] = sprite_colour(sprite, 0x64000000u);
    packet[3] = ((u32)(sprite->x + D_800FE0BC - mx) & 0xffff) | ((u32)(sprite->y + D_800FE0BE - my) << 16);
    packet[4] = sprite->u | ((u32)sprite->v << 8) | sprite_clut(sprite);
    packet[5] = sprite->w | ((u32)sprite->h << 16);
    D_800FE240 = (u32)(uintptr_t)make_packet(packet, ot, pri, 5);
}

void GsSortFastSprite(Sprite *sprite, u32 *ot, unsigned short pri)
{
    if ((int)sprite->attribute >= 0 && sprite->w && sprite->h) {
        sort_plain_sprite(sprite, ot, pri, 0, 0);
    }
}

void GsSortSprite(Sprite *sprite, u32 *ot, unsigned short pri)
{
    u32 attribute = sprite->attribute, *packet, xy[4];
    MATRIX matrix = D_800FE168;
    int u_left, u_right, v_top, v_bottom, i;
    if ((int)attribute < 0 || !sprite->w || !sprite->h) {
        return;
    }
    if (((attribute >> 27) & 1) || (sprite->scalex == 0x1000 && sprite->scaley == 0x1000 &&
                                    sprite->rotate == 0 && !(attribute & 0xc00000))) {
        sort_plain_sprite(sprite, ot, pri, sprite->mx, sprite->my);
        return;
    }
    if (sprite->rotate) {
        SVECTOR angle = {0, 0, (short)(sprite->rotate / 360), 0};
        RotMatrix(&angle, &matrix);
    }
    if (sprite->scalex != 0x1000 || sprite->scaley != 0x1000) {
        VECTOR scale = {sprite->scalex, sprite->scaley, 0, 0};
        ScaleMatrix(&matrix, &scale);
    }
    matrix.t[0] = sprite->x;
    matrix.t[1] = sprite->y;
    matrix.t[2] = ReadGeomScreen();
    SetRotMatrix(&matrix);
    SetTransMatrix(&matrix);
    for (i = 0; i < 4; i++) {
        SVECTOR corner = {(short)((i & 1 ? sprite->w : 0) - sprite->mx),
                          (short)((i & 2 ? sprite->h : 0) - sprite->my), 0, 0};
        long p, flag;
        RotTransPers(&corner, (long *)&xy[i], &p, &flag);
    }
    u_left = attribute & 0x800000 ? sprite->u + sprite->w - 1 : sprite->u;
    u_right = attribute & 0x800000 ? sprite->u : sprite->u + sprite->w - 1;
    v_top = attribute & 0x400000 ? sprite->v + sprite->h - 1 : sprite->v;
    v_bottom = attribute & 0x400000 ? sprite->v : sprite->v + sprite->h - 1;
    packet = (u32 *)(uintptr_t)D_800FE240;
    packet[1] = sprite_colour(sprite, 0x2c000000u);
    packet[2] = xy[0];
    packet[3] = (u32)(u_left & 0xff) | ((u32)(v_top & 0xff) << 8) | sprite_clut(sprite);
    packet[4] = xy[1];
    packet[5] = (u32)(u_right & 0xff) | ((u32)(v_top & 0xff) << 8) | ((u32)(sprite->tpage & 0x1f) << 16) |
                ((attribute >> 1) & 0x1800000u) | ((attribute >> 7) & 0x600000u);
    packet[6] = xy[2];
    packet[7] = (u32)(u_left & 0xff) | ((u32)(v_bottom & 0xff) << 8);
    packet[8] = xy[3];
    packet[9] = (u32)(u_right & 0xff) | ((u32)(v_bottom & 0xff) << 8);
    D_800FE240 = (u32)(uintptr_t)make_packet(packet, ot, pri, 9);
}

/* Copy a LIBGPU polygon primitive into the packet area, applying the
 * GTE-mode buffer offset to every vertex. The word order is colour, then per
 * vertex: [next colour when Gouraud], position, [texture word]. */
void GsSortPoly(u32 *primitive, u32 *ot, unsigned short pri)
{
    u32 *packet = (u32 *)(uintptr_t)D_800FE240;
    unsigned code = primitive[1] >> 24, words = 1, vertex, vertices = code & 8 ? 4 : 3;
    packet[1] = primitive[1];
    for (vertex = 0; vertex < vertices; vertex++) {
        u32 position;
        if (vertex && (code & 0x10)) {
            packet[1 + words] = primitive[1 + words];
            words++;
        }
        position = primitive[1 + words];
        packet[1 + words] = ((position + (u32)(u16)D_800FE0BC) & 0xffff) |
                            ((((position >> 16) + (u32)(u16)D_800FE0BE) & 0xffff) << 16);
        words++;
        if (code & 4) {
            packet[1 + words] = primitive[1 + words];
            words++;
        }
    }
    D_800FE240 = (u32)(uintptr_t)make_packet(packet, ot, pri, words);
}

/* Unscaled sprite as a textured quad so either axis can be mirrored. Mirrored
 * axes run from u+w-1 down to u-1, which keeps texels 1:1 with pixels. */
void GsSortFlipSprite(Sprite *sprite, u32 *ot, unsigned short pri)
{
    u32 attribute = sprite->attribute, *packet, left, right, top, bottom;
    int x, y, u_left, u_right, v_top, v_bottom;
    if ((int)attribute < 0 || !sprite->w || !sprite->h) {
        return;
    }
    x = sprite->x + D_800FE0BC - sprite->mx;
    y = sprite->y + D_800FE0BE - sprite->my;
    u_left = attribute & 0x800000 ? sprite->u + sprite->w - 1 : sprite->u;
    u_right = attribute & 0x800000 ? sprite->u - 1 : sprite->u + sprite->w;
    v_top = attribute & 0x400000 ? sprite->v + sprite->h - 1 : sprite->v;
    v_bottom = attribute & 0x400000 ? sprite->v - 1 : sprite->v + sprite->h;
    left = (u32)x & 0xffff;
    right = (u32)(x + sprite->w) & 0xffff;
    top = (u32)y << 16;
    bottom = (u32)(y + sprite->h) << 16;
    packet = (u32 *)(uintptr_t)D_800FE240;
    packet[1] = sprite_colour(sprite, 0x2c000000u);
    packet[2] = left | top;
    packet[3] = (u32)(u_left & 0xff) | ((u32)(v_top & 0xff) << 8) | sprite_clut(sprite);
    packet[4] = right | top;
    packet[5] = (u32)(u_right & 0xff) | ((u32)(v_top & 0xff) << 8) | ((u32)(sprite->tpage & 0x1f) << 16) |
                ((attribute >> 1) & 0x1800000u) | ((attribute >> 7) & 0x600000u);
    packet[6] = left | bottom;
    packet[7] = (u32)(u_left & 0xff) | ((u32)(v_bottom & 0xff) << 8);
    packet[8] = right | bottom;
    packet[9] = (u32)(u_right & 0xff) | ((u32)(v_bottom & 0xff) << 8);
    D_800FE240 = (u32)(uintptr_t)make_packet(packet, ot, pri, 9);
}
