#ifndef MEMORIES_PC_DEBUG_LOG_H
#define MEMORIES_PC_DEBUG_LOG_H

typedef enum {
    LOG_FRAMES, LOG_DISC, LOG_SPU, LOG_INPUT, LOG_MENU, LOG_MEMCARD, LOG_MODS,
    LOG_MODEL, LOG_DUEL_EFFECTS, LOG_MIPS_PRINTF, LOG_STUB, LOG_STATE, LOG_CLOCK,
    LOG_WINDOW, LOG_AUDIO, LOG_COUNT
} LogChannel;

void Log_Init(void);
int Log_Enabled(LogChannel channel);
/* Traced, or kept for crash reports without being printed (log.c). */
int Log_Wanted(LogChannel channel);
void Log_Enable(LogChannel channel, int on);
const char *Log_ChannelName(LogChannel channel);
void Log_Printf(LogChannel channel, const char *format, ...);
void Log_Signal(LogChannel channel, const char *literal_format,
                long a, long b, long c, long d, long e, long f);
void Log_Drain(void);
int Log_Tail(int n, const char **lines);

#define LOG(channel, ...) do { if (Log_Wanted(channel)) Log_Printf(channel, __VA_ARGS__); } while (0)

#endif
