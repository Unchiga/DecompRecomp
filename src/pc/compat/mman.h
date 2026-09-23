#ifndef MEMORIES_PC_COMPAT_MMAN_H
#define MEMORIES_PC_COMPAT_MMAN_H
/* <sys/mman.h> for the patterns the port uses: anonymous read/write memory,
 * at a fixed address (MAP_FIXED_NOREPLACE) or anywhere, and turning some of
 * it into code (mprotect, for mods: src/pc/mods/object_loader.c). Windows
 * maps it with VirtualAlloc, which also refuses an address that is already
 * taken. */
#ifdef _WIN32
#include <stddef.h>
/* Declared by hand: <windows.h> would bring RECT and other names that clash
 * with the game's own types. Same signatures as the SDK (LPVOID, SIZE_T, DWORD). */
#ifndef _WINDOWS_
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, unsigned long size, unsigned long type,
                                                   unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, unsigned long size, unsigned long type);
__declspec(dllimport) int __stdcall VirtualProtect(void *address, unsigned long size, unsigned long protect,
                                                   unsigned long *old_protect);
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#endif

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)

static inline void *mmap(void *address, size_t length, int prot, int flags, int fd, long long offset)
{
    void *p;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    p = VirtualAlloc(address, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p ? p : MAP_FAILED;
}

static inline int munmap(void *address, size_t length)
{
    (void)length;
    return VirtualFree(address, 0, MEM_RELEASE) ? 0 : -1;
}

static inline int mprotect(void *address, size_t length, int prot)
{
    unsigned long old;
    unsigned long protect = (prot & PROT_EXEC) ? ((prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                                               : ((prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY);
    return VirtualProtect(address, (unsigned long)length, protect, &old) ? 0 : -1;
}
#else
#include <sys/mman.h>
#endif
#endif
