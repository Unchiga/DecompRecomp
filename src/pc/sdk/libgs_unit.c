/* LIBGS "unit" (HMD) handling and the view matrix, ported from the resident
 * Psy-Q routines. HMD data is walked in place: its offsets are turned into
 * guest addresses, which are host addresses here. Primitive drivers are
 * function pointers that the game writes into the data after GsScanUnit
 * (native functions), or guest addresses of library drivers, which the guest
 * call redirect resolves. */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "pc/compat/gte.h"
#include <stdint.h>

extern int D_800FE0C8;                                  /* frame counter: coordinate cache stamp */
extern MATRIX D_800FE0E8; /* the three light directions */
extern MATRIX D_800FE128, D_800FE148, D_800FE168, D_800FE188; /* GsLIGHTWSMATRIX, GsWSMATRIX, GsIDMATRIX, GsIDMATRIX2 */
extern u32 D_800FE240;                                  /* output packet pointer */
extern u32 D_800FE278[];                                /* coordinate walk stack */
extern u32 D_80099E48, D_80099E4C, D_80099E50;          /* scan: next block, cursor, primitive header */
extern u32 D_800FE268, D_800FE26C, D_800FE270;          /* GsScanAnim: types left, type table, cursor */
extern u16 D_80099E54;                                  /* scan: primitives left in the block */

typedef struct CoordUnit {
    u32 flg;
    MATRIX coord, workm;
    SVECTOR rot;
    struct CoordUnit *super;
} CoordUnit;

typedef struct View2 {
    s32 vpx, vpy, vpz, vrx, vry, vrz, rz;
    CoordUnit *super;
} View2;

#define WORDS(p) ((u32 *)(uintptr_t)(p))

/* Scratch area handed to primitive drivers. */
enum { SCRATCH_PRIMITIVE, SCRATCH_OT, SCRATCH_SHIFT, SCRATCH_OFFSET, SCRATCH_PACKET, SCRATCH_SECTIONS };

u32 *GsGetHeadpUnit(void) { return WORDS(D_80099E50); }

void GsMapUnit(u32 *p)
{
    u32 *at, *block;
    s32 outer, inner, count, entries;
    if (p[1] & 1) {
        return;
    }
    p[1] |= 1;
    at = p + p[2];
    p[2] = (u32)(uintptr_t)at;
    count = (s32)*at++;
    for (outer = 0; outer < count; outer++) {
        entries = (s32)*at++;
        for (inner = 0; inner < entries; inner++, at++) {
            if ((s32)*at < 0) {
                *at = (u32)(uintptr_t)(p + (*at & 0x7fffffffu));
            }
        }
    }
    count = (s32)p[3];
    for (outer = 0; outer < count; outer++) {
        u32 *slot = &p[4 + outer];
        if (!*slot) {
            continue;
        }
        block = p + *slot;
        *slot = (u32)(uintptr_t)block;
        while (block[0] != 0xffffffffu) {
            if (!(block[2] & 0x80000000u)) {
                break;
            }
            block[0] = (u32)(uintptr_t)(p + block[0]);
            block[1] = (u32)(uintptr_t)(p + block[1]);
            block[2] &= 0x7fffffffu;
            block = WORDS(block[0]);
        }
        if (block[0] == 0xffffffffu && (block[2] & 0x80000000u)) {
            block[1] = (u32)(uintptr_t)(p + block[1]);
            block[2] &= 0x7fffffffu;
        }
    }
}

/* The coordinate section is the last one of the current primitive header. */
CoordUnit *GsMapCoordUnit(u32 *base)
{
    u32 *header = GsGetHeadpUnit(), *section = WORDS(header[header[0]]);
    s32 count = (s32)section[0], i;
    CoordUnit *coordinates = (CoordUnit *)(section + 1);
    header[0]--;
    for (i = 0; i < count; i++) {
        if (coordinates[i].super) {
            coordinates[i].super = (CoordUnit *)(base + (u32)(uintptr_t)coordinates[i].super);
        }
    }
    return coordinates;
}

static void begin_scratch(u32 *scratch, u32 *ot)
{
    scratch[SCRATCH_OT] = (u32)(uintptr_t)ot;
    scratch[SCRATCH_SHIFT] = 14 - ot[0];
    scratch[SCRATCH_OFFSET] = ot[2];
    scratch[SCRATCH_PACKET] = D_800FE240;
}

static void copy_sections(u32 *scratch, const u32 *header)
{
    u32 i;
    for (i = 0; i < header[0]; i++) {
        scratch[SCRATCH_SECTIONS + i] = header[1 + i];
    }
}

/* Called first with the primitive top, then with NULL for each further type
 * word. Reports unresolved types (top bit set) so the caller can install a
 * driver. Returns 0 at the end. */
