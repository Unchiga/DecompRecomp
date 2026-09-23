#ifndef MEMORIES_SDK_STDLIB_H
#define MEMORIES_SDK_STDLIB_H
/* A mod's stdlib: memory, numbers from text, sorting. No getenv (a mod's
 * knobs are its settings, host->setting), no exit, no system. */
#include <stddef.h>

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
int abs(int value);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));

#endif
