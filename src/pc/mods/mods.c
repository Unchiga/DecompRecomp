/* The mod system (mods.h): finding mods, applying them, and the services
 * they are given (modapi.h).
 *
 * A mod is a directory with a mod.json in it. The release's own mods sit in
 * `mods/` beside the executable; a player's own go in the mods directory of
 * their user directory (paths.h), and win when both hold the same id. The
 * manifest names the mod, says whether it needs a restart, and carries two
 * kinds of content, either or both:
 *
 *  - code: one object file, the same on every system, which the game's own
 *    loader (object_loader.c) links in when the mod is first applied and
 *    keeps for as long as the game runs (unloading one while the game holds
 *    pointers into it is how a port crashes for no visible reason);
 *  - data overrides, which replace or patch what the disc delivers, so that
 *    a mod of card statistics or artwork needs no code at all.
 *
 * What a mod may reach is the host table in modapi.h. There is no network
 * call in it and no way to name a file outside the mod's own directory and
 * its private data directory: Paths_Contained refuses absolute paths, "..",
 * and drive letters. A mod's code is still native code in this process,
 * which is what makes a mod like 3D Monsters possible at all, so the window
 * shows where each mod came from and the notes say plainly that installing
 * one is trusting it. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE   /* MAP_ANONYMOUS, MAP_FIXED_NOREPLACE */
#include "mods.h"
#include "modapi.h"
#include "exports.h"
#include "object_loader.h"
#include "json.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include "pc/platform/platform.h"
#include "pc/debug/log.h"
#include "pc/debug/symbols.h"
#include "pc/sdk/disc.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "pc/compat/mman.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define ID_MAX 64
#define NAME_MAX_ 96
#define PATH_MAX_ 1024
#define STATUS_MAX 160
#define SECTOR 2048
#define REGIONS_MAX 64
#define PATCHES_MAX 1024

#define OBJECT_MAX (64u << 20)   /* object_loader.c's own limit */

typedef struct {
    char id[ID_MAX];
    char name[NAME_MAX_];
    char directory[PATH_MAX_];
    char library[NAME_MAX_];     /* empty when the mod is data only */
    char status[STATUS_MAX];
    const char *origin;          /* "shipped" or "installed", for the window */
    int restart;                 /* the manifest asks for a fresh process */
    int default_enabled;
    int enabled;                 /* the player's choice, from the settings */
    int active;                  /* and whether it is in place right now */
    int broken;                  /* it failed to load; it cannot be applied */
    LoadedObject object;         /* the mod's code, once loaded */
    MemoriesMod hooks;
    MemoriesModHost host;
    JsonDocument *manifest;
    const JsonValue *data;       /* the "data" array, applied when enabled */
    const JsonValue *cards;      /* the "cards" array: cards the mod adds (src/pc/cards) */
    char textures[PATH_MAX_];    /* a texture pack directory inside the mod, or empty */
} Mod;

/* One stretch of the disc a mod replaces, and one run of patched bytes
 * inside a sector. Both are read from interrupt context (Mods_DiscSector),
 * so they are built before they are published and nothing frees them while
 * `overrides_live` is set. */
typedef struct {
    int mod, lba, sectors;
    unsigned char *image;    /* the replacement file, mapped read-only */
    size_t image_size, mapped;
} Region;

typedef struct {
    int mod, lba, offset, length;
    unsigned char *bytes;
} Patch;

static Mod mods[MODS_MAX];
static int mod_count;
static int scanned;

static Region regions[REGIONS_MAX];
static int region_count;
static Patch patches[PATCHES_MAX];
static int patch_count;
static volatile int overrides_live;
static int override_low, override_high;

static void say(const char *format, ...)
{
    char message[512];
    va_list arguments;
    if (!Log_Enabled(LOG_MODS)) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LOG(LOG_MODS, "%s", message);
}

