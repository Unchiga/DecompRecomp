#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include "pc/platform/platform.h"
#include "pc/sdk/display.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIGNAL_RECORDS 256
#define TAIL_LINES 64
#define LINE_SIZE 512

typedef struct {
    LogChannel channel;
    const char *format;
    long argument[6];
    uint64_t timestamp;
    unsigned frame, vblank;
} SignalRecord;

static const char *const names[LOG_COUNT] = {
    "frames", "disc", "spu", "input", "menu", "memcard", "mods", "model",
    "duel_effects", "mips_printf", "stub", "state", "clock", "window", "audio"
};
static const char *const legacy_env[LOG_COUNT] = {
    "MEMORIES_TRACE_FRAMES", "MEMORIES_TRACE_DISC", "MEMORIES_TRACE_SPU", "MEMORIES_TRACE_INPUT",
    "MEMORIES_TRACE_MENU", "MEMORIES_TRACE_MEMCARD", "MEMORIES_TRACE_MODS", "MEMORIES_TRACE_MODEL_MODULES",
    "MEMORIES_TRACE_DUEL_EFFECTS", "MEMORIES_TRACE_MIPS_PRINTF", NULL, "MEMORIES_TRACE_STATE", NULL, NULL, NULL
};
static volatile uint32_t enabled;
static SignalRecord signal_ring[SIGNAL_RECORDS];
static volatile unsigned signal_head, signal_tail, signal_dropped;
static char tail[TAIL_LINES][LINE_SIZE];
static volatile unsigned tail_head;
static FILE *log_file;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t timestamp_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

const char *Log_ChannelName(LogChannel channel)
{
    return channel >= 0 && channel < LOG_COUNT ? names[channel] : "unknown";
}

int Log_Enabled(LogChannel channel)
{
    return channel >= 0 && channel < LOG_COUNT && (__atomic_load_n(&enabled, __ATOMIC_RELAXED) >> channel & 1u);
}

void Log_Enable(LogChannel channel, int on)
{
    uint32_t bit;
    if (channel < 0 || channel >= LOG_COUNT) return;
    bit = 1u << channel;
    if (on) __atomic_fetch_or(&enabled, bit, __ATOMIC_RELAXED);
    else __atomic_fetch_and(&enabled, ~bit, __ATOMIC_RELAXED);
}

void Log_Init(void)
{
    const char *selected = getenv("MEMORIES_TRACE");
    const char *path = getenv("MEMORIES_LOG");
    int channel;
    enabled = 0;
    for (channel = 0; channel < LOG_COUNT; channel++) {
        if (legacy_env[channel] && getenv(legacy_env[channel])) Log_Enable((LogChannel)channel, 1);
    }
    if (selected && *selected) {
        const char *at = selected;
        while (*at) {
            size_t length = strcspn(at, ",");
            for (channel = 0; channel < LOG_COUNT; channel++) {
                if ((length == 3 && !strncmp(at, "all", length)) ||
                    (strlen(names[channel]) == length && !strncmp(at, names[channel], length))) {
                    Log_Enable((LogChannel)channel, 1);
                }
            }
            at += length;
            if (*at == ',') at++;
        }
    }
    if (path && *path) log_file = fopen(path, "a");
}

static void emit(LogChannel channel, uint64_t timestamp, unsigned frame, unsigned vblank, const char *message)
{
    char line[LINE_SIZE];
    unsigned at;
    snprintf(line, sizeof(line), "[t %llu us frame %u vb %u %s] %s",
             (unsigned long long)timestamp, frame, vblank, Log_ChannelName(channel), message);
    pthread_mutex_lock(&output_lock);
    at = __atomic_fetch_add(&tail_head, 1, __ATOMIC_RELAXED);
    snprintf(tail[at % TAIL_LINES], LINE_SIZE, "%s", line);
    fputs(line, stderr);
    if (!strchr(line, '\n')) fputc('\n', stderr);
    fflush(stderr);
    if (log_file) {
        fputs(line, log_file);
        if (!strchr(line, '\n')) fputc('\n', log_file);
        fflush(log_file);
    }
    pthread_mutex_unlock(&output_lock);
}

void Log_Printf(LogChannel channel, const char *format, ...)
{
    char message[384];
    va_list arguments;
    if (!Log_Enabled(channel)) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    emit(channel, timestamp_us(), Memories_PresentedFrames(), Platform_VBlankCount(), message);
}

void Log_Signal(LogChannel channel, const char *literal_format,
                long a, long b, long c, long d, long e, long f)
{
    unsigned head = __atomic_load_n(&signal_head, __ATOMIC_RELAXED);
    unsigned tail_at = __atomic_load_n(&signal_tail, __ATOMIC_ACQUIRE);
    SignalRecord *record;
    struct timespec now;
    if (!Log_Enabled(channel)) return;
    if (head - tail_at >= SIGNAL_RECORDS) {
        __atomic_fetch_add(&signal_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    record = &signal_ring[head % SIGNAL_RECORDS];
    clock_gettime(CLOCK_MONOTONIC, &now);
    record->channel = channel;
    record->format = literal_format;
    record->argument[0] = a; record->argument[1] = b; record->argument[2] = c;
    record->argument[3] = d; record->argument[4] = e; record->argument[5] = f;
    record->timestamp = (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
    record->frame = Memories_PresentedFrames();
    record->vblank = Platform_VBlankCount();
    __atomic_store_n(&signal_head, head + 1, __ATOMIC_RELEASE);
}

void Log_Drain(void)
{
    unsigned tail_at = __atomic_load_n(&signal_tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(&signal_head, __ATOMIC_ACQUIRE);
    while (tail_at != head) {
        SignalRecord *record = &signal_ring[tail_at % SIGNAL_RECORDS];
        char message[384];
        snprintf(message, sizeof(message), record->format, record->argument[0], record->argument[1],
                 record->argument[2], record->argument[3], record->argument[4], record->argument[5]);
        emit(record->channel, record->timestamp, record->frame, record->vblank, message);
        tail_at++;
    }
    __atomic_store_n(&signal_tail, tail_at, __ATOMIC_RELEASE);
    {
        unsigned dropped = __atomic_exchange_n(&signal_dropped, 0, __ATOMIC_RELAXED);
        if (dropped) {
            char message[96];
            snprintf(message, sizeof(message), "signal log ring dropped %u records", dropped);
            emit(LOG_STUB, timestamp_us(), Memories_PresentedFrames(), Platform_VBlankCount(), message);
        }
    }
}

int Log_Tail(int n, const char **lines)
{
    unsigned head = __atomic_load_n(&tail_head, __ATOMIC_ACQUIRE);
    int available = head < TAIL_LINES ? (int)head : TAIL_LINES;
    int i;
    if (n > available) n = available;
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) lines[i] = tail[(head - (unsigned)n + (unsigned)i) % TAIL_LINES];
    return n;
}
