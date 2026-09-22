/* What every window backend shares: the interrupt-style clock and the
 * scripted test input. The 1 kHz SIGALRM stands in for the console's
 * interrupts and must fire on the main thread, between instructions of the
 * game (busy-waits poll what the handlers update); backends block signals
 * on every thread they create. Windows interrupts the main thread from a
 * timer thread instead (win32.c). */
#define _GNU_SOURCE
#include "platform.h"
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include "pc/debug/crash.h"
#include "pc/debug/profile.h"
#include "pc/compat/signal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#include "win32.h"
#else
#include <sys/syscall.h>
#include <ucontext.h>
#endif

static volatile unsigned vblank_count;
static void (*vblank_handler)(void);
static void (*tick_handler)(uint64_t, uint64_t);
static volatile int rate = 100;
static uint64_t real_prev, virtual_now, next_vblank;
static volatile unsigned vblank_period = 16683;
static volatile int step_pending;
static float present_refresh;
static int present_cap;           /* frames per second; 0 display refresh; -1 every frame */
static uint64_t present_next_us;  /* the present pacer's next slot */
static uint64_t last_vsync_real;
static unsigned watchdog_seconds = 5;
static volatile int watchdog_reported;
static int deterministic_dump;

static uint64_t now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static void deliver_vblank(void)
{
    vblank_count++;
    if (vblank_handler) vblank_handler();
}

static void advance(uint64_t real_now)
{
    uint64_t elapsed = real_prev ? real_now - real_prev : 0;
    real_prev = real_now;
    if (elapsed > 100000) elapsed = 0;
    if (deterministic_dump && rate == -1) {
        /* Timer-driven disc waits still need progress, but a fixed tick keeps
         * their completion frame independent of host scheduling. */
        virtual_now += 1000;
        if (tick_handler) tick_handler(virtual_now, real_now);
        return;
    }
    if (rate > 0) virtual_now += elapsed * (uint64_t)rate / 100;
    if (rate == -1) virtual_now += elapsed;
    if (tick_handler) tick_handler(virtual_now, real_now);
    if (!next_vblank) next_vblank = virtual_now;
    while (virtual_now >= next_vblank) {
        next_vblank += vblank_period;
        if (virtual_now > next_vblank + 4 * vblank_period) next_vblank = virtual_now;
        deliver_vblank();
    }
    if (step_pending) {
        step_pending = 0;
        deliver_vblank();
    }
}

static void on_tick(uintptr_t eip, void *context)
{
    uint64_t real_now = now_us();
    Profile_Sample(eip);
#ifndef _WIN32 /* Windows watches from the clock thread (Win32_SetStallReporter) */
    if (watchdog_seconds && rate != 0 && !watchdog_reported &&
        real_now - last_vsync_real >= (uint64_t)watchdog_seconds * 1000000u) {
        watchdog_reported = 1;
        Crash_ReportHang(context);
    }
#else
    (void)context;
#endif
    advance(real_now);
}

#ifndef _WIN32
static void on_alarm(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    (void)number;
    (void)info;
    on_tick((uintptr_t)user->uc_mcontext.gregs[REG_EIP], context);
}
#endif

/* The 1 kHz signal is aimed at the main thread itself (SIGEV_THREAD_ID), not
 * the process: a process-directed signal lands on any thread that does not
 * block it, and graphics drivers start threads of their own after the
 * backend has finished creating its own with the signal masked. With the
 * game's interrupt code running on a driver thread the main thread stalls
 * on that driver's locks. The process-wide timer is only a fallback. */
