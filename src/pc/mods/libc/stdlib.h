#ifndef MEMORIES_MOD_STDLIB_H
#define MEMORIES_MOD_STDLIB_H
/* A mod's <stdlib.h> (README.md). rand() is the game's own generator, the
 * same sequence on every platform. There is no system, exit or atexit. */
#include <stddef.h>

#define RAND_MAX 0x7fffffff

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *memory, size_t size);
void free(void *memory);

int abs(int value);
long labs(long value);
long long llabs(long long value);
int atoi(const char *text);
long atol(const char *text);
double atof(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
double strtod(const char *text, char **end);
float strtof(const char *text, char **end);

int rand(void);
void srand(unsigned seed);
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));

char *getenv(const char *name);
__attribute__((noreturn)) void abort(void);

#endif
