/* Renders the Mods window to PPM files for review (tools/pc/preview_mods.sh),
 * with the real menu font and the repository's mods/ folder, without booting
 * guest code. PREVIEW_SCALE=2 draws at twice the size. */
#include "pc/compat/fs.h"
#include "../../src/pc/platform/menu.c"
#include "pc/mods/mods.h"
#include "pc/platform/mods_window.h"
#include "pc/platform/settings.h"
#include "pc/debug/symbols.h"
#include "pc/mods/exports.h"
#include <assert.h>
/* The port around the mod system: nothing here activates a mod. */
int Log_Enabled(LogChannel channel) { return (void)channel, 0; }
int Log_Wanted(LogChannel channel) { return (void)channel, 0; }
void Log_Printf(LogChannel channel, const char *format, ...) { (void)channel, (void)format; }
unsigned short Platform_Pad(int port) { return (void)port, 0; }
int Symbols_Add(const SymbolsEntry *entries, size_t count) { return (void)entries, (void)count, 0; }
int Memories_DiscReadSectors(int lba, int sectors, void *out) { return (void)lba, (void)out, sectors; }
int Memories_DiscSectorCount(void) { return 0; }
int Memories_DiscOriginalFileInfo(const char *path, int *lba, unsigned *size)
{
    return (void)path, (void)lba, (void)size, -1;
}
int Memories_DiscFileInfo(const char *path, int *lba, unsigned *size)
{
    return (void)path, (void)lba, (void)size, -1;
}
int Memories_DiscFileStart(const char *path) { return (void)path, -1; }
const MemoriesModExport Memories_ModExports[1];
const unsigned Memories_ModExportCount = 0;
int Platform_OpenFolder(const char *path)
{
    (void)path;
    return 0;
}
int Platform_RestartGame(void) { return 0; }
static void save(MenuCanvas *c, const char *path)
{
    ModsWindow_Draw(c);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", c->width, c->height);
    for (int y = 0; y < c->height; y++)
        for (int x = 0; x < c->width; x++) {
            uint32_t p = c->pixels[y * c->stride + x];
            unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
            assert(fwrite(rgb, 1, 3, f) == 3);
        }
    assert(!fclose(f));
}
static void send(MenuEventType type, int x, int y, int wheel, MenuKey key)
{
    MenuEvent e = {0};
    e.type = type;
    e.x = x;
    e.y = y;
    e.button = 1;
    e.wheel = wheel;
    e.key = key;
    ModsWindow_Event(&e);
}
static MenuCanvas make(int w, int h)
{
    MenuCanvas c = {0};
    c.width = c.stride = w;
    c.height = h;
    c.pixels = calloc((size_t)w * h, 4);
    assert(c.pixels);
    return c;
}
int main(void)
{
    int w, h, x, y;
    if (getenv("PREVIEW_SCALE"))
        ui = atoi(getenv("PREVIEW_SCALE"));
    load_font();
    Settings_Load();
    Mods_Load();
    ModsWindow_Init();
    ModsWindow_Size(&w, &h);
    MenuCanvas c = make(w, h);
    /* AI Hard Mode has the longest settings list. */
    for (int i = 0; i < Mods_Count() && strcmp(Mods_Id(i), "ai-hard-mode"); i++)
        send(MENU_EVENT_KEY_DOWN, 0, 0, 0, MENU_KEY_DOWN);
    save(&c, "tmp/pc/mods-about.ppm");
    x = w * 68 / 100; /* the Settings tab, where mods_window.c lays it out */
    y = 238 * h / 640;
    {
        send(MENU_EVENT_BUTTON_DOWN, x, y, 0, MENU_KEY_OTHER);
        save(&c, "tmp/pc/mods-settings.ppm");
        for (int i = 0; i < 8; i++)
            send(MENU_EVENT_WHEEL, x, y + 200, -1, MENU_KEY_OTHER);
        save(&c, "tmp/pc/mods-settings-scrolled.ppm");
        for (int i = 0; i < 40; i++)
            send(MENU_EVENT_WHEEL, x, y + 200, -1, MENU_KEY_OTHER);
        save(&c, "tmp/pc/mods-settings-end.ppm");
        ModsWindow_Resize(620, 480);
        ModsWindow_Size(&w, &h);
        MenuCanvas small = make(w, h);
        for (int i = 0; i < 20; i++)
            send(MENU_EVENT_WHEEL, w - 100, h - 150, 1, MENU_KEY_OTHER);
        for (int i = 0; i < 5; i++)
            send(MENU_EVENT_WHEEL, w - 100, h - 150, -1, MENU_KEY_OTHER);
        save(&small, "tmp/pc/mods-small.ppm");
        free(small.pixels);
    }
    free(c.pixels);
    return 0;
}
