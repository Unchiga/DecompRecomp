#include "pgxp.h"
#include "gte.h"
#include <string.h>

typedef struct Gte {
    int16_t v[3][3];
    uint8_t rgbc[4];
    uint16_t otz;
    int16_t ir[4];
    int16_t sxy[3][2];
    uint16_t sz[4];
    uint8_t rgb[3][4];
    uint32_t res1;
    int32_t mac[4];
    uint32_t lzcs, lzcr;
    int16_t matrix[3][3][3]; /* rotation, light, light colour */
    int32_t vector[3][3];    /* translation, background, far colour */
    int32_t ofx, ofy;
    uint16_t h;
    int16_t dqa;
    int32_t dqb;
    int16_t zsf3, zsf4;
    uint32_t flag;
} Gte;

static Gte gte;
static uint8_t unr_table[0x101];
static int unr_ready;

void Memories_GteReset(void)
{
    memset(&gte, 0, sizeof(gte));
    gte.lzcr = 32;
}

static uint32_t pair(int16_t low, int16_t high)
{
    return (uint16_t)low | ((uint32_t)(uint16_t)high << 16);
}

static uint32_t sign16(int16_t value)
{
    return (uint32_t)(int32_t)value;
}

static int32_t clamp(int32_t value, int32_t low, int32_t high, uint32_t bit)
{
    if (value < low) {
        gte.flag |= bit;
        return low;
    }
    if (value > high) {
        gte.flag |= bit;
        return high;
    }
    return value;
}

static uint32_t read_rgbc(void)
{
    return gte.rgbc[0] | ((uint32_t)gte.rgbc[1] << 8) |
           ((uint32_t)gte.rgbc[2] << 16) | ((uint32_t)gte.rgbc[3] << 24);
}

uint32_t Memories_GteReadData(unsigned index)
{
    unsigned i;
    switch (index & 31) {
    case 0: case 2: case 4: i = (index & 31) / 2; return pair(gte.v[i][0], gte.v[i][1]);
    case 1: case 3: case 5: return sign16(gte.v[(index & 31) / 2][2]);
    case 6: return read_rgbc();
    case 7: return gte.otz;
    case 8: case 9: case 10: case 11: return sign16(gte.ir[(index & 31) - 8]);
    case 12: case 13: case 14: i = (index & 31) - 12; return pair(gte.sxy[i][0], gte.sxy[i][1]);
    case 15: return pair(gte.sxy[2][0], gte.sxy[2][1]);
    case 16: case 17: case 18: case 19: return gte.sz[(index & 31) - 16];
    case 20: case 21: case 22:
        i = (index & 31) - 20;
        return gte.rgb[i][0] | ((uint32_t)gte.rgb[i][1] << 8) |
               ((uint32_t)gte.rgb[i][2] << 16) | ((uint32_t)gte.rgb[i][3] << 24);
    case 23: return gte.res1;
    case 24: case 25: case 26: case 27: return (uint32_t)gte.mac[(index & 31) - 24];
    case 28: case 29: {
        uint32_t r = (uint32_t)clamp(gte.ir[1] / 0x80, 0, 0x1f, 0);
        uint32_t g = (uint32_t)clamp(gte.ir[2] / 0x80, 0, 0x1f, 0);
        uint32_t b = (uint32_t)clamp(gte.ir[3] / 0x80, 0, 0x1f, 0);
        return r | (g << 5) | (b << 10);
    }
    case 30: return gte.lzcs;
    default: return gte.lzcr;
    }
}

