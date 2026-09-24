#ifndef MEMORIES_MOD_API_H
#define MEMORIES_MOD_API_H
/* The interface a code mod is built against. A mod is a directory with a
 * mod.json in it, either beside the executable (the ones the release ships)
 * or in the player's own mods directory (paths.h); see notes/modding.md.
 *
 * A mod that only replaces or patches data on the disc needs no code at all:
 * its manifest names the files. A mod with code is one object file, the same
 * for every system (tools/pc/build_mod.py builds it), and defines one symbol,
 *
 *     int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod);
 *
 * which fills `mod` in and returns nonzero to accept the load. It is called
 * when the mod is applied, never before, and the code then stays in the
 * process until the game exits.
 *
 * What the host offers here is what a mod can reach safely. There is no
 * network call and no way to name a file outside the mod's own directories:
 * paths are relative, and "..", absolute paths and drive letters are
 * refused. A code mod is still native code linked into the game -- it can
 * reach the whole game image the same way the port's own code does, which is
 * the point of it -- so install mods you trust, as you would any plugin.
 *
 * In a mod, <stdio.h> is the SDK's (src/pc/mods/sdk): FILE is opaque there.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Bumped when this header changes shape. A mod records the version it was
 * built against; the host refuses a mod built against a later one. New host
 * entries only ever go at the end, so a mod built against an earlier version
 * keeps working, and one built against a later version can check host->api
 * before it calls an entry the host may not have.
 *   1  the first
 *   2  now_us, map_fixed; setting() reads MEMORIES_MOD_<ID>_<KEY> first
 *   3  managed events, registered state and stable card lookup
 *   4  hook/unhook any game function; symbol looks a name up at run time;
 *      provide/find share functions between mods; draw over the picture
 *      (MemoriesMod.overlay); save slot events; more of the C library */
#include "mod_types.h"

typedef struct MemoriesModHost MemoriesModHost;


typedef struct {
    unsigned api;      /* MEMORIES_MOD_API */
    const char *name;  /* optional: the manifest's name is used when this is null */
    /* Once a frame, after the game has queued its own drawing and before the
     * picture is presented, and only while the mod is applied. */
    void (*frame)(void);
    /* The player applied (1) or removed (0) the mod while the game runs. */
    void (*applied)(int on);
    /* A save state was loaded: anything cached from the old game is stale. */
    void (*reset)(void);
    /* The game is closing. */
    void (*shutdown)(void);
    /* --- API 4 ---
     * Draw over the picture, at the window's own resolution, with the host's
     * draw_text and fill (only valid in here). The overlay is drawn again
     * only when overlay_signature returns something new, so return a number
     * that changes with what you draw; without one it is drawn every
     * frame. Only while the mod is applied. */
    void (*overlay)(void);
    unsigned (*overlay_signature)(void);
} MemoriesMod;

struct MemoriesModHost {
    unsigned api;            /* the host's MEMORIES_MOD_API */
    const char *id;          /* this mod's id, from its manifest */
    const char *directory;   /* where this mod was loaded from */
    void *reserved;          /* the host's own; do not touch */

    /* Whether the player has this mod applied. */
    int (*applied)(const MemoriesModHost *host);
    /* A line in the game's log, under the "mods" channel, and whether that
     * channel is on -- worth asking before working out what to say. */
    void (*log)(const MemoriesModHost *host, const char *format, ...);
    int (*log_enabled)(const MemoriesModHost *host);

    /* A file the mod ships, read-only, named relative to its directory. */
    FILE *(*open_asset)(const MemoriesModHost *host, const char *relative);
    /* A file of the mod's own, in the player's directory, opened with the
     * usual fopen modes. This is the only place a mod may write. */
    FILE *(*open_data)(const MemoriesModHost *host, const char *relative, const char *mode);

    /* The mod's settings, kept in the game's settings file as
     * `mod.<id>.<key>`; whole numbers, saved with the player's settings.
     * From API 2, the environment variable MEMORIES_MOD_<ID>_<KEY> (both
     * uppercased, anything but letters and digits an underscore; decimal, or
     * hexadecimal after 0x) wins over the file for the run, which is how a
     * knob is tried without writing it down. set_setting ignores a key that
     * is not letters, digits, '_' and '-', and "order", which the manager
     * keeps for the mod's load order. */
    int (*setting)(const MemoriesModHost *host, const char *key, int fallback);
    void (*set_setting)(const MemoriesModHost *host, const char *key, int value);

