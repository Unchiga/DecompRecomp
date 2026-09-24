/* Failures on purpose, to check that each kind ends in a report
 * (tools/pc/crash_check.py). MEMORIES_CRASH_TEST=<kind>@<frame>:
 *
 *   segv      a write to a page that is not there, on the game's thread
 *   thread    the same on a thread of its own
 *   overflow  unbounded recursion on the game's thread
 *   abort     abort()
 *   fatal     an error the game cannot go on from (exit 70)
 *   hang      the game's thread sleeps for ever
 *   deadlock  the game's thread waits for a lock it holds
 *   tickhang  the clock's tick never returns (the game's own watchdog
 *             runs there on Linux, so only the monitor sees it)
 *   kill      SIGKILL, as the out-of-memory killer sends (Linux); a
 *             fail-fast, which no handler sees (Windows)
 *   slow      no frame for 8 s, then on: a long wait, not a freeze
 *   null      a write to address 0 (MEMORIES_CRASH_TEST=1, as before)
 *   spin      the game's thread spins for ever (MEMORIES_HANG_TEST=1)
 *   restart   the game restarts itself (the mods window's restart), and
 *             the game after it stops as "fatal" does: the monitor follows
 *             a restart
 *
 * The frame defaults to 60. */
#define _GNU_SOURCE
#include "crash_test.h"
#include "crash.h"
#include "monitor.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

unsigned Memories_PresentedFrames(void);
int Platform_RestartGame(void); /* platform.h brings types that clash with <windows.h> */

volatile int CrashTest_TickHang;
static const char *kind;
static unsigned frame;
static int done;

static volatile int *missing_page(void)
{
#ifdef _WIN32
    return VirtualAlloc(NULL, 4096, MEM_RESERVE, PAGE_NOACCESS);
#else
    return mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
}

static void *crash_thread(void *unused)
{
    (void)unused;
    *missing_page() = 1;
    return NULL;
}

static int recurse(volatile int depth)
{
    volatile char pad[256];
    pad[0] = (char)depth;
    return recurse(depth + 1) + pad[0];
}

static void rest(unsigned seconds)
{
#ifdef _WIN32
    Sleep(seconds * 1000u);
#else
    struct timespec pause = {(time_t)seconds, 0};
    while (nanosleep(&pause, &pause)) {
    }
#endif
}

void CrashTest_Init(void)
{
    const char *text = getenv("MEMORIES_CRASH_TEST"), *hang = getenv("MEMORIES_HANG_TEST");
    const char *at;
    static char name[16];
    if ((!text || !*text) && hang && *hang) text = "spin";
    if (!text || !*text) return;
    if (!strcmp(text, "1")) text = "null";
    at = strchr(text, '@');
    snprintf(name, sizeof(name), "%.*s", at ? (int)(at - text) : (int)strlen(text), text);
    kind = name;
    frame = at ? (unsigned)strtoul(at + 1, NULL, 10) : 60;
    fprintf(stderr, "memories-pc: crash test: %s at frame %u\n", kind, frame);
}

void CrashTest_Frame(void)
{
    if (!kind || done || Memories_PresentedFrames() < frame) return;
    done = 1;
    fprintf(stderr, "memories-pc: crash test: %s now\n", kind);
    if (!strcmp(kind, "segv")) {
        *missing_page() = 1;
    } else if (!strcmp(kind, "thread")) {
        pthread_t thread;
        pthread_create(&thread, NULL, crash_thread, NULL);
        pthread_join(thread, NULL);
    } else if (!strcmp(kind, "overflow")) {
        recurse(0);
    } else if (!strcmp(kind, "abort")) {
        abort();
    } else if (!strcmp(kind, "fatal")) {
        Crash_ReportFatal("crash test", "MEMORIES_CRASH_TEST=fatal");
        exit(70);
    } else if (!strcmp(kind, "hang")) {
        for (;;) rest(60);
    } else if (!strcmp(kind, "deadlock")) {
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&lock);
        pthread_mutex_lock(&lock);
    } else if (!strcmp(kind, "tickhang")) {
        CrashTest_TickHang = 1;
        for (;;) rest(60);
    } else if (!strcmp(kind, "kill")) {
#ifdef _WIN32
        __fastfail(7);
#else
        raise(SIGKILL);
#endif
    } else if (!strcmp(kind, "slow")) {
        rest(8);
    } else if (!strcmp(kind, "restart")) {
        if (!Monitor_Active()) {
            fprintf(stderr, "memories-pc: crash test: restart needs the monitor\n");
        } else if (Monitor_Shared()->generation < 2) {
            Platform_RestartGame();
        } else {
            Crash_ReportFatal("crash test", "MEMORIES_CRASH_TEST=restart, after the restart");
            exit(70);
        }
    } else if (!strcmp(kind, "null")) {
        *(volatile int *)(uintptr_t)0 = 1;
    } else if (!strcmp(kind, "spin")) {
        volatile unsigned spin = 0;
        for (;;) spin++;
    } else {
        fprintf(stderr, "memories-pc: crash test: no kind %s\n", kind);
    }
}
