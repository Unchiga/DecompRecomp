/* The LIBGTE, LIBGPU and LIBGS routines that only the interpreted overlays
 * call (the WA duel-effect bank and the MODEL control modules): the resident
 * C never references them, so they are not linked by name and must be
 * defined here to enter the guest function map. Each follows the resident
 * assembly at its retail address (config/slus_01411/functions.csv). */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "pc/compat/gte.h"
#include <stdint.h>

/* The game's structures have GCC's bitfield layout; MinGW's default (MSVC's)
 * makes this one 8 bytes and every LIBGS ordering table walk wrong. */
_Static_assert(sizeof(GsOT_TAG) == 4, "GsOT_TAG needs GCC bitfield layout (-mno-ms-bitfields on MinGW)");

/* --- LIBGPU --------------------------------------------------------- */

u16 GetTPage(int tp, int abr, int x, int y)
{
    return (u16)(((tp & 3) << 7) | ((abr & 3) << 5) | ((y & 0x100) >> 4) | ((x & 0x3ff) >> 6) | ((y & 0x200) << 2));
}

u16 GetClut(int x, int y) { return (u16)((y << 6) | ((x >> 4) & 0x3f)); }

static void set_primitive(void *primitive, unsigned length, unsigned code)
{
    ((u8 *)primitive)[3] = (u8)length;
    ((u8 *)primitive)[7] = (u8)code;
}

void SetPolyF3(POLY_F3 *p) { set_primitive(p, 4, 0x20); }
void SetPolyFT3(POLY_FT3 *p) { set_primitive(p, 7, 0x24); }
void SetPolyGT3(POLY_GT3 *p) { set_primitive(p, 9, 0x34); }
void SetPolyF4(POLY_F4 *p) { set_primitive(p, 5, 0x28); }
void SetPolyFT4(POLY_FT4 *p) { set_primitive(p, 9, 0x2c); }

void SetShadeTex(void *p, int tge)
{
    u8 *code = (u8 *)p + 7;
    *code = tge ? (u8)(*code | 1) : (u8)(*code & ~1);
}

/* --- LIBGTE --------------------------------------------------------- */

/* 4096 {sin, cos} pairs, the library's own table in the resident image. */
#define SIN_COS ((const int16_t *)0x80095638u)

static void sin_cos(long angle, int *sine, int *cosine)
{
    const int16_t *pair = &SIN_COS[((angle < 0 ? -angle : angle) & 0xfff) * 2];
    *sine = angle < 0 ? -pair[0] : pair[0];
    *cosine = pair[1];
}

static void load_rotation(const MATRIX *m)
{
    const uint32_t *words = (const uint32_t *)m;
    Memories_GteWriteControl(0, words[0]);
    Memories_GteWriteControl(1, words[1]);
    Memories_GteWriteControl(2, words[2]);
    Memories_GteWriteControl(3, words[3]);
    Memories_GteWriteControl(4, words[4]);
}

void ReadRotMatrix(MATRIX *m)
{
    uint32_t *words = (uint32_t *)m;
    unsigned i;
    for (i = 0; i < 8; i++) {
        words[i] = Memories_GteReadControl(i);
    }
}

MATRIX *TransMatrix(MATRIX *m, VECTOR *v)
{
    m->t[0] = v->vx;
    m->t[1] = v->vy;
    m->t[2] = v->vz;
    return m;
}

/* One MVMVA per column of m1 against m0 on the GTE, as MulMatrix does; the
 * last element is stored as IR3's whole word, which sets the padding short
 * after it to the sign. */
MATRIX *MulMatrix0(MATRIX *m0, MATRIX *m1, MATRIX *m2)
{
    short result[3][3];
    int column, row;
    load_rotation(m0);
    for (column = 0; column < 3; column++) {
        Memories_GteWriteData(0, (uint16_t)m1->m[0][column] | ((uint32_t)(uint16_t)m1->m[1][column] << 16));
        Memories_GteWriteData(1, (uint32_t)(int32_t)m1->m[2][column]);
        Memories_GteCommand(0x0486012u);
        for (row = 0; row < 3; row++) {
            result[row][column] = (short)Memories_GteReadData(9 + (unsigned)row);
        }
    }
    for (row = 0; row < 3; row++) {
        for (column = 0; column < 3; column++) {
            m2->m[row][column] = result[row][column];
        }
    }
    ((short *)m2)[9] = (short)(result[2][2] < 0 ? -1 : 0);
    return m2;
}

