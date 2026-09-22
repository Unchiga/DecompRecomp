#ifndef MEMORIES_PC_COMPAT_DLFCN_H
#define MEMORIES_PC_COMPAT_DLFCN_H
/* <dlfcn.h> for the one use the port makes of it: loading a mod's library
 * and finding its entry point (src/pc/mods/mods.c). On Windows the library is
 * a DLL, and its references to the game bind through the executable's import
 * library at load time, as -rdynamic binds them on Linux. */
#ifdef _WIN32
#include <stdio.h>
/* Declared by hand: <windows.h> would bring RECT and other names that clash
 * with the game's own types. Same signatures as the SDK. */
#ifndef _WINDOWS_
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *path);
__declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
#endif

#define RTLD_NOW 2
#define RTLD_LOCAL 0

static inline void *dlopen(const char *path, int flags)
{
    (void)flags;
    return LoadLibraryA(path);
}

static inline void *dlsym(void *handle, const char *name)
{
    return GetProcAddress(handle, name);
}

/* The last LoadLibrary error, numbered as Windows reports it. */
static inline const char *dlerror(void)
{
    static char text[64];
    snprintf(text, sizeof(text), "cannot load the library (Windows error %lu)", GetLastError());
    return text;
}
#else
#include <dlfcn.h>
#endif
#endif
