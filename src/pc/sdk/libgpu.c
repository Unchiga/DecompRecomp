/* LIBGPU entry points over the software GPU. Drawing completes synchronously,
 * so the queue/idle queries always report an idle GPU. */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "pc/compat/libgs_ot.h"
#include "pc/guest/image.h"
#include "pc/platform/platform.h"
#include "pc/render/soft_gpu.h"
#include "pc/sdk/display.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include "pc/debug/crash.h"
#include "pc/mods/mods.h"
#include "pc/render/texture_pack.h"

#define IMAGE ((MemoriesMemory *)(uintptr_t)MEMORIES_GUEST_RAM) /* unused token */
#define MAX_FRAME_WORDS 0x80000u
#define MAX_CHAIN_HOPS 0x100000u

static DRAWENV draw_env;
static DISPENV disp_env;
static int display_enabled, frames_presented, frames_shown;
static uint32_t frame_words[MAX_FRAME_WORDS];
static size_t pending_words;
static void flush_drawing(void);

int ResetGraph(int mode)
{
    flush_drawing();
    if (mode == 0 || mode == 3) {
        SoftGpu_Reset();
    }
    return 0;
}

void SetDispMask(int mask)
{
    display_enabled = mask;
}

int GetVideoMode(void)
{
    return 0; /* NTSC */
}

DRAWENV *PutDrawEnv(DRAWENV *env)
{
    uint32_t words[6];
    flush_drawing();
    words[0] = 0xe1000000u | (env->tpage & 0x9ffu) | (env->dtd ? 0x200u : 0) | (env->dfe ? 0x400u : 0);
    words[1] = 0xe2000000u | (((uint32_t)-env->tw.w >> 3) & 0x1f) | ((((uint32_t)-env->tw.h >> 3) & 0x1f) << 5) |
               ((((uint32_t)env->tw.x >> 3) & 0x1f) << 10) | ((((uint32_t)env->tw.y >> 3) & 0x1f) << 15);
    words[2] = 0xe3000000u | ((uint32_t)env->clip.x & 0x3ff) | (((uint32_t)env->clip.y & 0x3ff) << 10);
    words[3] = 0xe4000000u | ((uint32_t)(env->clip.x + env->clip.w - 1) & 0x3ff) |
               (((uint32_t)(env->clip.y + env->clip.h - 1) & 0x3ff) << 10);
    words[4] = 0xe5000000u | ((uint32_t)env->ofs[0] & 0x7ff) | (((uint32_t)env->ofs[1] & 0x7ff) << 11);
    words[5] = 0xe6000000u;
    SoftGpu_Gp0(words, 6);
    if (env->isbg) {
        SoftGpu_Fill(env->clip.x, env->clip.y, env->clip.w, env->clip.h,
                     env->r0 | ((uint32_t)env->g0 << 8) | ((uint32_t)env->b0 << 16));
    }
    draw_env = *env;
    return env;
}

DRAWENV *GetDrawEnv(DRAWENV *env)
{
    *env = draw_env;
    return env;
}

DISPENV *PutDispEnv(DISPENV *env)
{
    disp_env = *env;
    return env;
}

DISPENV *GetDispEnv(DISPENV *env)
{
    *env = disp_env;
    return env;
}

unsigned Memories_PresentedFrames(void) { return (unsigned)frames_presented; }
unsigned Memories_ShownFrames(void) { return (unsigned)frames_shown; }