VECTOR *ApplyMatrix(MATRIX *m, SVECTOR *v0, VECTOR *v1)
{
    load_rotation(m);
    Memories_GteLoad(0, v0);
    Memories_GteLoad(1, &v0->vz);
    Memories_GteCommand(0x0486012u);
    v1->vx = (long)(int32_t)Memories_GteReadData(25);
    v1->vy = (long)(int32_t)Memories_GteReadData(26);
    v1->vz = (long)(int32_t)Memories_GteReadData(27);
    return v1;
}

SVECTOR *ApplyMatrixSV(MATRIX *m, SVECTOR *v0, SVECTOR *v1)
{
    load_rotation(m);
    Memories_GteLoad(0, v0);
    Memories_GteLoad(1, &v0->vz);
    Memories_GteCommand(0x0486012u);
    v1->vx = (short)Memories_GteReadData(9);
    v1->vy = (short)Memories_GteReadData(10);
    v1->vz = (short)Memories_GteReadData(11);
    return v1;
}

static void load_vertex(unsigned index, const SVECTOR *v)
{
    Memories_GteLoad(index * 2, v);
    Memories_GteLoad(index * 2 + 1, &v->vz);
}

long RotTransPers3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, long *sxy0, long *sxy1, long *sxy2, long *p, long *flag)
{
    load_vertex(0, v0);
    load_vertex(1, v1);
    load_vertex(2, v2);
    Memories_GteCommand(0x0280030u);
    Memories_GteStore(12, sxy0);
    Memories_GteStore(13, sxy1);
    Memories_GteStore(14, sxy2);
    Memories_GteStore(8, p);
    *flag = (long)Memories_GteReadControl(31);
    return (long)(int32_t)Memories_GteReadData(19) >> 2;
}

long RotTransPers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, long *sxy0, long *sxy1, long *sxy2,
                   long *sxy3, long *p, long *flag)
{
    uint32_t first;
    load_vertex(0, v0);
    load_vertex(1, v1);
    load_vertex(2, v2);
    Memories_GteCommand(0x0280030u);
    Memories_GteStore(12, sxy0);
    Memories_GteStore(13, sxy1);
    Memories_GteStore(14, sxy2);
    first = Memories_GteReadControl(31);
    load_vertex(0, v3);
    Memories_GteCommand(0x0180001u);
    Memories_GteStore(14, sxy3);
    Memories_GteStore(8, p);
    *flag = (long)(Memories_GteReadControl(31) | first);
    return (long)(int32_t)Memories_GteReadData(19) >> 2;
}

/* Rotate an existing matrix about one axis: two rows are mixed by the
 * angle's sine and cosine, the products truncated as the resident CPU
 * routines truncate them (32-bit, shifted right by 12). */
static void mix_rows(MATRIX *m, int a, int b, long angle)
{
    int s, c, i;
    sin_cos(angle, &s, &c);
    for (i = 0; i < 3; i++) {
        int ra = m->m[a][i], rb = m->m[b][i];
        m->m[a][i] = (short)((int32_t)((uint32_t)c * (uint32_t)ra - (uint32_t)s * (uint32_t)rb) >> 12);
        m->m[b][i] = (short)((int32_t)((uint32_t)s * (uint32_t)ra + (uint32_t)c * (uint32_t)rb) >> 12);
    }
}

MATRIX *RotMatrixX(long r, MATRIX *m) { mix_rows(m, 1, 2, r); return m; }
MATRIX *RotMatrixY(long r, MATRIX *m) { mix_rows(m, 0, 2, r); return m; }
MATRIX *RotMatrixZ(long r, MATRIX *m) { mix_rows(m, 0, 1, r); return m; }

/* The CPU forms of the two remaining rotation orders differ from the GTE
 * forms only in where they round (before or after a negation), at most one
 * unit in 4096; the GTE forms already ported stand in for them. */
