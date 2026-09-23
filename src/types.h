#ifndef YUGIOH_TYPES_H
#define YUGIOH_TYPES_H

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
#if defined(MEMORIES_PC) && defined(__i386__)
/* 32-bit x86 Linux aligns a 64-bit integer to 4 inside a structure; the
 * retail layout (MIPS) and 32-bit Windows align it to 8. This makes all three
 * agree, which the game's memory image and a mod built once for both systems
 * rely on (tools/pc/check_layouts.py). */
typedef signed long long s64 __attribute__((aligned(8)));
typedef unsigned long long u64 __attribute__((aligned(8)));
#else
typedef signed long long s64;
typedef unsigned long long u64;
#endif

#endif
