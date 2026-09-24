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
#include "pc/compat/fs.h"
#include "mods.h"
#include "modapi.h"
#include "exports.h"
#include "object_loader.h"
#include "json.h"
#include "events.h"
#include "hooks.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include "pc/platform/platform.h"
#include "pc/platform/menu.h"
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
#include <limits.h>
#include "pc/compat/mman.h"
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define ID_MAX 64
#define NAME_MAX_ 96
#define PATH_MAX_ 1024
#define STATUS_MAX 512
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
    char warnings[STATUS_MAX];   /* what the manifest got wrong: shown again whenever status is reset */
    const char *origin;          /* "shipped" or "installed", for the window */
    int restart;                 /* the manifest asks for a fresh process */
    int default_enabled;
    int enabled;                 /* the player's choice, from the settings */
    int active;                  /* and whether it is in place right now */
    int failed;
    int initialized;
    int data_prepared;
    int broken;                  /* it failed to load; it cannot be applied */
    int *runtime_options;
    unsigned sequence;
    unsigned code_hash;
    LoadedObject object;         /* the mod's code, once loaded */
    MemoriesMod hooks;
    MemoriesModHost host;
    JsonDocument *manifest;
    const JsonValue *data;       /* the "data" array, applied when enabled */
    const JsonValue *cards;      /* the "cards" array: cards the mod adds (src/pc/cards) */
    char textures[PATH_MAX_];    /* a texture pack directory inside the mod, or empty */
    const JsonValue *audio;      /* the "audio" object: replacement sounds (src/pc/audio/replace.h) */
} Mod;

/* One stretch of the disc a mod replaces, and one run of patched bytes
 * inside a sector. Both are read from interrupt context (Mods_DiscSector),
 * so they are built before they are published and nothing frees them while
 * `overrides_live` is set. */
typedef struct {
    int mod, lba, sectors, file;
    unsigned char *image;    /* the replacement file, mapped read-only */
    size_t image_size, mapped;
    unsigned hash;
} Region;

typedef struct {
    int mod, lba, offset, length, file;
    unsigned char *bytes;
} Patch;

/* File identities remain retail LBAs; only readers see the virtual positions.
 * A patch's lba is file-relative when file >= 0, physical otherwise. */
typedef struct {
    int retail_lba, retail_sectors, lba, sectors, reserved;
    unsigned retail_size, size;
} DiscFile;
static DiscFile disc_files[REGIONS_MAX];
static int disc_file_count, disc_layout_frozen, preparing_data;

static Mod mods[MODS_MAX];
static int mod_count;
static unsigned activation_sequence;
static int scanned;
static int shut_down;   /* Mods_Shutdown has run: no mod code is called again */

static Region regions[REGIONS_MAX];
static int region_count;
static Patch patches[PATCHES_MAX];
static int patch_count;
static int published_regions, published_patches;
/* The mods applied at startup, in the order they were loaded. */
static int loaded[MODS_MAX], loaded_count;
static volatile int overrides_live;
static int override_low, override_high;
static void activate(int index, int on);

