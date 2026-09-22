#ifndef MEMORIES_MOD_STRINGS_H
#define MEMORIES_MOD_STRINGS_H
/* A mod's <strings.h> (README.md): ASCII case-insensitive comparison. */
#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t count);

#endif
