/* The C library a code mod may call: a short, fixed list the host lends it
 * (exports.h). A mod is one object file for both systems, built without any
 * system headers (notes/modding.md), so it cannot link against glibc or the
 * Windows C runtime itself; the names below resolve to whichever one this
 * executable was linked with. They are the functions whose behaviour the two
 * agree on. Files are only ever opened by the host (open_asset, open_data),
 * so a mod gets the calls that use a FILE and not fopen.
 *
 * The compiler's own helpers are here too: 32-bit x86 code does 64-bit
 * division and some conversions by calling them, and both toolchains
 * (libgcc, compiler-rt) have the same ones under the same names.
 *
 * Adding a name here is a promise to every mod built afterwards; removing one
 * breaks the mods that use it. The SDK's headers (sdk/include) declare
 * exactly this list. */
#include "exports.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*Function)(void);

#if defined(__i386__)
/* The compiler's helpers have no header, and only 32-bit x86 has these. */
extern long long __divdi3(long long, long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);
extern unsigned long long __udivmoddi4(unsigned long long, unsigned long long, unsigned long long *);
extern long long __divmoddi4(long long, long long, long long *);
extern long long __fixdfdi(double);
extern long long __fixsfdi(float);
extern unsigned long long __fixunsdfdi(double);
extern unsigned long long __fixunssfdi(float);
extern double __floatdidf(long long);
extern float __floatdisf(long long);
extern double __floatundidf(unsigned long long);
extern float __floatundisf(unsigned long long);
#endif

/* rand as the C standard's own example has it, so a mod draws the same
 * numbers on both systems (glibc's and the Windows runtime's differ). One
 * sequence for all mods; the game's own random numbers are separate. */
static unsigned long mod_seed = 1;
static int mod_rand(void)
{
    mod_seed = mod_seed * 1103515245u + 12345u;
    return (int)(mod_seed / 65536u % 32768u);
}
static void mod_srand(unsigned seed) { mod_seed = seed; }

#define F(name) {#name, (Function)name}
#define AS(name, function) {#name, (Function)function}

/* Sorted by name (strcmp): Mods_Lookup searches it. */
static const struct { const char *name; Function function; } functions[] = {
#if defined(__i386__)
    F(__divdi3), F(__divmoddi4), F(__fixdfdi), F(__fixsfdi), F(__fixunsdfdi), F(__fixunssfdi),
    F(__floatdidf), F(__floatdisf), F(__floatundidf), F(__floatundisf), F(__moddi3), F(__udivdi3),
    F(__udivmoddi4), F(__umoddi3),
#endif
    F(abs), F(acos), F(asin), F(atan), F(atan2), F(atan2f), F(atoi), F(bsearch), F(calloc), F(ceil),
    F(ceilf), F(cos), F(cosf), F(exp), F(expf), F(fabs), F(fabsf), F(fclose), F(fgets), F(floor),
    F(floorf), F(fmod), F(fmodf), F(fread), F(free), F(fseek), F(ftell), F(fwrite), F(labs), F(log),
    F(log10), F(logf), F(malloc), F(memchr), F(memcmp), F(memcpy), F(memmove), F(memset), F(pow),
    F(powf), F(qsort), AS(rand, mod_rand), F(realloc), F(sin), F(sinf), F(snprintf), F(sqrt),
    F(sqrtf), AS(srand, mod_srand), F(strcat), F(strchr), F(strcmp), F(strcpy), F(strlen),
    F(strncat), F(strncmp), F(strncpy), F(strrchr), F(strstr), F(strtod), F(strtol), F(strtoul),
    F(tan), F(tanf), F(vsnprintf),
};

Function Mods_LibcLookup(const char *name)
{
    unsigned low = 0, high = sizeof(functions) / sizeof(functions[0]);
    while (low < high) {
        unsigned middle = (low + high) / 2;
        int order = strcmp(name, functions[middle].name);
        if (!order) return functions[middle].function;
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return NULL;
}

int Mods_LibcSorted(void)
{
    unsigned i;
    for (i = 1; i < sizeof(functions) / sizeof(functions[0]); i++) {
        if (strcmp(functions[i - 1].name, functions[i].name) >= 0) return 0;
    }
    return 1;
}