void Memories_DumpFrame(const char *path, int full_vram)
{
    FILE *file;
    int x, y, w = disp_env.disp.w > 0 ? disp_env.disp.w : 320;
    int h = disp_env.disp.h > 0 ? disp_env.disp.h : 240;
    int x0 = full_vram ? 0 : disp_env.disp.x, y0 = full_vram ? 0 : disp_env.disp.y;
    const uint16_t *source = SoftGpu_Vram();
    flush_drawing();
    if (full_vram) {
        w = SOFT_GPU_WIDTH;
        h = SOFT_GPU_HEIGHT;
    } else if (Platform_Widescreen() && !disp_env.isrgb24) {
        /* The picture the window shows: the widened one, when there is one. */
        const uint16_t *pixels;
        int wide_x, wide_w;
        if (SoftGpu_WideFrameView(x0, y0, w, h, &pixels, &wide_x, &wide_w)) {
            source = pixels;
            x0 = wide_x;
            w = wide_w;
        }
    }
    file = fopen(path, "wb");
    if (!file) {
        LOG(LOG_FRAMES, "cannot dump %s", path);
        return;
    }
    if (SoftGpu_Scale() > 1 && getenv("MEMORIES_DUMP_PICTURE") && !disp_env.isrgb24) {
        /* The scaled picture of the display area, as the window shows it. */
        int at_scale = SoftGpu_Scale(), stride = SOFT_GPU_WIDTH * at_scale;
        const uint32_t *picture = SoftGpu_Picture();
        uint32_t *read = NULL;
        if (!picture) { /* the backend's own renderer drew it (gl_picture.h) */
            read = malloc((size_t)w * at_scale * (size_t)h * at_scale * sizeof(*read));
            if (read && Platform_ReadPicture(read, x0 * at_scale, y0 * at_scale, w * at_scale, h * at_scale)) {
                picture = read;
                stride = w * at_scale;
                x0 = y0 = 0;
            } else {
                free(read);
                read = NULL;
            }
        }
        if (picture) {
            fprintf(file, "P6\n%d %d\n255\n", w * at_scale, h * at_scale);
            for (y = 0; y < h * at_scale; y++) {
                for (x = 0; x < w * at_scale; x++) {
                    uint32_t c = picture[(size_t)((y0 * at_scale + y) % (SOFT_GPU_HEIGHT * at_scale)) * stride +
                                         (size_t)((x0 * at_scale + x) % stride)];
                    fputc((c >> 16) & 0xff, file);
                    fputc((c >> 8) & 0xff, file);
                    fputc(c & 0xff, file);
                }
            }
            fclose(file);
            free(read);
            LOG(LOG_FRAMES, "dumped %s (picture at %dx)", path, at_scale);
            return;
        }
    }
    fprintf(file, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint16_t c = source[((y0 + y) & 511) * SOFT_GPU_WIDTH + ((x0 + x) & 1023)];
            if (disp_env.isrgb24 && !full_vram) {
                fwrite((const uint8_t *)&SoftGpu_Vram()[((y0 + y) & 511) * SOFT_GPU_WIDTH + x0] + x * 3,
                       1, 3, file);
                continue;
            }
            fputc((c & 0x1f) << 3, file);
            fputc(((c >> 5) & 0x1f) << 3, file);
            fputc(((c >> 10) & 0x1f) << 3, file);
        }
    }
    fclose(file);
    LOG(LOG_FRAMES, "dumped %s", path);
}

/* Widescreen shows the display area 4/3 as wide: the widened picture the GPU
 * drew for it, or, for a screen with no widescreen target (a movie, a small
 * drawing area), the 4:3 picture between black sides, so the window always
 * gets a 16:9 frame. */
static void present_wide(int w, int h)
{
    static uint16_t sides[SOFT_GPU_WIDTH * 2 * SOFT_GPU_HEIGHT]; /* room for a 24-bit row */
    const uint16_t *pixels;
    static int logged = -1;
    int x = disp_env.disp.x, y = disp_env.disp.y, wide_x, wide_w, margin = (w / 6 + 1) & ~1, row;
    int drawn = !disp_env.isrgb24 && SoftGpu_WideFrame(x, y, w, h, &pixels, &wide_x, &wide_w);
    if (drawn != logged) {
        LOG(LOG_FRAMES, "widescreen %dx%d at %d,%d: %s", w, h, x, y, drawn ? "widened" : "4:3 between black sides");
        logged = drawn;
    }
    if (drawn) {
        Platform_Present(pixels, SOFT_GPU_WIDTH, wide_x, y, wide_w, h, 0);
        return;
    }
    for (row = 0; row < h; row++) {
        uint8_t *out = (uint8_t *)(sides + row * SOFT_GPU_WIDTH * 2);
        const uint8_t *in = (const uint8_t *)(SoftGpu_Vram() + ((y + row) & 511) * SOFT_GPU_WIDTH);
        int size = disp_env.isrgb24 ? 3 : 2;
        memset(out, 0, (size_t)(w + 2 * margin) * (size_t)size);
        if (disp_env.isrgb24) {
            memcpy(out + margin * 3, in + x * 2, (size_t)w * 3);
        } else {
            int i;
            for (i = 0; i < w; i++) {
                ((uint16_t *)out)[margin + i] = ((const uint16_t *)in)[(x + i) & 1023];
            }
        }
    }
    Platform_Present(sides, SOFT_GPU_WIDTH * 2, 0, 0, w + 2 * margin, h, disp_env.isrgb24);
}

void Memories_PresentDisplay(void)
{
    const char *dump = getenv("MEMORIES_DUMP_FRAME");
    int w = disp_env.disp.w > 0 ? disp_env.disp.w : 320, h = disp_env.disp.h > 0 ? disp_env.disp.h : 240;
    int wide = Platform_Widescreen();
    flush_drawing();
    TexturePack_Service();
    frames_presented++;
    Platform_Frame((unsigned)frames_presented);
    if (dump && frames_presented == atoi(dump)) {
        const char *path = getenv("MEMORIES_DUMP_PATH");
        Memories_DumpFrame(path ? path : "tmp/pc/frame.ppm", getenv("MEMORIES_DUMP_VRAM") != NULL);
        Platform_StopTimers();
        exit(0);
    }
    if (display_enabled && Platform_PresentDue()) {
        int at_scale = SoftGpu_Scale();
        frames_shown++;
        if (wide) {
            present_wide(w, h);
        } else {
        if (at_scale <= 1 || disp_env.isrgb24 ||
            !Platform_PresentPicture(SoftGpu_Picture(), SOFT_GPU_WIDTH * at_scale, disp_env.disp.x * at_scale,
                                     disp_env.disp.y * at_scale, w * at_scale, h * at_scale, at_scale)) {
            Platform_Present(SoftGpu_Vram(), SOFT_GPU_WIDTH, disp_env.disp.x, disp_env.disp.y, w, h,
                             disp_env.isrgb24);
            }
        }
    } else {
        Platform_PumpEvents(); /* input and the menu keep up on frames that are not shown */
    }
    SoftGpu_SetWidescreen(wide); /* between frames, so a frame is drawn one way */
}

