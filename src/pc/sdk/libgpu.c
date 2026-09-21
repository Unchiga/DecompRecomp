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

#define IMAGE ((MemoriesMemory *)(uintptr_t)MEMORIES_GUEST_RAM) /* unused token */
#define MAX_FRAME_WORDS 0x80000u
#define MAX_CHAIN_HOPS 0x100000u

static DRAWENV draw_env;
static DISPENV disp_env;
static int display_enabled, frames_presented;
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

void Memories_DumpFrame(const char *path, int full_vram)
{
    FILE *file;
    int x, y, w = disp_env.disp.w > 0 ? disp_env.disp.w : 320;
    int h = disp_env.disp.h > 0 ? disp_env.disp.h : 240;
    int x0 = full_vram ? 0 : disp_env.disp.x, y0 = full_vram ? 0 : disp_env.disp.y;
    flush_drawing();
    if (full_vram) {
        w = SOFT_GPU_WIDTH;
        h = SOFT_GPU_HEIGHT;
    }
    file = fopen(path, "wb");
    if (!file) {
        LOG(LOG_FRAMES, "cannot dump %s", path);
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint16_t c = SoftGpu_Vram()[((y0 + y) & 511) * SOFT_GPU_WIDTH + ((x0 + x) & 1023)];
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

void Memories_PresentDisplay(void)
{
    const char *dump = getenv("MEMORIES_DUMP_FRAME");
    int w = disp_env.disp.w > 0 ? disp_env.disp.w : 320, h = disp_env.disp.h > 0 ? disp_env.disp.h : 240;
    int skip_present = 0;
    flush_drawing();
    frames_presented++;
    Platform_Frame((unsigned)frames_presented);
    if (dump && frames_presented == atoi(dump)) {
        const char *path = getenv("MEMORIES_DUMP_PATH");
        Memories_DumpFrame(path ? path : "tmp/pc/frame.ppm", getenv("MEMORIES_DUMP_VRAM") != NULL);
        exit(0);
    }
    if (display_enabled) {
        int clock_rate = Platform_ClockRate();
        if (clock_rate > 100 || clock_rate == -1) {
            static uint64_t last_present_us;
            struct timespec now;
            uint64_t real_now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            real_now = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
            skip_present = last_present_us && real_now - last_present_us < 16000;
            if (!skip_present) last_present_us = real_now;
        }
        if (!skip_present) {
            Platform_Present(SoftGpu_Vram(), SOFT_GPU_WIDTH, disp_env.disp.x, disp_env.disp.y, w, h,
                             disp_env.isrgb24);
        }
    }
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