void Memories_GteWriteData(unsigned index, uint32_t value)
{
    unsigned i, count;
    uint32_t bits;
    switch (index & 31) {
    case 0: case 2: case 4:
        i = (index & 31) / 2;
        gte.v[i][0] = (int16_t)value;
        gte.v[i][1] = (int16_t)(value >> 16);
        break;
    case 1: case 3: case 5: gte.v[(index & 31) / 2][2] = (int16_t)value; break;
    case 6: for (i = 0; i < 4; i++) { gte.rgbc[i] = (uint8_t)(value >> (8 * i)); } break;
    case 7: gte.otz = (uint16_t)value; break;
    case 8: case 9: case 10: case 11: gte.ir[(index & 31) - 8] = (int16_t)value; break;
    case 12: case 13: case 14:
        i = (index & 31) - 12;
        gte.sxy[i][0] = (int16_t)value;
        gte.sxy[i][1] = (int16_t)(value >> 16);
        break;
    case 15:
        memmove(gte.sxy[0], gte.sxy[1], sizeof(gte.sxy[0]) * 2);
        gte.sxy[2][0] = (int16_t)value;
        gte.sxy[2][1] = (int16_t)(value >> 16);
        break;
    case 16: case 17: case 18: case 19: gte.sz[(index & 31) - 16] = (uint16_t)value; break;
    case 20: case 21: case 22:
        for (i = 0; i < 4; i++) { gte.rgb[(index & 31) - 20][i] = (uint8_t)(value >> (8 * i)); }
        break;
    case 23: gte.res1 = value; break;
    case 24: case 25: case 26: case 27: gte.mac[(index & 31) - 24] = (int32_t)value; break;
    case 28:
        gte.ir[1] = (int16_t)((value & 0x1f) * 0x80);
        gte.ir[2] = (int16_t)(((value >> 5) & 0x1f) * 0x80);
        gte.ir[3] = (int16_t)(((value >> 10) & 0x1f) * 0x80);
        break;
    case 30:
        gte.lzcs = value;
        bits = (value & UINT32_C(0x80000000)) ? ~value : value;
        for (count = 0; count < 32 && !(bits & (UINT32_C(0x80000000) >> count)); count++) {
        }
        gte.lzcr = count;
        break;
    default: break; /* ORGB and LZCR are read-only */
    }
}

uint32_t Memories_GteReadControl(unsigned index)
{
    unsigned i = (index & 31) / 8, r = (index & 31) % 8;
    if (i < 3) {
        int16_t *m = gte.matrix[i][0];
        if (r < 4) {
            return pair(m[r * 2], m[r * 2 + 1]);
        }
        return r == 4 ? sign16(m[8]) : (uint32_t)gte.vector[i][r - 5];
    }
    switch (index & 31) {
    case 24: return (uint32_t)gte.ofx;
    case 25: return (uint32_t)gte.ofy;
    case 26: return sign16((int16_t)gte.h); /* hardware sign-extends H on read */
    case 27: return sign16(gte.dqa);
    case 28: return (uint32_t)gte.dqb;
    case 29: return sign16(gte.zsf3);
    case 30: return sign16(gte.zsf4);
    default: return gte.flag;
    }
}

void Memories_GteWriteControl(unsigned index, uint32_t value)
{
    unsigned i = (index & 31) / 8, r = (index & 31) % 8;
    if (i < 3) {
        int16_t *m = gte.matrix[i][0];
        if (r < 4) {
            m[r * 2] = (int16_t)value;
            m[r * 2 + 1] = (int16_t)(value >> 16);
        } else if (r == 4) {
            m[8] = (int16_t)value;
        } else {
            gte.vector[i][r - 5] = (int32_t)value;
        }
        return;
    }
    switch (index & 31) {
    case 24: gte.ofx = (int32_t)value; break;
    case 25: gte.ofy = (int32_t)value; break;
    case 26: gte.h = (uint16_t)value; break;
    case 27: gte.dqa = (int16_t)value; break;
    case 28: gte.dqb = (int32_t)value; break;
    case 29: gte.zsf3 = (int16_t)value; break;
    case 30: gte.zsf4 = (int16_t)value; break;
    default:
        gte.flag = value & UINT32_C(0x7ffff000);
        if (gte.flag & UINT32_C(0x7f87e000)) {
            gte.flag |= UINT32_C(0x80000000);
        }
        break;
    }
}

/* 44-bit accumulator: flag the overflow, then wrap like the hardware adder. */
static int64_t mac_check(unsigned i, int64_t value)
{
    if (value >= (INT64_C(1) << 43)) {
        gte.flag |= UINT32_C(1) << (31 - i);
    } else if (value < -(INT64_C(1) << 43)) {
        gte.flag |= UINT32_C(1) << (28 - i);
    }
    return (int64_t)((uint64_t)value << 20) >> 20;
}

