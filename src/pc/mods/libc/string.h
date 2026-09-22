#ifndef MEMORIES_MOD_STRING_H
#define MEMORIES_MOD_STRING_H
/* A mod's <string.h> (README.md). */
#include <stddef.h>

void *memcpy(void *out, const void *from, size_t size);
void *memmove(void *out, const void *from, size_t size);
void *memset(void *out, int value, size_t size);
int memcmp(const void *a, const void *b, size_t size);
void *memchr(const void *memory, int value, size_t size);

size_t strlen(const char *text);
size_t strnlen(const char *text, size_t limit);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t count);
char *strcpy(char *out, const char *from);
char *strncpy(char *out, const char *from, size_t count);
char *strcat(char *out, const char *from);
char *strncat(char *out, const char *from, size_t count);
char *strchr(const char *text, int c);
char *strrchr(const char *text, int c);
char *strstr(const char *text, const char *part);
size_t strspn(const char *text, const char *accept);
size_t strcspn(const char *text, const char *reject);
char *strpbrk(const char *text, const char *accept);
char *strtok(char *text, const char *separators);
char *strdup(const char *text);

#endif