static unsigned hash_bytes(unsigned hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static void say(const char *format, ...)
{
    char message[512];
    va_list arguments;
    if (!Log_Wanted(LOG_MODS)) return;
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

/* Something the player should see that does not stop the mod: added to its
 * status after whatever is there, so several warnings show together. With
 * `lasting` it is also kept in `warnings`, which status goes back to when
 * the mod is applied again (a manifest's problems do not go away). */
static void warn(Mod *mod, int lasting, const char *format, ...)
{
    char message[STATUS_MAX];
    va_list arguments;
    size_t length;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    fprintf(stderr, "memories-pc: mod %s: warning: %s\n", mod->id, message);
    length = strlen(mod->status);
    snprintf(mod->status + length, sizeof(mod->status) - length, "%s%s", length ? "; " : "", message);
    if (lasting) {
        length = strlen(mod->warnings);
        snprintf(mod->warnings + length, sizeof(mod->warnings) - length, "%s%s", length ? "; " : "", message);
    }
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
    if (!Log_Enabled(LOG_MODS)) return; /* a mod may log every frame: traced only */
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
    char name[256];
    const char *text;
    if (!mod || !key || !*key) return fallback;
    text = environment_name(name, sizeof(name), mod->id, key) ? getenv(name) : NULL;
    if (text && *text) return (int)strtol(text, NULL, 0);
    if (!setting_key(name, sizeof(name), mod->id, key)) return fallback;
    return Settings_GetNamed(name, fallback);
}

static void host_set_setting(const MemoriesModHost *host, const char *key, int value)
{
    Mod *mod = owner(host);
    char name[256];
    /* The same keys a declared setting may have: a mod cannot write its own
     * load order, or a key the manager would refuse to show. */
    if (!mod || !Mods_SettingKeyValid(key) || !setting_key(name, sizeof(name), mod->id, key)) return;
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

static int (*card_resolver)(const char *);
static unsigned card_signature;
void Mods_SetCardSignature(unsigned signature) { card_signature = signature; }
unsigned Mods_CardSignature(void) { return card_signature; }
void Mods_SetCardResolver(int (*resolve_card)(const char *)) { card_resolver = resolve_card; }
static int host_card_id(const MemoriesModHost *host, const char *identity)
{ (void)host; return card_resolver ? card_resolver(identity) : 0; }
static int host_subscribe(const MemoriesModHost *host, unsigned event, int priority, MemoriesModCallback callback)
{ return owner(host) ? Mods_Subscribe((int)(owner(host) - mods), event, priority, callback) : 0; }
static void host_unsubscribe(const MemoriesModHost *host, int token)
{ if (owner(host)) Mods_Unsubscribe((int)(owner(host) - mods), token); }
static int host_register_state(const MemoriesModHost *host, void *data, size_t size, unsigned version)
{ return owner(host) ? Mods_RegisterState((int)(owner(host) - mods), data, size, version) : 0; }

static int host_hook(const MemoriesModHost *host, void *function, void *replacement, void **original)
{ return owner(host) ? Hooks_Add((int)(owner(host) - mods), function, replacement, original) : 0; }
static void host_unhook(const MemoriesModHost *host, int token)
{ if (owner(host)) Hooks_Remove((int)(owner(host) - mods), token); }
static void *host_symbol(const MemoriesModHost *host, const char *name)
{ return owner(host) && name ? Mods_Lookup(name) : NULL; }
static int host_provide(const MemoriesModHost *host, const char *name, void *pointer)
{ return owner(host) ? Mods_Provide((int)(owner(host) - mods), name, pointer) : 0; }
static void *host_find(const MemoriesModHost *host, const char *qualified)
{ return owner(host) ? Mods_Find(qualified) : NULL; }

/* --- drawing over the picture ------------------------------------------ */

/* Only while Mods_DrawOverlay runs a mod's overlay callback. */
static struct {
    MenuCanvas *canvas;
    int scale;
    void (*text)(MenuCanvas *, int, int, const char *, uint32_t, int);
    int (*width)(const char *, int);
    int x0, y0, x1, y1;   /* what the mods drew, to report as the overlay's bounds */
} overlay;

static void overlay_touch(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > overlay.canvas->width) x1 = overlay.canvas->width;
    if (y1 > overlay.canvas->height) y1 = overlay.canvas->height;
    if (x >= x1 || y >= y1) return;
    if (overlay.x0 >= overlay.x1) { overlay.x0 = x; overlay.y0 = y; overlay.x1 = x1; overlay.y1 = y1; return; }
    if (x < overlay.x0) overlay.x0 = x;
    if (y < overlay.y0) overlay.y0 = y;
    if (x1 > overlay.x1) overlay.x1 = x1;
    if (y1 > overlay.y1) overlay.y1 = y1;
}

static void host_overlay_size(const MemoriesModHost *host, int *width, int *height, int *scale)
{
    int on = owner(host) && overlay.canvas;
    if (width) *width = on ? overlay.canvas->width : 0;
    if (height) *height = on ? overlay.canvas->height : 0;
    if (scale) *scale = on ? overlay.scale : 1;
}

static int host_text_width(const MemoriesModHost *host, const char *text, int scale)
{
    return owner(host) && overlay.width && text ? overlay.width(text, scale < 1 ? 1 : scale) : 0;
}

static void host_draw_text(const MemoriesModHost *host, int x, int middle, const char *text, uint32_t rgb, int scale)
{
    if (!owner(host) || !overlay.canvas || !text) return;
    if (scale < 1) scale = 1;
    overlay.text(overlay.canvas, x, middle, text, rgb & 0xFFFFFFu, scale);
    overlay_touch(x, middle - 10 * scale, overlay.width(text, scale), 20 * scale);
}

static void host_fill(const MemoriesModHost *host, int x, int y, int w, int h, uint32_t rgb, unsigned alpha)
{
    MenuCanvas *canvas = overlay.canvas;
    unsigned inverse;
    if (!owner(host) || !canvas || w <= 0 || h <= 0) return;
    if (alpha > 255) alpha = 255;
    inverse = 255 - alpha;
    for (int row = y < 0 ? 0 : y; row < y + h && row < canvas->height; row++) {
        for (int column = x < 0 ? 0 : x; column < x + w && column < canvas->width; column++) {
            uint32_t *pixel = canvas->pixels + (size_t)row * (size_t)canvas->stride + (size_t)column, under = *pixel;
            unsigned r = ((rgb >> 16 & 255) * alpha + (under >> 16 & 255) * inverse) / 255;
            unsigned g = ((rgb >> 8 & 255) * alpha + (under >> 8 & 255) * inverse) / 255;
            unsigned b = ((rgb & 255) * alpha + (under & 255) * inverse) / 255;
            unsigned a = alpha + (under >> 24) * inverse / 255;
            *pixel = a << 24 | r << 16 | g << 8 | b;
        }
    }
    overlay_touch(x, y, w, h);
}

void Mods_DrawOverlay(MenuCanvas *canvas, int scale, void (*text)(MenuCanvas *, int, int, const char *, uint32_t, int),
                      int (*width)(const char *, int), int *x, int *y, int *w, int *h)
{
    *x = *y = *w = *h = 0;
    if (shut_down || !canvas || !canvas->pixels || !text || !width) return;
    overlay.canvas = canvas;
    overlay.scale = scale < 1 ? 1 : scale;
    overlay.text = text;
    overlay.width = width;
    overlay.x0 = overlay.y0 = overlay.x1 = overlay.y1 = 0;
    for (int i = 0; i < mod_count; i++) {
        if (mods[i].active && mods[i].initialized && mods[i].hooks.overlay) mods[i].hooks.overlay();
    }
    overlay.canvas = NULL;
    if (overlay.x0 < overlay.x1) {
        *x = overlay.x0; *y = overlay.y0; *w = overlay.x1 - overlay.x0; *h = overlay.y1 - overlay.y0;
    }
}

unsigned Mods_OverlaySignature(unsigned frame)
{
    unsigned signature = 0;
    if (shut_down) return 0;
    for (int i = 0; i < mod_count; i++) {
        const MemoriesMod *hooks = &mods[i].hooks;
        if (!mods[i].active || !mods[i].initialized || !hooks->overlay) continue;
        signature = signature * 31u + (unsigned)i + 1u;
        signature = signature * 2654435761u + (hooks->overlay_signature ? hooks->overlay_signature() : frame);
    }
    return signature;
}

static void fill_host(Mod *mod)
{
    mod->host.hook = host_hook;
    mod->host.unhook = host_unhook;
    mod->host.symbol = host_symbol;
    mod->host.provide = host_provide;
    mod->host.find = host_find;
    mod->host.overlay_size = host_overlay_size;
    mod->host.draw_text = host_draw_text;
    mod->host.text_width = host_text_width;
    mod->host.fill = host_fill;
    mod->host.subscribe = host_subscribe;
    mod->host.unsubscribe = host_unsubscribe;
    mod->host.register_state = host_register_state;
    mod->host.card_id = host_card_id;
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

static int disc_file(int lba, unsigned size)
{
    int i;
    for (i = 0; i < disc_file_count; i++)
        if (disc_files[i].retail_lba == lba) return i;
    if (i == REGIONS_MAX || !size || size > (unsigned)INT_MAX - SECTOR) return -1;
    disc_files[i] = (DiscFile){lba, (int)((size + SECTOR - 1) / SECTOR), lba,
                             (int)((size + SECTOR - 1) / SECTOR), 0, size, size};
    disc_file_count++;
    return i;
}

/* Prepare all data before any mod initializer may cache a position. After
 * publication, even a failed code mod must not move another mod's file. */
static int layout_disc_files(void)
{
    int order[REGIONS_MAX], next = -1;
    for (int i = 0; i < disc_file_count; i++) {
        DiscFile *file = &disc_files[i];
        int maximum = file->retail_sectors, sectors;
        unsigned size = file->retail_size; /* readers never see a partial value */
        for (int j = 0; j < region_count; j++) if (regions[j].file == i) {
            sectors = (int)((regions[j].image_size + SECTOR - 1) / SECTOR);
            if (sectors > maximum) maximum = sectors;
            size = (unsigned)regions[j].image_size;
        }
        sectors = (int)(((size_t)size + SECTOR - 1) / SECTOR);
        file->size = size;
        file->sectors = sectors < file->retail_sectors ? file->retail_sectors : sectors;
        if (!disc_layout_frozen) {
            file->lba = file->retail_lba;
            file->reserved = maximum > file->retail_sectors ? maximum : 0;
        }
        order[i] = i;
        for (int j = i; j > 0 && disc_files[order[j]].retail_lba < disc_files[order[j - 1]].retail_lba; j--) {
            int swap = order[j]; order[j] = order[j - 1]; order[j - 1] = swap;
        }
    }
    if (disc_layout_frozen) return 1;
    for (int i = 0; i < disc_file_count; i++) {
        DiscFile *file = &disc_files[order[i]];
        if (!file->reserved) continue;
        if (next < 0) next = Memories_DiscSectorCount();
        /* One guard sector, and room for the drive's one-past-end position. */
        if (next < 0 || next >= MEMORIES_DISC_MAX_LBA ||
            file->reserved > MEMORIES_DISC_MAX_LBA - next - 1) return 0;
        file->lba = next;
        next += file->reserved + 1;
    }
    return 1;
}

int Mods_DiscFileInfo(int retail_lba, int *lba, unsigned *size)
{
    if (!overrides_live) return 0;
    for (int i = 0; i < disc_file_count; i++) if (disc_files[i].retail_lba == retail_lba) {
        if (lba) *lba = disc_files[i].lba;
        if (size) *size = disc_files[i].size;
        return 1;
    }
    return 0;
}

static int virtual_file(int lba)
{
    if (!overrides_live) return -1;
    for (int i = 0; i < disc_file_count; i++) {
        const DiscFile *file = &disc_files[i];
        if (file->reserved && lba >= file->lba && lba - file->lba < file->sectors) return i;
    }
    return -1;
}

int Mods_DiscSource(int lba, int *source)
{
    int i = virtual_file(lba);
    if (i < 0) return 0;
    *source = lba - disc_files[i].lba < disc_files[i].retail_sectors
        ? disc_files[i].retail_lba + lba - disc_files[i].lba : -1;
    return 1;
}

int Mods_DiscOrigin(int lba)
{
    if (!overrides_live) return lba;
    for (int i = 0; i < disc_file_count; i++) {
        const DiscFile *file = &disc_files[i];
        if (file->reserved && lba >= file->retail_lba && lba - file->retail_lba < file->retail_sectors)
            return file->lba + lba - file->retail_lba;
    }
    return lba;
}

unsigned Mods_DiscSignature(void)
{
    unsigned hash = 2166136261u;
    int relocated = 0;
    for (int i = 0; i < disc_file_count; i++) relocated |= disc_files[i].reserved != 0;
    if (!region_count && !patch_count && !relocated) return 0;
    for (int i = 0; i < disc_file_count; i++) {
        const DiscFile *file = &disc_files[i];
        hash = hash_bytes(hash, &file->retail_lba, sizeof(file->retail_lba));
        hash = hash_bytes(hash, &file->lba, sizeof(file->lba));
        hash = hash_bytes(hash, &file->size, sizeof(file->size));
    }
    for (int i = 0; i < region_count; i++) {
        hash = hash_bytes(hash, &regions[i].hash, sizeof(regions[i].hash));
        hash = hash_bytes(hash, &regions[i].lba, sizeof(regions[i].lba));
        hash = hash_bytes(hash, &regions[i].sectors, sizeof(regions[i].sectors));
    }
    for (int i = 0; i < patch_count; i++) {
        hash = hash_bytes(hash, &patches[i].lba, sizeof(patches[i].lba));
        hash = hash_bytes(hash, &patches[i].offset, sizeof(patches[i].offset));
        hash = hash_bytes(hash, patches[i].bytes, (size_t)patches[i].length);
    }
    return hash;
}

static void publish_overrides(void)
{
    int i, low = 0x7fffffff, high = -1;
    layout_disc_files(); /* capacity is checked before adding a region */
    for (i = 0; i < region_count; i++) {
        if (regions[i].lba < low) low = regions[i].lba;
        if (regions[i].lba + regions[i].sectors - 1 > high) high = regions[i].lba + regions[i].sectors - 1;
    }
    for (i = 0; i < patch_count; i++) {
        if (patches[i].file >= 0) {
            const DiscFile *file = &disc_files[patches[i].file];
            if (file->retail_lba < low) low = file->retail_lba;
            if (file->retail_lba + file->retail_sectors - 1 > high)
                high = file->retail_lba + file->retail_sectors - 1;
            continue;
        }
        if (patches[i].lba < low) low = patches[i].lba;
        if (patches[i].lba > high) high = patches[i].lba;
    }
    for (i = 0; i < disc_file_count; i++) if (disc_files[i].reserved) {
        if (disc_files[i].lba < low) low = disc_files[i].lba;
        if (disc_files[i].lba + disc_files[i].sectors - 1 > high)
            high = disc_files[i].lba + disc_files[i].sectors - 1;
    }
    published_regions = region_count;
    published_patches = patch_count;
    override_low = low;
    override_high = high;
    /* The tables are written before the interrupt is told to read them. */
    __asm__ volatile("" ::: "memory");
    overrides_live = high >= low;
}

/* A replacement file, read-only, for as long as the mod is applied. Linux
 * maps it; Windows has no mmap of a file the port could declare by hand
 * without <windows.h>, so it is read into memory. */
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
    int i, kept = 0, owned = 0;
    mods[mod].data_prepared = 0;
    /* Many failure paths call this for mods with no data. Unpublishing for
     * them would briefly hide every relocated file from the drive. */
    for (i = 0; i < region_count; i++) owned |= regions[i].mod == mod;
    for (i = 0; i < patch_count; i++) owned |= patches[i].mod == mod;
    if (!owned) return;
    overrides_live = 0;   /* the drive model stops looking before anything goes */
    __asm__ volatile("" ::: "memory");
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
static int add_patch(Mod *mod, int index, int file, int lba, int offset, const unsigned char *bytes, int length)
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
        patches[patch_count].file = file;
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

static int add_region(Mod *mod, int index, int named, int lba, int sectors, const char *replacement)
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
    if ((uint64_t)info.st_size > (uint64_t)MEMORIES_DISC_MAX_LBA * SECTOR ||
        (named < 0 && (uint64_t)info.st_size > (uint64_t)sectors * SECTOR)) {
        close(file);
        note(mod, "%s exceeds %s capacity; replacement rejected", replacement,
             named < 0 ? "the raw sector region" : "the virtual disc address");
        return 0;
    }
    image = map_file(file, (size_t)info.st_size);
    close(file);
    if (!image) {
        note(mod, "cannot map %s", replacement);
        return 0;
    }
    regions[region_count].mod = index;
    regions[region_count].file = named;
    regions[region_count].lba = lba;
    regions[region_count].sectors = sectors;
    regions[region_count].image = image;
    regions[region_count].image_size = (size_t)info.st_size;
    regions[region_count].mapped = (size_t)info.st_size;
    regions[region_count].hash = hash_bytes(2166136261u, image, (size_t)info.st_size);
    for (int i = 0; named >= 0 && i < region_count; i++) if (regions[i].file == named) {
        warn(mod, 0, "%s replaces the same disc file as %s; later replacement wins", replacement, mods[regions[i].mod].id);
        break;
    }
    region_count++;
    if (!layout_disc_files()) {
        note(mod, "virtual disc address space exhausted by %s; replacement rejected", replacement);
        return 0; /* apply_overrides rolls back every contribution of this mod */
    }
    return 1;
}

/* The "data" array of a manifest, once the mod is applied. */
static int apply_overrides(Mod *mod, int index)
{
    int i, count = Json_Count(mod->data);
    for (i = 0; i < count; i++) {
        const JsonValue *entry = Json_At(mod->data, i);
        const char *file = Json_String(Json_Member(entry, "file"), NULL);
        const JsonValue *replace = Json_Member(entry, "replace");
        const JsonValue *patch = Json_Member(entry, "patch");
        long lba_number = Json_Number(Json_Member(entry, "lba"), -1);
        int lba = lba_number >= 0 && lba_number <= INT_MAX ? (int)lba_number : -1;
        unsigned size = 0;
        int named = -1;
        if ((replace && Json_TypeOf(replace) != JSON_STRING) ||
            (patch && Json_TypeOf(patch) != JSON_ARRAY) || (!replace && !patch)) {
            note(mod, "a data entry needs a string \"replace\" or an array \"patch\"");
            goto failed;
        }
        if (file) {
            if (Memories_DiscOriginalFileInfo(file, &lba, &size) || lba < 0) {
                note(mod, "%s is not on the disc", file);
                goto failed;
            }
            named = disc_file(lba, size);
            if (named < 0) { note(mod, "cannot register disc file %s", file); goto failed; }
        } else if (lba < 0) {
            note(mod, "a data entry names neither \"file\" nor \"lba\"");
            goto failed;
        }
        if (Json_String(replace, NULL)) {
            long sector_number = size ? (long)(((uint64_t)size + SECTOR - 1) / SECTOR)
                : Json_Number(Json_Member(entry, "sectors"), 0);
            int sectors = sector_number > 0 && sector_number <= INT_MAX ? (int)sector_number : 0;
            if (sectors <= 0 || lba > INT_MAX - sectors) {
                note(mod, "\"replace\" at sector %d needs a \"sectors\" count", lba);
                goto failed;
            } else {
                /* Existing in-place streaming overrides keep their raw
                 * headers. Growth cannot synthesize XA/STR metadata. */
                int backing_file = file && (strstr(file, ".XA") || strstr(file, ".STR")) ? -1 : named;
                if (!add_region(mod, index, backing_file, lba, sectors, Json_String(replace, NULL))) goto failed;
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
                    goto failed;
                }
                length = read_bytes(text, &bytes);
                if (length < 0) {
                    note(mod, "\"bytes\": %s is not hexadecimal", text);
                    goto failed;
                }
                if (named >= 0) {
                    size = disc_files[named].size;
                    if (size < disc_files[named].retail_size) size = disc_files[named].retail_size;
                }
                if (!preparing_data && file && size && ((unsigned long)at > size || (unsigned long)length > size - (unsigned long)at)) {
                    note(mod, "a patch at 0x%lX reaches past %s", at, file);
                    free(bytes);
                    goto failed;
                }
                if (at > INT_MAX - length || at / SECTOR > INT_MAX - lba - (length + SECTOR - 1) / SECTOR ||
                    !add_patch(mod, index, named, (named < 0 ? lba : 0) + (int)(at / SECTOR), (int)(at % SECTOR), bytes, length)) {
                    free(bytes);
                    goto failed;
                }
                free(bytes);
            }
        }
    }
    publish_overrides();
    if (region_count || patch_count) {
        say("%s: %d replaced regions, %d patched runs, sectors %d to %d",
            mod->id, region_count, patch_count, override_low, override_high);
    }
    return 1;
failed:
    drop_overrides(index);
    if (!mod->status[0]) note(mod, "could not prepare data overrides");
    return 0;
}

static int validate_disc_patches(void)
{
    for (int i = 0; i < patch_count; i++) if (patches[i].file >= 0) {
        const Patch *patch = &patches[i];
        const DiscFile *file = &disc_files[patch->file];
        uint64_t end = (uint64_t)patch->lba * SECTOR + patch->offset + patch->length;
        unsigned size = file->size > file->retail_size ? file->size : file->retail_size;
        if (end > size) {
            int mod = patch->mod;
            note(&mods[mod], "a patch reaches past the final replacement (size %u)", size);
            mods[mod].failed = 1;
            mods[mod].data_prepared = 0;
            if (mods[mod].active) activate(mod, 0);
            else drop_overrides(mod);
            return 0;
        }
    }
    return 1;
}

int Mods_DiscSector(int lba, void *user_data)
{
    unsigned char *out = user_data;
    int changed = 0, i, named, relative = -1, physical = lba;
    if (!overrides_live) return 0;
    __asm__ volatile("" ::: "memory");
    if (lba < override_low || lba > override_high) return 0;
    named = virtual_file(lba);
    if (named >= 0) {
        relative = lba - disc_files[named].lba;
        physical = relative < disc_files[named].retail_sectors ? disc_files[named].retail_lba + relative : -1;
    }
    for (i = 0; i < published_regions; i++) {
        const Region *region = &regions[i];
        size_t at, have;
        if (named >= 0 && region->file == named) at = (size_t)relative * SECTOR;
        else {
            if (physical < region->lba || physical - region->lba >= region->sectors) continue;
            at = (size_t)(physical - region->lba) * SECTOR;
        }
        have = at < region->image_size ? region->image_size - at : 0;
        if (have > SECTOR) have = SECTOR;
        if (have) memcpy(out, region->image + at, have);
        if (have < SECTOR) memset(out + have, 0, SECTOR - have);
        changed = 1;
    }
    for (i = 0; i < published_patches; i++) {
        const Patch *patch = &patches[i];
        if (patch->file >= 0) {
            const DiscFile *file = &disc_files[patch->file];
            if (named == patch->file) {
                if (patch->lba != relative) continue;
            } else if (physical < file->retail_lba || physical - file->retail_lba >= file->retail_sectors ||
                       patch->lba != physical - file->retail_lba) continue;
        } else if (patch->lba != physical) continue;
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
    mod->code_hash = 2166136261u;
    for (long i = 0; i < size; i++) mod->code_hash = (mod->code_hash ^ data[i]) * 16777619u;
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
        memset(&mod->hooks, 0, sizeof(mod->hooks));
        Mods_ClearHooks((int)(mod - mods));
        note(mod, "refused to start");
        return 0;
    }
    if (!mod->hooks.api || mod->hooks.api > MEMORIES_MOD_API) {
        /* 0 is a mod that never said which table it filled in. */
        if (!mod->hooks.api) note(mod, "MemoriesModInit did not set mod->api");
        else note(mod, "was built for mod API %u; this game has %u", mod->hooks.api, MEMORIES_MOD_API);
        memset(&mod->hooks, 0, sizeof(mod->hooks));
        Mods_ClearHooks((int)(mod - mods));
        return 0;
    }
    if (mod->hooks.name && *mod->hooks.name && !mod->name[0]) {
        copy_text(mod->name, sizeof(mod->name), mod->hooks.name);
    }
    mod->initialized = 1;
    say("%s: loaded %s", mod->id, mod->library);
    return 1;
}

/* Every key a manifest's top level may have (here, in manager.c and in
 * notes/modding.md and mod-api-3.md). Anything else is most likely a typo
 * that would leave the mod doing nothing, so it is a warning, not an error. */
static const char *const manifest_keys[] = {
    "id", "name", "version", "author", "description", "library", "enabled", "restart", "legacy_setting",
    "data", "textures", "cards", "audio", "min_api", "game", "requires", "after", "conflicts", "priority",
    "settings", "fusions", "equips", "rituals", "drops", "decks", "text", "font",
};

/* How many letters to add, remove or change to turn one word into the
 * other, for "did you mean": 99 for words too long to bother with. */
static int distance(const char *a, const char *b)
{
    int row[33], i, j, length_a = (int)strlen(a), length_b = (int)strlen(b);
    if (length_a > 32 || length_b > 32) return 99;
    for (j = 0; j <= length_b; j++) row[j] = j;
    for (i = 1; i <= length_a; i++) {
        int diagonal = row[0];
        row[0] = i;
        for (j = 1; j <= length_b; j++) {
            int above = row[j], best = diagonal + (tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]));
            if (above + 1 < best) best = above + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            diagonal = above;
            row[j] = best;
        }
    }
    return row[length_b];
}

