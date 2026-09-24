#include "pc/mods/mods.h"
/* LIBETC/LIBAPI: callbacks, VSync, critical sections and the BIOS pad driver.
 * The VBlank "interrupt" is the platform's 60 Hz signal; everything reachable
 * from it must stay async-signal-safe (no stdio, no allocation, no Xlib). */
#include "pc/platform/platform.h"
#include "pc/platform/ai_trace.h"
#include "pc/platform/title_jump.h"
#include "pc/saves/deck_menu.h"
#include "pc/render/soft_gpu.h"
#include "pc/sdk/disc.h"
#include "pc/sdk/display.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include "pc/debug/monitor.h"
#include "pc/mods/mods.h"
#ifdef _WIN32
#include "pc/platform/win32.h"
#endif

static void (*vsync_callback)(void);
static long (*counter_handler)(void);
static volatile int critical, started, pads_started, counter_running;
static volatile int pending_vblank, pending_tick;
static volatile uint64_t counter_period_us, counter_next_us, last_game_us, last_real_us;
static unsigned char *pad_buffer[2];
static unsigned last_vsync;
static volatile unsigned counter_calls; /* MEMORIES_TRACE_FRAMES: sequencer ticks delivered */
static FrameStats frame_stats;

const FrameStats *Memories_FrameStats(void) { return &frame_stats; }
void Memories_SetDrawStats(unsigned words, unsigned us)
{
    frame_stats.draw_words = words;
    frame_stats.draw_us = us;
}

static void run_tick(uint64_t game_now, uint64_t real_now)
{
    int budget = 8; /* after a stall, catch up a little and drop the rest */
    Memories_DiscService(game_now);
    Memories_MdecService();
    if (!counter_running || !counter_handler || !counter_period_us) {
        return;
    }
    if (!counter_next_us) {
        counter_next_us = real_now + counter_period_us;
    }
    while (real_now >= counter_next_us && budget--) {
        /* MEMORIES_TRACE=frames: how late the sequencer's ticks run against
         * when they were due, which is what the music's timing hears. */
        static unsigned late_max, late_over_5ms, late_over_20ms, late_ticks;
        static uint64_t late_total;
        unsigned late = (unsigned)(real_now - counter_next_us);
        late_total += late;
        late_max = late > late_max ? late : late_max;
        late_over_5ms += late > 5000;
        late_over_20ms += late > 20000;
        if (++late_ticks == 1000) {
            /* Run from the timer's handler: Log_Signal (log.c). */
            Log_Signal(LOG_FRAMES, "sequencer lateness over 1000 ticks: mean %ld us, max %ld us, %ld over 5 ms, %ld over 20 ms",
                       (long)(late_total / 1000), (long)late_max, (long)late_over_5ms, (long)late_over_20ms, 0, 0);
            late_ticks = late_max = late_over_5ms = late_over_20ms = 0;
            late_total = 0;
        }
        counter_next_us += counter_period_us;
        counter_calls++;
        counter_handler();
    }
    if (real_now >= counter_next_us) {
        counter_next_us = real_now + counter_period_us;
    }
}

