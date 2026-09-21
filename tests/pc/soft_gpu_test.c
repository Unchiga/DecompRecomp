#include "pc/render/soft_gpu.h"
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)
#define AT(x, y) (SoftGpu_Vram()[(y) * SOFT_GPU_WIDTH + (x)])

static int count(uint16_t colour)
{
    int n = 0, i;
    for (i = 0; i < SOFT_GPU_WIDTH * SOFT_GPU_HEIGHT; i++) {
        n += SoftGpu_Vram()[i] == colour;
    }
    return n;
}

int main(void)
{
    /* Flat quad (2,3)-(12,13): top-left rule gives exactly 10x10 pixels, and
     * the shared diagonal is drawn once (semi-transparency would show twice). */
    const uint32_t quad[] = {0xe1000000u, 0x2a0000f8u, 0x00030002u, 0x0003000cu, 0x000d0002u, 0x000d000cu};
    /* 4bpp sprite through a CLUT, texel 0 transparent, raw colours. */
    const uint32_t sprite[] = {0xe1000001u, 0x65808080u, 0x00200020u, 0x00500000u, 0x00020004u};
    const uint16_t clut[16] = {0, 0x001f, 0x03e0, 0x7c00};
    const uint16_t texture[2] = {0x3210, 0x0123};
    const uint32_t clip[] = {0xe3000000u | 100 | (100 << 10), 0xe4000000u | 103 | (101 << 10),
                             0xe5000000u | 90 | (90 << 11), 0x60ffffffu, 0x00000000u, 0x00400040u};
    const uint32_t cut[] = {0x2c808080u, 0, 0};
    SoftGpu_Reset();
    CHECK(SoftGpu_Gp0(quad, 6) == 6);
    /* averaged with black: 31 >> 1 = 15; a doubly drawn diagonal would be 23 */
    CHECK(count(15) == 100 && AT(2, 3) == 15 && AT(11, 12) == 15 && AT(12, 12) == 0 && AT(11, 13) == 0);

    SoftGpu_Load(16 * 16, 1, 16, 1, clut);
    SoftGpu_Load(64, 0, 2, 1, texture);
    CHECK(SoftGpu_Gp0(sprite, 5) == 5);
    CHECK(AT(32, 32) == 0 && AT(33, 32) == 0x001f && AT(34, 32) == 0x03e0 && AT(35, 32) == 0x7c00);
    CHECK(AT(32, 33) == 0 && AT(36, 32) == 0);

    CHECK(SoftGpu_Gp0(clip, 6) == 6);
    CHECK(count(0x7fff) == 8 && AT(100, 100) == 0x7fff && AT(103, 101) == 0x7fff);
    CHECK(SoftGpu_Gp0(cut, 3) == 0); /* truncated FT4 is not drawn */
    SoftGpu_Fill(0, 0, 4, 4, 0xff0000);
    SoftGpu_Move(0, 0, 1020, 510, 4, 4); /* wraps both axes */
    CHECK(AT(1023, 511) == 0x7c00 && AT(1, 1) == 0x7c00);
    puts("Software GPU fill-rule, CLUT, clip and transfer checks passed");
    return 0;
}