int GsScanUnit(u32 *p, u32 *type, u32 *ot, u32 *scratch)
{
    u32 *cursor;
    if (p) {
        D_80099E48 = p[0];
        D_80099E50 = p[1];
        D_80099E54 = (u16)p[2];
        D_80099E4C = (u32)(uintptr_t)(p + 3);
        return 0;
    }
    D_80099E54--;
    if ((s16)D_80099E54 < 0) {
        u32 *block = WORDS(D_80099E48);
        if (D_80099E48 == 0xffffffffu) {
            return 0;
        }
        D_80099E48 = block[0];
        D_80099E50 = block[1];
        D_80099E54 = (u16)((u16)block[2] - 1);
        D_80099E4C = (u32)(uintptr_t)(block + 3);
    }
    cursor = WORDS(D_80099E4C);
    if ((s32)cursor[1] < 0) {
        type[0] = cursor[0];
        type[1] = (u32)(uintptr_t)cursor;
        cursor[1] &= 0x7fffffffu;
    } else {
        type[0] = type[1] = 0;
    }
    begin_scratch(scratch, ot);
    scratch[SCRATCH_PRIMITIVE] = (u32)(uintptr_t)(cursor + 1);
    copy_sections(scratch, WORDS(D_80099E50));
    D_80099E4C = (u32)(uintptr_t)(cursor + 1 + (u16)cursor[1]);
    return 1;
}

void GsSortUnit(u32 *object, u32 *ot, u32 *scratch)
{
    u32 *block = WORDS(object[1]), *next;
    begin_scratch(scratch, ot);
    do {
        u32 count, i;
        next = WORDS(block[0]);
        copy_sections(scratch, WORDS(block[1]));
        count = block[2];
        scratch[SCRATCH_PRIMITIVE] = (u32)(uintptr_t)(block + 3);
        for (i = 0; i < count; i++) {
            u32 *primitive = WORDS(scratch[SCRATCH_PRIMITIVE]);
            u32 *(*driver)(u32 *) = (u32 *(*)(u32 *))(uintptr_t)primitive[0];
            scratch[SCRATCH_PRIMITIVE] = (u32)(uintptr_t)(primitive + 1);
            scratch[SCRATCH_PRIMITIVE] = (u32)(uintptr_t)driver(scratch);
            scratch[SCRATCH_PACKET] = D_800FE240;
        }
        block = next;
    } while ((u32)(uintptr_t)next != 0xffffffffu);
}

/* Library drivers: a null primitive, and image uploads without and with a CLUT. */
u32 *GsU_00000000(u32 *scratch)
{
    u32 *primitive = WORDS(scratch[SCRATCH_PRIMITIVE]);
    return primitive + *(u16 *)primitive;
}

static u32 *upload_images(u32 *scratch, int with_clut)
{
    u32 *primitive = WORDS(scratch[SCRATCH_PRIMITIVE]);
    int count = ((u16 *)primitive)[1], i;
    primitive++;
    for (i = 0; i < count; i++) {
        LoadImage((RECT *)primitive, WORDS(scratch[SCRATCH_SECTIONS]) + primitive[2]);
        primitive += 3;
        if (with_clut) {
            LoadImage((RECT *)primitive, WORDS(scratch[SCRATCH_SECTIONS + 1]) + primitive[2]);
            primitive += 3;
        }
    }
    scratch[SCRATCH_PRIMITIVE] = (u32)(uintptr_t)primitive;
    return primitive;
}

u32 *GsU_02000000(u32 *scratch) { return upload_images(scratch, 0); }
u32 *GsU_02000001(u32 *scratch) { return upload_images(scratch, 1); }

/* Animation section: collect a pointer to each sequence (their lengths, in
 * words, are at +4), returning how many there are. */
int GsLinkAnim(u32 **sequences, u32 *header)
{
    u32 *sequence = header + 2;
    int count = ((u16 *)header)[3], i;
    for (i = 0; i < count; i++) {
        sequences[i] = sequence;
        sequence += ((u16 *)sequence)[2];
    }
    return count;
}

/* Walk the interpolation-driver type words of an animation section, like
 * GsScanUnit: prime with the section, then call with NULL until it returns 0. */
u32 *GsScanAnim(u32 *p, u32 *type)
{
    if (p) {
        u32 *table = WORDS(WORDS(p[-2])[2]);
        D_800FE26C = (u32)(uintptr_t)table;
        D_800FE268 = table[0] & 0x7fffffffu;
        table[0] = D_800FE268;
        D_800FE270 = (u32)(uintptr_t)(table + 1);
        return p;
    }
    if (!D_800FE268) {
        return 0;
    }
    {
        u32 *cursor = WORDS(D_800FE270);
        type[1] = (u32)(uintptr_t)cursor;
        type[0] = *cursor;
        D_800FE270 = (u32)(uintptr_t)(cursor + 1);
        D_800FE268--;
        return cursor;
    }
}

