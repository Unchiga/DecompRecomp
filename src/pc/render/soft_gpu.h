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
/* The bank's pixels if it has been made, NULL otherwise (a primitive naming
 * a bank that was never made samples VRAM). */
const uint16_t *SoftGpu_BankPixels(int bank);
/* Widescreen: full-screen drawing areas get a companion buffer 4/3 as wide
 * (see soft_gpu.c). Turning it off frees them. */
void SoftGpu_SetWidescreen(int on);
int SoftGpu_Widescreen(void);
/* The rule a drawing area x1..x2, y1..y2 (inclusive) gets a widescreen
 * target by: the margin it is widened by on each side, 0 for none (off, not
 * a full screen, or no room in VRAM's width). The OpenGL picture draws its
 * own targets by it (gl_picture.c). */
int SoftGpu_WideMargin(int x1, int y1, int x2, int y2);
/* The widened picture of the display area x,y,w,h, if it has a target:
 * VRAM-shaped pixels, and the x and width to show. Returns 0 otherwise. */
int SoftGpu_WideFrame(int x, int y, int w, int h, const uint16_t **pixels, int *out_x, int *out_w);
/* The same picture without presenting it (frame dumps): nothing changes. */
int SoftGpu_WideFrameView(int x, int y, int w, int h, const uint16_t **pixels, int *out_x, int *out_w);
/* Scaled widened picture, SOFT_GPU_WIDTH * scale pixels per row. NULL at
 * console resolution, while a recorder draws the picture (it draws the
 * widened ones too), or if allocation failed. Call after WideFrame to also
 * observe its clearing of stale side borders. */
const uint32_t *SoftGpu_WidePicture(int x, int y, int w, int h);
/* Internal resolution: with a scale above 1 every primitive is also drawn,
 * at scale x scale pixels per VRAM word, into a second picture of the whole
 * of VRAM in 24-bit colour, which is what is presented; VRAM itself stays
 * exactly what the console's would be, since the game reads it back and
 * states hold it. Uploads, fills and moves keep the picture in step; a
 * texture pack's images are sampled at their own resolution there
 * (texture_dump.h, `sample`). Returns 0 if the picture cannot be made. */
int SoftGpu_SetScale(int scale);
int SoftGpu_Scale(void);
/* The picture: SOFT_GPU_WIDTH * scale words per row, 0x00RRGGBB. */
const uint32_t *SoftGpu_Picture(void);
/* Redraw the picture from VRAM (after a state load). */
void SoftGpu_PictureFromVram(void);
/* A second renderer's record of what changes VRAM (gl_picture.h): every
 * GP0 batch as it is executed, every transfer made outside one, in order,
 * and a resync when VRAM or the state changed wholesale (a state load, a
 * reset, a new scale), with the drawing state as GP0 words E1 to E6. At a
 * scale of 1 only the resyncs are recorded (nothing draws a picture then;
 * the next scale's resync brings the state). While a recorder is set and
 * the scale is above 1 no picture is drawn here and SoftGpu_Picture() is
 * NULL: the recorder draws it. A load's pixels are
 * the caller's and only valid during the call. */
typedef struct SoftGpuRecorder {
    void (*gp0)(const uint32_t *words, size_t count);
    void (*load)(int x, int y, int w, int h, const uint16_t *pixels);
    void (*move)(int sx, int sy, int dx, int dy, int w, int h);
    void (*fill)(int x, int y, int w, int h, uint32_t rgb24);
    void (*resync)(int scale, const uint32_t state[6]);
} SoftGpuRecorder;
void SoftGpu_SetRecorder(const SoftGpuRecorder *recorder);
void SoftGpu_StateWords(uint32_t words[6]);

/* Save states: VRAM (index 0) and the drawing state (index 1). */
void *SoftGpu_StateData(int index, size_t *size);
#endif
