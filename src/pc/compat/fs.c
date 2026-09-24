/* Windows UTF-8 filesystem boundary. Linux uses libc directly. */
#ifdef _WIN32
#define _CRT_RAND_S
#define MEMORIES_FS_IMPLEMENTATION
#include "fs.h"
#include <windows.h>
#include <shellapi.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>

wchar_t *Memories_Utf8ToWide(const char *text)
{
    int length;
    wchar_t *wide;
    if (!text) { errno = EINVAL; return NULL; }
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!length) { errno = EILSEQ; return NULL; }
    wide = malloc((size_t)length * sizeof(*wide));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length)) {
        free(wide); errno = EILSEQ; return NULL;
    }
    return wide;
}

char *Memories_WideToUtf8(const wchar_t *wide)
{
    int length;
    char *text;
    if (!wide) { errno = EINVAL; return NULL; }
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (!length) { errno = EILSEQ; return NULL; }
    text = malloc((size_t)length);
    if (!text) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text, length, NULL, NULL)) {
        free(text); errno = EILSEQ; return NULL;
    }
    return text;
}

FILE *Memories_Fopen(const char *path, const char *mode)
{
    wchar_t *wide = Memories_Utf8ToWide(path), *wide_mode;
    FILE *file;
    if (!wide) return NULL;
    wide_mode = Memories_Utf8ToWide(mode);
    file = wide_mode ? _wfopen(wide, wide_mode) : NULL;
    free(wide_mode); free(wide);
    return file;
}

int Memories_Open(const char *path, int flags, ...)
{
    wchar_t *wide = Memories_Utf8ToWide(path);
    int mode = 0, result;
    if (!wide) return -1;
    if (flags & _O_CREAT) {
        va_list args;
        va_start(args, flags); mode = va_arg(args, int); va_end(args);
    }
    result = _wopen(wide, flags, mode);
    free(wide);
    return result;
}

int Memories_Access(const char *path, int mode)
{
    wchar_t *wide = Memories_Utf8ToWide(path);
    int result;
    if (!wide) return -1;
    /* Windows has no execute bit; directory creation uses X_OK. */
    result = _waccess(wide, mode & 6);
    free(wide);
    return result;
}

#define PATH_OPERATION(name, operation) \
int name(const char *path) { \
    wchar_t *wide = Memories_Utf8ToWide(path); \
    int result; \
    if (!wide) return -1; \
    result = operation(wide); free(wide); return result; \
}
PATH_OPERATION(Memories_Mkdir, _wmkdir)
PATH_OPERATION(Memories_Rmdir, _wrmdir)
PATH_OPERATION(Memories_Remove, _wremove)
PATH_OPERATION(Memories_Unlink, _wunlink)

int Memories_Rename(const char *from, const char *to)
{
    wchar_t *a = Memories_Utf8ToWide(from), *b;
    int result;
    if (!a) return -1;
    b = Memories_Utf8ToWide(to);
    /* Write-through: on the disk when it returns. A file another program
     * holds for a moment (a cloud sync or a virus scanner looking at
     * Documents) refuses the replace; that is tried again for a second. */
    for (int attempt = 0;; attempt++) {
        result = b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
        if (!b || !result || attempt == 20) break;
        if (GetLastError() != ERROR_ACCESS_DENIED && GetLastError() != ERROR_SHARING_VIOLATION) break;
        Sleep(50);
    }
    if (b && result) {
        switch (GetLastError()) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
        case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
        case ERROR_NOT_SAME_DEVICE: errno = EXDEV; break;
        case ERROR_DISK_FULL: errno = ENOSPC; break;
        default: errno = EIO; break;
        }
    }
    free(a); free(b);
    return result;
}

long Memories_Readlink(const char *path, char *out, size_t size)
{
    wchar_t *wide;
    char *text;
    DWORD length, capacity = 32768;
    size_t bytes, i;
    if (strcmp(path, "/proc/self/exe")) { errno = EINVAL; return -1; }
    wide = malloc(capacity * sizeof(*wide));
    if (!wide) return -1;
    length = GetModuleFileNameW(NULL, wide, capacity);
    if (!length || length >= capacity) { free(wide); errno = ENAMETOOLONG; return -1; }
    text = Memories_WideToUtf8(wide);
    free(wide);
    if (!text) return -1;
    bytes = strlen(text);
    if (bytes >= size) { free(text); errno = ENAMETOOLONG; return -1; }
    for (i = 0; i < bytes; i++) out[i] = text[i] == '\\' ? '/' : text[i];
    free(text);
    return (long)bytes; /* readlink does not append a terminator */
}

/* Separate cache entries keep getenv pointers valid when another variable
 * is read. Refresh from the OS, including changes made by Win32/SDL code. */
