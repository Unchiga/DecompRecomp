/* The C library a mod is given (modload.h), declared for mods by the headers
 * in src/pc/mods/libc. A mod is built once, against those headers, and runs
 * against the Linux and the Windows game alike, so every name here means one
 * thing on both: where the host libraries agree (the string, memory, maths
 * and formatted-text functions, which take the same arguments in the same
 * registers on both) the host's own function is given; where they differ
 * (the width of time_t, CLOCKS_PER_SEC, RAND_MAX, errno, mmap's flags) a
 * wrapper here gives the mod the one meaning its headers promise.
 *
 * What is left out is the point of it. There is no fopen, freopen, remove,
 * rename or tmpfile: the files a mod may read and write are the ones the
 * host hands it (modapi.h, open_asset and open_data), inside its own
 * directories. There is no socket, no system or exec, no dlopen and no
 * LoadLibrary, so a mod reaches no network, runs no program and loads no
 * other code. A name a mod uses that is in neither table refuses the load
 * and says which name it was. */
#define _POSIX_C_SOURCE 200809L
#include "modload.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pc/compat/mman.h"

/* --- what differs between the host libraries ------------------------- */

/* The mod headers' types: a 32-bit time_t and the Linux clock numbers. */
typedef struct {
    long tv_sec, tv_nsec;
} ModTimespec;
#define MOD_CLOCK_REALTIME 0
#define MOD_CLOCK_MONOTONIC 1
#define MOD_CLOCKS_PER_SEC 1000000l
/* The mod headers' flags, which are Linux's (compat/mman.h uses them too). */
#define MOD_MAP_ANONYMOUS 0x20
#define MOD_EACCES 13

static FILE *mod_stdout, *mod_stderr;

static int mod_clock_gettime(int clock, ModTimespec *out)
{
    struct timespec now;
    if (!out || clock_gettime(clock == MOD_CLOCK_MONOTONIC ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now)) return -1;
    out->tv_sec = (long)now.tv_sec;
    out->tv_nsec = now.tv_nsec;
    return 0;
}

static long mod_time(long *out)
{
    long now = (long)time(NULL);
    if (out) *out = now;
    return now;
}

static long mod_clock(void)
{
    return (long)((double)clock() * MOD_CLOCKS_PER_SEC / CLOCKS_PER_SEC);
}

/* One generator on both platforms, with the mod headers' RAND_MAX
 * (0x7fffffff): the hosts' rand() differ in range and in sequence. */
static unsigned long long rand_state = 1;
static void mod_srand(unsigned seed) { rand_state = seed; }
static int mod_rand(void)
{
    rand_state = rand_state * 6364136223846793005ull + 1442695040888963407ull;
    return (int)(rand_state >> 33);
}

static int *mod_errno_location(void) { return &errno; }

static void mod_assert_fail(const char *assertion, const char *file, unsigned line, const char *function)
{
    fprintf(stderr, "memories-pc: mod assertion failed: %s (%s:%u, %s)\n", assertion, file, line,
            function ? function : "?");
    fflush(stderr);
    abort();
}

/* Anonymous memory only: a mapped file would be a way round open_asset. */
static void *mod_mmap(void *address, size_t length, int prot, int flags, int fd, long offset)
{
    if (!(flags & MOD_MAP_ANONYMOUS) || fd != -1) {
        errno = MOD_EACCES;
        return MAP_FAILED;
    }
    return mmap(address, length, prot, flags, -1, offset);
}

static int mod_munmap(void *address, size_t length) { return munmap(address, length); }

static int mod_strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y || !x) return x - y;
    }
}

static int mod_strncasecmp(const char *a, const char *b, size_t count)
{
    for (; count; a++, b++, count--) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y || !x) return x - y;
    }
    return 0;
}

/* The helpers a 32-bit compiler calls for 64-bit division and for unsigned
 * 64-bit conversions. A mod is linked without a runtime library of its own,
 * so the host's compiler does the arithmetic here. */
