#include "state_remap.h"
#include <string.h>

void Memories_StateRemapImage(uint8_t *image, size_t image_size, uint32_t from, uint32_t to, uint32_t size)
{
    size_t at = 16;
    if (!size || from == to) return;
    while (at <= image_size && image_size - at >= 20) {
        uint32_t length;
        size_t offset = 0;
        char tag[17];
        memcpy(tag, image + at, 16);
        tag[16] = 0;
        memcpy(&length, image + at + 16, 4);
        if (length > image_size - at - 20) return;
        if (!strncmp(tag, "data:", 5)) {
            /* The first half is the startup image, used to recognize data
             * the game never changed. Only the saved half has live pointers. */
            offset = length / 2;
        } else if (strcmp(tag, "memory") && strncmp(tag, "bss:", 4) &&
                   strcmp(tag, "stack") && strcmp(tag, "entry")) {
            at += 20 + length;
            continue;
        }
        for (; offset + 4 <= length; offset += 4) {
            uint32_t word;
            uint8_t *bytes = image + at + 20 + offset;
            memcpy(&word, bytes, 4);
            if (word >= from && word - from <= size) {
                word = to + (word - from); /* includes the end-of-stream pointer */
                memcpy(bytes, &word, 4);
            }
        }
        at += 20 + length;
    }
}