int Platform_StartTimers(void (*tick)(uint64_t, uint64_t), void (*vblank)(void))
{
#ifndef _WIN32
    struct sigaction action;
    struct sigevent event;
    struct itimerspec spec;
    timer_t timer;
#endif
    tick_handler = tick;
    vblank_handler = vblank;
    deterministic_dump = getenv("MEMORIES_HEADLESS") != NULL && getenv("MEMORIES_DUMP_FRAME") != NULL;
    last_vsync_real = now_us();
    {
        const char *watchdog = getenv("MEMORIES_WATCHDOG");
        if (watchdog && *watchdog) watchdog_seconds = (unsigned)strtoul(watchdog, NULL, 10);
    }
    Profile_Init();
#ifdef _WIN32
    Win32_SetStallReporter(Crash_ReportHang, watchdog_seconds);
    return Win32_StartInterrupt(on_tick);
#else
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_alarm;
    action.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL)) {
        return -1;
    }
    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGALRM;
    event._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    spec.it_interval.tv_sec = spec.it_value.tv_sec = 0;
    spec.it_interval.tv_nsec = spec.it_value.tv_nsec = 1000000;
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) == 0 && timer_settime(timer, 0, &spec, NULL) == 0) {
        return 0;
    }
    {
        struct itimerval fallback;
        fallback.it_interval.tv_sec = fallback.it_value.tv_sec = 0;
        fallback.it_interval.tv_usec = fallback.it_value.tv_usec = 1000;
        return setitimer(ITIMER_REAL, &fallback, NULL) ? -1 : 0;
    }
#endif
}

unsigned Platform_VBlankCount(void)
{
    return vblank_count;
}

void Platform_SetClockRate(int percent)
{
    if (percent < -1) percent = -1;
    if (percent > 400) percent = 400;
    if (percent > 0 && percent < 25) percent = 25;
    rate = percent;
}

int Platform_ClockRate(void) { return rate; }
void Platform_StepFrame(void) { step_pending = 1; }

float Platform_GameHz(void)
{
    return rate > 0 ? 1000000.0f / (float)vblank_period * (float)rate / 100.0f : 0.0f;
}

void Platform_SetPresentCap(int fps)
{
    present_cap = fps < -1 ? -1 : fps;
    present_next_us = 0;
}

int Platform_PresentCap(void) { return present_cap; }

unsigned Platform_PresentPeriodUs(void)
{
    if (present_cap == -1) return 0;
    if (present_cap > 0) return (unsigned)(1000000.0f / (float)present_cap + 0.5f);
    return present_refresh > 0.0f ? (unsigned)(1000000.0f / present_refresh + 0.5f) : 0u;
}

/* One present per period, on a fixed grid so a game frame that arrives a
 * little before its slot (frames come from a 1 kHz clock, and the game rate
 * may sit within a few microseconds of the cap) still takes it: a frame is
 * due from half a period before its slot. A stall resets the grid. */
int Platform_PresentDue(void)
{
    unsigned period = Platform_PresentPeriodUs();
    uint64_t now = now_us();
    if (!period) return 1;
    if (!present_next_us || now >= present_next_us + period) present_next_us = now;
    if (now + period / 2 < present_next_us) return 0;
    present_next_us += period;
    return 1;
}

int Platform_VSyncPacesGame(void)
{
    if (rate == -1) return 0;
    if (present_refresh <= 0.0f) return rate <= 100; /* every display refreshes at 59.94 Hz or faster */
    return Platform_GameHz() <= present_refresh + 0.5f;
}

void Platform_VSyncHeartbeat(void)
{
    sigset_t set, previous;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    last_vsync_real = now_us();
    watchdog_reported = 0;
#ifdef _WIN32
    Win32_Heartbeat();
#endif
    sigprocmask(SIG_SETMASK, &previous, NULL);
#ifdef _WIN32
    /* A VSync(-1) polling loop (the movie waiting for sectors) spends most
     * of its time reading the clock, outside the executable, where the clock
     * thread only leaves the tick pending. Take it here, as the waits do. */
    Win32_ServiceInterrupt();
#endif
}

void Platform_SetVBlankPeriod(unsigned us)
{
    sigset_t set, previous;
    if (!us) return;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, &previous);
    vblank_period = us;
    sigprocmask(SIG_SETMASK, &previous, NULL);
}

void Platform_SetPresentRefresh(float hz)
{
    present_refresh = hz;
    present_next_us = 0;
}

float Platform_PresentRefresh(void) { return present_refresh; }

