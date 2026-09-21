/* LIBETC/LIBAPI: callbacks, VSync, critical sections and the BIOS pad driver.
 * The VBlank "interrupt" is the platform's 60 Hz signal; everything reachable
 * from it must stay async-signal-safe (no stdio, no allocation, no Xlib). */
#include "pc/platform/platform.h"
#include "pc/render/soft_gpu.h"
#include "pc/sdk/disc.h"
#include "pc/sdk/display.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pc/guest/state.h"
#include "pc/debug/log.h"

static void (*vsync_callback)(void);
static long (*counter_handler)(void);
static volatile int critical, started, pads_started, counter_running;
static volatile int pending_vblank, pending_tick;
static volatile uint64_t counter_period_us, counter_next_us, last_now_us;
static unsigned char *pad_buffer[2];
static unsigned last_vsync;
static volatile unsigned counter_calls; /* MEMORIES_TRACE_FRAMES: sequencer ticks delivered */

static void run_tick(uint64_t now)
{
    int budget = 8; /* after a stall, catch up a little and drop the rest */
    Memories_DiscService(now);
    Memories_MdecService();
    if (!counter_running || !counter_handler || !counter_period_us) {
        return;
    }
    if (!counter_next_us) {
        counter_next_us = now + counter_period_us;
    }
    while (now >= counter_next_us && budget--) {
        counter_next_us += counter_period_us;
        counter_calls++;
        counter_handler();
    }
    if (now >= counter_next_us) {
        counter_next_us = now + counter_period_us;
    }
}

static void run_vblank(void)
{
    int port;
    for (port = 0; port < 2 && pads_started; port++) {
        if (pad_buffer[port]) {
            unsigned bits = Platform_Pad(port);
            pad_buffer[port][0] = Platform_PadConnected(port) ? 0x00 : 0xff; /* 0xff: no pad */
            pad_buffer[port][1] = 0x41;
            pad_buffer[port][2] = (unsigned char)~bits;
            pad_buffer[port][3] = (unsigned char)~(bits >> 8);
        }
    }
    if (vsync_callback) {
        vsync_callback();
    }
}

static void on_tick(uint64_t now)
{
    last_now_us = now;
    if (critical) {
        pending_tick = 1;
    } else {
        run_tick(now);
    }
}

static void on_vblank(void)
{
    if (critical) {
        pending_vblank = 1;
    } else {
        run_vblank();
    }
}

long EnterCriticalSection(void)
{
    long was_enabled = !critical;
    critical = 1;
    return was_enabled;
}

void ExitCriticalSection(void)
{
    critical = 0;
    if (pending_tick) {
        pending_tick = 0;
        run_tick(last_now_us);
    }
    if (pending_vblank) {
        pending_vblank = 0;
        run_vblank();
    }
}

int ResetCallback(void)
{
    if (!started) {
        started = 1;
        if (Platform_StartTimers(on_tick, on_vblank) != 0) {
            abort();
        }
    }
    return 0;
}

/* Root counter 2 drives the sound sequencer. Without the system-clock flag
 * SetRCnt selects sysclk/8 (mode 0x248), so the period is target*8/33.8688MHz:
 * 0xE000 gives 73.8 Hz. Other counters and events are accepted and inert. */
long OpenEvent(unsigned long descriptor, long spec, long mode, long (*handler)(void))
{
    (void)spec; (void)mode;
    if (descriptor == 0xf2000002ul) {
        counter_handler = handler;
    }
    return 0x100 + (long)(descriptor & 0xff);
}

long CloseEvent(long event) { (void)event; return 1; }
long EnableEvent(long event) { (void)event; return 1; }
long DisableEvent(long event) { (void)event; return 1; }
long TestEvent(long event) { (void)event; return 0; }

long SetRCnt(unsigned long spec, unsigned short target, long flags)
{
    if (spec == 0xf2000002ul) {
        uint64_t divider = flags & 1 ? 1 : 8;
        counter_period_us = (uint64_t)(target ? target : 0x10000) * divider * 1000000u / 33868800u;
        counter_next_us = 0;
    }
    return 1;
}

long GetRCnt(unsigned long spec) { (void)spec; return 0; }
long StartRCnt(unsigned long spec) { if (spec == 0xf2000002ul) { counter_running = 1; } return 1; }
long StopRCnt(unsigned long spec) { if (spec == 0xf2000002ul) { counter_running = 0; } return 1; }

int StopCallback(void)
{
    return 0;
}

int VSyncCallback(void (*callback)(void))
{
    vsync_callback = callback;
    return 0;
}

