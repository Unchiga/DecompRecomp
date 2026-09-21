#ifndef MEMORIES_PC_SOFT_GPU_H
#define MEMORIES_PC_SOFT_GPU_H
#include <stddef.h>
#include <stdint.h>

/* Software PS1 GPU: 1024x512 words of 15-bit colour plus the mask bit.
 * Single-threaded. Timing, interlace and 24-bit display are not modelled. */
#define SOFT_GPU_WIDTH 1024
#define SOFT_GPU_HEIGHT 512

void SoftGpu_Reset(void);
/* Execute complete GP0 commands. A command cut off by the end of the buffer
 * is dropped; unknown opcodes consume one word. Returns words consumed. */
size_t SoftGpu_Gp0(const uint32_t *words, size_t count);
/* CPU-side transfers, clipped to VRAM with coordinate wraparound. */
void SoftGpu_Load(int x, int y, int w, int h, const uint16_t *pixels);
void SoftGpu_Store(int x, int y, int w, int h, uint16_t *pixels);
void SoftGpu_Move(int sx, int sy, int dx, int dy, int w, int h);
void SoftGpu_Fill(int x, int y, int w, int h, uint32_t rgb24);
const uint16_t *SoftGpu_Vram(void);

/* Texture banks: alternate VRAM that a primitive can sample instead, chosen
 * by bits 11-14 of its texture-page word, which the hardware leaves unused
 * and retail always writes as zero (bank 0 is VRAM itself). They let several
 * monsters keep their 256x256 texture block at once, which the console's one
 * megabyte could never do; src/pc/mods uses them. A bank is VRAM-shaped, so
 * a primitive's page, window and palette coordinates mean the same in it.
 * Returns NULL if the bank cannot be allocated. */
#define SOFT_GPU_BANKS 16
uint16_t *SoftGpu_Bank(int bank);
/* Save states: VRAM (index 0) and the drawing state (index 1). */
void *SoftGpu_StateData(int index, size_t *size);
#endif
