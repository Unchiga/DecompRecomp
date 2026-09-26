#ifndef MEMORIES_PC_STATE_REMAP_H
#define MEMORIES_PC_STATE_REMAP_H
#include <stddef.h>
#include <stdint.h>

/* Rebase native text pointers in a save image before restoring its chunks. */
void Memories_StateRemapImage(uint8_t *image, size_t image_size, uint32_t from, uint32_t to, uint32_t size);
#endif
