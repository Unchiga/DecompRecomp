#ifndef MEMORIES_PC_COMPAT_MMAN_H
#define MEMORIES_PC_COMPAT_MMAN_H
/* <sys/mman.h> for the one pattern the port uses: anonymous read/write memory
 * at a fixed address (MAP_FIXED_NOREPLACE). Windows maps it with VirtualAlloc,
 * which also refuses an address that is already taken. */
#ifdef _WIN32
#include <stddef.h>
/* Declared by hand: <windows.h> would bring RECT and other names that clash
 * with the game's own types. Same signatures as the SDK (LPVOID, SIZE_T, DWORD). */
#ifndef _WINDOWS_
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, unsigned long size, unsigned long type,
                                                   unsigned long protect);
__declspec(dllimport) int __stdcall VirtualFree(void *address, unsigned long size, unsigned long type);
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
#endif

#define PROT_READ 0x1
#define PROT_WRITE 0x2
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
#else
#include <sys/mman.h>
#endif
#endif