void Platform_NotifyPresent(uint64_t real_now_us, int vsynced)
{
    (void)real_now_us;
    if (vsynced && rate == 100 && present_refresh >= 59.0f && present_refresh <= 61.0f) {
        sigset_t set, previous;
        unsigned period = (unsigned)(1000000.0f / present_refresh + 0.5f);
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_BLOCK, &set, &previous);
        vblank_period = period;
        next_vblank = virtual_now + period - 1500;
        sigprocmask(SIG_SETMASK, &previous, NULL);
    }
}

void Platform_WaitVBlank(unsigned count_at_entry)
{
    struct timespec nap = {0, 500000};
    while (vblank_count == count_at_entry && !Platform_ShouldQuit()) {
        if (rate == -1 && deterministic_dump) {
            sigset_t set, previous;
            sigemptyset(&set);
            sigaddset(&set, SIGALRM);
            sigprocmask(SIG_BLOCK, &set, &previous);
            if (!next_vblank) next_vblank = virtual_now + vblank_period;
            if (virtual_now >= next_vblank) {
                next_vblank += vblank_period;
                deliver_vblank();
            }
            sigprocmask(SIG_SETMASK, &previous, NULL);
#ifdef _WIN32
            Win32_ServiceInterrupt();
#endif
            if (vblank_count == count_at_entry) nanosleep(&nap, NULL);
            continue;
        }
        if (rate == -1) {
            sigset_t set, previous;
            uint64_t real_now;
            sigemptyset(&set);
            sigaddset(&set, SIGALRM);
            sigprocmask(SIG_BLOCK, &set, &previous);
            real_now = now_us();
            advance(real_now);
            if (vblank_count == count_at_entry) {
                if (!next_vblank) next_vblank = virtual_now;
                virtual_now = next_vblank;
                advance(real_now);
            }
            sigprocmask(SIG_SETMASK, &previous, NULL);
        } else {
            if (rate == 0) Platform_PumpEvents();
#ifdef _WIN32
            if (rate == 0) Win32_Heartbeat(); /* paused, not hung */
            Win32_ServiceInterrupt();
            if (vblank_count != count_at_entry) break;
#endif
            nanosleep(&nap, NULL);
        }
    }
}

/* The VBlank count is what the game sees through VSync(-1). */
void Platform_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {{(void *)&vblank_count, sizeof(vblank_count)}};
    Memories_StateChunk(state, "platform", fields, 1);
}

/* MEMORIES_INPUT="600:0008,610:0000": hex pad bits applied from a frame on.
 * Returns the bits in force at `frame`. */
uint16_t Platform_ScriptedBits(unsigned frame)
{
    static const char *script;
    static int loaded;
    static uint16_t bits;
    if (!loaded) {
        loaded = 1;
        script = getenv("MEMORIES_INPUT");
    }
    while (script && *script) {
        char *end;
        unsigned long at = strtoul(script, &end, 10);
        if (*end != ':' || at > frame) {
            break;
        }
        bits = (uint16_t)strtoul(end + 1, &end, 16);
        LOG(LOG_INPUT, "script frame %u pad %04x", frame, bits);
        script = *end == ',' ? end + 1 : end;
    }
    return bits;
}

/* MEMORIES_DUMP_AUDIO=path: no device; mix in real time into raw s16le
 * stereo 44.1 kHz so output can be inspected without speakers. With no path
 * this is the silent sink: the SPU still has to run, because the game polls
 * envelopes and the disc service waits for CD input room. */
static void (*silent_mixer)(int16_t *, size_t);

static void *run_silent(void *path)
{
    static int16_t buffer[256 * 2];
    FILE *file = path ? fopen(path, "wb") : NULL;
    struct timespec nap = {0, 256 * 1000000000ll / 44100};
    for (;;) {
        silent_mixer(buffer, 256);
        if (file) {
            fwrite(buffer, sizeof(buffer), 1, file);
            fflush(file);
        }
        nanosleep(&nap, NULL);
    }
    return NULL;
}

int Platform_StartSilentAudio(void (*mix)(int16_t *, size_t), const char *dump_path)
{
    pthread_t thread;
    sigset_t all, previous;
    int error;
    silent_mixer = mix;
    /* The timer signal is the game's interrupt and must stay on the main thread. */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
    error = pthread_create(&thread, NULL, run_silent, (void *)dump_path);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return error ? -1 : 0;
}
