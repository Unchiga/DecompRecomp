#include "hud.h"
#include "log.h"
#include "pc/cards/fusion_helper.h"
#include "pc/audio/spu.h"
#include "pc/mods/mods.h"
#include "pc/guest/state.h"
#include "pc/platform/platform.h"
#include "pc/platform/settings.h"
#include "pc/saves/deck_menu.h"
#include "pc/saves/save_menu.h"
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
    int s = Menu_Scale();
    Menu_DrawText(canvas, x, *y + 8 * s, value, 0xf2f2f4u);
    *y += 17 * s;
}

static void draw_stats(MenuCanvas *canvas)
{
    const FrameStats *stats = Memories_FrameStats();
    char line[512], rate[32] = "";
    int level = Settings_Get(SET_SHOW_HUD), y, queued, lba, music = 0, sfx = 0, v, s = Menu_Scale();
    unsigned underruns, bytes_per_second;
    const char *tail[8];
    int tail_count, i;
    bounds.x = bounds.y = bounds.w = bounds.h = 0;
    if (!level || !canvas || !canvas->pixels) return;
    if (Platform_ClockRate() == 0) snprintf(rate, sizeof(rate), " [paused]");
    else if (Platform_ClockRate() == -1) snprintf(rate, sizeof(rate), " [uncapped]");
    else if (Platform_ClockRate() != 100) snprintf(rate, sizeof(rate), " [%d%%]", Platform_ClockRate());
    snprintf(line, sizeof(line), "%u.%u fps%s, %u.%u shown", stats->fps_tenths / 10, stats->fps_tenths % 10, rate,
             stats->shown_tenths / 10, stats->shown_tenths % 10);
    if (level == 1) {
        bounds.w = Menu_TextWidth(line) + 16 * s;
        bounds.h = 24 * s;
        bounds.x = canvas->width - bounds.w - 6 * s;
        bounds.y = 4 * s;
        panel(canvas, bounds.x, bounds.y, bounds.w, bounds.h);
        Menu_DrawText(canvas, bounds.x + 8 * s, bounds.y + 12 * s, line, 0xf2f2f4u);
        return;
    }
    bounds.x = 8 * s;
    bounds.y = Menu_Height() + 8 * s;
    bounds.w = canvas->width < 620 * s ? canvas->width - 16 * s : 612 * s;
    bounds.h = 300 * s;
    panel(canvas, bounds.x, bounds.y, bounds.w, bounds.h);
    y = bounds.y + 8 * s;
    text(canvas, bounds.x + 10 * s, &y, line);
    snprintf(line, sizeof(line), "frame %u / VBlank %u", Memories_PresentedFrames(), Platform_VBlankCount());
    text(canvas, bounds.x + 10 * s, &y, line);
    snprintf(line, sizeof(line), "game %u us (max %u), present %u us (max %u)", stats->game_us,
             stats->game_max_us, stats->present_us, stats->present_max_us);
    text(canvas, bounds.x + 10 * s, &y, line);
    snprintf(line, sizeof(line), "missed VBlanks %u/120; DrawOTag %u words, %u us; present cap %d (%u us)",
             stats->missed_vblanks, stats->draw_words, stats->draw_us, Platform_PresentCap(),
             Platform_PresentPeriodUs());
    text(canvas, bounds.x + 10 * s, &y, line);
    Platform_AudioStats(&queued, &underruns);
    snprintf(line, sizeof(line), "audio queued %d frames, underruns %u", queued, underruns);
    text(canvas, bounds.x + 10 * s, &y, line);
    Memories_DiscStats(&lba, &bytes_per_second);
    snprintf(line, sizeof(line), "disc LBA %d, %u bytes/s", lba, bytes_per_second);
    text(canvas, bounds.x + 10 * s, &y, line);
    for (v = 0; v < SPU_VOICES; v++) if (Spu_KeyStatus((unsigned)v)) {
        if (v < 20) music++; else sfx++;
    }
    snprintf(line, sizeof(line), "voices music %d, SFX %d; clock %d; state slot %d (loaded %d)",
             music, sfx, Platform_ClockRate(), Platform_StateSlot(), Memories_LastStateSlot());
    text(canvas, bounds.x + 10 * s, &y, line);
    tail_count = Log_Tail(8, tail);
    for (i = 0; i < tail_count; i++) {
        snprintf(line, sizeof(line), "%.90s", tail[i]);
        line[strcspn(line, "\n")] = 0;
        text(canvas, bounds.x + 10 * s, &y, line);
    }
}

/* One rectangle over what has been drawn so far and x, y, w, h. */
static void cover(int x, int y, int w, int h)
{
    if (!w || !h) return;
    if (bounds.w && bounds.h) {
        int right = bounds.x + bounds.w > x + w ? bounds.x + bounds.w : x + w;
        int bottom = bounds.y + bounds.h > y + h ? bounds.y + bounds.h : y + h;
        x = bounds.x < x ? bounds.x : x;
        y = bounds.y < y ? bounds.y : y;
        w = right - x;
        h = bottom - y;
    }
    bounds.x = x; bounds.y = y; bounds.w = w; bounds.h = h;
}

/* The statistics, the mods' overlays, then the save slot or deck menu over them. */
void Hud_Draw(MenuCanvas *canvas)
{
    int x, y, w, h;
    draw_stats(canvas);
    FusionHelper_Draw(canvas, &x, &y, &w, &h);
    cover(x, y, w, h);
    Mods_DrawOverlay(canvas, Menu_Scale(), Menu_DrawTextScaled, Menu_TextWidthScaled, &x, &y, &w, &h);
    cover(x, y, w, h);
    SaveMenu_Draw(canvas, &x, &y, &w, &h);
    if (!w || !h) DeckMenu_Draw(canvas, &x, &y, &w, &h);
    cover(x, y, w, h);
}

static unsigned stats_signature(void)
{
    const FrameStats *stats = Memories_FrameStats();
    int level = Settings_Get(SET_SHOW_HUD);
    if (!level) return 0;
    if (level == 2) return Memories_PresentedFrames() * 4u + 2u;
    return (stats->fps_tenths * 4096u + stats->shown_tenths) * 512u + (unsigned)(Platform_ClockRate() + 1) * 4u + 1u;
}

unsigned Hud_Signature(void)
{
    return stats_signature() ^ SaveMenu_Signature() * 2654435761u ^ DeckMenu_Signature() * 40503u ^
           FusionHelper_Signature() * 16777619u ^ Mods_OverlaySignature(Memories_PresentedFrames()) * 2246822519u;
}

void Hud_Bounds(int *x, int *y, int *w, int *h)
{
    *x = bounds.x; *y = bounds.y; *w = bounds.w; *h = bounds.h;
}
