/* A mod library for tests/pc/modload_test.c: every kind of relocation a
 * compiled mod carries, the C library it is given, and the game's own names,
 * each checked by the test through one of the functions below. */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

/* Stand-ins for the game, from the test's own symbol table. */
extern int Memories_TestValue;
int Memories_TestTwice(int value);

static int constructed;
__attribute__((constructor)) static void construct(void) { constructed = 42; }

/* Pointers in initialised data: R_386_RELATIVE and R_386_32. */
static const char *const words[] = {"alpha", "beta", "gamma"};
int *const value_address = &Memories_TestValue;
int (*const twice_address)(int) = Memories_TestTwice;

int fixture_constructed(void) { return constructed; }
const char *fixture_word(volatile int index) { return words[index]; }
int *fixture_value_address(void) { return value_address; }
int (*fixture_twice_address(void))(int) { return twice_address; }
int fixture_call_twice(int value) { return Memories_TestTwice(value) + Memories_TestValue; }

/* 64-bit division, which a 32-bit compiler hands to a runtime helper. */
long long fixture_divide(volatile long long a, volatile long long b) { return a / b; }
unsigned long long fixture_modulo(volatile unsigned long long a, volatile unsigned long long b) { return a % b; }

int fixture_format(char *out, int size)
{
    return snprintf(out, (size_t)size, "%s %d %.2f %lld", words[1], (int)strlen("four"), sqrt(2.0),
                    (long long)1 << 40);
}

int fixture_clock(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_nsec >= 0 && now.tv_nsec < 1000000000;
}

/* Anonymous memory is given; a mapping of a file is refused. */
int fixture_mmap(void)
{
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 0;
    ((char *)page)[4095] = 1;
    munmap(page, 4096);
    errno = 0;
    return mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, 3, 0) == MAP_FAILED && errno == EACCES;
}
