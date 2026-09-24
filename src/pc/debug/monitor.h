#ifndef MEMORIES_PC_DEBUG_MONITOR_H
#define MEMORIES_PC_DEBUG_MONITOR_H
/* The crash monitor: a second copy of the executable that runs the game as
 * its child and watches it from outside. Whatever ends the game (a crash
 * the game's own handlers never saw, a fail-fast, the out-of-memory killer,
 * the player closing a frozen window) or freezes it, the monitor is still
 * there to write the report: how the game ended, the system it ran on, the
 * last lines of its log and of its console output, and on a freeze every
 * thread's registers and frames (and a minidump on Windows). The game's own
 * reports (crash.c) come first in the same file when it could write one.
 *
 * The two share one block of memory, written by the game and read by the
 * monitor, so what the game knew survives however it ended. The game's
 * console output (stderr) passes through the monitor, which keeps it in
 * <Crash_ReportDir>/last-session.log (the one before in previous-session.log).
 *
 * MEMORIES_NO_MONITOR=1 runs the game alone, as does a debugger. */
#include <stdint.h>

#define MONITOR_TAIL_LINES 128
#define MONITOR_LINE_SIZE 256
#define MONITOR_FACTS_SIZE 4096

typedef struct {
    uint32_t magic, version;
    volatile uint32_t generation;  /* a new game started in this block (first start, restart) */
    volatile uint32_t heartbeat;   /* counts VSyncs */
    volatile uint32_t running;     /* the first VSync has happened */
    volatile uint32_t paused;      /* the clock is stopped: no VSync is expected */
    volatile uint32_t modal;       /* a message box is up on the game's thread */
    volatile uint32_t exiting;     /* the game is leaving by its own choice */
    volatile uint32_t restart;     /* Windows: start the game again once it exits */
    volatile uint32_t error_shown; /* the game told the player why it stopped */
    volatile uint32_t reported;    /* the game wrote report_path itself */
    volatile uint32_t frame, vblank;
    volatile uint32_t facts_sequence; /* odd while facts is being rewritten */
    char report_path[640];
    char module[32];                  /* the runtime module last loaded (duel, field...) */
    char facts[MONITOR_FACTS_SIZE];   /* "key: value" lines: the build, the system, the settings */
    volatile uint32_t tail_head;      /* the log's last lines (log.c), a ring */
    char tail[MONITOR_TAIL_LINES][MONITOR_LINE_SIZE];
} MonitorShared;

/* First thing in main. Returns 0 to go on as the game (monitored or not);
 * 1 when this process was the monitor and the game has ended: main returns
 * *status, the game's own exit status. */
int Monitor_Main(int argc, char **argv, int *status);
/* The shared block, or a block of this process's own when unmonitored, so
 * callers never check. Never NULL. */
MonitorShared *Monitor_Shared(void);
int Monitor_Active(void);
/* Set a line of the facts every report starts with. Not signal-safe. */
void Monitor_Fact(const char *key, const char *format, ...) __attribute__((format(printf, 2, 3)));
/* The build, the operating system, the CPU and the memory, as facts. */
void Monitor_NoteSystem(void);
/* Around a message box or anything else that holds up the game's thread on
 * purpose: the monitor does not count it as a freeze. */
void Monitor_Modal(int on);

#endif
