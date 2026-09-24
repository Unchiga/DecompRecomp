#ifndef MEMORIES_PC_COMPAT_FS_H
#define MEMORIES_PC_COMPAT_FS_H
/* Native paths and environment strings are UTF-8 on every platform. On
 * Windows, convert at the OS boundary, independent of the system code page.
 * Include this in native file-I/O units (and after feature-test defines).
 * No Windows headers here: their types conflict with the game's headers. */
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <wchar.h>
#include <sys/stat.h>
#include <dirent.h>
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#include <unistd.h>

/* Allocated conversions; invalid text fails with EILSEQ, never replacement
 * characters that could redirect a write. The caller frees the result. */
wchar_t *Memories_Utf8ToWide(const char *text);
char *Memories_WideToUtf8(const wchar_t *text);
FILE *Memories_Fopen(const char *path, const char *mode);
int Memories_Open(const char *path, int flags, ...);
int Memories_Access(const char *path, int mode);
int Memories_Mkdir(const char *path);
int Memories_Rmdir(const char *path);
int Memories_Remove(const char *path);
int Memories_Unlink(const char *path);
int Memories_Rename(const char *from, const char *to);
long Memories_Readlink(const char *path, char *out, size_t size);
char *Memories_Getenv(const char *name);
int Memories_Setenv(const char *name, const char *value, int overwrite);
int Memories_Unsetenv(const char *name);
int Memories_Mkstemp(char *pattern);
char *Memories_Mkdtemp(char *pattern);
/* UTF-8 argv from the original Unicode command line, owned for process life. */
char **Memories_Argv(int *argc);

/* stat's layout depends on _FILE_OFFSET_BITS in the caller. Keep this
 * adapter inline so large-disc callers retain their 64-bit file sizes. */
static inline int Memories_Stat(const char *path, struct stat *info)
{
    wchar_t *wide = Memories_Utf8ToWide(path);
    int result;
    if (!wide) return -1;
    result = wstat(wide, info);
    free(wide);
    return result;
}

typedef struct MemoriesDir MemoriesDir;
struct MemoriesDirent { char d_name[4 * 260]; };
MemoriesDir *Memories_Opendir(const char *path);
struct MemoriesDirent *Memories_Readdir(MemoriesDir *dir);
int Memories_Closedir(MemoriesDir *dir);

#ifndef MEMORIES_FS_IMPLEMENTATION
#define fopen Memories_Fopen
#define open Memories_Open
#define access Memories_Access
#define mkdir(path, mode) Memories_Mkdir(path)
#define rmdir Memories_Rmdir
#define remove Memories_Remove
#define unlink Memories_Unlink
#define rename Memories_Rename
#define readlink Memories_Readlink
#define stat(path, info) Memories_Stat(path, info)
#define getenv Memories_Getenv
#define setenv Memories_Setenv
#define unsetenv Memories_Unsetenv
#define mkstemp Memories_Mkstemp
#define mkdtemp Memories_Mkdtemp
#define DIR MemoriesDir
#define dirent MemoriesDirent
#define opendir Memories_Opendir
#define readdir Memories_Readdir
#define closedir Memories_Closedir
#endif
#endif
#endif
