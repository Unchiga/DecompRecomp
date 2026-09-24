/* Finding mods, applying them, and what a mod's data overrides do to the
 * sectors the drive model delivers. The disc is a stub here: the only thing
 * that matters is that a file lookup answers, since everything past that is
 * sector arithmetic. */
#define _POSIX_C_SOURCE 200809L
#include "pc/mods/mods.h"
#include "pc/mods/exports.h"
#include "pc/mods/json.h"
#include "pc/debug/symbols.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"
#include "scratch.h"
#include <unistd.h>

#define CARD_LBA 5000
#define CARD_SIZE 5000u

/* --- the port around the mod system ---------------------------------- */

int Log_Enabled(int channel) { (void)channel; return 0; }
int Log_Wanted(int channel) { (void)channel; return 0; }
void Log_Printf(int channel, const char *format, ...) { (void)channel; (void)format; }
unsigned short Platform_Pad(int port) { (void)port; return 0; }
int Symbols_Add(const SymbolsEntry *entries, size_t count) { (void)entries; (void)count; return 0; }
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

/* The table build_game32.py generates for the game: sorted by name. */
static char exported_function[16];
static int exported_variable = 42;
const MemoriesModExport Memories_ModExports[] = {
    {"D_80010000", (void *)0x80010000u},
    {"Duel_DrawFieldCards", exported_function},
    {"gDuel_wSceneStateFlags", &exported_variable},
};
const unsigned Memories_ModExportCount = sizeof(Memories_ModExports) / sizeof(Memories_ModExports[0]);

/* --- fixtures -------------------------------------------------------- */

static char root[SCRATCH_MAX];

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

/* The audio replacement an "audio" mod goes through (src/pc/audio/replace.h). */
static int audio_loads, audio_unloads, audio_mod = -1;
static int fake_audio_load(int mod, const char *id, const char *directory, const struct JsonValue *audio,
                           char *error, size_t size)
{
    (void)directory;
    assert(!strcmp(id, "sounds") && Json_Count(Json_Member(audio, "music")) == 1);
    audio_loads++;
    audio_mod = mod;
    snprintf(error, size, "audio: gone.wav cannot be read");
    return 1;
}
static void fake_audio_unload(int mod) { assert(mod == audio_mod); audio_unloads++; }

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
    int patcher, replacer, broken, camera, sounds, i;

    /* A relative path may not leave the directory it is relative to. */
    assert(Paths_Contained("card.mrg") && Paths_Contained("art/monster.tim"));
    assert(!Paths_Contained("../card.mrg") && !Paths_Contained("art/../../card.mrg"));
    assert(!Paths_Contained("/etc/passwd") && !Paths_Contained("") && !Paths_Contained("."));
    assert(!Paths_Contained("art//monster.tim") && !Paths_Contained("art/"));
    assert(!Paths_Contained("art\\monster.tim") && !Paths_Contained("C:/art"));

    /* The names a code mod binds to. */
    assert(Mods_Lookup("D_80010000") == (void *)0x80010000u);
    assert(Mods_Lookup("Duel_DrawFieldCards") == exported_function);
    assert(Mods_Lookup("gDuel_wSceneStateFlags") == &exported_variable);
    assert(!Mods_Lookup("Duel_DrawFieldCard") && !Mods_Lookup("") && !Mods_Lookup(NULL));
    /* And the C library the host lends, which is searched first. */
    assert(Mods_LibcSorted());
    assert(Mods_LibcLookup("memcpy") == (void (*)(void))memcpy);
    assert(Mods_Lookup("vsnprintf") && Mods_Lookup("strtol"));
    assert(!Mods_Lookup("fopen") && !Mods_Lookup("system") && !Mods_Lookup("getenv"));

    scratch_template(root, sizeof(root), "memories-mods");
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

    /* Replacement sounds, which apply without a restart. */
    make_dir("mods/sounds");
    write_text("mods/sounds/mod.json",
               "{ \"id\": \"sounds\", \"enabled\": true, \"audio\": { \"music\": { \"0x2D0\": \"gone.wav\" } } }");
    Mods_SetAudio(fake_audio_load, fake_audio_unload);

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
    sounds = find("sounds");
    assert(Mods_Count() == 5);
    /* An audio mod is live: applied now, its skipped files noted beside it. */
    assert(sounds >= 0 && Mods_Active(sounds) && !Mods_RequiresRestart(sounds));
    assert(audio_loads == 1 && audio_mod == sounds && strstr(Mods_Status(sounds), "gone.wav"));
    Mods_SetEnabled(sounds, 0);
    assert(audio_unloads == 1 && !Mods_Active(sounds));
    Mods_SetEnabled(sounds, 1);
    assert(audio_loads == 2 && Mods_Active(sounds));
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