    /* The first sector of a file on the disc by its retail path
     * ("\\DATA\\MODEL.MRG;1"), or -1; and a bulk read of 2048-byte blocks
     * beside the drive model, which does not disturb the game's streaming. */
    int (*disc_file_start)(const MemoriesModHost *host, const char *iso_path);
    int (*disc_read)(const MemoriesModHost *host, int lba, int sectors, void *out);

    /* Normalized pad buttons of port 0 or 1, before managed input hooks. */
    unsigned short (*pad)(const MemoriesModHost *host, int port);

    /* --- API 2 --- */

    /* A clock for timing things, in microseconds from an arbitrary start. */
    uint64_t (*now_us)(const MemoriesModHost *host);
    /* Zeroed read/write memory at exactly `address`, which must be a multiple
     * of 64 KiB (Windows reserves memory in those), for the life of the
     * process. NULL if any of the range is already taken. For a mod that
     * needs data at a fixed guest-sized address, as the game's own model
     * code does; anything else should use malloc. */
    void *(*map_fixed)(const MemoriesModHost *host, uintptr_t address, size_t size);
    /* --- API 3 ---
     * A token of zero means registration failed. Hooks remain registered
     * while disabled but never run; failed initialization removes them.
     * Register during Init; do not install raw pointers in game save data. */
    int (*subscribe)(const MemoriesModHost *, unsigned event, int priority, MemoriesModCallback);
    void (*unsubscribe)(const MemoriesModHost *, int token);
    /* One pointer-free state buffer per mod, persisted in save states.
     * Keep it alive until shutdown. Bump version whenever its layout changes.
     * SAVE/BEFORE and LOAD/AFTER let a mod pack/unpack its state here. */
    int (*register_state)(const MemoriesModHost *, void *data, size_t size, unsigned version);
    /* Resolve a stable "mod-id:card-key" identity after card tables build. */
    int (*card_id)(const MemoriesModHost *, const char *identity);

    /* --- API 4 ---
     * Replace one of the game's functions (anything in src/game, named
     * directly: `host->hook(host, Duel_DrawFieldCards, my_draw, &original)`)
     * with `replacement`, which has the same signature. While the mod is
     * applied every call goes to `replacement`; `*original`, when given, is
     * kept pointing at what it displaced -- the game's function, or another
     * mod's replacement made earlier -- so a hook that calls it wraps the
     * function rather than replacing it. `original` must point at storage
     * that lives as long as the mod (a static variable, not a local): the
     * host rewrites it whenever other mods are applied or removed, so read
     * it at each call. Removing the mod
     * takes its hooks out of the way at once. A token, or 0 when `function`
     * is not a game function (port code and the C library cannot be hooked). */
    int (*hook)(const MemoriesModHost *, void *function, void *replacement, void **original);
    void (*unhook)(const MemoriesModHost *, int token);
    /* The address of a game or port name, as the loader binds a mod's
     * undefined names, or NULL: for a name a mod can do without. */
    void *(*symbol)(const MemoriesModHost *, const char *name);
    /* Share something with other mods under a name of this mod's own
     * (letters, digits, '_' and '-'); another mod finds it as
     * "<this mod's id>:<name>" -- in its MemoriesModInit too, when it
     * "requires" this mod, which is then initialized first. 0 when the name
     * is not valid or the table is full; find gives NULL when no loaded mod
     * provides that. What is shared stays loaded until the game exits,
     * whether or not its mod is applied. */
    int (*provide)(const MemoriesModHost *, const char *name, void *pointer);
    void *(*find)(const MemoriesModHost *, const char *qualified);
    /* Overlay drawing, for MemoriesMod.overlay: the canvas's size in pixels
     * and the scale the port draws its own menus at (1 at 480 lines, more in
     * a bigger window); text (ASCII) with `middle` its vertical centre and
     * its width; a rectangle blended in at `alpha` (0-255). Colours are
     * 0xRRGGBB. Outside the overlay callback these do nothing. */
    void (*overlay_size)(const MemoriesModHost *, int *width, int *height, int *scale);
    void (*draw_text)(const MemoriesModHost *, int x, int middle, const char *text, uint32_t rgb, int scale);
    int (*text_width)(const MemoriesModHost *, const char *text, int scale);
    void (*fill)(const MemoriesModHost *, int x, int y, int w, int h, uint32_t rgb, unsigned alpha);
};

/* The symbol a mod's object defines, and its type. */
typedef int (*MemoriesModEntry)(const MemoriesModHost *host, MemoriesMod *mod);
int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod);

#endif
