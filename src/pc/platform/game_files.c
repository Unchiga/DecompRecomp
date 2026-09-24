/* Finding the player's disc image and reading the game's executable out of
 * it (game_files.h). Plain reads at startup, before the drive model (libds.c)
 * opens the image for the game. */
#include "pc/compat/fs.h"
#include "game_files.h"
#include "paths.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAW_SECTOR 2352
#define USER_DATA 24
#define SECTOR 2048
#define PATH_MAX_ 1024
#define EXECUTABLE "SLUS_014.11;1"
#ifdef _WIN32
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

static char found[PATH_MAX_];

static unsigned le32(const unsigned char *bytes)
{
    return bytes[0] | (unsigned)bytes[1] << 8 | (unsigned)bytes[2] << 16 | (unsigned)bytes[3] << 24;
}

static int read_sector(FILE *file, unsigned lba, unsigned char *out)
{
    unsigned char raw[RAW_SECTOR];
    if (fseek(file, (long)lba * RAW_SECTOR, SEEK_SET) || fread(raw, 1, RAW_SECTOR, file) != RAW_SECTOR) return 0;
    memcpy(out, raw + USER_DATA, SECTOR);
    return 1;
}

/* Where SLUS_014.11 is on the image, or 0 when the image is not that disc
 * (not raw, not ISO 9660, or another game). */
static int find_executable(FILE *file, unsigned *lba, unsigned *size)
{
    unsigned char sector[SECTOR];
    unsigned root, root_size, at;
    if (!read_sector(file, 16, sector) || sector[0] != 1 || memcmp(sector + 1, "CD001", 5)) return 0;
    root = le32(sector + 156 + 2);
    root_size = le32(sector + 156 + 10);
    if (root_size > 64 * SECTOR) return 0;
    for (at = 0; at < root_size; at += SECTOR) {
        unsigned offset = 0;
        if (!read_sector(file, root + at / SECTOR, sector)) return 0;
        while (offset < SECTOR && sector[offset]) {
            const unsigned char *record = sector + offset;
            if (record[0] < 34 || offset + record[0] > SECTOR) break;
            if (record[32] == strlen(EXECUTABLE) && !memcmp(record + 33, EXECUTABLE, record[32])) {
                *lba = le32(record + 2);
                *size = le32(record + 10);
                return *size > SECTOR && *size < 4u << 20;
            }
            offset += record[0];
        }
    }
    return 0;
}

static int is_the_disc(const char *path)
{
    FILE *file = fopen(path, "rb");
    unsigned lba, size;
    int yes;
    if (!file) return 0;
    yes = find_executable(file, &lba, &size);
    fclose(file);
    return yes;
}

static int ends_with_bin(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && (!strcmp(name + length - 4, ".bin") || !strcmp(name + length - 4, ".BIN") ||
                          !strcmp(name + length - 4, ".Bin"));
}

/* The first image of the disc in a folder, trying the usual name first and
 * the rest in name order, so the choice does not change between launches. */
static int search(const char *folder)
{
    DIR *directory;
    struct dirent *entry;
    char best[PATH_MAX_] = "";
    if (snprintf(found, sizeof(found), "%s/rpg-yfm.bin", folder) < (int)sizeof(found) && is_the_disc(found)) return 1;
    directory = opendir(folder);
    if (!directory) return 0;
    while ((entry = readdir(directory))) {
        if (!ends_with_bin(entry->d_name) || strlen(entry->d_name) >= sizeof(best)) continue;
        if (best[0] && strcmp(entry->d_name, best) >= 0) continue;
        if (snprintf(found, sizeof(found), "%s/%s", folder, entry->d_name) >= (int)sizeof(found)) continue;
        if (is_the_disc(found)) snprintf(best, sizeof(best), "%s", entry->d_name);
    }
    closedir(directory);
    return best[0] && snprintf(found, sizeof(found), "%s/%s", folder, best) < (int)sizeof(found);
}

const char *GameFiles_Disc(char *why, size_t why_size)
{
    const char *named = getenv("MEMORIES_DISC");
    char folder[PATH_MAX_];
    if (found[0]) return found;
    if (named && *named) {
        if (is_the_disc(named)) return snprintf(found, sizeof(found), "%s", named) < (int)sizeof(found) ? found : NULL;
        snprintf(why, why_size, "MEMORIES_DISC names %s, which is not a raw image of the Forbidden Memories "
                                "(USA) disc.", named);
        return NULL;
    }
    if ((!Paths_Program(folder, sizeof(folder), "game") && search(folder)) || search(Paths_ProgramDir()) ||
        (!Paths_User(folder, sizeof(folder), "game") && search(folder)) || search("game")) {
        return found;
    }
    found[0] = '\0';
    snprintf(folder, sizeof(folder), "%s%cgame", Paths_ProgramDir(), SEPARATOR);
#ifdef _WIN32
    for (named = folder; *named; named++) {
        if (*named == '/') folder[named - folder] = '\\';   /* shown the way Explorer shows it */
    }
#endif
    snprintf(why, why_size,
             "The game's disc image was not found.\n\n"
             "This port needs your own copy of Yu-Gi-Oh! Forbidden Memories (USA, SLUS-01411) as a raw "
             ".bin image (the .bin of a .bin/.cue pair). Put it in the \"game\" folder next to the "
             "program:\n\n%s\n\nAny file name ending in .bin will do.",
             folder);
    return NULL;
}

unsigned char *GameFiles_ReadExecutable(const char *disc, size_t *size)
{
    FILE *file = fopen(disc, "rb");
    unsigned lba, length, at;
    unsigned char *data = NULL;
    if (!file) return NULL;
    if (find_executable(file, &lba, &length) && (data = malloc((length + SECTOR - 1) / SECTOR * SECTOR))) {
        for (at = 0; at < length; at += SECTOR) {
            if (!read_sector(file, lba + at / SECTOR, data + at)) {
                free(data);
                data = NULL;
                break;
            }
        }
    }
    fclose(file);
    if (data) *size = length;
    return data;
}