/* mode 0: wait for the next VBlank; n > 1: wait until n have passed since the
 * previous call; negative: just the running count. Returns the elapsed time
 * in horizontal lines (263 per NTSC field), which callers compare to budgets. */
/* Entered through the assembly VSync, which records the caller's registers
 * for save states (src/pc/guest/state_i386.S). */
int Memories_VSync(int mode)
{
    unsigned now = Platform_VBlankCount(), elapsed;
    Platform_VSyncHeartbeat();
    if (mode < 0) {
        return (int)now;
    }
    if (mode == 0) {
        struct timespec t0, t1;
        static struct timespec left;
        static unsigned frames, game_us, present_us, late, game_max, present_max;
        if (Log_Enabled(LOG_FRAMES)) {
            clock_gettime(CLOCK_MONOTONIC, &t0);
        }
        Memories_PresentDisplay();
        if (Log_Enabled(LOG_FRAMES)) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            {
                unsigned p = (unsigned)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000), g = 0;
                present_us += p;
                present_max = p > present_max ? p : present_max;
                if (left.tv_sec) {
                    g = (unsigned)((t0.tv_sec - left.tv_sec) * 1000000 + (t0.tv_nsec - left.tv_nsec) / 1000);
                    game_us += g;
                    game_max = g > game_max ? g : game_max;
                }
                if (g > 20000 || p > 20000) {
                    LOG(LOG_FRAMES, "spike at frame %u: game %u us, present %u us",
                        Memories_PresentedFrames(), g, p);
                }
            }
            late += Platform_VBlankCount() - last_vsync > 1;
            if (++frames == 120) {
                static struct timespec since;
                static unsigned ticks_then, vblanks_then;
                double seconds = since.tv_sec ? (double)(t1.tv_sec - since.tv_sec) + (t1.tv_nsec - since.tv_nsec) / 1e9 : 0;
                LOG(LOG_FRAMES, "game %u us, present %u us per frame (max %u, %u); %u of 120 missed a VBlank",
                    game_us / 120, present_us / 120, game_max, present_max, late);
                if (seconds > 0) {
                    LOG(LOG_FRAMES, "clocks: %.2f sequencer ticks/s (period %llu us), %.2f VBlanks/s",
                        (counter_calls - ticks_then) / seconds, (unsigned long long)counter_period_us,
                        (Platform_VBlankCount() - vblanks_then) / seconds);
                }
                since = t1;
                ticks_then = counter_calls;
                vblanks_then = Platform_VBlankCount();
                frames = game_us = present_us = late = game_max = present_max = 0;
            }
        }
        Platform_WaitVBlank(now);
        if (Log_Enabled(LOG_FRAMES)) {
            clock_gettime(CLOCK_MONOTONIC, &left);
        }
        Memories_StatePoint(Memories_PresentedFrames());
    } else if (mode > 1) {
        while (Platform_VBlankCount() - last_vsync < (unsigned)mode) {
            Platform_WaitVBlank(Platform_VBlankCount());
        }
    }
    elapsed = Platform_VBlankCount() - last_vsync;
    if (mode != 1) {
        last_vsync = Platform_VBlankCount();
    }
    if (Platform_ShouldQuit()) {
        exit(0);
    }
    return (int)(elapsed * 263u);
}

void GsInitVcount(void)
{
}

void SetMem(long size)
{
    (void)size;
}

long InitPAD(char *first, long first_length, char *second, long second_length)
{
    pad_buffer[0] = first_length >= 4 ? (unsigned char *)first : NULL;
    pad_buffer[1] = second_length >= 4 ? (unsigned char *)second : NULL;
    return 1;
}

long StartPAD(void)
{
    pads_started = 1;
    return 1;
}

void ChangeClearPAD(long value)
{
    (void)value;
}

/* Callbacks are game functions and the pad buffers are guest addresses. The
 * timer phase belongs to the running process and restarts. */
void LibEtc_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {{&vsync_callback, sizeof(vsync_callback)},
                                         {&counter_handler, sizeof(counter_handler)},
                                         {(void *)&pads_started, sizeof(pads_started)},
                                         {(void *)&counter_running, sizeof(counter_running)},
                                         {(void *)&counter_period_us, sizeof(counter_period_us)},
                                         {pad_buffer, sizeof(pad_buffer)}, {&last_vsync, sizeof(last_vsync)}};
    if (Memories_StateChunk(state, "libetc", fields, sizeof(fields) / sizeof(fields[0]))) {
        counter_next_us = 0;
        critical = pending_tick = pending_vblank = 0;
    }
}
