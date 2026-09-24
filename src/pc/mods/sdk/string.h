#ifndef MEMORIES_SDK_STRING_H
#define MEMORIES_SDK_STRING_H
/* A mod's string.h. Mods are built freestanding, so the compiler does not
 * assume these are the standard functions; the macros give it back that
 * knowledge for the ones worth inlining (a short memcpy becomes moves). */
#include <stddef.h>

void *memcpy(void *to, const void *from, size_t size);
void *memmove(void *to, const void *from, size_t size);
void *memset(void *to, int value, size_t size);
int memcmp(const void *a, const void *b, size_t size);
void *memchr(const void *in, int value, size_t size);
size_t strlen(const char *text);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t size);
char *strchr(const char *text, int value);
char *strrchr(const char *text, int value);
char *strstr(const char *text, const char *wanted);
char *strncpy(char *to, const char *from, size_t size);
char *strcpy(char *to, const char *from);
char *strcat(char *to, const char *from);
char *strncat(char *to, const char *from, size_t size);

#define memcpy(to, from, size) __builtin_memcpy(to, from, size)
#define memmove(to, from, size) __builtin_memmove(to, from, size)
#define memset(to, value, size) __builtin_memset(to, value, size)
#define memcmp(a, b, size) __builtin_memcmp(a, b, size)
#define strlen(text) __builtin_strlen(text)

#endif
