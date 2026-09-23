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
 *   2  now_us, map_fixed; setting() reads MEMORIES_MOD_<ID>_<KEY> first */
#define MEMORIES_MOD_API 2

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
     * knob is tried without writing it down. */
    int (*setting)(const MemoriesModHost *host, const char *key, int fallback);
    void (*set_setting)(const MemoriesModHost *host, const char *key, int value);

    /* The first sector of a file on the disc by its retail path
     * ("\\DATA\\MODEL.MRG;1"), or -1; and a bulk read of 2048-byte blocks
     * beside the drive model, which does not disturb the game's streaming. */
    int (*disc_file_start)(const MemoriesModHost *host, const char *iso_path);
    int (*disc_read)(const MemoriesModHost *host, int lba, int sectors, void *out);

    /* The pad, as the game sees it: the buttons of port 0 or 1. */
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
};

/* The symbol a mod's object defines, and its type. */
typedef int (*MemoriesModEntry)(const MemoriesModHost *host, MemoriesMod *mod);
int MemoriesModInit(const MemoriesModHost *host, MemoriesMod *mod);

#endif
