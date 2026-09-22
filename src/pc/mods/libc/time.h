#ifndef MEMORIES_MOD_TIME_H
#define MEMORIES_MOD_TIME_H
/* A mod's <time.h> (README.md): 32-bit seconds, Linux's clock numbers and a
 * microsecond clock(), on every platform. */
#include <stddef.h>

typedef long time_t;
typedef long clock_t;
typedef int clockid_t;
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCKS_PER_SEC 1000000l

int clock_gettime(clockid_t clock, struct timespec *now);
time_t time(time_t *now);
clock_t clock(void);

#endif