static void set_mac(unsigned i, int64_t value, unsigned shift)
{
    gte.mac[i] = (int32_t)(value >> shift);
}

static void set_mac0(int64_t value)
{
    if (value > INT64_C(0x7fffffff)) {
        gte.flag |= UINT32_C(1) << 16;
    } else if (value < -INT64_C(0x80000000)) {
        gte.flag |= UINT32_C(1) << 15;
    }
    gte.mac[0] = (int32_t)value;
}

static void set_ir(unsigned i, int32_t value, int lm)
{
    gte.ir[i] = (int16_t)clamp(value, lm ? 0 : -0x8000, 0x7fff, UINT32_C(1) << (25 - i));
}

static void set_mac_ir(unsigned i, int64_t value, unsigned shift, int lm)
{
    set_mac(i, mac_check(i, value), shift);
    set_ir(i, gte.mac[i], lm);
}

static void push_sz(int32_t value)
{
    memmove(&gte.sz[0], &gte.sz[1], sizeof(gte.sz[0]) * 3);
    gte.sz[3] = (uint16_t)clamp(value, 0, 0xffff, UINT32_C(1) << 18);
}

static void push_sxy(int32_t x, int32_t y)
{
    memmove(gte.sxy[0], gte.sxy[1], sizeof(gte.sxy[0]) * 2);
    gte.sxy[2][0] = (int16_t)clamp(x, -0x400, 0x3ff, UINT32_C(1) << 14);
    gte.sxy[2][1] = (int16_t)clamp(y, -0x400, 0x3ff, UINT32_C(1) << 13);
}

static void push_rgb_from_mac(void)
{
    memmove(gte.rgb[0], gte.rgb[1], sizeof(gte.rgb[0]) * 2);
    gte.rgb[2][0] = (uint8_t)clamp(gte.mac[1] >> 4, 0, 255, UINT32_C(1) << 21);
    gte.rgb[2][1] = (uint8_t)clamp(gte.mac[2] >> 4, 0, 255, UINT32_C(1) << 20);
    gte.rgb[2][2] = (uint8_t)clamp(gte.mac[3] >> 4, 0, 255, UINT32_C(1) << 19);
    gte.rgb[2][3] = gte.rgbc[3];
}

/* translation * 0x1000 + matrix * vector, row by row with per-step checks */
/* The last multiply's rows before the shift: PGXP's view position. */
static int64_t row_sums[3];

static void multiply(const int16_t *m, const int32_t *translation,
                     const int16_t *input, unsigned shift, int lm, int64_t *row3)
{
    /* Latched: callers pass IR1..IR3 as the input of the colour stage and of
     * MVMVA, and the rows below write those registers as they go. */
    const int16_t vector[3] = {input[0], input[1], input[2]};
    unsigned i;
    for (i = 0; i < 3; i++) {
        int64_t sum = translation ? (int64_t)translation[i] * 0x1000 : 0;
        sum = mac_check(i + 1, sum + (int64_t)m[i * 3 + 0] * vector[0]);
        sum = mac_check(i + 1, sum + (int64_t)m[i * 3 + 1] * vector[1]);
        sum = mac_check(i + 1, sum + (int64_t)m[i * 3 + 2] * vector[2]);
        row_sums[i] = sum;
        set_mac(i + 1, sum, shift);
        if (row3 && i == 2) {
            *row3 = sum;
        } else {
            set_ir(i + 1, gte.mac[i + 1], lm);
        }
    }
}

static uint32_t divide(uint32_t h, uint32_t sz)
{
    unsigned z = 0;
    uint32_t n, d, u;
    if (!unr_ready) {
        int i;
        for (i = 0; i <= 0x100; i++) {
            int value = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            unr_table[i] = (uint8_t)(value < 0 ? 0 : value);
        }
        unr_ready = 1;
    }
    if (h >= sz * 2) {
        gte.flag |= UINT32_C(1) << 17;
        return 0x1ffff;
    }
    while (!((sz << z) & 0x8000)) {
        z++;
    }
    n = h << z;
    d = sz << z;
    u = unr_table[(d - 0x7fc0) >> 7] + 0x101u;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    n = (uint32_t)((((uint64_t)n * d) + 0x8000u) >> 16);
    return n > 0x1ffff ? 0x1ffff : n;
}

