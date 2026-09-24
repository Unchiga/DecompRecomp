#ifndef MEMORIES_PC_CARDS_ART_H
#define MEMORIES_PC_CARDS_ART_H
/* Custom card artwork (art.c): the first 0x3060 bytes of a card's art record,
 * made from a mod's PNGs and its name. */
#include <stddef.h>

#define CARD_ART_WIDTH 102
#define CARD_ART_HEIGHT 96
#define CARD_THUMB_WIDTH 40
#define CARD_THUMB_HEIGHT 32
#define CARD_TITLE_WIDTH 96
#define CARD_TITLE_HEIGHT 14

#define CARD_ART_PIXELS 0x0000
#define CARD_ART_CLUT 0x2640
#define CARD_TITLE_PIXELS 0x2840
#define CARD_TITLE_BYTES 0x2A0
#define CARD_THUMB_PIXELS 0x2AE0     /* also the start of the duel's 0x580-byte block */
#define CARD_THUMB_CLUT 0x2FE0
#define CARD_THUMB_BLOCK 0x580
#define CARD_ART_RECORD 0x3060

/* 1 on success; `why` says what went wrong otherwise. */
int CardArt_FromImage(const char *path, unsigned char *record, char *why, size_t why_size);
int CardArt_ThumbnailFromImage(const char *path, unsigned char *record, char *why, size_t why_size);
/* The title plate alone, CARD_TITLE_BYTES, what a record holds at
 * CARD_TITLE_PIXELS. */
int CardArt_TitleFromImage(const char *path, unsigned char *plate, char *why, size_t why_size);
/* `name` in UTF-8. 0 when no serif font could be found; the plate is left
 * as it was. */
int CardArt_TitleFromName(const char *name, unsigned char *plate);

#endif