extern MATRIX *RotMatrixYXZ_gte(SVECTOR *r, MATRIX *m);
extern MATRIX *RotMatrixZYX_gte(SVECTOR *r, MATRIX *m);
MATRIX *RotMatrixYXZ(SVECTOR *r, MATRIX *m) { return RotMatrixYXZ_gte(r, m); }
MATRIX *RotMatrixZYX(SVECTOR *r, MATRIX *m) { return RotMatrixZYX_gte(r, m); }

VECTOR *Square0(VECTOR *v0, VECTOR *v1)
{
    Memories_GteLoad(9, &v0->vx);
    Memories_GteLoad(10, &v0->vy);
    Memories_GteLoad(11, &v0->vz);
    Memories_GteCommand(0x0a00428u);
    Memories_GteStore(25, &v1->vx);
    Memories_GteStore(26, &v1->vy);
    Memories_GteStore(27, &v1->vz);
    return v1;
}

long AverageZ4(long sz0, long sz1, long sz2, long sz3)
{
    Memories_GteWriteData(16, (uint32_t)sz0);
    Memories_GteWriteData(17, (uint32_t)sz1);
    Memories_GteWriteData(18, (uint32_t)sz2);
    Memories_GteWriteData(19, (uint32_t)sz3);
    Memories_GteCommand(0x168002eu);
    return (long)(int32_t)Memories_GteReadData(7);
}

/* Mesh interpolation: otp += dfp * p / 4096, each vertex through GPF. */
void gteMIMefunc(SVECTOR *otp, SVECTOR *dfp, long n, long p)
{
    Memories_GteWriteData(8, (uint32_t)p);
    for (; n > 0; n--, otp++, dfp++) {
        Memories_GteWriteData(9, (uint32_t)(uint16_t)dfp->vx);
        Memories_GteWriteData(10, (uint32_t)(int32_t)dfp->vy);
        Memories_GteWriteData(11, (uint32_t)(int32_t)dfp->vz);
        Memories_GteCommand(0x198003du);
        otp->vx = (short)(otp->vx + (int32_t)Memories_GteReadData(9));
        otp->vy = (short)(otp->vy + (int32_t)Memories_GteReadData(10));
        otp->vz = (short)(otp->vz + (int32_t)Memories_GteReadData(11));
    }
}

/* Arctangent of a 1.12 fixed-point value in 4096ths of a turn: twelve
 * CORDIC steps over the library's angle table at 0x80095168. */
int catan(int a)
{
    const int32_t *table = (const int32_t *)0x80095168u;
    int32_t x = 4096, y = a, z = 0;
    int i;
    for (i = 0; i < 12; i++) {
        int32_t nx, ny;
        if (y < 0) {
            nx = x - (y >> i);
            ny = y + (x >> i);
            z -= table[i];
        } else {
            nx = x + (y >> i);
            ny = y - (x >> i);
            z += table[i];
        }
        x = nx;
        y = ny;
    }
    return z;
}

/* --- LIBGS ---------------------------------------------------------- */

extern int D_800FE0C8;      /* frame counter: coordinate cache stamp */
extern MATRIX D_800FE148;   /* GsWSMATRIX */
extern u32 D_800FE278[];    /* coordinate walk stack */

/* Local-to-world for a GsCOORDINATE2 chain: the same walk as GsGetLwUnit
 * (libgs_unit.c) over the library's own coordinate record, which keeps its
 * parent one word further on. */
static void get_lw(GsCOORDINATE2 *coordinate, MATRIX *m)
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
                *m = ((GsCOORDINATE2 *)(uintptr_t)D_800FE278[0])->workm;
                depth = 0;
            } else {
                depth = manual + 1;
                *m = ((GsCOORDINATE2 *)(uintptr_t)D_800FE278[depth])->workm;
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
        GsCOORDINATE2 *level = (GsCOORDINATE2 *)(uintptr_t)D_800FE278[depth - 1];
        GsMulCoord3(m, &level->coord);
        level->workm = *m;
        level->flg = (u32)D_800FE0C8;
        depth--;
    }
}

void GsGetLs(GsCOORDINATE2 *coordinate, MATRIX *m)
{
    get_lw(coordinate, m);
    GsMulCoord2(&D_800FE148, m);
}