/* m1 = m1 * m2 including translation; GsMulCoord2 leaves the result in m2. */
void GsMulCoord3(MATRIX *m1, MATRIX *m2)
{
    VECTOR moved;
    ApplyMatrixLV(m1, (VECTOR *)m2->t, &moved);
    MulMatrix(m1, m2);
    m1->t[0] += moved.vx;
    m1->t[1] += moved.vy;
    m1->t[2] += moved.vz;
}

void GsMulCoord2(MATRIX *m1, MATRIX *m2)
{
    VECTOR moved;
    ApplyMatrixLV(m1, (VECTOR *)m2->t, &moved);
    MulMatrix2(m1, m2);
    m2->t[0] = moved.vx + m1->t[0];
    m2->t[1] = moved.vy + m1->t[1];
    m2->t[2] = moved.vz + m1->t[2];
}

/* Local-to-world matrix. Walks up to the root, or to the nearest coordinate
 * already computed this frame (flg == frame counter), then multiplies back
 * down, caching every level. A root with some other non-zero flg is taken as
 * computed by the caller; `manual` remembers the deepest level with flg 0. */
void GsGetLwUnit(CoordUnit *coordinate, MATRIX *m)
{
    s32 depth = 0, manual = 100;
    for (;;) {
        D_800FE278[depth] = (u32)(uintptr_t)coordinate;
        if (!coordinate->super) {
            if (coordinate->flg == (u32)D_800FE0C8 || coordinate->flg == 0) {
                coordinate->workm = coordinate->coord;
                *m = coordinate->workm;
                coordinate->flg = (u32)D_800FE0C8;
            } else if (manual == 100) {
                *m = ((CoordUnit *)WORDS(D_800FE278[0]))->workm;
                depth = 0;
            } else {
                depth = manual + 1;
                *m = ((CoordUnit *)WORDS(D_800FE278[depth]))->workm;
            }
            break;
        }
        if (coordinate->flg == (u32)D_800FE0C8) {
            *m = coordinate->workm;
            break;
        }
        if (coordinate->flg == 0) {
            manual = depth;
        }
        coordinate = coordinate->super;
        depth++;
    }
    while (depth > 0) {
        CoordUnit *level = (CoordUnit *)WORDS(D_800FE278[depth - 1]);
        GsMulCoord3(m, &level->coord);
        level->workm = *m;
        level->flg = (u32)D_800FE0C8;
        depth--;
    }
}

void GsGetLsUnit(CoordUnit *coordinate, MATRIX *m)
{
    GsGetLwUnit(coordinate, m);
    GsMulCoord2(&D_800FE148, m);
}

/* Both at once: local-to-world in `world`, local-to-screen in `screen`. */
void GsGetLwsUnit(CoordUnit *coordinate, MATRIX *world, MATRIX *screen)
{
    GsGetLwUnit(coordinate, world);
    *screen = *world;
    GsMulCoord2(&D_800FE148, screen);
}

/* Light directions are kept in world space; bring them into the object's. */
void GsSetLightMatrix(MATRIX *local_to_world)
{
    MATRIX light = D_800FE0E8;
    PushMatrix();
    MulMatrix(&light, local_to_world);
    PopMatrix();
    SetLightMatrix(&light);
}

static void rotate_z(MATRIX *m, s32 rz)
{
    MATRIX turn;
    s32 angle = rz / 360, cosine = rcos(angle), sine = rsin(angle);
    if (!rz) {
        return;
    }
    turn.m[0][0] = (short)cosine; turn.m[0][1] = (short)-sine; turn.m[0][2] = 0;
    turn.m[1][0] = (short)sine;   turn.m[1][1] = (short)cosine; turn.m[1][2] = 0;
    turn.m[2][0] = 0;             turn.m[2][1] = 0;             turn.m[2][2] = 0x1000;
    turn.t[0] = turn.t[1] = turn.t[2] = 0;
    MulMatrix(m, &turn);
}

/* Identity with one axis rotation given as sine and cosine. */
static void axis_matrix(MATRIX *m, short sine, short cosine, char axis)
{
    *m = D_800FE168;
    if (axis == 'x') {
        m->m[1][1] = m->m[2][2] = cosine; m->m[1][2] = (short)-sine; m->m[2][1] = sine;
    } else if (axis == 'y') {
        m->m[0][0] = m->m[2][2] = cosine; m->m[0][2] = sine; m->m[2][0] = (short)-sine;
    } else {
        m->m[0][0] = m->m[1][1] = cosine; m->m[0][1] = (short)-sine; m->m[1][0] = sine;
    }
}