/* A manifest's id, name and file names are cut to fit their fields. */
static void copy_text(char *out, size_t size, const char *text)
{
    size_t length = strlen(text);
    if (length >= size) length = size - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

static void note(Mod *mod, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(mod->status, sizeof(mod->status), format, arguments);
    va_end(arguments);
    fprintf(stderr, "memories-pc: mod %s: %s\n", mod->id, mod->status);
}

/* --- settings -------------------------------------------------------- */

/* Both names return 0 when they do not fit: a cut name would be another
 * setting, so the caller treats it as unset. A mod id always fits (ID_MAX);
 * a key a mod passes may not. */
static int setting_key(char *out, size_t size, const char *id, const char *key)
{
    int length = key ? snprintf(out, size, "mod.%s.%s", id, key) : snprintf(out, size, "mod.%s", id);
    return length >= 0 && (size_t)length < size;
}

/* MEMORIES_MOD_<ID>, or MEMORIES_MOD_<ID>_<KEY> for one of its settings,
 * uppercased, with everything that is not a letter or a digit turned into an
 * underscore: MEMORIES_MOD_3D_MONSTERS=0 for a test run. */
static int environment_name(char *name, size_t size, const char *id, const char *key)
{
    size_t i;
    int length = snprintf(name, size, "MEMORIES_MOD_%s%s%s", id, key ? "_" : "", key ? key : "");
    if (length < 0 || (size_t)length >= size) return 0;
    for (i = strlen("MEMORIES_MOD_"); name[i]; i++) {
        name[i] = isalnum((unsigned char)name[i]) ? (char)toupper((unsigned char)name[i]) : '_';
    }
    return 1;
}

static int environment_choice(const char *id, int fallback)
{
    char name[ID_MAX + 16];
    const char *text;
    if (!environment_name(name, sizeof(name), id, NULL)) return fallback;
    text = getenv(name);
    return text && *text ? atoi(text) != 0 : fallback;
}

/* --- the host table -------------------------------------------------- */

static Mod *owner(const MemoriesModHost *host)
{
    return host ? (Mod *)host->reserved : NULL;
}

static int host_applied(const MemoriesModHost *host)
{
    Mod *mod = owner(host);
    return mod && mod->enabled;
}

static void host_log(const MemoriesModHost *host, const char *format, ...)
{
    char message[512];
    va_list arguments;
    Mod *mod = owner(host);
    if (!Log_Enabled(LOG_MODS)) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LOG(LOG_MODS, "%s: %s", mod ? mod->id : "mod", message);
}

static int host_log_enabled(const MemoriesModHost *host)
{
    (void)host;
    return Log_Enabled(LOG_MODS);
}

static FILE *host_open_asset(const MemoriesModHost *host, const char *relative)
{
    Mod *mod = owner(host);
    char path[PATH_MAX_];
    if (!mod || !Paths_Contained(relative)) return NULL;
    if (snprintf(path, sizeof(path), "%s/%s", mod->directory, relative) >= (int)sizeof(path)) return NULL;
    return fopen(path, "rb");
}

static FILE *host_open_data(const MemoriesModHost *host, const char *relative, const char *mode)
{
    Mod *mod = owner(host);
    char relative_path[PATH_MAX_], path[PATH_MAX_];
    if (!mod || !Paths_Contained(relative) || !mode) return NULL;
    if (snprintf(relative_path, sizeof(relative_path), "mod-data/%s/%s", mod->id, relative) >= (int)sizeof(relative_path)) {
        return NULL;
    }
    if (Paths_User(path, sizeof(path), relative_path)) return NULL;
    return fopen(path, mode);
}

static int host_setting(const MemoriesModHost *host, const char *key, int fallback)
{
    Mod *mod = owner(host);
    char name[ID_MAX + 96];
    const char *text;
    if (!mod || !key || !*key) return fallback;
    if (!environment_name(name, sizeof(name), mod->id, key)) return fallback;
    text = getenv(name);
    if (text && *text) return (int)strtol(text, NULL, 0);
    if (!setting_key(name, sizeof(name), mod->id, key)) return fallback;
    return Settings_GetNamed(name, fallback);
}

static void host_set_setting(const MemoriesModHost *host, const char *key, int value)
{
    Mod *mod = owner(host);
    char name[ID_MAX + 96];
    if (!mod || !key || !*key || !setting_key(name, sizeof(name), mod->id, key)) return;
    Settings_SetNamed(name, value);
}

static int host_disc_file_start(const MemoriesModHost *host, const char *iso_path)
{
    (void)host;
    return iso_path ? Memories_DiscFileStart(iso_path) : -1;
}

static int host_disc_read(const MemoriesModHost *host, int lba, int sectors, void *out)
{
    (void)host;
    return out && sectors > 0 ? Memories_DiscReadSectors(lba, sectors, out) : 0;
}

static unsigned short host_pad(const MemoriesModHost *host, int port)
{
    (void)host;
    return Platform_Pad(port);
}

static uint64_t host_now_us(const MemoriesModHost *host)
{
    struct timespec now;
    (void)host;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static void *host_map_fixed(const MemoriesModHost *host, uintptr_t address, size_t size)
{
    void *wanted = (void *)address, *got;
    if (!owner(host) || !address || address % 0x10000u || !size || address + size < address) return NULL;
#ifdef _WIN32
    /* VirtualAlloc refuses an address that is already reserved. */
    got = VirtualAlloc(wanted, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return got == wanted ? got : NULL;
#else
    got = mmap(wanted, size, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (got == MAP_FAILED) return NULL;
    if (got != wanted) {   /* a kernel older than MAP_FIXED_NOREPLACE takes it as a hint */
        munmap(got, size);
        return NULL;
    }
    return got;
#endif
}

static void fill_host(Mod *mod)
{
    mod->host.api = MEMORIES_MOD_API;
    mod->host.id = mod->id;
    mod->host.directory = mod->directory;
    mod->host.reserved = mod;
    mod->host.applied = host_applied;
    mod->host.log = host_log;
    mod->host.log_enabled = host_log_enabled;
    mod->host.open_asset = host_open_asset;
    mod->host.open_data = host_open_data;
    mod->host.setting = host_setting;
    mod->host.set_setting = host_set_setting;
    mod->host.disc_file_start = host_disc_file_start;
    mod->host.disc_read = host_disc_read;
    mod->host.pad = host_pad;
    mod->host.now_us = host_now_us;
    mod->host.map_fixed = host_map_fixed;
}

/* --- data overrides -------------------------------------------------- */

static void publish_overrides(void)
{
    int i, low = 0x7fffffff, high = -1;
    for (i = 0; i < region_count; i++) {
        if (regions[i].lba < low) low = regions[i].lba;
        if (regions[i].lba + regions[i].sectors - 1 > high) high = regions[i].lba + regions[i].sectors - 1;
    }
    for (i = 0; i < patch_count; i++) {
        if (patches[i].lba < low) low = patches[i].lba;
        if (patches[i].lba > high) high = patches[i].lba;
    }
    override_low = low;
    override_high = high;
    /* The tables are written before the interrupt is told to read them. */
    __asm__ volatile("" ::: "memory");
    overrides_live = high >= low;
}

/* A replacement file, read-only, for as long as the mod is applied. Linux
 * maps it; Windows has no mmap of a file the port could declare by hand
 * without <windows.h>, and a replacement is at most a few MiB, so it is read
 * into memory. */
static void *map_file(int file, size_t size)
{
#ifdef _WIN32
    unsigned char *image = malloc(size);
    size_t got = 0;
    if (!image) return NULL;
    while (got < size) {
        int chunk = _read(file, image + got, (unsigned)(size - got > 0x10000000u ? 0x10000000u : size - got));
        if (chunk <= 0) { free(image); return NULL; }
        got += (size_t)chunk;
    }
    return image;
#else
    void *image = mmap(NULL, size, PROT_READ, MAP_PRIVATE, file, 0);
    return image == MAP_FAILED ? NULL : image;
#endif
}

static void unmap_file(void *image, size_t size)
{
#ifdef _WIN32
    (void)size;
    free(image);
#else
    munmap(image, size);
#endif
}

static void drop_overrides(int mod)
{
    int i, kept = 0;
    overrides_live = 0;   /* the drive model stops looking before anything goes */
    for (i = 0; i < region_count; i++) {
        if (regions[i].mod != mod) { regions[kept++] = regions[i]; continue; }
        if (regions[i].image) unmap_file(regions[i].image, regions[i].mapped);
    }
    region_count = kept;
    for (i = 0, kept = 0; i < patch_count; i++) {
        if (patches[i].mod != mod) { patches[kept++] = patches[i]; continue; }
        free(patches[i].bytes);
    }
    patch_count = kept;
    publish_overrides();
}

/* "26 25" or "2625": the bytes a tutorial writes out. Returns how many were
 * read, or -1 if the text is not pairs of hexadecimal digits. */
static int read_bytes(const char *text, unsigned char **out)
{
    size_t digits = 0, i;
    unsigned char *bytes;
    int value = 0, half = 0;
    for (i = 0; text[i]; i++) {
        if (isxdigit((unsigned char)text[i])) digits++;
        else if (!isspace((unsigned char)text[i]) && text[i] != ',') return -1;
    }
    if (!digits || digits % 2) return -1;
    bytes = malloc(digits / 2);
    if (!bytes) return -1;
    for (i = 0, digits = 0; text[i]; i++) {
        int digit;
        if (!isxdigit((unsigned char)text[i])) continue;
        digit = isdigit((unsigned char)text[i]) ? text[i] - '0' : tolower((unsigned char)text[i]) - 'a' + 10;
        value = value * 16 + digit;
        if (half) { bytes[digits++] = (unsigned char)value; value = 0; }
        half = !half;
    }
    *out = bytes;
    return (int)digits;
}

/* One patch, split at the sector boundaries it crosses. */
static int add_patch(Mod *mod, int index, int lba, int offset, const unsigned char *bytes, int length)
{
    while (length > 0) {
        int here = SECTOR - offset;
        if (here > length) here = length;
        if (patch_count >= PATCHES_MAX) {
            note(mod, "more than %d patched byte runs", PATCHES_MAX);
            return 0;
        }
        patches[patch_count].bytes = malloc((size_t)here);
        if (!patches[patch_count].bytes) return 0;
        memcpy(patches[patch_count].bytes, bytes, (size_t)here);
        patches[patch_count].mod = index;
        patches[patch_count].lba = lba;
        patches[patch_count].offset = offset;
        patches[patch_count].length = here;
        patch_count++;
        bytes += here;
        length -= here;
        offset = 0;
        lba++;
    }
    return 1;
}

static int add_region(Mod *mod, int index, int lba, int sectors, const char *replacement)
{
    char path[PATH_MAX_];
    struct stat info;
    int file;
    void *image;
    if (region_count >= REGIONS_MAX) {
        note(mod, "more than %d replaced files", REGIONS_MAX);
        return 0;
    }
    if (!Paths_Contained(replacement)) {
        note(mod, "\"replace\": %s is outside the mod", replacement);
        return 0;
    }
    if (snprintf(path, sizeof(path), "%s/%s", mod->directory, replacement) >= (int)sizeof(path)) return 0;
#ifdef _WIN32
    file = open(path, O_RDONLY | O_BINARY);
#else
    file = open(path, O_RDONLY);
#endif
    if (file < 0 || fstat(file, &info) || info.st_size <= 0) {
        if (file >= 0) close(file);
        note(mod, "cannot read %s", replacement);
        return 0;
    }
    /* The game reaches a file through sector numbers it worked out from the
     * disc's own tables, so a replacement may fill the original's sectors
     * and no more: the sectors past them belong to the next file. */
    if ((size_t)info.st_size > (size_t)sectors * SECTOR) {
        say("%s: %s is larger than the file it replaces; the tail is ignored", mod->id, replacement);
    }
    image = map_file(file, (size_t)info.st_size);
    close(file);
    if (!image) {
        note(mod, "cannot map %s", replacement);
        return 0;
    }
    regions[region_count].mod = index;
    regions[region_count].lba = lba;
    regions[region_count].sectors = sectors;
    regions[region_count].image = image;
    regions[region_count].image_size = (size_t)info.st_size;
    regions[region_count].mapped = (size_t)info.st_size;
    region_count++;
    return 1;
}

/* The "data" array of a manifest, once the mod is applied. */
static void apply_overrides(Mod *mod, int index)
{
    int i, count = Json_Count(mod->data);
    for (i = 0; i < count; i++) {
        const JsonValue *entry = Json_At(mod->data, i);
        const char *file = Json_String(Json_Member(entry, "file"), NULL);
        const JsonValue *replace = Json_Member(entry, "replace");
        const JsonValue *patch = Json_Member(entry, "patch");
        int lba = (int)Json_Number(Json_Member(entry, "lba"), -1);
        unsigned size = 0;
        if (file) {
            if (Memories_DiscFileInfo(file, &lba, &size) || lba < 0) {
                note(mod, "%s is not on the disc", file);
                continue;
            }
        } else if (lba < 0) {
            note(mod, "a data entry names neither \"file\" nor \"lba\"");
            continue;
        }
        if (Json_String(replace, NULL)) {
            int sectors = size ? (int)((size + SECTOR - 1) / SECTOR) : (int)Json_Number(Json_Member(entry, "sectors"), 0);
            if (sectors <= 0) {
                note(mod, "\"replace\" at sector %d needs a \"sectors\" count", lba);
            } else {
                add_region(mod, index, lba, sectors, Json_String(replace, NULL));
            }
        }
        if (patch) {
            int p, runs = Json_Count(patch);
            for (p = 0; p < runs; p++) {
                const JsonValue *run = Json_At(patch, p);
                long at = Json_Number(Json_Member(run, "at"), -1);
                const char *text = Json_String(Json_Member(run, "bytes"), NULL);
                unsigned char *bytes;
                int length;
                if (at < 0 || !text) {
                    note(mod, "a patch needs \"at\" and \"bytes\"");
                    continue;
                }
                length = read_bytes(text, &bytes);
                if (length < 0) {
                    note(mod, "\"bytes\": %s is not hexadecimal", text);
                    continue;
                }
                if (file && size && at + length > (long)size) {
                    note(mod, "a patch at 0x%lX reaches past %s", at, file);
                    free(bytes);
                    continue;
                }
                add_patch(mod, index, lba + (int)(at / SECTOR), (int)(at % SECTOR), bytes, length);
                free(bytes);
            }
        }
    }
    publish_overrides();
    if (region_count || patch_count) {
        say("%s: %d replaced regions, %d patched runs, sectors %d to %d",
            mod->id, region_count, patch_count, override_low, override_high);
    }
}

int Mods_DiscSector(int lba, void *user_data)
{
    unsigned char *out = user_data;
    int changed = 0, i;
    if (!overrides_live) return 0;
    __asm__ volatile("" ::: "memory");
    if (lba < override_low || lba > override_high) return 0;
    for (i = 0; i < region_count; i++) {
        const Region *region = &regions[i];
        size_t at, have;
        if (lba < region->lba || lba >= region->lba + region->sectors) continue;
        at = (size_t)(lba - region->lba) * SECTOR;
        have = at < region->image_size ? region->image_size - at : 0;
        if (have > SECTOR) have = SECTOR;
        if (have) memcpy(out, region->image + at, have);
        if (have < SECTOR) memset(out + have, 0, SECTOR - have);
        changed = 1;
    }
    for (i = 0; i < patch_count; i++) {
        if (patches[i].lba != lba) continue;
        memcpy(out + patches[i].offset, patches[i].bytes, (size_t)patches[i].length);
        changed = 1;
    }
    return changed;
}

/* --- the names a code mod binds to ----------------------------------- */

static int by_name(const void *key, const void *entry)
{
    return strcmp(key, ((const MemoriesModExport *)entry)->name);
}

void *Mods_Lookup(const char *name)
{
    const MemoriesModExport *found;
    union { void (*function)(void); void *pointer; } libc;
    if (!name) return NULL;
    libc.function = Mods_LibcLookup(name);
    if (libc.function) return libc.pointer;
    found = bsearch(name, Memories_ModExports, Memories_ModExportCount, sizeof(*Memories_ModExports), by_name);
    return found ? found->address : NULL;
}

int Mods_PrintExports(void)
{
    unsigned i;
    for (i = 0; i < Memories_ModExportCount; i++) {
        printf("%08lx %s\n", (unsigned long)(size_t)Memories_ModExports[i].address, Memories_ModExports[i].name);
    }
    return fflush(stdout) ? 1 : 0;
}

/* --- loading --------------------------------------------------------- */

static void *resolve(const char *name, void *context)
{
    (void)context;
    return Mods_Lookup(name);
}

/* The mod's functions, for crash and hang reports: "3d-monsters:draw_frame". */
static void register_symbols(Mod *mod)
{
    SymbolsEntry *entries = calloc(mod->object.symbol_count ? mod->object.symbol_count : 1, sizeof(*entries));
    char (*names)[72] = calloc(mod->object.symbol_count ? mod->object.symbol_count : 1, sizeof(*names));
    size_t i, count = 0;
    if (entries && names) {
        for (i = 0; i < mod->object.symbol_count; i++) {
            const struct ObjectSymbol *symbol = &mod->object.symbols[i];
            if (!symbol->function) continue;
            snprintf(names[count], sizeof(names[count]), "%s:%s", mod->id, symbol->name);
            entries[count].address = symbol->address;
            entries[count].size = symbol->size;
            entries[count].name = names[count];
            count++;
        }
        Symbols_Add(entries, count);
    }
    free(entries);
    free(names);
}

/* The mod's object file, read whole and handed to the loader. */
static void *load_object(Mod *mod, const char *path)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data = NULL;
    long size;
    char error[STATUS_MAX];
    void *entry;
    if (!file) {
        note(mod, "cannot read %s", mod->library);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || (unsigned long)size > OBJECT_MAX ||
        fseek(file, 0, SEEK_SET) || !(data = malloc(size ? (size_t)size : 1)) ||
        fread(data, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(data);
        note(mod, "cannot read %s", mod->library);
        return NULL;
    }
    fclose(file);
    if (ObjectLoader_Load(data, (size_t)size, resolve, mod, &mod->object, error, sizeof(error))) {
        free(data);
        note(mod, "%s %s", mod->library, error);
        return NULL;
    }
    free(data);
    entry = ObjectLoader_Symbol(&mod->object, "MemoriesModInit");
    if (!entry) {
        note(mod, "%s has no MemoriesModInit", mod->library);
        return NULL;
    }
    register_symbols(mod);
    return entry;
}

static int load_library(Mod *mod)
{
    char path[PATH_MAX_];
    MemoriesModEntry entry;
    union { void *pointer; int (*function)(const MemoriesModHost *, MemoriesMod *); } symbol;
    if (!mod->library[0]) return 1;   /* data only: nothing to load */
    if (mod->object.image) return 1;
    if (snprintf(path, sizeof(path), "%s/%s", mod->directory, mod->library) >= (int)sizeof(path)) return 0;
    symbol.pointer = load_object(mod, path);
    if (!symbol.pointer) return 0;
    entry = symbol.function;
    fill_host(mod);
    memset(&mod->hooks, 0, sizeof(mod->hooks));
    if (!entry(&mod->host, &mod->hooks)) {
        note(mod, "refused to start");
        return 0;
    }
    if (mod->hooks.api > MEMORIES_MOD_API) {
        note(mod, "was built for mod API %u; this game has %u", mod->hooks.api, MEMORIES_MOD_API);
        memset(&mod->hooks, 0, sizeof(mod->hooks));
        return 0;
    }
    if (mod->hooks.name && *mod->hooks.name && !mod->name[0]) {
        copy_text(mod->name, sizeof(mod->name), mod->hooks.name);
    }
    say("%s: loaded %s", mod->id, mod->library);
    return 1;
}

/* mod.json, read into the record. Returns 0 when it is not a mod at all. */
static int read_manifest(Mod *mod, const char *directory, const char *origin)
{
    char path[PATH_MAX_], error[128];
    const JsonValue *root;
    const char *text;
    if (snprintf(path, sizeof(path), "%s/mod.json", directory) >= (int)sizeof(path)) return 0;
    if (access(path, R_OK)) return 0;
    memset(mod, 0, sizeof(*mod));
    copy_text(mod->directory, sizeof(mod->directory), directory);
    mod->origin = origin;
    mod->manifest = Json_ParseFile(path, error, sizeof(error));
    root = Json_Root(mod->manifest);
    if (!root || Json_TypeOf(root) != JSON_OBJECT) {
        const char *slash = strrchr(directory, '/');
        copy_text(mod->id, sizeof(mod->id), slash ? slash + 1 : directory);
        copy_text(mod->name, sizeof(mod->name), mod->id);
        mod->broken = 1;
        note(mod, "mod.json %s", root ? "is not an object" : error);
        return 1;
    }
    text = Json_String(Json_Member(root, "id"), NULL);
    if (!text || !*text) {
        const char *slash = strrchr(directory, '/');
        text = slash ? slash + 1 : directory;
    }
    copy_text(mod->id, sizeof(mod->id), text);
    copy_text(mod->name, sizeof(mod->name), Json_String(Json_Member(root, "name"), mod->id));
    text = Json_String(Json_Member(root, "library"), NULL);
    if (text && *text) {
        if (!Paths_Contained(text)) {
            mod->broken = 1;
            note(mod, "\"library\": %s is outside the mod", text);
        } else if (strchr(text, '.')) {
            copy_text(mod->library, sizeof(mod->library), text);
        } else {
            snprintf(mod->library, sizeof(mod->library), "%s.o", text);   /* one object for every system */
        }
    }
    mod->restart = Json_Bool(Json_Member(root, "restart"), 0);
    mod->default_enabled = Json_Bool(Json_Member(root, "enabled"), 0);
    text = Json_String(Json_Member(root, "textures"), NULL);
    if (text && *text) {
        if (!Paths_Contained(text)) {
            mod->broken = 1;
            note(mod, "\"textures\": %s is outside the mod", text);
        } else if (snprintf(mod->textures, sizeof(mod->textures), "%s/%s", directory, text) >= (int)sizeof(mod->textures)) {
            mod->broken = 1;
            note(mod, "\"textures\": %s is too long", text);
        }
    }
    mod->data = Json_Member(root, "data");
    if (mod->data && Json_TypeOf(mod->data) != JSON_ARRAY) {
        mod->broken = 1;
        note(mod, "\"data\" is not an array");
    }
    mod->cards = Json_Member(root, "cards");
    if (mod->cards && Json_TypeOf(mod->cards) != JSON_ARRAY) {
        mod->broken = 1;
        note(mod, "\"cards\" is not an array");
    }
    /* Data overrides change what the game loaded on its way up, so they are
     * only whole while the game starts with them in place; the cards a mod
     * adds are counted once, when the game starts. */
    if (Json_Count(mod->data) || Json_Count(mod->cards)) mod->restart = Json_Bool(Json_Member(root, "restart"), 1);
    {   /* The key this mod's choice was stored under before it was a mod. */
        const char *legacy = Json_String(Json_Member(root, "legacy_setting"), NULL);
        char key[ID_MAX + 96];
        int fallback = mod->default_enabled;
        if (legacy && *legacy) fallback = Settings_GetNamed(legacy, fallback);
        if (setting_key(key, sizeof(key), mod->id, NULL)) fallback = Settings_GetNamed(key, fallback);
        mod->enabled = environment_choice(mod->id, fallback) != 0;
    }
    return 1;
}

static int by_id(const char *id)
{
    int i;
    for (i = 0; i < mod_count; i++) {
        if (!strcmp(mods[i].id, id)) return i;
    }
    return -1;
}

static void scan(const char *root, const char *origin)
{
    char path[PATH_MAX_];
    char **names = NULL;
    int count = 0, room = 0, i;
    DIR *directory = opendir(root);
    struct dirent *entry;
    if (!directory) return;
    /* Read the names first and sort them: readdir's order is the file
     * system's, and the window should not shuffle between launches. */
    while ((entry = readdir(directory))) {
        if (entry->d_name[0] == '.') continue;
        if (count == room) {
            char **grown = realloc(names, (size_t)(room = room ? room * 2 : 16) * sizeof(*names));
            if (!grown) break;
            names = grown;
        }
        names[count] = strdup(entry->d_name);
        if (!names[count]) break;
        count++;
    }
    closedir(directory);
    for (i = 1; i < count; i++) {   /* an insertion sort: a handful of names */
        char *name = names[i];
        int at = i;
        while (at > 0 && strcmp(names[at - 1], name) > 0) { names[at] = names[at - 1]; at--; }
        names[at] = name;
    }
    for (i = 0; i < count; i++) {
        Mod candidate;
        int existing;
        if (snprintf(path, sizeof(path), "%s/%s", root, names[i]) >= (int)sizeof(path)) continue;
        if (!read_manifest(&candidate, path, origin)) continue;
        existing = by_id(candidate.id);
        if (existing >= 0) {
            /* The player's copy wins over the one the release ships. */
            say("%s in %s replaces the one in %s", candidate.id, path, mods[existing].directory);
            Json_Free(mods[existing].manifest);
            candidate.enabled = mods[existing].enabled;
            mods[existing] = candidate;
        } else if (mod_count < MODS_MAX) {
            mods[mod_count++] = candidate;
        } else {
            Json_Free(candidate.manifest);
            fprintf(stderr, "memories-pc: more than %d mods; %s was skipped\n", MODS_MAX, candidate.id);
        }
    }
    for (i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* Turn a mod on or off for real: its library, its overrides, its hook. */
static int (*texture_pack_load)(const char *directory);
static void (*texture_pack_unload)(void);

void Mods_SetTexturePack(int (*load)(const char *directory), void (*unload)(void))
{
    texture_pack_load = load;
    texture_pack_unload = unload;
}

static void activate(int index, int on)
{
    Mod *mod = &mods[index];
    if (on == mod->active) return;
    if (on) {
        /* A mod that cannot load keeps the player's choice and its reason:
         * the window shows both, and removing it still works. */
        if (mod->broken || !load_library(mod)) {
            mod->broken = 1;
            return;
        }
        apply_overrides(mod, index);
        if (mod->textures[0]) {
            if (!texture_pack_load) note(mod, "this build has no texture packs");
            else if (!texture_pack_load(mod->textures)) note(mod, "texture pack %s did not load", mod->textures);
        }
        mod->active = 1;
        if (mod->hooks.applied) mod->hooks.applied(1);
    } else {
        drop_overrides(index);
        if (mod->textures[0] && texture_pack_unload) {
            /* The packs add up: the others' come back without this one's. */
            int other;
            texture_pack_unload();
            for (other = 0; other < mod_count; other++) {
                if (other != index && mods[other].active && mods[other].textures[0] && texture_pack_load) {
                    texture_pack_load(mods[other].textures);
                }
            }
        }
        mod->active = 0;
        if (mod->hooks.applied) mod->hooks.applied(0);
    }
}

void Mods_Load(void)
{
    const char *all = getenv("MEMORIES_MODS");
    char path[PATH_MAX_];
    int i;
    if (!scanned) {
        const char *named = getenv("MEMORIES_MODS_DIR");
        scanned = 1;
        if (named && *named) {
            scan(named, "installed");
        } else {
            if (!Paths_Program(path, sizeof(path), "mods")) scan(path, "shipped");
            if (!Paths_User(path, sizeof(path), "mods")) scan(path, "installed");
        }
        say("%d mods found", mod_count);
    }
    for (i = 0; i < mod_count; i++) {
        /* The settings are the choice, here and after a settings reload.
         * A mod that wants a restart is still put in place at startup: it
         * is only a live change it cannot take. */
        char key[ID_MAX + 96];
        int want;
        want = mods[i].enabled;
        if (setting_key(key, sizeof(key), mods[i].id, NULL)) want = Settings_GetNamed(key, want);
        want = environment_choice(mods[i].id, want) != 0;
        if (all && (!strcmp(all, "0") || !strcmp(all, "off"))) want = 0;
        mods[i].enabled = want;
        activate(i, want);
    }
}

void Mods_Shutdown(void)
{
    int i;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].hooks.shutdown) mods[i].hooks.shutdown();
    }
}

int Mods_Count(void) { return mod_count; }

static Mod *at(int index)
{
    return index >= 0 && index < mod_count ? &mods[index] : NULL;
}

const char *Mods_Id(int mod) { return at(mod) ? mods[mod].id : ""; }
const char *Mods_Name(int mod) { return at(mod) ? mods[mod].name : ""; }
const char *Mods_Status(int mod) { return at(mod) ? mods[mod].status : ""; }
int Mods_Enabled(int mod) { return at(mod) ? mods[mod].enabled : 0; }
int Mods_RequiresRestart(int mod) { return at(mod) ? mods[mod].restart : 0; }

void Mods_SetEnabled(int mod, int enabled)
{
    char key[ID_MAX + 96];
    if (!at(mod)) return;
    enabled = enabled != 0;
    if (setting_key(key, sizeof(key), mods[mod].id, NULL)) Settings_SetNamed(key, enabled);
    if (mods[mod].enabled == enabled) return;
    mods[mod].enabled = enabled;
    /* A mod that asks for a restart is only recorded here; the next launch
     * is what puts it in place (the mods window offers the restart). */
    if (!mods[mod].restart) activate(mod, enabled);
}

void Mods_VisitCards(void (*visit)(const char *id, const struct JsonValue *cards, void *context), void *context)
{
    int i;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].active && Json_Count(mods[i].cards)) visit(mods[i].id, mods[i].cards, context);
    }
}

int Mods_Setting(const char *id, const char *key, int fallback)
{
    char name[ID_MAX + 96];
    const char *text;
    if (by_id(id) < 0 || !key || !*key || !environment_name(name, sizeof(name), id, key)) return fallback;
    text = getenv(name);
    if (text && *text) return (int)strtol(text, NULL, 0);
    if (!setting_key(name, sizeof(name), id, key)) return fallback;
    return Settings_GetNamed(name, fallback);
}

void Mods_Note(const char *id, const char *format, ...)
{
    va_list arguments;
    int i = by_id(id);
    if (i < 0) return;
    va_start(arguments, format);
    vsnprintf(mods[i].status, sizeof(mods[i].status), format, arguments);
    va_end(arguments);
    fprintf(stderr, "memories-pc: mod %s: %s\n", mods[i].id, mods[i].status);
}

void Mods_DrawFrame(void)
{
    int i;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].enabled && mods[i].hooks.frame) mods[i].hooks.frame();
    }
}

void Mods_Reset(void)
{
    int i;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].hooks.reset) mods[i].hooks.reset();
    }
}