static void run_vblank(void)
{
    int port;
    for (port = 0; port < 2 && pads_started; port++) {
        if (pad_buffer[port]) {
            /* Released while the deck slot screen reads the pad (deck_menu.h). */
            unsigned bits = DeckMenu_HoldsPads() ? 0 : Platform_Pad(port);
            MemoriesModEvent input = {MEMORIES_EVENT_INPUT, MEMORIES_BEFORE, port, (int)bits, 0, (int)bits, 0};
            Mods_Dispatch(&input);
            bits = (unsigned)(input.handled ? input.result : input.b) & 0xffffu;
            input.result = (int)bits; input.phase = MEMORIES_AFTER; Mods_Dispatch(&input);
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

static void on_tick(uint64_t game_now, uint64_t real_now)
{
    last_game_us = game_now;
    last_real_us = real_now;
    if (critical) {
        pending_tick = 1;
    } else {
        run_tick(game_now, real_now);
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
        run_tick(last_game_us, last_real_us);
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

/* mode 0: present, then wait for the next VBlank; n > 1: wait until n have
 * passed since the previous call; negative: just the running count. Returns
 * the elapsed time in horizontal lines (263 per NTSC field), which callers
 * compare to budgets.
 *
 * Mode 0 must wait for a VBlank after entry, never return at once because
 * one passed since the previous call: Graphics_SyncFrame resets the game's
 * own VBlank counter to -1 just before calling, and Input_UpdatePads takes
 * a counter that is still -1 afterwards as a lag frame and publishes every
 * press a second time on the next frame (doubled inputs at 300%). */
/* Entered through the assembly VSync, which records the caller's registers
 * for save states (src/pc/guest/state_i386.S). */
int Memories_VSync(int mode)
{
    unsigned now, elapsed;
    /* The game's time between two calls here, whatever the mode: the
     * longest it runs without reaching a point where the clock could be
     * serviced (MEMORIES_TRACE=frames reports each over 20 ms). */
    static struct timespec service_left;
    struct timespec service_entry;
    clock_gettime(CLOCK_MONOTONIC, &service_entry);
    if (service_left.tv_sec && Log_Enabled(LOG_FRAMES)) {
        unsigned gap = (unsigned)((service_entry.tv_sec - service_left.tv_sec) * 1000000 +
                                  (service_entry.tv_nsec - service_left.tv_nsec) / 1000);
        if (gap > 20000) {
            LOG(LOG_FRAMES, "service gap at frame %u: %u us before VSync(%d)", Memories_PresentedFrames(), gap, mode);
        }
    }
    if (mode < 0 || mode == 1) Platform_PollTime();
    now = Platform_VBlankCount();
    Platform_VSyncHeartbeat();
    if (mode < 0) {
        clock_gettime(CLOCK_MONOTONIC, &service_left);
        return (int)now;
    }
    if (mode == 0) {
        struct timespec t0, t1;
        static struct timespec left;
        static struct timespec since;
        static unsigned frames, game_us, present_us, late, game_max, present_max;
        static unsigned ticks_then, vblanks_then, shown_then;
        AiTrace_Frame(Memories_PresentedFrames());
        clock_gettime(CLOCK_MONOTONIC, &t0);
        Memories_PresentDisplay();
        TitleJump_Frame(Memories_PresentedFrames());
        DeckMenu_Frame(Memories_PresentedFrames());
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
        /* VBlanks that passed with no game frame for them: those that got in
         * during the game's frame, and during the present that just ran. */
        if (Platform_VBlankCount() - last_vsync > 1) late += Platform_VBlankCount() - last_vsync - 1;
        if (++frames == 120) {
            double seconds = since.tv_sec ? (double)(t1.tv_sec - since.tv_sec) + (t1.tv_nsec - since.tv_nsec) / 1e9 : 0;
            frame_stats.game_us = game_us / 120;
            frame_stats.present_us = present_us / 120;
            frame_stats.game_max_us = game_max;
            frame_stats.present_max_us = present_max;
            frame_stats.missed_vblanks = late;
            frame_stats.fps_tenths = seconds > 0 ? (unsigned)(1200.0 / seconds + 0.5) : 0;
            frame_stats.shown_tenths =
                seconds > 0 ? (unsigned)((Memories_ShownFrames() - shown_then) * 10.0 / seconds + 0.5) : 0;
            if (Log_Enabled(LOG_FRAMES)) {
                LOG(LOG_FRAMES, "game %u us, present %u us per frame (max %u, %u); %u VBlanks missed in 120 frames",
                    game_us / 120, present_us / 120, game_max, present_max, late);
                if (seconds > 0) {
                    LOG(LOG_FRAMES, "clocks: rate %d, %.2f game frames/s, %.2f shown/s, %.2f VBlanks/s, %.2f sequencer ticks/s (period %llu us)",
                        Platform_ClockRate(), 120.0 / seconds, (Memories_ShownFrames() - shown_then) / seconds,
                        (Platform_VBlankCount() - vblanks_then) / seconds,
                        (counter_calls - ticks_then) / seconds, (unsigned long long)counter_period_us);
                }
#ifdef _WIN32
                {
                    unsigned lost, undone, skipped;
                    Win32_ClockRepairs(&lost, &undone, &skipped);
                    LOG(LOG_FRAMES, "clock repairs: %u lost redirects, %u interrupted faults, %u ticks deferred past a kernel entry",
                        lost, undone, skipped);
                }
#endif
            }
            since = t1;
            ticks_then = counter_calls;
            vblanks_then = Platform_VBlankCount();
            shown_then = Memories_ShownFrames();
            frames = game_us = present_us = late = game_max = present_max = 0;
        }
        Platform_WaitVBlank(now);
        clock_gettime(CLOCK_MONOTONIC, &left);
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
        Monitor_Shared()->exiting = 1; /* a slow shutdown is not a freeze */
        /* The clock first: VBlank can hand a mod an INPUT event, and after
         * its shutdown hook the memory that callback uses may be gone. */
        Platform_StopTimers();
        Mods_Shutdown(); /* mods get a word in before the process goes */
        exit(0);
    }
    clock_gettime(CLOCK_MONOTONIC, &service_left);
    if (mode == 1) return AiTrace_VSync1((int)(elapsed * 263u), __builtin_return_address(0));
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