/* World-to-screen matrix from a viewpoint, a reference point and a twist.
 * The six positions are first scaled down together to 15 bits so the
 * squares below fit. Returns 1 when viewpoint and reference coincide. */
int GsSetRefView2(View2 *view)
{
    const s32 *input = &view->vpx;
    MATRIX step, parent;
    VECTOR back;
    s32 p[6], largest = 0, bits = 0, i, distance, ground, sine, cosine, dx, dy, dz;
    D_800FE148 = D_800FE188;
    rotate_z(&D_800FE148, -view->rz);
    for (i = 0; i < 6; i++) {
        s32 size = input[i] < 0 ? -input[i] : input[i];
        largest = largest < size ? size : largest;
    }
    for (i = largest; i > 0; i >>= 1) {
        bits++;
    }
    for (i = 0; i < 6; i++) {
        p[i] = bits >= 16 ? input[i] >> (bits - 15) : input[i];
    }
    dx = p[3] - p[0];
    dy = p[4] - p[1];
    dz = p[5] - p[2];
    distance = (s32)SquareRoot0((long)((u32)dx * (u32)dx + (u32)dy * (u32)dy + (u32)dz * (u32)dz));
    if (!distance) {
        return 1;
    }
    sine = -((s32)((u32)(p[1] - p[4]) << 12) / distance);
    ground = (s32)SquareRoot0((long)((u32)dx * (u32)dx + (u32)dz * (u32)dz));
    cosine = (s32)((u32)ground << 12) / distance;
    axis_matrix(&step, (short)sine, (short)cosine, 'x');
    MulMatrix(&D_800FE148, &step);
    if (ground) {
        sine = (s32)((u32)dx << 12) / ground;
        cosine = (s32)((u32)dz << 12) / ground;
        axis_matrix(&step, (short)-sine, (short)cosine, 'y');
        MulMatrix(&D_800FE148, &step);
    }
    back.vx = -view->vpx;
    back.vy = -view->vpy;
    back.vz = -view->vpz;
    ApplyMatrixLV(&D_800FE148, &back, (VECTOR *)D_800FE148.t);
    if (view->super) {
        GsGetLwUnit(view->super, &step);
        TransposeMatrix(&step, &parent);
        ApplyMatrixLV(&parent, (VECTOR *)step.t, &back);
        parent.t[0] = -back.vx;
        parent.t[1] = -back.vy;
        parent.t[2] = -back.vz;
        GsMulCoord2(&D_800FE148, &parent);
        D_800FE148 = parent;
    }
    D_800FE128 = D_800FE148;
    return 0;
}

typedef struct FlatLight {
    s32 vx, vy, vz;
    u8 r, g, b;
} FlatLight;

/* One of three parallel lights: its direction becomes a row of the light
 * matrix (normalized, pointing at the light) and its colour a column of the
 * GTE colour matrix, which is read back so the other two lights stay. */
int GsSetFlatLight(int id, FlatLight *light)
{
    MATRIX colour;
    s32 length, i;
    u32 words[5];
    for (i = 0; i < 5; i++) {
        words[i] = Memories_GteReadControl(16 + (unsigned)i);
    }
    colour.m[0][0] = (short)words[0]; colour.m[0][1] = (short)(words[0] >> 16);
    colour.m[0][2] = (short)words[1]; colour.m[1][0] = (short)(words[1] >> 16);
    colour.m[1][1] = (short)words[2]; colour.m[1][2] = (short)(words[2] >> 16);
    colour.m[2][0] = (short)words[3]; colour.m[2][1] = (short)(words[3] >> 16);
    colour.m[2][2] = (short)words[4];
    length = (s32)SquareRoot0((long)((u32)light->vx * (u32)light->vx + (u32)light->vy * (u32)light->vy +
                                     (u32)light->vz * (u32)light->vz));
    if (!length) {
        return -1;
    }
    if (id >= 0 && id <= 2) {
        D_800FE0E8.m[id][0] = (short)((s32)((u32)-light->vx << 12) / length);
        D_800FE0E8.m[id][1] = (short)((s32)((u32)-light->vy << 12) / length);
        D_800FE0E8.m[id][2] = (short)((s32)((u32)-light->vz << 12) / length);
        colour.m[0][id] = (short)((light->r << 12) / 255);
        colour.m[1][id] = (short)((light->g << 12) / 255);
        colour.m[2][id] = (short)((light->b << 12) / 255);
    }
    SetColorMatrix(&colour);
    return 0;
}