typedef struct EnvironmentEntry { struct EnvironmentEntry *next; char *name, *value; } EnvironmentEntry;
static EnvironmentEntry *environment;
static SRWLOCK environment_lock = SRWLOCK_INIT;

char *Memories_Getenv(const char *name)
{
    wchar_t *wide_name = Memories_Utf8ToWide(name), *wide;
    EnvironmentEntry *entry;
    char *value = NULL, *result = NULL;
    DWORD size, got;
    if (!wide_name) return NULL;
    AcquireSRWLockExclusive(&environment_lock);
    /* Retry if an external writer grows the value between the two calls. */
    size = GetEnvironmentVariableW(wide_name, NULL, 0);
    while (size) {
        wide = malloc((size_t)size * sizeof(*wide));
        if (!wide) break;
        got = GetEnvironmentVariableW(wide_name, wide, size);
        if (got && got < size) value = Memories_WideToUtf8(wide);
        free(wide);
        if (got < size) break;
        size = got + 1;
    }
    for (entry = environment; entry && strcmp(entry->name, name); entry = entry->next) {}
    if (value && !entry) {
        entry = calloc(1, sizeof(*entry));
        if (entry) {
            entry->name = strdup(name);
            if (!entry->name) { free(entry); entry = NULL; }
            else { entry->next = environment; environment = entry; }
        }
    }
    if (entry) {
        if (!entry->value || !value || strcmp(entry->value, value)) {
            free(entry->value); entry->value = value; value = NULL;
        }
        result = entry->value;
    }
    free(value); free(wide_name);
    ReleaseSRWLockExclusive(&environment_lock);
    return result;
}

int Memories_Setenv(const char *name, const char *value, int overwrite)
{
    wchar_t *wide_name, *wide_value;
    int result;
    if (!name || !*name || strchr(name, '=') || !value) { errno = EINVAL; return -1; }
    if (!overwrite && Memories_Getenv(name)) return 0;
    wide_name = Memories_Utf8ToWide(name);
    if (!wide_name) return -1;
    wide_value = Memories_Utf8ToWide(value);
    result = wide_value ? _wputenv_s(wide_name, wide_value) : EILSEQ;
    free(wide_name); free(wide_value);
    if (result) { errno = result; return -1; }
    return 0;
}
int Memories_Unsetenv(const char *name) { return Memories_Setenv(name, "", 1); }

static int temporary(char *pattern, int directory)
{
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    size_t length = strlen(pattern), i;
    unsigned random;
    int attempt, result;
    if (length < 6 || strcmp(pattern + length - 6, "XXXXXX")) { errno = EINVAL; return -1; }
    for (attempt = 0; attempt < 100; attempt++) {
        if (rand_s(&random)) { errno = EIO; return -1; }
        for (i = length - 6; i < length; i++) { pattern[i] = alphabet[random % 62]; random /= 62; }
        result = directory ? Memories_Mkdir(pattern) : Memories_Open(pattern, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                                                                     _S_IREAD | _S_IWRITE);
        if (result >= 0 || errno != EEXIST) return result;
    }
    errno = EEXIST;
    return -1;
}
int Memories_Mkstemp(char *pattern) { return temporary(pattern, 0); }
char *Memories_Mkdtemp(char *pattern) { return temporary(pattern, 1) == 0 ? pattern : NULL; }

struct MemoriesDir { _WDIR *wide; struct MemoriesDirent entry; };
MemoriesDir *Memories_Opendir(const char *path)
{
    wchar_t *wide = Memories_Utf8ToWide(path);
    MemoriesDir *dir;
    if (!wide) return NULL;
    dir = calloc(1, sizeof(*dir));
    if (dir) {
        dir->wide = _wopendir(wide);
        if (!dir->wide) { free(dir); dir = NULL; }
    }
    free(wide);
    return dir;
}
struct MemoriesDirent *Memories_Readdir(MemoriesDir *dir)
{
    struct _wdirent *entry = _wreaddir(dir->wide);
    if (!entry) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry->d_name, -1, dir->entry.d_name,
                            sizeof(dir->entry.d_name), NULL, NULL)) { errno = EILSEQ; return NULL; }
    return &dir->entry;
}
int Memories_Closedir(MemoriesDir *dir)
{
    int result = _wclosedir(dir->wide);
    free(dir);
    return result;
}

char **Memories_Argv(int *argc)
{
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), argc);
    char **args;
    int i;
    if (!wide) return NULL;
    args = calloc((size_t)*argc + 1, sizeof(*args));
    if (args) {
        for (i = 0; i < *argc; i++) {
            args[i] = Memories_WideToUtf8(wide[i]);
            if (!args[i]) {
                while (i) free(args[--i]);
                free(args); args = NULL; break;
            }
        }
    }
    LocalFree(wide);
    return args;
}
#else
typedef int memories_fs_uses_host_libc;
#endif