static void check_keys(Mod *mod, const JsonValue *root)
{
    static const char *const booleans[] = {"enabled", "restart"};
    const JsonValue *member;
    unsigned i;
    for (member = Json_At(root, 0); member; member = Json_Next(member)) {
        const char *name = Json_Name(member), *closest = NULL;
        int best = 3;   /* at most two letters off, to be a likely typo */
        for (i = 0; i < sizeof(manifest_keys) / sizeof(manifest_keys[0]); i++) {
            int apart;
            if (!strcmp(name, manifest_keys[i])) break;
            apart = distance(name, manifest_keys[i]);   /* letter case counts as no distance: "Name" */
            if (apart < best) { best = apart; closest = manifest_keys[i]; }
        }
        if (i < sizeof(manifest_keys) / sizeof(manifest_keys[0])) continue;
        if (closest) warn(mod, 1, "unknown key '%s' (did you mean '%s'?)", name, closest);
        else warn(mod, 1, "unknown key '%s'", name);
    }
    for (i = 0; i < sizeof(booleans) / sizeof(booleans[0]); i++) {
        const JsonValue *value = Json_Member(root, booleans[i]);
        if (value && Json_TypeOf(value) != JSON_BOOL) warn(mod, 1, "\"%s\" should be true or false", booleans[i]);
    }
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
    if (strlen(text) >= sizeof(mod->id) || !*text || strspn(text, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(text)) {
        copy_text(mod->id, sizeof(mod->id), strrchr(directory, '/') + 1);
        mod->broken = 1;
        note(mod, "id must be 1-63 letters, digits, hyphens or underscores");
    } else copy_text(mod->id, sizeof(mod->id), text);
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
    mod->audio = Json_Member(root, "audio");
    if (mod->audio && Json_TypeOf(mod->audio) != JSON_OBJECT) {
        mod->broken = 1;
        note(mod, "\"audio\" is not an object");
    }
    mod->cards = Json_Member(root, "cards");
    if (mod->cards && Json_TypeOf(mod->cards) != JSON_ARRAY) {
        mod->broken = 1;
        note(mod, "\"cards\" is not an array");
    }
    /* Data overrides change what the game loaded on its way up, so they are
     * only whole while the game starts with them in place; the cards a mod
     * adds are counted once, when the game starts. */
    if (Json_Count(mod->data)) mod->restart = Json_Bool(Json_Member(root, "restart"), 1);
    for (int i = 0; i < Json_Count(mod->data); i++) {
        const JsonValue *entry = Json_At(mod->data, i);
        if (Json_Member(entry, "file") && Json_Member(entry, "replace")) mod->restart = 1;
    }
    if (Json_Count(mod->cards)) mod->restart = 1;
    {   /* So are the rule tables (src/pc/cards/tables.c) and a translation
         * (src/pc/text): both are read once, at startup. */
        static const char *const tables[] = {"fusions", "equips", "rituals", "drops", "decks", "text", "font"};
        for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
            const JsonValue *value = Json_Member(root, tables[t]);
            /* "text": "text.txt" is one file named as a string. */
            if (Json_Count(value) || *Json_String(value, "")) mod->restart = 1;
        }
    }
    {   /* The key this mod's choice was stored under before it was a mod. */
        const char *legacy = Json_String(Json_Member(root, "legacy_setting"), NULL);
        char key[256];
        int fallback = mod->default_enabled;
        if (legacy && *legacy) fallback = Settings_GetNamed(legacy, fallback);
        if (setting_key(key, sizeof(key), mod->id, NULL)) fallback = Settings_GetNamed(key, fallback);
        mod->enabled = environment_choice(mod->id, fallback) != 0;
    }
    check_keys(mod, root);
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
    unsigned char here[MODS_MAX] = {0};   /* the mods this directory has given so far */
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
        if (existing >= 0 && here[existing]) {
            /* Two folders of one directory with one id: the first, in the
             * sorted order, is kept, whatever order the disk lists them. */
            warn(&mods[existing], 1, "%s was left out: same id as %s", path, mods[existing].directory);
            Json_Free(candidate.manifest);
        } else if (existing >= 0) {
            /* The player's copy wins over the one the release ships. */
            say("%s in %s replaces the one in %s", candidate.id, path, mods[existing].directory);
            Json_Free(mods[existing].manifest);
            candidate.enabled = mods[existing].enabled;
            mods[existing] = candidate;
            here[existing] = 1;
        } else if (mod_count < MODS_MAX) {
            here[mod_count] = 1;
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
static int (*texture_pack_load)(const char *directory, unsigned rank, char *problems, size_t size);
static void (*texture_pack_unload)(void);
static unsigned texture_rank;   /* the highest rank a loaded pack has */
static int (*audio_load)(int, const char *, const char *, const JsonValue *, char *, size_t);
static void (*audio_unload)(int);

void Mods_SetAudio(int (*load)(int mod, const char *id, const char *directory, const struct JsonValue *audio,
                               char *error, size_t size),
                   void (*unload)(int mod))
{
    audio_load = load;
    audio_unload = unload;
}

void Mods_SetTexturePack(int (*load)(const char *directory, unsigned rank, char *problems, size_t size),
                         void (*unload)(void))
{
    texture_pack_load = load;
    texture_pack_unload = unload;
}

/* The texture packs of the active mods, with `with` about to be one and
 * without `without`, in the mods' load order (Mods_Order): a pack's rank
 * follows its place there, so where two packs read the same words the same
 * way the later one wins, whichever the player applied first. A pack that
 * comes last, as each does while the game starts, goes on top of those
 * loaded; otherwise they are all loaded again. Returns what loading `with`
 * returned, its problems in `problems`. */
static int load_texture_packs(int with, int without, char *problems, size_t size)
{
    int wanted[MODS_MAX], order[MODS_MAX], i, n, loaded = with < 0;
    char ignored[STATUS_MAX];
    for (i = 0; i < mod_count; i++)
        wanted[i] = i != without && mods[i].textures[0] && (mods[i].active || i == with);
    n = Mods_Order(wanted, order, ignored, sizeof(ignored));
    if (n < 0) for (n = 0; order[n] >= 0; n++) continue;   /* a cycle: the packs that could be placed */
    if (with >= 0 && n > 0 && order[n - 1] == with) {
        return texture_pack_load(mods[with].textures, ++texture_rank, problems, size);
    }
    texture_pack_unload();
    texture_rank = 0;
    for (i = 0; i < n; i++) {
        int got = texture_pack_load(mods[order[i]].textures, ++texture_rank, order[i] == with ? problems : NULL, size);
        if (order[i] == with) loaded = got;
    }
    return loaded;
}

static void activate_once(int index, int on);

/* Function hooks follow what is applied, however activation ended. */
static void activate(int index, int on)
{
    activate_once(index, on);
    Hooks_Relink();
}

static void activate_once(int index, int on)
{
    Mod *mod = &mods[index];
    if (on == mod->active) return;
    if (on) {
        int option_count = Mods_OptionCount(index);
        if (option_count && !mod->runtime_options) {
            mod->runtime_options = malloc((size_t)option_count * sizeof(int));
            if (!mod->runtime_options) {
                mod->failed = 1; note(mod, "out of memory for settings"); drop_overrides(index); return;
            }
        }
        /* A mod that cannot load keeps the player's choice and its reason:
         * the window shows both, and removing it still works. */
        if (mod->broken || !load_library(mod)) {
            mod->broken = 1;
            drop_overrides(index);
            return;
        }
        if (!mod->data_prepared) copy_text(mod->status, sizeof(mod->status), mod->warnings);
        if (!mod->data_prepared && !apply_overrides(mod, index)) { mod->failed = 1; return; }
        mod->data_prepared = 1;
        if (mod->textures[0]) {
            char problems[STATUS_MAX] = "";
            if (!texture_pack_load || !texture_pack_unload || !load_texture_packs(index, -1, problems, sizeof(problems))) {
                mod->failed = 1;
                note(mod, "texture pack could not load%s%s", problems[0] ? ": " : "", problems);
                drop_overrides(index);
                if (texture_pack_load && texture_pack_unload) load_texture_packs(-1, index, NULL, 0);
                return;
            }
            if (problems[0]) warn(mod, 0, "texture pack: %s", problems);
        }
        if (mod->audio) {
            /* Replacement sounds are decoded now; one that will not decode
             * is skipped with its reason beside the mod, and the rest play. */
            char error[STATUS_MAX];
            int added = audio_load ? audio_load(index, mod->id, mod->directory, mod->audio, error, sizeof(error)) : 0;
            if (!audio_load) warn(mod, 0, "this build has no audio replacement");
            else if (error[0]) warn(mod, 0, "%s", error);
            if (added > 0) say("%s: %d sounds replaced", mod->id, added);
        }
        for (int option = 0; option < option_count; option++) mod->runtime_options[option] = Mods_OptionValue(index, option);
        mod->sequence = ++activation_sequence;
        mod->failed = 0;
        mod->active = 1;
        if (mod->hooks.applied) mod->hooks.applied(1);
    } else {
        drop_overrides(index);
        if (mod->audio && audio_unload) audio_unload(index);
        if (mod->textures[0] && texture_pack_load && texture_pack_unload) {
            /* The packs add up: the others' come back without this one's. */
            load_texture_packs(-1, index, NULL, 0);
        }
        mod->active = 0;
        if (mod->hooks.applied) mod->hooks.applied(0);
    }
}

int Mods_InstallDirectory(char *out, size_t size)
{
    const char *named = getenv("MEMORIES_MODS_DIR");
    if (named && *named) {
        if ((size_t)snprintf(out, size, "%s", named) >= size) return -1;
    } else if (Paths_User(out, size, "mods")) {
        return -1;
    }
    return Paths_MakeDirs(out);
}

void Mods_Load(void)
{
    const char *all = getenv("MEMORIES_MODS");
    char path[PATH_MAX_];
    int i, enabled[MODS_MAX], order[MODS_MAX], count, first = !scanned;
    char error[160];
    if (!scanned) {
        const char *named = getenv("MEMORIES_MODS_DIR");
        scanned = 1;
        if (named && *named) {
            scan(named, "installed");
        } else {
            if (!Paths_Program(path, sizeof(path), "mods")) scan(path, "shipped");
            if (!Paths_User(path, sizeof(path), "mods")) scan(path, "installed");
        }
        for (i = 0; i < mod_count; i++) {
            if (!Mods_CheckManifest(i, error, sizeof(error))) { mods[i].broken = 1; note(&mods[i], "%s", error); }
        }
        say("%d mods found", mod_count);
    }
    for (i = 0; i < mod_count; i++) {
        /* The settings are the choice, here and after a settings reload.
         * A mod that wants a restart is still put in place at startup: it
         * is only a live change it cannot take. */
        char key[256];
        int want;
        want = mods[i].enabled;
        if (setting_key(key, sizeof(key), mods[i].id, NULL)) want = Settings_GetNamed(key, want);
        want = environment_choice(mods[i].id, want) != 0;
        if (all && (!strcmp(all, "0") || !strcmp(all, "off"))) want = 0;
        mods[i].enabled = want;
        enabled[i] = want;
    }
    for (i = 0; i < mod_count; i++) if (enabled[i] && !Mods_Compatible(i, enabled, error, sizeof(error))) {
        note(&mods[i], "%s", error); enabled[i] = 0;
    }
    count = Mods_Order(enabled, order, error, sizeof(error));
    if (count < 0) {
        /* Only the mods in the cycle, or waiting on it, stay off. */
        int placed[MODS_MAX] = {0};
        for (count = 0; order[count] >= 0; count++) placed[order[count]] = 1;
        for (i = 0; i < mod_count; i++) if (enabled[i] && !placed[i]) { note(&mods[i], "%s", error); enabled[i] = 0; }
    }
    if (first) {
        preparing_data = 1;
        for (i = 0; i < count; i++) {
            int current = order[i];
            if (!enabled[current] || mods[current].broken) continue;
            if (!apply_overrides(&mods[current], current)) mods[current].failed = 1;
            else mods[current].data_prepared = 1;
        }
        preparing_data = 0;
        for (;;) {
            int changed = 0;
            while (!validate_disc_patches()) changed = 1;
            /* Failed preparations and their dependents cannot contribute data. */
            for (i = 0; i < count; i++) {
                int current = order[i];
                if (!enabled[current]) continue;
                if (mods[current].failed || mods[current].broken) enabled[current] = 0;
                if (enabled[current] && !Mods_Compatible(current, enabled, error, sizeof(error))) {
                    note(&mods[current], "%s", error);
                    mods[current].failed = 1;
                    enabled[current] = 0;
                }
                if (!enabled[current]) { drop_overrides(current); changed = 1; }
            }
            if (!changed) break;
        }
        disc_layout_frozen = 1;
    }
    /* After the first load (a settings reload) a mod that wants a restart is
     * only recorded, as Mods_SetEnabled does, and a live one that requires
     * it waits for the same restart. */
    {   /* Applied last, removed first (as Mods_Apply does). */
        int newest[MODS_MAX], j;
        for (i = 0; i < mod_count; i++) {
            for (j = i; j > 0 && mods[newest[j - 1]].sequence < mods[i].sequence; j--) newest[j] = newest[j - 1];
            newest[j] = i;
        }
        for (j = 0; j < mod_count; j++) {
            i = newest[j];
            if (!enabled[i] && (first || !mods[i].restart)) activate(i, 0);
        }
    }
    for (i = 0; i < count; i++) {
        int current = order[i], j, active[MODS_MAX];
        if (!enabled[current] || (first && mods[current].failed)) continue;
        if (!first && mods[current].restart) continue;
        if (!first && !mods[current].active && (j = Mods_WaitsForRestart(current, enabled)) >= 0) {
            note(&mods[current], "waits for a restart: %s, which it requires, is applied at the next launch", mods[j].name);
            continue;
        }
        for (j = 0; j < mod_count; j++) active[j] = mods[j].active;
        active[current] = 1;
        if (!Mods_Compatible(current, active, error, sizeof(error))) {
            note(&mods[current], "%s", error); drop_overrides(current); continue;
        }
        activate(current, 1);
    }
    /* A code/texture initializer can still fail after data preparation.
     * Recheck tail patches and dependencies against the surviving files. */
    for (;;) {
        int changed = 0, active[MODS_MAX];
        while (!validate_disc_patches()) changed = 1;
        for (i = 0; i < mod_count; i++) active[i] = mods[i].active;
        for (i = 0; i < count; i++) {
            int current = order[i];
            if (active[current] && !Mods_Compatible(current, active, error, sizeof(error))) {
                note(&mods[current], "%s", error);
                activate(current, 0);
                mods[current].failed = 1;
                active[current] = 0;
                changed = 1;
            }
        }
        if (!changed) break;
    }
    /* What the startup-only readers (rule tables, translation) see: the mods
     * active once the game has started, in load order. A settings reload
     * leaves it alone, as those readers do not run again. */
    if (first) {
        loaded_count = 0;
        for (i = 0; i < count; i++) if (mods[order[i]].active) loaded[loaded_count++] = order[i];
    }
}

int Mods_LoadedCount(void) { return loaded_count; }
int Mods_Loaded(int index) { return index >= 0 && index < loaded_count ? loaded[index] : -1; }

void Mods_Shutdown(void)
{
    int i;
    /* Every mod's event callbacks go before any shutdown hook runs, and the
     * frame and reset hooks stop: the process is on its way out, and a hook
     * may free what the callbacks use. The caller stops the clock first. */
    shut_down = 1;
    for (i = 0; i < mod_count; i++) Mods_ClearHooks(i);
    for (i = 0; i < mod_count; i++) {
        if (mods[i].initialized && mods[i].hooks.shutdown) mods[i].hooks.shutdown();
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
    char key[256];
    if (!at(mod)) return;
    enabled = enabled != 0;
    /* Unchanged, nothing is written: the choice stored stays, even when an
     * environment variable overrides it for this run. */
    if (mods[mod].enabled == enabled) return;
    if (setting_key(key, sizeof(key), mods[mod].id, NULL)) Settings_SetNamed(key, enabled);
    mods[mod].enabled = enabled;
    /* A mod that could not go in place (a replacement file missing, say) is
     * tried again when the player next applies it; one that cannot load at
     * all stays broken. */
    if (!enabled) mods[mod].failed = 0;
    /* A mod that asks for a restart is only recorded here; the next launch
     * is what puts it in place (the mods window offers the restart). So is
     * a live one that requires a mod still waiting for that launch. */
    if (mods[mod].restart) return;
    if (enabled) {
        int i, dep, wanted[MODS_MAX];
        for (i = 0; i < mod_count; i++) wanted[i] = mods[i].enabled;
        if ((dep = Mods_WaitsForRestart(mod, wanted)) >= 0) {
            note(&mods[mod], "waits for a restart: %s, which it requires, is applied at the next launch", mods[dep].name);
            return;
        }
    } else if (!mods[mod].active && !Mods_Failed(mod)) {
        /* whatever it was waiting for, it no longer is */
        copy_text(mods[mod].status, sizeof(mods[mod].status), mods[mod].warnings);
    }
    activate(mod, enabled);
}

void Mods_VisitCards(void (*visit)(const char *id, const char *directory, const struct JsonValue *cards, void *context),
                     void *context)
{
    int i;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].active && Json_Count(mods[i].cards)) visit(mods[i].id, mods[i].directory, mods[i].cards, context);
    }
}

