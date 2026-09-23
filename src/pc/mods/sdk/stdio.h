#ifndef MEMORIES_SDK_STDIO_H
#define MEMORIES_SDK_STDIO_H
/* A mod's stdio: the files the host opens for it (open_asset, open_data in
 * modapi.h) and formatting into memory. There is no fopen, printf or stdout:
 * a mod names files through the host and writes to the log with host->log.
 * FILE is opaque; it is whichever C library the game was linked with. */
#include <stdarg.h>
#include <stddef.h>

typedef struct MemoriesSdkFile FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

size_t fread(void *buffer, size_t size, size_t count, FILE *file);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *file);
int fseek(FILE *file, long offset, int origin);
long ftell(FILE *file);
int fclose(FILE *file);
char *fgets(char *buffer, int size, FILE *file);
int snprintf(char *buffer, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments) __attribute__((format(printf, 3, 0)));

#endif
