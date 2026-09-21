#ifndef MEMORIES_PC_SDK_DISC_H
#define MEMORIES_PC_SDK_DISC_H
/* Complete queued drive commands and deliver ready sectors. Interrupt
 * context: called from the 1 kHz interrupt tick. */
#include <stdint.h>
void Memories_DiscService(uint64_t now_us);
/* Complete one pending DecDCTout request and run its callback. */
void Memories_MdecService(void);
/* Read `sectors` 2048-byte user-data blocks from the disc image, starting at
 * absolute `lba`, outside the drive model: no queue, no head movement, no
 * delivery callbacks, so it does not disturb a transfer the game has running.
 * Native extras (src/pc/mods) fetch whole records with it. Returns the number
 * of sectors read. */
int Memories_DiscReadSectors(int lba, int sectors, void *out);
/* The first sector of a file, by its retail path ("\\DATA\\MODEL.MRG;1"),
 * or -1. */
int Memories_DiscFileStart(const char *path);
void Memories_DiscStats(int *head_lba, unsigned *bytes_per_second);
#endif