static void rtp(unsigned index, unsigned shift, int lm, int last)
{
    int64_t z, x, y;
    int32_t quotient;
    multiply(gte.matrix[0][0], gte.vector[0], gte.v[index], shift, lm, &z);
    /* IR3's saturation flag always tests the value shifted by 12, while the
     * stored IR3 is clamped from MAC3 itself. */
    clamp((int32_t)(z >> 12), -0x8000, 0x7fff, UINT32_C(1) << 22);
    gte.ir[3] = (int16_t)clamp(gte.mac[3], lm ? 0 : -0x8000, 0x7fff, 0);
    push_sz((int32_t)(z >> 12));
    quotient = (int32_t)divide(gte.h, gte.sz[3]);
    x = (int64_t)quotient * gte.ir[1] + gte.ofx;
    y = (int64_t)quotient * gte.ir[2] + gte.ofy;
    set_mac0(x);
    set_mac0(y);
    push_sxy((int32_t)(x >> 16), (int32_t)(y >> 16));
    if (Pgxp_Active && gte.h < (uint32_t)gte.sz[3] * 2 && z > 0) {
        /* Where the vertex really falls, from the view position before the
         * shift and the division in full; kept when it rounds to the word
         * the GTE made (not clamped off the screen, say). */
        double depth = (double)z / 4096.0, unit = (double)(1u << shift);
        double sx = (double)gte.ofx / 65536.0 + (double)gte.h * ((double)row_sums[0] / unit) / depth;
        double sy = (double)gte.ofy / 65536.0 + (double)gte.h * ((double)row_sums[1] / unit) / depth;
        int32_t wx = gte.sxy[2][0], wy = gte.sxy[2][1];
        if (sx > wx - 1 && sx < wx + 2 && sy > wy - 1 && sy < wy + 2) {
            Pgxp_Project((uint32_t)(uint16_t)wx | (uint32_t)(uint16_t)wy << 16, sx, sy, depth);
        }
    }
    if (last) {
        int64_t depth = (int64_t)quotient * gte.dqa + gte.dqb;
        set_mac0(depth);
        gte.ir[0] = (int16_t)clamp((int32_t)(depth >> 12), 0, 0x1000, UINT32_C(1) << 12);
    }
}

/* MAC = MAC + (FC - MAC) * IR0, the depth-cue tail shared by colour commands */
static void interpolate(int64_t m1, int64_t m2, int64_t m3, unsigned shift, int lm)
{
    int64_t in[3];
    unsigned i;
    in[0] = m1; in[1] = m2; in[2] = m3;
    for (i = 0; i < 3; i++) {
        set_mac_ir(i + 1, (int64_t)gte.vector[2][i] * 0x1000 - in[i], shift, 0);
    }
    for (i = 0; i < 3; i++) {
        set_mac_ir(i + 1, (int64_t)gte.ir[i + 1] * gte.ir[0] + in[i], shift, lm);
    }
}

static void light(unsigned index, unsigned shift, int lm, int colour, int depth)
{
    multiply(gte.matrix[1][0], NULL, gte.v[index], shift, lm, NULL);
    multiply(gte.matrix[2][0], gte.vector[1], &gte.ir[1], shift, lm, NULL);
    if (colour) {
        int64_t r = ((int64_t)gte.rgbc[0] * gte.ir[1]) << 4;
        int64_t g = ((int64_t)gte.rgbc[1] * gte.ir[2]) << 4;
        int64_t b = ((int64_t)gte.rgbc[2] * gte.ir[3]) << 4;
        if (depth) {
            interpolate(r, g, b, shift, lm);
        } else {
            set_mac_ir(1, r, shift, lm);
            set_mac_ir(2, g, shift, lm);
            set_mac_ir(3, b, shift, lm);
        }
    }
    push_rgb_from_mac();
}

