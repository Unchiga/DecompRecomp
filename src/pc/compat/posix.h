#ifndef MEMORIES_PC_COMPAT_POSIX_H
#define MEMORIES_PC_COMPAT_POSIX_H
/* The few POSIX calls the port makes that the Windows C runtime spells
 * differently or lacks. Elsewhere this is the system's own headers. */
#include <sys/stat.h>
#include <time.h>
#ifndef _MSC_VER /* the CMake core tests also build with MSVC, which has no unistd.h */
#include <unistd.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No permission bits on Windows; folders get the default ones. */
#define mkdir(path, mode) _mkdir(path)
#define fsync(fd) _commit(fd)

static inline struct tm *memories_localtime_r(const time_t *when, struct tm *out)
{
    return localtime_s(out, when) ? NULL : out;
}
#define localtime_r memories_localtime_r

/* Seek and read: not atomic, which is enough while one thread reads the disc. */
static inline long memories_pread(int fd, void *buffer, size_t count, long long offset)
{
    if (_lseeki64(fd, offset, SEEK_SET) != offset) return -1;
    return _read(fd, buffer, (unsigned)count);
}
#define pread memories_pread

/* The C runtime has no setenv/unsetenv; _putenv_s takes an empty value as
 * removal. Used by the port's restart and by the tests, which set the
 * MEMORIES_* switches the code under test reads. */
static inline int memories_setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
#define setenv memories_setenv

static inline int memories_unsetenv(const char *name)
{
    return _putenv_s(name, "") ? -1 : 0;
}
#define unsetenv memories_unsetenv

/* POSIX rename replaces an existing file, which the port's atomic saves
 * (states, settings, controls, memory cards) rely on; Windows' does not. */
#ifndef _WINDOWS_
__declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to, unsigned long flags);
#endif
static inline int memories_rename(const char *from, const char *to)
{
    return MoveFileExA(from, to, 1 /* MOVEFILE_REPLACE_EXISTING */) ? 0 : -1;
}
#define rename memories_rename

/* Only /proc/self/exe is asked for: the executable's path, with forward
 * slashes like the paths the port builds. Declared by hand: <windows.h>
 * clashes with the game's types. */
#ifndef _WINDOWS_
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *module, char *path, unsigned long size);
#endif
static inline long memories_readlink(const char *path, char *out, size_t size)
{
    unsigned long length, i;
    if (strcmp(path, "/proc/self/exe") != 0) return -1;
    length = GetModuleFileNameA(NULL, out, (unsigned long)size);
    if (!length || length >= size) return -1;
    for (i = 0; i < length; i++) {
        if (out[i] == '\\') out[i] = '/';
    }
    return (long)length;
}
#define readlink memories_readlink
#endif
#endif
