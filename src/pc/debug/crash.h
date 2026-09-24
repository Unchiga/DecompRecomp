#ifndef MEMORIES_PC_DEBUG_CRASH_H
#define MEMORIES_PC_DEBUG_CRASH_H
#include <signal.h>

void Crash_Init(void);
/* Sets Crash_ReportDir; Crash_Init does, and the monitor (monitor.h). */
void Crash_ChooseReportDir(void);
/* Where crash and hang reports, minidumps and frame dumps go: tmp/pc when
 * the game runs from a checkout (it exists there), else `reports` in the
 * user directory (paths.h), so a player can find them to send. Set by
 * Crash_Init, before any handler can need it. */
extern char Crash_ReportDir[];
#ifndef _WIN32
void Crash_HandleSignal(int number, siginfo_t *info, void *context);
#endif
void Crash_ReportSoft(const char *kind, const char *detail);
/* An error the game cannot go on from, before the caller exits: the report
 * goes to crash-<pid>.txt as well as the console. */
void Crash_ReportFatal(const char *kind, const char *detail);
void Crash_ReportHang(void *context);

#endif