static void mvmva(uint32_t command, unsigned shift, int lm)
{
    unsigned mx = (command >> 17) & 3, vx = (command >> 15) & 3, tx = (command >> 13) & 3;
    int16_t garbage[9];
    const int16_t *m = gte.matrix[mx < 3 ? mx : 0][0];
    const int16_t *v = vx < 3 ? gte.v[vx] : &gte.ir[1];
    unsigned i;
    if (mx == 3) {
        garbage[0] = (int16_t)-(gte.rgbc[0] << 4);
        garbage[1] = (int16_t)(gte.rgbc[0] << 4);
        garbage[2] = gte.ir[0];
        garbage[3] = garbage[4] = garbage[5] = gte.matrix[0][0][2];
        garbage[6] = garbage[7] = garbage[8] = gte.matrix[0][1][1];
        m = garbage;
    }
    if (tx == 2) {
        /* Far-colour translation bug: the first product only sets flags. */
        for (i = 0; i < 3; i++) {
            int64_t first = mac_check(i + 1, (int64_t)gte.vector[2][i] * 0x1000 +
                                                 (int64_t)m[i * 3] * v[0]);
            int64_t rest;
            clamp((int32_t)(first >> shift), -0x8000, 0x7fff, UINT32_C(1) << (24 - i));
            rest = mac_check(i + 1, (int64_t)m[i * 3 + 1] * v[1]);
            rest = mac_check(i + 1, rest + (int64_t)m[i * 3 + 2] * v[2]);
            set_mac(i + 1, rest, shift);
            set_ir(i + 1, gte.mac[i + 1], lm);
        }
        return;
    }
    multiply(m, tx == 3 ? NULL : gte.vector[tx], v, shift, lm, NULL);
}

static void push_colour_ir(unsigned shift, int lm, int64_t r, int64_t g, int64_t b)
{
    set_mac_ir(1, r, shift, lm);
    set_mac_ir(2, g, shift, lm);
    set_mac_ir(3, b, shift, lm);
    push_rgb_from_mac();
}