int Mods_Setting(const char *id, const char *key, int fallback)
{
    char name[256];
    const char *text;
    if (by_id(id) < 0 || !key || !*key) return fallback;
    text = environment_name(name, sizeof(name), id, key) ? getenv(name) : NULL;
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
    if (shut_down) return;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].active && mods[i].initialized && mods[i].hooks.frame) mods[i].hooks.frame();
    }
}

void Mods_Reset(void)
{
    int i;
    if (shut_down) return;
    for (i = 0; i < mod_count; i++) {
        if (mods[i].active && mods[i].initialized && mods[i].hooks.reset) mods[i].hooks.reset();
    }
}

const JsonValue *Mods_Manifest(int index) { return at(index) ? Json_Root(mods[index].manifest) : NULL; }
const char *Mods_Metadata(int index, const char *key) { return Json_String(Json_Member(Mods_Manifest(index), key), ""); }
const char *Mods_Directory(int index) { return at(index) ? mods[index].directory : ""; }
const char *Mods_Origin(int index) { return at(index) ? mods[index].origin : ""; }
int Mods_Active(int index) { return at(index) && mods[index].active; }
int Mods_Failed(int index) { return at(index) && (mods[index].broken || mods[index].failed); }

unsigned Mods_CodeHash(int index) { return at(index) ? mods[index].code_hash : 0; }

unsigned Mods_Sequence(int index) { return at(index) ? mods[index].sequence : 0; }
int Mods_RuntimeOption(int index, int option) {
    const JsonValue *spec = Mods_Option(index, option);
    if (at(index) && mods[index].active && mods[index].runtime_options &&
        (mods[index].restart || Json_Bool(Json_Member(spec, "restart"), 0))) return mods[index].runtime_options[option];
    return Mods_OptionValue(index, option);
}
