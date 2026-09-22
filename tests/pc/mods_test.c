/* Finding mods, applying them, and what a mod's data overrides do to the
 * sectors the drive model delivers. The disc is a stub here: the only thing
 * that matters is that a file lookup answers, since everything past that is
 * sector arithmetic. */
#define _POSIX_C_SOURCE 200809L
#include "pc/mods/mods.h"
#include "pc/mods/modload.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"
#include <unistd.h>

#define CARD_LBA 5000
#define CARD_SIZE 5000u

/* --- the port around the mod system ---------------------------------- */

int Log_Enabled(int channel) { (void)channel; return 0; }
void Log_Printf(int channel, const char *format, ...) { (void)channel; (void)format; }
unsigned short Platform_Pad(int port) { (void)port; return 0; }
int Memories_DiscReadSectors(int lba, int sectors, void *out)
{
    (void)lba;
    memset(out, 0, (size_t)sectors * 2048);
    return sectors;
}
int Memories_DiscFileInfo(const char *path, int *lba, unsigned *size)
{
    if (strcmp(path, "\\DATA\\CARD.MRG;1")) return -1;
    if (lba) *lba = CARD_LBA;
    if (size) *size = CARD_SIZE;
    return 0;
}
int Memories_DiscFileStart(const char *path)
{
    int lba = -1;
    return Memories_DiscFileInfo(path, &lba, NULL) ? -1 : lba;
}

/* The game's names a mod library could link to: none here
 * (tests/pc/modload_test.c loads real libraries). */
const ModSymbol Memories_ModSymbols[] = {{"", NULL}};
const unsigned Memories_ModSymbolCount = 0;

/* --- fixtures -------------------------------------------------------- */

static char root[] = "/tmp/memories-mods-XXXXXX";

static void write_file(const char *relative, const void *data, size_t size)
{
    char path[1024];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(data, 1, size, file) == size);
    assert(!fclose(file));
}

static void write_text(const char *relative, const char *text)
{
    write_file(relative, text, strlen(text));
}

static void make_dir(const char *relative)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    assert(!mkdir(path, 0777));
}

static int find(const char *id)
{
    int i;
    for (i = 0; i < Mods_Count(); i++) {
        if (!strcmp(Mods_Id(i), id)) return i;
    }
    return -1;
}