static long long mod_divdi3(long long a, long long b) { return a / b; }
static long long mod_moddi3(long long a, long long b) { return a % b; }
static unsigned long long mod_udivdi3(unsigned long long a, unsigned long long b) { return a / b; }
static unsigned long long mod_umoddi3(unsigned long long a, unsigned long long b) { return a % b; }
static long long mod_fixdfdi(double a) { return (long long)a; }
static long long mod_fixsfdi(float a) { return (long long)a; }
static unsigned long long mod_fixunsdfdi(double a) { return (unsigned long long)a; }
static unsigned long long mod_fixunssfdi(float a) { return (unsigned long long)a; }
static double mod_floatdidf(long long a) { return (double)a; }
static float mod_floatdisf(long long a) { return (float)a; }
static double mod_floatundidf(unsigned long long a) { return (double)a; }
static float mod_floatundisf(unsigned long long a) { return (float)a; }

/* printf and friends by way of the v- forms, so that the mod gets the host's
 * formatting and never an inline copy the host headers might define. */
static int mod_printf(const char *format, ...)
{
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vfprintf(stdout, format, arguments);
    va_end(arguments);
    return written;
}

static int mod_fprintf(FILE *file, const char *format, ...)
{
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vfprintf(file, format, arguments);
    va_end(arguments);
    return written;
}

static int mod_sprintf(char *out, const char *format, ...)
{
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vsprintf(out, format, arguments);
    va_end(arguments);
    return written;
}

static int mod_snprintf(char *out, size_t size, const char *format, ...)
{
    va_list arguments;
    int written;
    va_start(arguments, format);
    written = vsnprintf(out, size, format, arguments);
    va_end(arguments);
    return written;
}

static int mod_sscanf(const char *text, const char *format, ...)
{
    va_list arguments;
    int read;
    va_start(arguments, format);
    read = vsscanf(text, format, arguments);
    va_end(arguments);
    return read;
}

static int mod_vprintf(const char *format, va_list arguments) { return vfprintf(stdout, format, arguments); }
static int mod_vfprintf(FILE *file, const char *format, va_list arguments) { return vfprintf(file, format, arguments); }
static int mod_vsprintf(char *out, const char *format, va_list arguments) { return vsprintf(out, format, arguments); }
static int mod_vsnprintf(char *out, size_t size, const char *format, va_list arguments)
{
    return vsnprintf(out, size, format, arguments);
}
static int mod_vsscanf(const char *text, const char *format, va_list arguments)
{
    return vsscanf(text, format, arguments);
}
static int mod_putchar(int c) { return fputc(c, stdout); }
static int mod_puts(const char *text) { return puts(text); }

void ModLibc_Start(void)
{
    mod_stdout = stdout;
    mod_stderr = stderr;
}

/* --- the table, by name ---------------------------------------------- */

#define HOST(name) {#name, (void *)(uintptr_t)&name}
#define MOD(name, function) {name, (void *)(uintptr_t)&function}

