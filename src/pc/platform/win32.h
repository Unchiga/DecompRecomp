#ifndef MEMORIES_PC_PLATFORM_WIN32_H
#define MEMORIES_PC_PLATFORM_WIN32_H
/* Windows stand-ins for the signal machinery the Linux build relies on
 * (win32.c). Only this file's implementation includes <windows.h>; its RECT
 * and other names clash with the game's own types. */
#ifdef _WIN32
#include <stdint.h>

/* The game clock (platform_common.c): calls `tick` at 1 kHz on the main
 * thread, between instructions of whatever it is running in the executable,
 * as SIGALRM does on Linux. `context` describes the interrupted registers
 * (see Win32_ContextRegisters). Returns 0 on success. */
int Win32_StartInterrupt(void (*tick)(uintptr_t eip, void *context));
/* For the exception handlers, first thing: the clock can redirect the main
 * thread while an exception it raised is still on its way to user mode, and
 * the exception then arrives with its registers at the tick's entry. This
 * puts them back where the exception really is (the tick is left pending)
 * and returns 1 when it did; the handler then goes on as usual. */
int Win32_UndoInterruptedFault(void *context);

/* Runs a tick the clock could not deliver because the main thread was outside
 * the executable (sleeping, in a driver). Call from waits on the main thread. */
void Win32_ServiceInterrupt(void);

/* The hang watchdog, run from the clock thread so that it also sees a main
 * thread stuck outside the executable (a driver, a lock), which the tick
 * cannot reach: `report` gets the main thread's registers once VSync has not
 * been called for `seconds`. Win32_Heartbeat marks each VSync. */
void Win32_SetStallReporter(void (*report)(void *context), unsigned seconds);
void Win32_Heartbeat(void);
/* How often the clock's two exception races were repaired (clock trace). */
void Win32_ClockRepairs(unsigned *lost_redirects, unsigned *undone_faults);

void Win32_ContextRegisters(const void *context, uintptr_t *eip, uintptr_t *esp, uintptr_t *ebp);
/* The executable image, and the calling thread's stack. */
void Win32_ImageRange(uintptr_t *low, uintptr_t *high);
void Win32_StackRange(uintptr_t *low, uintptr_t *high);

/* The DLL an address is in, as "name.dll"; 0 when it is in none. */
int Win32_ModuleName(uintptr_t address, char *out, unsigned size, uintptr_t *offset);

/* A font file under %WINDIR%\Fonts standing in for fontconfig's match:
 * a Japanese face when `japanese`, else a plain sans-serif. NULL if none. */
const char *Win32_FontPath(int japanese);

/* Starts the executable again with the same command line, without an
 * automatic state load, and exits. Returns -1 if it could not. */
int Win32_Restart(void);

/* Called for a fatal exception raised by code in the executable. */
typedef void (*Win32CrashReport)(unsigned long code, uintptr_t fault, uintptr_t eip, uintptr_t esp, uintptr_t ebp);
void Win32_SetCrashReporter(Win32CrashReport report);
#endif
#endif