int main(void)
{
    unsigned char sector[2048], replacement[3000];
    char settings[1024];
    int patcher, replacer, broken, camera, i;

    /* A relative path may not leave the directory it is relative to. */
    assert(Paths_Contained("card.mrg") && Paths_Contained("art/monster.tim"));
    assert(!Paths_Contained("../card.mrg") && !Paths_Contained("art/../../card.mrg"));
    assert(!Paths_Contained("/etc/passwd") && !Paths_Contained("") && !Paths_Contained("."));
    assert(!Paths_Contained("art//monster.tim") && !Paths_Contained("art/"));
    assert(!Paths_Contained("art\\monster.tim") && !Paths_Contained("C:/art"));

    assert(mkdtemp(root));
    make_dir("mods");
    /* A patch mod: bytes at a file offset, which crosses a sector boundary. */
    make_dir("mods/patcher");
    write_text("mods/patcher/mod.json",
               "{ \"id\": \"patcher\", \"name\": \"Patcher\", \"enabled\": true, \"restart\": false,"
               "  \"data\": [ { \"file\": \"\\\\DATA\\\\CARD.MRG;1\","
               "               \"patch\": [ { \"at\": \"0x7FF\", \"bytes\": \"AABBCC\" } ] } ] }");
    /* A replacement mod: a file shorter than the one it stands in for. */
    make_dir("mods/replacer");
    for (i = 0; i < (int)sizeof(replacement); i++) replacement[i] = (unsigned char)(i & 0xff);
    write_file("mods/replacer/card.mrg", replacement, sizeof(replacement));
    write_text("mods/replacer/mod.json",
               "{ \"id\": \"replacer\", \"name\": \"Replacer\","
               "  \"data\": [ { \"file\": \"\\\\DATA\\\\CARD.MRG;1\", \"replace\": \"card.mrg\" } ] }");
    /* A manifest that is not JSON at all, and one that is not a mod. */
    make_dir("mods/broken");
    write_text("mods/broken/mod.json", "{ \"id\": \"broken\", ");
    make_dir("mods/not-a-mod");
    write_text("mods/not-a-mod/readme.txt", "nothing to see");
    /* A mod with a library that is not there, applied from the settings. */
    make_dir("mods/camera");
    write_text("mods/camera/mod.json",
               "{ \"id\": \"camera\", \"name\": \"Camera\", \"library\": \"camera\","
               "  \"legacy_setting\": \"hand_camera\" }");

    snprintf(settings, sizeof(settings), "%s/settings.txt", root);
    write_text("settings.txt", "hand_camera=1\nmod.replacer=1\n");
    assert(!setenv("MEMORIES_SETTINGS", settings, 1));
    assert(!setenv("MEMORIES_USER_DIR", root, 1));
    Settings_Load();
    Mods_Load();

    patcher = find("patcher");
    replacer = find("replacer");
    broken = find("broken");
    camera = find("camera");
    assert(patcher >= 0 && replacer >= 0 && broken >= 0 && camera >= 0);
    assert(find("not-a-mod") < 0);          /* no manifest, no mod */
    assert(Mods_Count() == 4);
    assert(!strcmp(Mods_Name(patcher), "Patcher"));
    assert(!strcmp(Mods_Name(broken), "broken") && Mods_Status(broken)[0]);
    /* The manifest's own default, and what the settings say instead. */
    assert(Mods_Enabled(patcher) && Mods_Enabled(replacer));
    /* The key this mod's choice used to live under is still read. */
    assert(Mods_Enabled(camera));
    /* Data overrides only hold from a fresh launch, so they ask for one
     * unless the manifest says otherwise; code mods take the manifest's. */
    assert(Mods_RequiresRestart(replacer) && !Mods_RequiresRestart(patcher));
    /* A library that cannot be opened leaves the mod with a reason. */
    assert(Mods_Status(camera)[0]);

    /* Both mods are applied, so the replacement stands in for the file and
     * the patch is written over it: a sector replaced, then patched. The
     * patch straddles the boundary, one byte in one sector and two in the
     * next, and the sectors past the replacement's end read as zeroes rather
     * than as whatever the disc still holds there. */
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector));
    assert(!memcmp(sector, replacement, 2047) && sector[2047] == 0xAA);
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA + 1, sector));
    assert(sector[0] == 0xBB && sector[1] == 0xCC);
    assert(!memcmp(sector + 2, replacement + 2050, sizeof(replacement) - 2050));
    assert(!sector[sizeof(replacement) - 2048] && !sector[2047]);
    assert(!Mods_DiscSector(CARD_LBA - 1, sector));
    assert(!Mods_DiscSector(CARD_LBA + 3, sector)); /* past both mods' reach */

    /* Removing a mod takes its overrides out with it, the moment it can:
     * the patch mod goes now, and the replacement, which asked for a
     * restart, stays in place until the game is launched again. */
    Mods_SetEnabled(patcher, 0);
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector) && sector[2047] == replacement[2047]);
    Mods_SetEnabled(replacer, 0);
    assert(!Mods_Enabled(replacer) && !Settings_GetNamed("mod.replacer", 1));
    memset(sector, 0xEE, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector));
    Mods_SetEnabled(patcher, 1);
    assert(Settings_GetNamed("mod.patcher", 0) == 1);
    memset(sector, 0, sizeof(sector));
    assert(Mods_DiscSector(CARD_LBA, sector) && sector[2047] == 0xAA);

    /* The settings are saved with the mod keys in them, and read back. */
    assert(Settings_Save());
    Settings_Load();
    assert(Settings_GetNamed("mod.patcher", 0) == 1 && !Settings_GetNamed("mod.replacer", 1));
    /* A mod can be settled from the environment, for a test run, and a
     * launch with the replacement removed leaves the disc alone entirely. */
    assert(!setenv("MEMORIES_MOD_PATCHER", "0", 1));
    Mods_Load();
    assert(!Mods_Enabled(patcher));
    memset(sector, 0xEE, sizeof(sector));
    assert(!Mods_DiscSector(CARD_LBA, sector) && sector[0] == 0xEE);

    Mods_Shutdown();
    return 0;
}
