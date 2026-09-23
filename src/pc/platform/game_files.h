#ifndef MEMORIES_PC_GAME_FILES_H
#define MEMORIES_PC_GAME_FILES_H
/* The player's own copy of the game: a raw (MODE2/2352) image of the USA
 * disc, SLUS-01411, the .bin of a .bin/.cue pair. Nothing of the game ships
 * with the port, so this is the one file a player has to bring.
 *
 * MEMORIES_DISC names it outright. Otherwise the first image of the right
 * disc is taken from, in turn: the `game` folder beside the executable, the
 * folder of the executable itself, the `game` folder in the user directory
 * (paths.h), and `game` in the current directory. Any file name will do; a
 * .bin that is not that disc is passed over. */
#include <stddef.h>

/* The image's path, or NULL with the reason (for the player) in `why`. */
const char *GameFiles_Disc(char *why, size_t why_size);
/* The game's executable, SLUS_014.11, read out of the disc image into a
 * fresh buffer (free it). NULL when it cannot be read. */
unsigned char *GameFiles_ReadExecutable(const char *disc, size_t *size);

#endif