/* Sorted by name: ModLoad_Resolve searches it by halves. */
const ModSymbol Memories_ModLibc[] = {
    MOD("__assert_fail", mod_assert_fail),
    MOD("__divdi3", mod_divdi3),
    MOD("__errno_location", mod_errno_location),
    MOD("__fixdfdi", mod_fixdfdi),
    MOD("__fixsfdi", mod_fixsfdi),
    MOD("__fixunsdfdi", mod_fixunsdfdi),
    MOD("__fixunssfdi", mod_fixunssfdi),
    MOD("__floatdidf", mod_floatdidf),
    MOD("__floatdisf", mod_floatdisf),
    MOD("__floatundidf", mod_floatundidf),
    MOD("__floatundisf", mod_floatundisf),
    MOD("__moddi3", mod_moddi3),
    MOD("__udivdi3", mod_udivdi3),
    MOD("__umoddi3", mod_umoddi3),
    HOST(abort), HOST(abs), HOST(acos), HOST(acosf), HOST(asin), HOST(asinf), HOST(atan), HOST(atan2),
    HOST(atan2f), HOST(atanf), HOST(atof), HOST(atoi), HOST(atol), HOST(bsearch), HOST(calloc), HOST(cbrt),
    HOST(cbrtf), HOST(ceil), HOST(ceilf), HOST(clearerr),
    MOD("clock", mod_clock),
    MOD("clock_gettime", mod_clock_gettime),
    HOST(copysign), HOST(copysignf), HOST(cos), HOST(cosf), HOST(cosh), HOST(coshf), HOST(exp), HOST(exp2),
    HOST(exp2f), HOST(expf), HOST(fabs), HOST(fabsf), HOST(fclose), HOST(feof), HOST(ferror), HOST(fflush),
    HOST(fgetc), HOST(fgets), HOST(floor), HOST(floorf), HOST(fmax), HOST(fmaxf), HOST(fmin), HOST(fminf),
    HOST(fmod), HOST(fmodf),
    MOD("fprintf", mod_fprintf),
    HOST(fputc), HOST(fputs), HOST(fread), HOST(free), HOST(frexp), HOST(frexpf), HOST(fseek), HOST(ftell),
    HOST(fwrite),
    MOD("getc", fgetc),
    HOST(getenv), HOST(hypot), HOST(hypotf), HOST(labs), HOST(ldexp), HOST(ldexpf), HOST(llabs), HOST(llround),
    HOST(llroundf), HOST(log), HOST(log10), HOST(log10f), HOST(log2), HOST(log2f), HOST(logf), HOST(lround),
    HOST(lroundf), HOST(malloc), HOST(memchr), HOST(memcmp), HOST(memcpy), HOST(memmove), HOST(memset),
    MOD("mmap", mod_mmap),
    HOST(modf), HOST(modff),
    MOD("munmap", mod_munmap),
    HOST(pow), HOST(powf),
    MOD("printf", mod_printf),
    MOD("putc", fputc),
    MOD("putchar", mod_putchar),
    MOD("puts", mod_puts),
    HOST(qsort),
    MOD("rand", mod_rand),
    HOST(realloc), HOST(rewind), HOST(round), HOST(roundf), HOST(sin), HOST(sinf), HOST(sinh), HOST(sinhf),
    MOD("snprintf", mod_snprintf),
    MOD("sprintf", mod_sprintf),
    HOST(sqrt), HOST(sqrtf),
    MOD("srand", mod_srand),
    MOD("sscanf", mod_sscanf),
    MOD("stderr", mod_stderr),
    MOD("stdout", mod_stdout),
    MOD("strcasecmp", mod_strcasecmp),
    HOST(strcat), HOST(strchr), HOST(strcmp), HOST(strcpy), HOST(strcspn), HOST(strdup), HOST(strlen),
    MOD("strncasecmp", mod_strncasecmp),
    HOST(strncat), HOST(strncmp), HOST(strncpy), HOST(strnlen), HOST(strpbrk), HOST(strrchr), HOST(strspn),
    HOST(strstr), HOST(strtod), HOST(strtof), HOST(strtok), HOST(strtol), HOST(strtoll), HOST(strtoul),
    HOST(strtoull), HOST(tan), HOST(tanf), HOST(tanh), HOST(tanhf),
    MOD("time", mod_time),
    HOST(trunc), HOST(truncf), HOST(ungetc),
    MOD("vfprintf", mod_vfprintf),
    MOD("vprintf", mod_vprintf),
    MOD("vsnprintf", mod_vsnprintf),
    MOD("vsprintf", mod_vsprintf),
    MOD("vsscanf", mod_vsscanf),
};
const unsigned Memories_ModLibcCount = sizeof(Memories_ModLibc) / sizeof(Memories_ModLibc[0]);
