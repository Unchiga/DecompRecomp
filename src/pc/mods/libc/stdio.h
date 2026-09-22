#ifndef MEMORIES_MOD_STDIO_H
#define MEMORIES_MOD_STDIO_H
/* A mod's <stdio.h> (README.md): streams the host opened for it, and
 * formatted text. There is no fopen, remove, rename or tmpfile. */
#include <stdarg.h>
#include <stddef.h>

typedef struct MemoriesModFile FILE;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 8192

#define MEMORIES_MOD_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define MEMORIES_MOD_SCANF(f, a) __attribute__((format(scanf, f, a)))

int printf(const char *format, ...) MEMORIES_MOD_PRINTF(1, 2);
int fprintf(FILE *file, const char *format, ...) MEMORIES_MOD_PRINTF(2, 3);
int sprintf(char *out, const char *format, ...) MEMORIES_MOD_PRINTF(2, 3);
int snprintf(char *out, size_t size, const char *format, ...) MEMORIES_MOD_PRINTF(3, 4);
int vprintf(const char *format, va_list arguments) MEMORIES_MOD_PRINTF(1, 0);
int vfprintf(FILE *file, const char *format, va_list arguments) MEMORIES_MOD_PRINTF(2, 0);
int vsprintf(char *out, const char *format, va_list arguments) MEMORIES_MOD_PRINTF(2, 0);
int vsnprintf(char *out, size_t size, const char *format, va_list arguments) MEMORIES_MOD_PRINTF(3, 0);
int sscanf(const char *text, const char *format, ...) MEMORIES_MOD_SCANF(2, 3);
int vsscanf(const char *text, const char *format, va_list arguments) MEMORIES_MOD_SCANF(2, 0);

int puts(const char *text);
int putchar(int c);
int fputc(int c, FILE *file);
int putc(int c, FILE *file);
int fputs(const char *text, FILE *file);
int fgetc(FILE *file);
int getc(FILE *file);
int ungetc(int c, FILE *file);
char *fgets(char *out, int size, FILE *file);
size_t fread(void *out, size_t size, size_t count, FILE *file);
size_t fwrite(const void *data, size_t size, size_t count, FILE *file);
int fseek(FILE *file, long offset, int whence);
long ftell(FILE *file);
void rewind(FILE *file);
int fflush(FILE *file);
int fclose(FILE *file);
int feof(FILE *file);
int ferror(FILE *file);
void clearerr(FILE *file);

#endif
