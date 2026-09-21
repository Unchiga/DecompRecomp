#include "pc/compat/gte.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)
#define RTPS 0x0180001u
#define NCLIP 0x1400006u
#define AVSZ3 0x158002du
#define NCDS 0x0e80413u
#define SQR 0x0a00428u

static void identity(unsigned base)
{
    Memories_GteWriteControl(base, 0x1000);
    Memories_GteWriteControl(base + 1, 0);
    Memories_GteWriteControl(base + 2, 0x1000);
    Memories_GteWriteControl(base + 3, 0);
    Memories_GteWriteControl(base + 4, 0x1000);
}

int main(void)
{
    uint8_t vector[8] = {100, 0, 0xce, 0xff, 0, 0, 0, 0}; /* (100, -50, 0) */
    uint8_t out[4];
    Memories_GteReset();
    CHECK(Memories_GteReadData(31) == 32);

    /* Perspective: z = 0x400, h = 0x200 gives an exact 0.5 (0x8000) quotient
     * from the reciprocal table. */
    identity(0);
    Memories_GteWriteControl(7, 0x400);
    Memories_GteWriteControl(24, 160u << 16);
    Memories_GteWriteControl(25, 120u << 16);
    Memories_GteWriteControl(26, 0x200);
    Memories_GteWriteControl(27, (uint32_t)-0x100);
    Memories_GteWriteControl(28, 0x1000000);
    Memories_GteLoad(0, vector);
    Memories_GteLoad(1, vector + 4);
    CHECK(Memories_GteReadData(0) == 0xffce0064u && Memories_GteReadData(1) == 0);
    CHECK(Memories_GteCommand(RTPS));
    CHECK(Memories_GteReadData(19) == 0x400 && Memories_GteReadData(11) == 0x400);
    CHECK(Memories_GteReadData(14) == ((95u << 16) | 210u));
    CHECK(Memories_GteReadData(15) == Memories_GteReadData(14));
    /* depth cue: 0x8000 * -0x100 + 0x1000000 = 0x800000 -> IR0 0x800 */
    CHECK(Memories_GteReadData(24) == 0x800000u && Memories_GteReadData(8) == 0x800);
    CHECK(Memories_GteReadControl(31) == 0);
    Memories_GteStore(14, out);
    CHECK(out[0] == 210 && out[1] == 0 && out[2] == 95 && out[3] == 0);

    /* Near plane: h >= 2z overflows the divide and saturates the screen x. */
    Memories_GteWriteControl(7, 0x10);
    CHECK(Memories_GteCommand(RTPS));
    /* the depth cue goes negative as well, so IR0 saturates (bit 12) */
    CHECK(Memories_GteReadControl(31) == 0x80021000u && Memories_GteReadData(8) == 0);
    CHECK(Memories_GteReadData(14) == ((20u << 16) | 359u)); /* quotient 0x1ffff */
    Memories_GteWriteData(0, 1000);
    CHECK(Memories_GteCommand(RTPS));
    CHECK(Memories_GteReadControl(31) == 0x80025000u);
    CHECK(Memories_GteReadData(14) == ((120u << 16) | 0x3ffu));
    Memories_GteLoad(0, vector);
    /* Behind the camera: SZ clamps to zero, IR3 keeps the signed value. */
    Memories_GteWriteControl(7, (uint32_t)-0x20);
    CHECK(Memories_GteCommand(RTPS));
    CHECK(Memories_GteReadData(19) == 0 && Memories_GteReadData(11) == (uint32_t)-0x20);
    CHECK(Memories_GteReadControl(31) & (1u << 18));
    /* lm clamps IR3 to zero without raising its flag for an in-range MAC3. */
    CHECK(Memories_GteCommand(RTPS | 0x400));
    CHECK(Memories_GteReadData(11) == 0 && !(Memories_GteReadControl(31) & (1u << 22)));

    Memories_GteWriteData(12, 0);
    Memories_GteWriteData(13, 10);
    Memories_GteWriteData(14, 10u << 16);
    CHECK(Memories_GteCommand(NCLIP) && Memories_GteReadData(24) == 100);
    Memories_GteWriteData(13, 10u << 16);
    Memories_GteWriteData(14, 10);
    CHECK(Memories_GteCommand(NCLIP) && Memories_GteReadData(24) == (uint32_t)-100);
    /* SXYP pushes the FIFO; SXY2 does not. */
    Memories_GteWriteData(15, 0x00010002);
    CHECK(Memories_GteReadData(12) == (10u << 16) && Memories_GteReadData(13) == 10 &&
          Memories_GteReadData(14) == 0x00010002);

    Memories_GteWriteData(17, 0x400);
    Memories_GteWriteData(18, 0x400);
    Memories_GteWriteData(19, 0x400);
    Memories_GteWriteControl(29, 0x555);
    CHECK(Memories_GteCommand(AVSZ3) && Memories_GteReadData(7) == 0x3ff);
    CHECK(Memories_GteReadData(24) == 0x3ffc00);
    Memories_GteWriteControl(29, 0x7fff);
    Memories_GteWriteData(19, 0xffff);
    CHECK(Memories_GteCommand(AVSZ3) && Memories_GteReadData(7) == 0xffff);
    CHECK((Memories_GteReadControl(31) & 0x80040000u) == 0x80040000u);

    /* Lit grey facing the light, then fully depth-cued to the far colour. */
    Memories_GteWriteControl(8, 0);
    Memories_GteWriteControl(9, 0x1000); /* L13 */
    Memories_GteWriteControl(10, 0);
    Memories_GteWriteControl(11, 0);
    Memories_GteWriteControl(12, 0);
    identity(16);
    Memories_GteWriteData(0, 0);
    Memories_GteWriteData(1, 0x1000);
    Memories_GteWriteData(6, 0x2c808080u);
    Memories_GteWriteData(8, 0);
    CHECK(Memories_GteCommand(NCDS) && Memories_GteReadData(22) == 0x2c000080u);
    CHECK(Memories_GteReadData(9) == 0x800 && Memories_GteReadControl(31) == 0);
    Memories_GteWriteControl(21, 0xff0);
    Memories_GteWriteControl(22, 0xff0);
    Memories_GteWriteControl(23, 0x2000); /* saturates blue */
    Memories_GteWriteData(8, 0x1000);
    CHECK(Memories_GteCommand(NCDS) && Memories_GteReadData(22) == 0x2cffffffu);
    CHECK(Memories_GteReadData(20) == 0 && Memories_GteReadData(21) == 0x2c000080u);
    CHECK(Memories_GteReadControl(31) == (1u << 19));

    Memories_GteWriteData(9, (uint32_t)-0x100);
    Memories_GteWriteData(10, 0x7fff);
    Memories_GteWriteData(11, 3);
    CHECK(Memories_GteCommand(SQR));
    /* the retail SQR word has sf = 0: unshifted MACs, saturated IR1/IR2 */
    CHECK(Memories_GteReadData(25) == 0x10000 && Memories_GteReadData(26) == 0x3fff0001 &&
          Memories_GteReadData(9) == 0x7fff && Memories_GteReadData(10) == 0x7fff &&
          Memories_GteReadData(11) == 9);
    CHECK(Memories_GteReadControl(31) == (0x80000000u | (3u << 23)));
    CHECK(Memories_GteCommand(SQR | 0x80000) && Memories_GteReadData(26) == 0x3fff0 &&
          Memories_GteReadData(11) == 0);

    Memories_GteWriteData(30, 1);
    CHECK(Memories_GteReadData(31) == 31);
    Memories_GteWriteData(30, 0x80000000u);
    CHECK(Memories_GteReadData(31) == 1);
    Memories_GteWriteData(30, 0xffffffffu);
    CHECK(Memories_GteReadData(31) == 32);
    Memories_GteWriteData(28, 0x7fff);
    CHECK(Memories_GteReadData(9) == 0xf80 && Memories_GteReadData(29) == 0x7fff);
    Memories_GteWriteControl(26, 0x8000);
    CHECK(Memories_GteReadControl(26) == 0xffff8000u);
    Memories_GteWriteControl(31, 0xffffffffu);
    CHECK(Memories_GteReadControl(31) == 0xfffff000u);
    CHECK(!Memories_GteCommand(0x3a) && Memories_GteReadControl(31) == 0);
    puts("Software GTE register, projection, lighting and flag checks passed");
    /* MVMVA on the IR vector: inputs are latched, so a rotation that swaps
     * x and y must not see the IR1 it has just written. */
    Memories_GteWriteControl(0, 0x10000000u); /* R11 = 0, R12 = 0x1000 */
    Memories_GteWriteControl(1, 0x10000000u); /* R13 = 0, R21 = 0x1000 */
    Memories_GteWriteControl(2, 0);
    Memories_GteWriteControl(3, 0);
    Memories_GteWriteControl(4, 0x1000);
    Memories_GteWriteData(9, 5);
    Memories_GteWriteData(10, 7);
    Memories_GteWriteData(11, 9);
    CHECK(Memories_GteCommand(0x049e012));
    CHECK(Memories_GteReadData(9) == 7 && Memories_GteReadData(10) == 5 && Memories_GteReadData(11) == 9);

    return 0;
}
