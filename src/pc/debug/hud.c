#include "hud.h"
#include "log.h"
#include "pc/audio/spu.h"
#include "pc/guest/state.h"
#include "pc/platform/platform.h"
#include "pc/platform/settings.h"
#include "pc/sdk/disc.h"
#include "pc/sdk/display.h"
#include <stdio.h>
#include <string.h>

static struct { int x, y, w, h; } bounds;

static uint32_t blend(uint32_t under, uint32_t over, unsigned alpha)
{
    unsigned inverse = 255 - alpha;
    unsigned r = ((over >> 16 & 255) * alpha + (under >> 16 & 255) * inverse) / 255;
    unsigned g = ((over >> 8 & 255) * alpha + (under >> 8 & 255) * inverse) / 255;
    unsigned b = ((over & 255) * alpha + (under & 255) * inverse) / 255;
    unsigned a = alpha + (under >> 24) * inverse / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

static void panel(MenuCanvas *canvas, int x, int y, int w, int h)
{
    int row, column;
    for (row = y; row < y + h && row < canvas->height; row++) {
        for (column = x; column < x + w && column < canvas->width; column++) {
            uint32_t *pixel = canvas->pixels + (size_t)row * (size_t)canvas->stride + (size_t)column;
            *pixel = blend(*pixel, 0x101216u, 210);
        }
    }
}

static void text(MenuCanvas *canvas, int x, int *y, const char *value)
{
    Menu_DrawText(canvas, x, *y + 8, value, 0xf2f2f4u);
    *y += 17;
}

void Hud_Draw(MenuCanvas *canvas)
{
    const FrameStats *stats = Memories_FrameStats();
    char line[512], rate[32] = "";
    int level = Settings_Get(SET_SHOW_HUD), y, queued, lba, music = 0, sfx = 0, v;
    unsigned underruns, bytes_per_second;
    const char *tail[8];
    int tail_count, i;
    bounds.x = bounds.y = bounds.w = bounds.h = 0;
    if (!level || !canvas || !canvas->pixels) return;
    if (Platform_ClockRate() == 0) snprintf(rate, sizeof(rate), " [paused]");
    else if (Platform_ClockRate() == -1) snprintf(rate, sizeof(rate), " [uncapped]");
    else if (Platform_ClockRate() != 100) snprintf(rate, sizeof(rate), " [%d%%]", Platform_ClockRate());
    snprintf(line, sizeof(line), "%u.%u fps%s", stats->fps_tenths / 10, stats->fps_tenths % 10, rate);
    if (level == 1) {
        bounds.w = Menu_TextWidth(line) + 16;
        bounds.h = 24;
        bounds.x = canvas->width - bounds.w - 6;
        bounds.y = 4;
        panel(canvas, bounds.x, bounds.y, bounds.w, bounds.h);
        Menu_DrawText(canvas, bounds.x + 8, bounds.y + 12, line, 0xf2f2f4u);
        return;
    }
    bounds.x = 8;
    bounds.y = 34;
    bounds.w = canvas->width < 620 ? canvas->width - 16 : 612;
    bounds.h = 300;
    panel(canvas, bounds.x, bounds.y, bounds.w, bounds.h);
    y = bounds.y + 8;
    text(canvas, bounds.x + 10, &y, line);
    snprintf(line, sizeof(line), "frame %u / VBlank %u", Memories_PresentedFrames(), Platform_VBlankCount());
    text(canvas, bounds.x + 10, &y, line);
    snprintf(line, sizeof(line), "game %u us (max %u), present %u us (max %u)", stats->game_us,
             stats->game_max_us, stats->present_us, stats->present_max_us);
    text(canvas, bounds.x + 10, &y, line);
    snprintf(line, sizeof(line), "missed VBlanks %u/120; DrawOTag %u words, %u us", stats->missed_vblanks,
             stats->draw_words, stats->draw_us);
    text(canvas, bounds.x + 10, &y, line);
    Platform_AudioStats(&queued, &underruns);
    snprintf(line, sizeof(line), "audio queued %d frames, underruns %u", queued, underruns);
    text(canvas, bounds.x + 10, &y, line);
    Memories_DiscStats(&lba, &bytes_per_second);
    snprintf(line, sizeof(line), "disc LBA %d, %u bytes/s", lba, bytes_per_second);
    text(canvas, bounds.x + 10, &y, line);
    for (v = 0; v < SPU_VOICES; v++) if (Spu_KeyStatus((unsigned)v)) {
        if (v < 20) music++; else sfx++;
    }
    snprintf(line, sizeof(line), "voices music %d, SFX %d; clock %d; state slot %d (loaded %d)",
             music, sfx, Platform_ClockRate(), Platform_StateSlot(), Memories_LastStateSlot());
    text(canvas, bounds.x + 10, &y, line);
    tail_count = Log_Tail(8, tail);
    for (i = 0; i < tail_count; i++) {
        snprintf(line, sizeof(line), "%.90s", tail[i]);
        line[strcspn(line, "\n")] = 0;
        text(canvas, bounds.x + 10, &y, line);
    }
}

void Hud_Bounds(int *x, int *y, int *w, int *h)
{
    *x = bounds.x; *y = bounds.y; *w = bounds.w; *h = bounds.h;
}
