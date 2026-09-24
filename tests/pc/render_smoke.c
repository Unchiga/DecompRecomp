/* Synthetic retail-format packets rendered through PSY-Z, without game assets. */
#include "pc/compat/fs.h"
#include <psyz.h>
#include <libgpu.h>
#include <libetc.h>
#include "pc/render/psyz_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)
static MemoriesMemory memory;
static unsigned short texture[16 * 16];
static unsigned short indexed[4 * 16];
static unsigned short palette[16];
static unsigned short pixels[320 * 240];
static uint32_t words[20000];

static void put(uint32_t address, uint32_t value)
{
    Memories_WriteLE32(memory.ram + address, value);
}

static int capture(const char *path)
{
    FILE *file = fopen(path, "wb");
    size_t i;
    if (!file) return 0;
    if (fprintf(file, "P6\n320 240\n255\n") < 0) { fclose(file); return 0; }
    for (i = 0; i < 320 * 240; ++i) {
        unsigned p = pixels[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)((p & 31) * 255 / 31);
        rgb[1] = (unsigned char)(((p >> 5) & 31) * 255 / 31);
        rgb[2] = (unsigned char)(((p >> 10) & 31) * 255 / 31);
        if (fwrite(rgb, 1, 3, file) != 3) { fclose(file); return 0; }
    }
    return fclose(file) == 0;
}

