#ifndef MEMORIES_PC_DEBUG_CRASH_H
#define MEMORIES_PC_DEBUG_CRASH_H
#include <signal.h>

void Crash_Init(void);
#ifndef _WIN32
void Crash_HandleSignal(int number, siginfo_t *info, void *context);
#endif
void Crash_ReportSoft(const char *kind, const char *detail);
void Crash_ReportHang(void *context);

#endif