int Memories_GteCommand(uint32_t command)
{
    unsigned shift = (command & (UINT32_C(1) << 19)) ? 12 : 0;
    int lm = (command >> 10) & 1;
    unsigned i;
    int implemented = 1;
    gte.flag = 0;
    switch (command & 0x3f) {
    case 0x01: rtp(0, shift, lm, 1); break;
    case 0x30: rtp(0, shift, lm, 0); rtp(1, shift, lm, 0); rtp(2, shift, lm, 1); break;
    case 0x06:
        set_mac0((int64_t)gte.sxy[0][0] * gte.sxy[1][1] + (int64_t)gte.sxy[1][0] * gte.sxy[2][1] +
                 (int64_t)gte.sxy[2][0] * gte.sxy[0][1] - (int64_t)gte.sxy[0][0] * gte.sxy[2][1] -
                 (int64_t)gte.sxy[1][0] * gte.sxy[0][1] - (int64_t)gte.sxy[2][0] * gte.sxy[1][1]);
        break;
    case 0x0c: {
        int32_t d1 = gte.matrix[0][0][0], d2 = gte.matrix[0][1][1], d3 = gte.matrix[0][2][2];
        int16_t a = gte.ir[1], b = gte.ir[2], c = gte.ir[3];
        set_mac_ir(1, (int64_t)c * d2 - (int64_t)b * d3, shift, lm);
        set_mac_ir(2, (int64_t)a * d3 - (int64_t)c * d1, shift, lm);
        set_mac_ir(3, (int64_t)b * d1 - (int64_t)a * d2, shift, lm);
        break;
    }
    case 0x10:
        interpolate((int64_t)gte.rgbc[0] << 16, (int64_t)gte.rgbc[1] << 16,
                    (int64_t)gte.rgbc[2] << 16, shift, lm);
        push_rgb_from_mac();
        break;
    case 0x2a:
        for (i = 0; i < 3; i++) {
            interpolate((int64_t)gte.rgb[0][0] << 16, (int64_t)gte.rgb[0][1] << 16,
                        (int64_t)gte.rgb[0][2] << 16, shift, lm);
            push_rgb_from_mac();
        }
        break;
    case 0x11:
        interpolate((int64_t)gte.ir[1] << 12, (int64_t)gte.ir[2] << 12,
                    (int64_t)gte.ir[3] << 12, shift, lm);
        push_rgb_from_mac();
        break;
    case 0x12: mvmva(command, shift, lm); break;
    case 0x13: light(0, shift, lm, 1, 1); break;
    case 0x16: for (i = 0; i < 3; i++) { light(i, shift, lm, 1, 1); } break;
    case 0x1b: light(0, shift, lm, 1, 0); break;
    case 0x3f: for (i = 0; i < 3; i++) { light(i, shift, lm, 1, 0); } break;
    case 0x1e: light(0, shift, lm, 0, 0); break;
    case 0x20: for (i = 0; i < 3; i++) { light(i, shift, lm, 0, 0); } break;
    case 0x14:
        multiply(gte.matrix[2][0], gte.vector[1], &gte.ir[1], shift, lm, NULL);
        interpolate(((int64_t)gte.rgbc[0] * gte.ir[1]) << 4, ((int64_t)gte.rgbc[1] * gte.ir[2]) << 4,
                    ((int64_t)gte.rgbc[2] * gte.ir[3]) << 4, shift, lm);
        push_rgb_from_mac();
        break;
    case 0x1c:
        multiply(gte.matrix[2][0], gte.vector[1], &gte.ir[1], shift, lm, NULL);
        push_colour_ir(shift, lm, ((int64_t)gte.rgbc[0] * gte.ir[1]) << 4,
                       ((int64_t)gte.rgbc[1] * gte.ir[2]) << 4, ((int64_t)gte.rgbc[2] * gte.ir[3]) << 4);
        break;
    case 0x29:
        interpolate(((int64_t)gte.rgbc[0] * gte.ir[1]) << 4, ((int64_t)gte.rgbc[1] * gte.ir[2]) << 4,
                    ((int64_t)gte.rgbc[2] * gte.ir[3]) << 4, shift, lm);
        push_rgb_from_mac();
        break;
    case 0x28:
        for (i = 1; i <= 3; i++) {
            set_mac_ir(i, (int64_t)gte.ir[i] * gte.ir[i], shift, lm);
        }
        break;
    case 0x2d:
    case 0x2e: {
        int64_t sum = (int64_t)gte.sz[1] + gte.sz[2] + gte.sz[3];
        int64_t value = (command & 0x3f) == 0x2d ? sum * gte.zsf3 : (sum + gte.sz[0]) * gte.zsf4;
        set_mac0(value);
        gte.otz = (uint16_t)clamp((int32_t)(value >> 12), 0, 0xffff, UINT32_C(1) << 18);
        break;
    }
    case 0x3d:
        push_colour_ir(shift, lm, (int64_t)gte.ir[1] * gte.ir[0], (int64_t)gte.ir[2] * gte.ir[0],
                       (int64_t)gte.ir[3] * gte.ir[0]);
        break;
    case 0x3e:
        push_colour_ir(shift, lm,
                       (int64_t)gte.ir[1] * gte.ir[0] + (int64_t)((uint64_t)(int64_t)gte.mac[1] << shift),
                       (int64_t)gte.ir[2] * gte.ir[0] + (int64_t)((uint64_t)(int64_t)gte.mac[2] << shift),
                       (int64_t)gte.ir[3] * gte.ir[0] + (int64_t)((uint64_t)(int64_t)gte.mac[3] << shift));
        break;
    default: implemented = 0; break;
    }
    if (gte.flag & UINT32_C(0x7f87e000)) {
        gte.flag |= UINT32_C(0x80000000);
    }
    return implemented;
}

void Memories_GteLoad(unsigned index, const void *address)
{
    const uint8_t *bytes = address;
    Memories_GteWriteData(index, (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                                     ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24));
}

void Memories_GteStoreWord(uint32_t value, void *address)
{
    uint8_t *bytes = address;
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

void Memories_GteStore(unsigned index, void *address)
{
    Memories_GteStoreWord(Memories_GteReadData(index), address);
}

/* Save states: the register file, without tying the GTE to the state code. */
void *Gte_StateData(unsigned *size)
{
    *size = sizeof(gte);
    return &gte;
}