int main(int argc, char **argv)
{
    DRAWENV draw;
    DISPENV display;
    RECT upload = {640, 0, 16, 16};
    RECT readback = {0, 0, 320, 240};
    RECT indexed_upload = {704, 0, 4, 16};
    RECT palette_upload = {0, 256, 16, 1};
    size_t count, x, y;
    const char *capture_path = NULL;
    unsigned long frames = 0, frame = 0;
    int arg;
#ifdef MEMORIES_PREVIEW
    int show = 1;
#else
    int show = 0;
#endif
    for (arg = 1; arg < argc; ++arg) {
        if (!strcmp(argv[arg], "--show")) show = 1;
        else if (!strcmp(argv[arg], "--headless")) show = 0;
        else if (!strcmp(argv[arg], "--frames") && arg + 1 < argc) {
            char *end;
            const char *value = argv[++arg];
            errno = 0;
            frames = strtoul(value, &end, 10);
            if (errno || !*value || *value == '-' || *end || !frames) {
                fprintf(stderr, "--frames requires a positive integer\n");
                return 2;
            }
        } else if (!strcmp(argv[arg], "--help")) {
            puts("Native graphics preview; the game is not playable yet.\n"
                 "Usage: memories-pc-preview [--show|--headless] [--frames N] [capture.ppm]\n"
                 "Close the window to exit. --headless skips the presentation loop;\n"
                 "for an offscreen driver also set SDL_VIDEODRIVER=offscreen.");
            return 0;
        } else if (argv[arg][0] != '-' && !capture_path) capture_path = argv[arg];
        else { fprintf(stderr, "Unknown argument: %s (try --help)\n", argv[arg]); return 2; }
    }
    Psyz_SetTitle("Forbidden Memories - Graphics Preview (Not Playable Yet)");
    Psyz_VideoSetVsyncMode(PSYZ_VSYNC_LIMITLESS);
    Psyz_VideoSetDitheringMode(PSYZ_DITHER_OFF);
    Psyz_VideoSetInternalResolution(1);
    ResetGraph(0);
    SetDefDrawEnv(&draw, 0, 0, 320, 240);
    SetDefDispEnv(&display, 0, 0, 320, 240);
    draw.dtd = 0;
    PutDrawEnv(&draw);
    PutDispEnv(&display);
    SetDispMask(1);
    for (y = 0; y < 16; ++y)
        for (x = 0; x < 16; ++x)
            texture[y * 16 + x] = ((x / 8) ^ (y / 8)) ? 0x03e0 : 0x7c1f;
    LoadImage(&upload, (u_long *)texture);
    palette[1] = 0x001f;
    palette[2] = 0x7fe0;
    for (y = 0; y < 16; ++y)
        for (x = 0; x < 4; ++x)
            indexed[y * 4 + x] = ((x / 2) ^ (y / 8)) ? 0x2222 : 0x1111;
    LoadImage(&indexed_upload, (u_long *)indexed);
    LoadImage(&palette_upload, (u_long *)palette);
    DrawSync(0);

    put(0x100, 0x200);
    put(0x200, 0x03000300); /* clear framebuffer */
    put(0x204, 0x02000000); put(0x208, 0); put(0x20c, 0x00f00140);
    put(0x300, 0x01000400); /* 16-bit texture page at x=640 */
    put(0x304, 0xe100010a);
    put(0x400, 0x04000500); /* raw-texture variable-size sprite */
    put(0x404, 0x65000000); put(0x408, 0x00100010);
    put(0x40c, 0); put(0x410, 0x00100010);
    put(0x500, 0x01000600); put(0x504, 0xe100000b); /* 4bpp page x=704 */
    put(0x600, 0x04000700);
    put(0x604, 0x65000000); put(0x608, 0x00100030);
    put(0x60c, 0x40000000); put(0x610, 0x00100010); /* CLUT y=256 */
    put(0x700, 0x04ffffff); /* terminal NOP payload, like the retail SDK */
    CHECK(Memories_GpuCollect(&memory, 0x80000100u, words, 20000, 7, &count) == MEMORIES_GPU_OK);
    CHECK(Memories_PsyzSubmit(words, count) == MEMORIES_GPU_OK);
    DrawSync(0);
    StoreImage(&readback, (u_long *)pixels);
    DrawSync(0);
    for (y = 0; y < 240; ++y) {
        for (x = 0; x < 320; ++x) {
            unsigned short expected = 0;
            if (x >= 16 && x < 32 && y >= 16 && y < 32)
                expected = texture[(y - 16) * 16 + x - 16];
            if (x >= 48 && x < 64 && y >= 16 && y < 32)
                expected = palette[(((x - 48) / 8) ^ ((y - 16) / 8)) ? 2 : 1];
            if ((pixels[y * 320 + x] & 0x7fff) != expected) {
                fprintf(stderr, "pixel %zu,%zu: %04x != %04x\n", x, y,
                        pixels[y * 320 + x], expected);
                return 1;
            }
        }
    }
    if (capture_path) CHECK(capture(capture_path));

    /* Cross the SDK queue's 16K-word boundary using complete drawing commands,
     * then draw a final colored pixel: a silent dropped tail must fail. */
    for (x = 0; x < 18000; x += 3) {
        words[x] = 0x60000000; words[x + 1] = 0; words[x + 2] = 0x00010001;
    }
    words[18000] = 0x600000ff; words[18001] = 0; words[18002] = 0x00010001;
    CHECK(Memories_PsyzSubmit(words, 18003) == MEMORIES_GPU_OK);
    StoreImage(&readback, (u_long *)pixels); DrawSync(0);
    CHECK((pixels[0] & 0x7fff) == 0x001f);
    /* A valid green draw followed by a bad command must reject atomically. */
    words[0] = 0x6000ff00; words[1] = 0; words[2] = 0x00010001;
    words[3] = 0xa0000000;
    CHECK(Memories_PsyzSubmit(words, 4) == MEMORIES_GPU_UNSUPPORTED_COMMAND);
    StoreImage(&readback, (u_long *)pixels); DrawSync(0);
    CHECK((pixels[0] & 0x7fff) == 0x001f);
    puts("Retail 16bpp/CLUT textures match all 76,800 pixels; queue and atomic-rejection checks passed");
    if (show) {
        FntLoad(960, 256);
        FntOpen(16, 64, 288, 160, 0, 512);
        FntPrint("FORBIDDEN MEMORIES\n\nNATIVE GRAPHICS PREVIEW\n\n"
                 "GAME PORT IN PROGRESS\n\nCLOSE WINDOW TO EXIT");
        FntFlush(-1);
        DrawSync(0);
        Psyz_VideoSetVsyncMode(PSYZ_VSYNC_OFF);
        while (!Psyz_QuitRequested() && (!frames || frame < frames)) {
            VSync(0);
            ++frame;
        }
    }
    return 0;
}