/* The GPU draws a list in the background while the game builds its next
 * frame, and the game relies on that: Graphics_BeginFrame starts the list
 * between VSync and the pad update, where a slow call lets a second VBlank
 * in and the pad code reports each press twice. So DrawOTag only snapshots
 * the list, and it is rasterized where the hardware would have finished it:
 * at DrawSync, or before anything else that reads or writes VRAM. */
static void flush_drawing(void)
{
    static unsigned draws, total_us, total_words;
    struct timespec t0, t1;
    size_t count = pending_words;
    pending_words = 0; /* first: an interrupt-level LoadImage may re-enter */
    if (!count) {
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    SoftGpu_Gp0(frame_words, count);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    {
        unsigned elapsed = (unsigned)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
        Memories_SetDrawStats((unsigned)count, elapsed);
        total_us += elapsed;
        total_words += (unsigned)count;
        if (++draws == 120) {
            LOG(LOG_FRAMES, "draws: %u us, %u words per DrawOTag", total_us / 120, total_words / 120);
            draws = total_us = total_words = 0;
        }
    }
}

void DrawOTag(u32 *list)
{
    size_t count;
    MemoriesGpuResult result;
    flush_drawing();
    result = Memories_GpuCollect(IMAGE, (uint32_t)(uintptr_t)list, frame_words, MAX_FRAME_WORDS, MAX_CHAIN_HOPS,
                                 &count);
    if (result != MEMORIES_GPU_OK) {
        char detail[160];
        snprintf(detail, sizeof(detail), "DrawOTag(%p): %s", (void *)list, Memories_GpuResultName(result));
        Crash_ReportSoft("DrawOTag", detail);
        exit(70);
    }
    pending_words = count;
}

void GsDrawOt(void *descriptor)
{
    /* The last moment before a frame is sent to the GPU, and so where an
     * enabled mod adds to it (src/pc/mods): its primitives sort into the
     * game's own tables and are layered by them. Nothing happens while the
     * mods are all off. */
    Mods_DrawFrame();
    DrawOTag(*(u32 **)((char *)descriptor + 16));
}

int DrawSync(int mode)
{
    (void)mode;
    flush_drawing();
    return 0;
}

int IsIdleGPU(int max_count)
{
    (void)max_count;
    return 0;
}

int ClearImage(RECT *rect, u8 r, u8 g, u8 b)
{
    flush_drawing();
    SoftGpu_Fill(rect->x, rect->y, rect->w, rect->h, r | ((uint32_t)g << 8) | ((uint32_t)b << 16));
    return 0;
}

int LoadImage(RECT *rect, u32 *pixels)
{
    flush_drawing();
    SoftGpu_Load(rect->x, rect->y, rect->w, rect->h, (const uint16_t *)pixels);
    return 0;
}

int StoreImage(RECT *rect, u32 *pixels)
{
    flush_drawing();
    SoftGpu_Store(rect->x, rect->y, rect->w, rect->h, (uint16_t *)pixels);
    return 0;
}

int MoveImage(RECT *rect, int x, int y)
{
    flush_drawing();
    SoftGpu_Move(rect->x, rect->y, x, y, rect->w, rect->h);
    return 0;
}

int LoadImage2(RECT *rect, u32 *pixels) { return LoadImage(rect, pixels); }
int StoreImage2(RECT *rect, u32 *pixels) { return StoreImage(rect, pixels); }
int MoveImage2(RECT *rect, int x, int y) { return MoveImage(rect, x, y); }

static void set_primitive(void *primitive, unsigned length, unsigned code)
{
    ((u8 *)primitive)[3] = (u8)length;
    ((u8 *)primitive)[7] = (u8)code;
}

void SetPolyG3(POLY_G3 *p) { set_primitive(p, 6, 0x30); }
void SetPolyG4(POLY_G4 *p) { set_primitive(p, 8, 0x38); }
void SetPolyGT4(POLY_GT4 *p) { set_primitive(p, 12, 0x3c); }

void SetSemiTrans(void *primitive, int enabled)
{
    u8 *code = (u8 *)primitive + 7;
    *code = enabled ? (u8)(*code | 2) : (u8)(*code & ~2);
}

/* Taken at VSync, after the present flushed any pending list. The presented
 * frame counter is the input script's clock and is left running. */
void LibGpu_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {{&draw_env, sizeof(draw_env)}, {&disp_env, sizeof(disp_env)},
                                         {&display_enabled, sizeof(display_enabled)}};
    flush_drawing();
    Memories_StateChunk(state, "libgpu", fields, 3);
}
