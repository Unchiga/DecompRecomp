#include "pc/guest/state_remap.h"
#include <assert.h>
#include <string.h>

static size_t chunk(uint8_t *image, size_t at, const char *tag, const uint32_t *words, uint32_t length)
{
    memset(image + at, 0, 20);
    strcpy((char *)image + at, tag);
    memcpy(image + at + 16, &length, 4);
    memcpy(image + at + 20, words, length);
    return at + 20 + length;
}
static uint32_t word(const uint8_t *at) { uint32_t value; memcpy(&value, at, 4); return value; }

int main(void)
{
    uint8_t image[512] = {0}, truncated[512];
    uint32_t pointers[] = {0x1000, 0x1004, 0x1080, 0x1084, 0xffc};
    uint32_t data[] = {0x1004, 0x1004};
    size_t end = 16, memory, variables, stack, entry, native, bss;
    memory = end + 20; end = chunk(image, end, "memory", pointers, sizeof(pointers));
    variables = end + 20; end = chunk(image, end, "data:game", data, sizeof(data));
    stack = end + 20; end = chunk(image, end, "stack", pointers, sizeof(pointers));
    entry = end + 20; end = chunk(image, end, "entry", pointers, sizeof(pointers));
    native = end + 20; end = chunk(image, end, "deck-shop", pointers, sizeof(pointers));
    bss = end + 20; end = chunk(image, end, "bss:game", pointers, sizeof(pointers));
    memcpy(truncated, image, sizeof(image));
    Memories_StateRemapImage(image, end, 0x1000, 0x3000, 0x80);
    assert(word(image + memory) == 0x3000 && word(image + memory + 4) == 0x3004);
    assert(word(image + memory + 8) == 0x3080); /* one past the text */
    assert(word(image + memory + 12) == 0x1084 && word(image + memory + 16) == 0xffc);
    assert(word(image + variables) == 0x1004 && word(image + variables + 4) == 0x3004);
    assert(word(image + stack) == 0x3000 && word(image + entry) == 0x3000);
    assert(word(image + bss) == 0x3000 && word(image + native) == 0x1000);
    Memories_StateRemapImage(truncated, end - 1, 0x1000, 0x3000, 0x80);
    assert(word(truncated + bss) == 0x1000); /* incomplete chunk is untouched */
    return 0;
}
