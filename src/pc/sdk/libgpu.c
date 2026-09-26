/* LIBGPU entry points over the software GPU. Drawing completes synchronously,
 * so the queue/idle queries always report an idle GPU. */
#include "pc/compat/fs.h"
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "pc/compat/libgs_ot.h"
#include "pc/guest/image.h"
#include "pc/platform/platform.h"
#include "pc/platform/credits.h"
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
#include "pc/render/texture_dump.h"
#include "pc/compat/signal.h"
#include "pc/compat/pgxp.h"
#include "pc/platform/settings.h"

#define IMAGE ((MemoriesMemory *)(uintptr_t)MEMORIES_GUEST_RAM) /* unused token */
#define MAX_FRAME_WORDS 0x80000u
#define MAX_CHAIN_HOPS 0x100000u

static DRAWENV draw_env;
static DISPENV disp_env;
static int display_enabled, frames_presented, frames_shown;
static uint32_t frame_words[MAX_FRAME_WORDS];
static uint32_t frame_addresses[MAX_FRAME_WORDS]; /* where each was in guest RAM (PGXP) */
static size_t pending_words;
static void flush_drawing(void);
#define MAX_FRAME_PRECISE 65536
static PgxpVertex frame_precise[MAX_FRAME_PRECISE];
static size_t pending_precise;

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
        const uint32_t *picture = !full_vram && Platform_Widescreen()
            ? SoftGpu_WidePicture(disp_env.disp.x, disp_env.disp.y, disp_env.disp.w, h) : SoftGpu_Picture();
        uint32_t *read = NULL;
        if (!picture && (full_vram || !Platform_Widescreen())) { /* the backend's own renderer drew it (gl_picture.h) */
            read = malloc((size_t)w * at_scale * (size_t)h * at_scale * sizeof(*read));
            if (read && Platform_ReadPicture(read, x0 * at_scale, y0 * at_scale, w * at_scale, h * at_scale)) {
                picture = read;
                stride = w * at_scale;
                x0 = y0 = 0;
            } else {
                free(read);
                read = NULL;
            }
        } else if (!picture) { /* widened, by the backend's own renderer */
            read = malloc((size_t)w * at_scale * (size_t)h * at_scale * sizeof(*read));
            if (read && Platform_ReadWidePicture(read, disp_env.disp.x, disp_env.disp.y, disp_env.disp.w, h, w, at_scale)) {
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
        int at_scale = SoftGpu_Scale();
        const uint32_t *picture = SoftGpu_WidePicture(x, y, w, h);
        if (picture && Platform_PresentPicture(picture, SOFT_GPU_WIDTH * at_scale,
                wide_x * at_scale, y * at_scale, wide_w * at_scale, h * at_scale, at_scale)) return;
        /* The backend's own renderer drew it (gl_picture.h). */
        if (!picture && at_scale > 1 && Platform_PresentWidePicture(x, y, w, h, wide_w, at_scale)) return;
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

int Memories_SetInternalScale(int wanted)
{
    sigset_t held, previous;
    int done;
    /* 1, 2, 4 or 8: the setting's 3 is 2, 5 to 7 are 4. */
    wanted = wanted >= 8 ? 8 : wanted >= 4 ? 4 : wanted >= 2 ? 2 : 1;
    /* The old picture is freed and a new one made, and an upload from the
     * interrupt tick draws into the picture: the clock waits meanwhile. */
    sigemptyset(&held);
    sigaddset(&held, SIGALRM);
    sigprocmask(SIG_BLOCK, &held, &previous);
    done = SoftGpu_SetScale(wanted);
    sigprocmask(SIG_SETMASK, &previous, NULL);
    return done;
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
    Credits_Frame();
    {
        /* MEMORIES_WINDOW_SHOT=<frame>: the window as shown at that frame,
         * as the screenshot key would save it (checking the window itself
         * from a scripted run). */
        const char *shot = getenv("MEMORIES_WINDOW_SHOT");
        const char *rescale = getenv("MEMORIES_SCALE_AT"); /* "<frame>:<scale>": the View menu's change, scripted */
        /* "<frame>:<mode>[,<frame>:<mode>...]": from that frame on, the next
         * mode a screen publishes (a menu choice, a screen's exit) becomes
         * <mode> instead (main_modes.h; 0 is the game's own debug menu), so
         * a scripted run reaches any screen. Main_Loop starts a mode whose
         * byte lacks 0x80; a present comes before its check. */
        const char *mode_at = getenv("MEMORIES_MODE_AT");
        static int mode_step, mode_seen = -1;
        const char *dump_from = getenv("MEMORIES_DUMP_TEXTURES_FROM");
        if (shot && frames_presented == atoi(shot)) Platform_Screenshot(1);
        if (dump_from && frames_presented == (unsigned)atoi(dump_from)) TextureDump_Restart();
        if (mode_at) {
            extern unsigned char D_8009B26C; /* main_mode_state.h: the active mode */
            const char *at = mode_at;
            int i, published = D_8009B26C != mode_seen && !(D_8009B26C & 0x80);
            mode_seen = D_8009B26C;
            for (i = 0; i < mode_step && at; i++) at = strchr(at, ',') ? strchr(at, ',') + 1 : NULL;
            if (at && *at && strchr(at, ':') && frames_presented >= (unsigned)atoi(at) && published) {
                int mode = atoi(strchr(at, ':') + 1);
                fprintf(stderr, "memories-pc: frame %u: mode %d published, running %d\n", (unsigned)frames_presented,
                        D_8009B26C & 0x1f, mode);
                D_8009B26C = (unsigned char)mode;
                mode_seen = mode;
                mode_step++;
            }
        }
        if (rescale && frames_presented == atoi(rescale) && strchr(rescale, ':')) {
            Memories_SetInternalScale(atoi(strchr(rescale, ':') + 1));
        }
    }
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
    SoftGpu_SetPrecise(frame_precise, pending_precise);
    pending_precise = 0;
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

/* A model's parts are each projected with their own matrix, and where they
 * meet the vertices differ by up to a pixel or so: the console closes the
 * seam by rounding them into the same whole pixel. So a frame word that
 * carries two different precise positions keeps its whole-pixel one on
 * each of its vertices (the depth stays precise). */
#define SEAM_TABLE (2 * MAX_FRAME_PRECISE)
static void snap_seams(void)
{
    static struct {
        uint32_t word, stamp;
        float x, y;
        int seam;
    } table[SEAM_TABLE];
    static uint32_t stamp;
    size_t i, pass;
    if (++stamp == 0) {
        memset(table, 0, sizeof(table));
        stamp = 1;
    }
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < pending_precise; i++) {
            PgxpVertex *vertex = &frame_precise[i];
            uint32_t word = frame_words[vertex->index];
            unsigned at = (word * 2654435761u) >> 15; /* 17 bits: SEAM_TABLE */
            while (table[at].stamp == stamp && table[at].word != word) at = (at + 1) & (SEAM_TABLE - 1);
            if (!pass) {
                if (table[at].stamp != stamp) {
                    table[at].word = word;
                    table[at].stamp = stamp;
                    table[at].x = vertex->x;
                    table[at].y = vertex->y;
                    table[at].seam = 0;
                } else if (table[at].x != vertex->x || table[at].y != vertex->y) {
                    table[at].seam = 1;
                }
            } else if (table[at].seam) {
                vertex->x = (float)(int16_t)(word & 0xffffu);
                vertex->y = (float)(int16_t)(word >> 16);
            }
        }
    }
}

void DrawOTag(u32 *list)
{
    size_t count;
    MemoriesGpuResult result;
    flush_drawing();
    result = Memories_GpuCollectAt(IMAGE, (uint32_t)(uintptr_t)list, frame_words, frame_addresses, MAX_FRAME_WORDS,
                                   MAX_CHAIN_HOPS, &count);
    if (result != MEMORIES_GPU_OK) {
        char detail[160];
        snprintf(detail, sizeof(detail), "DrawOTag(%p): %s", (void *)list, Memories_GpuResultName(result));
        Crash_ReportFatal("DrawOTag", detail);
        exit(70);
    }
    pending_words = count;
    /* PGXP (pgxp.h): the frame's words that are vertices projected since the
     * last DrawOTag, with where they really are: those the game's drawing
     * wrote by address, the rest by value; then the next frame's. Below
     * level 2 a vertex keeps the console's whole-pixel position and only its
     * depth is used (textures in perspective); at level 2 the seams between
     * a model's parts stay closed (snap_seams). */
    pending_precise = 0;
    if (Pgxp_Active) {
        static unsigned frames, placed, matched;
        int positions = Settings_Get(SET_PGXP) >= 2;
        size_t i;
        for (i = 0; i < count && pending_precise < MAX_FRAME_PRECISE; i++) {
            PgxpVertex *vertex = &frame_precise[pending_precise];
            int at = Pgxp_FindAt(frame_addresses[i], frame_words[i], &vertex->x, &vertex->y, &vertex->w);
            if (at > 0) {
                placed++;
            } else if (at < 0) {
                continue;
            } else if (Pgxp_Find(frame_words[i], &vertex->x, &vertex->y, &vertex->w)) {
                matched++;
            } else {
                continue;
            }
            if (!positions) {
                vertex->x = (float)(int16_t)(frame_words[i] & 0xffffu);
                vertex->y = (float)(int16_t)(frame_words[i] >> 16);
            }
            vertex->index = (uint32_t)i;
            pending_precise++;
        }
        if (positions) snap_seams();
        if (++frames == 120) {
            LOG(LOG_FRAMES, "pgxp: %u precise vertex words per DrawOTag (%u by address, %u by value)",
                (placed + matched) / 120, placed / 120, matched / 120);
            frames = placed = matched = 0;
        }
    }
    Pgxp_NextFrame();
    Pgxp_Active = Settings_Get(SET_PGXP) && SoftGpu_Scale() > 1;
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
