/* Windows stand-ins for SIGALRM and the fatal-signal handlers (win32.h).
 *
 * The clock. Linux aims a 1 kHz SIGALRM at the main thread; its handler runs
 * between two instructions of the game, on the game's stack, which is what
 * the game's busy-waits need (they poll variables the VBlank handler
 * updates). Here a timer thread suspends the main thread every millisecond
 * and, when it is executing code of this executable and does not hold
 * SIGALRM, pushes its instruction pointer and redirects it to
 * Win32_InterruptEntry, which saves every register and the FPU/SSE state,
 * runs the tick and returns to the interrupted instruction. Code outside the
 * executable (the C runtime, SDL, drivers) is never interrupted, which also
 * keeps the tick away from their locks; a tick missed there is taken by
 * Win32_ServiceInterrupt from the next wait, and the clock itself catches up
 * from elapsed time. */
#ifdef _WIN32
#define _WIN32_WINNT 0x0A00 /* GetCurrentThreadStackLimits, high-resolution timers */
#include "win32.h"
#include "pc/compat/signal.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <dbghelp.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef CONTEXT_EXCEPTION_REQUEST /* mingw-w64 defines these for x86-64 only */
#define CONTEXT_EXCEPTION_ACTIVE 0x08000000
#define CONTEXT_SERVICE_ACTIVE 0x10000000
#define CONTEXT_EXCEPTION_REQUEST 0x40000000
#define CONTEXT_EXCEPTION_REPORTING 0x80000000
#endif

static DWORD main_id;
static HANDLE main_thread;
static volatile LONG held;       /* the main thread holds SIGALRM */
static volatile LONG in_tick;    /* a tick is running on the main thread */
static volatile LONG pending;    /* a tick could not be delivered */
static volatile LONG redirected; /* the clock redirected the main thread, the tick has not run yet */
static DWORD pushed_at;          /* where that redirect left the interrupted EIP */
static volatile LONG lost_redirects, undone_faults, skipped_unreliable;
static void (*tick_handler)(uintptr_t, void *);
static CONTEXT interrupted;      /* the registers a delivered tick interrupted */
static uintptr_t image_low, image_high;
static Win32CrashReport crash_report;
static volatile LONG heartbeat;
static void (*stall_report)(void *context);
static unsigned stall_ms;

void Win32_InterruptEntry(void);
void Win32_InterruptBody(void);
static void write_dump(const char *kind, EXCEPTION_POINTERS *pointers, DWORD thread);

/* A redirect the main thread never took: the exception that was on its way
 * when the clock redirected it resumed the thread with the registers it had
 * captured before. The tick is released and taken from the next wait. Only
 * the main thread, or the clock while the main thread is suspended, may
 * call this: `redirected` is set while the main thread is suspended and
 * cleared first thing by the tick, so the main thread seeing it set while
 * running anything else is that lost redirect. */
static void release_lost_redirect(void)
{
    redirected = 0;
    in_tick = 0;
    pending = 1;
    InterlockedIncrement(&lost_redirects);
}

/* Every register and the FPU/SSE state around the tick; the direction flag
 * is cleared for the C code. The interrupted EIP is the return address. */
__asm__(".text\n"
        ".globl _Win32_InterruptEntry\n"
        "_Win32_InterruptEntry:\n"
        "    pushfl\n"
        "    pushal\n"
        "    cld\n"
        "    movl %esp, %ebp\n"
        "    subl $512, %esp\n"
        "    andl $-16, %esp\n"
        "    fxsave (%esp)\n"
        "    call _Win32_InterruptBody\n"
        "    fxrstor (%esp)\n"
        "    movl %ebp, %esp\n"
        "    popal\n"
        "    popfl\n"
        "    ret\n");

void Win32_InterruptBody(void)
{
    redirected = 0;
    tick_handler(interrupted.Eip, &interrupted);
    in_tick = 0;
}

/* The game calls the BSD bzero, which the Windows C runtime lacks. */
void bzero(void *address, size_t size)
{
    memset(address, 0, size);
}

int Memories_SigProcMask(int how, const sigset_t *set, sigset_t *previous)
{
    unsigned long bit = 1ul << SIGALRM;
    if (GetCurrentThreadId() != main_id) {
        if (previous) *previous = 0;
        return 0;
    }
    if (previous) *previous = held ? bit : 0;
    if (set) {
        if (how == SIG_SETMASK) held = (*set & bit) != 0;
        else if (*set & bit) held = how == SIG_BLOCK;
    }
    return 0;
}

static DWORD WINAPI run_clock(void *unused)
{
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    (void)unused;
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    due.QuadPart = -10000; /* 1 ms, in 100 ns units */
    if (!timer || !SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE)) {
        fprintf(stderr, "memories-pc: game clock timer failed (error %lu)\n", GetLastError());
        return 1;
    }
    for (;;) {
        CONTEXT context;
        static LONG seen_beat;
        static ULONGLONG quiet_since;
        static int stall_reported;
        WaitForSingleObject(timer, INFINITE);
        /* Elapsed time, as Linux measures it: a wakeup is 1 ms only with the
         * high-resolution timer; the fallback timer coalesces to ~15 ms. */
        if (heartbeat != seen_beat) {
            seen_beat = heartbeat;
            quiet_since = GetTickCount64();
            stall_reported = 0;
        } else if (stall_report && stall_ms && seen_beat && !stall_reported &&
                   GetTickCount64() - quiet_since >= stall_ms) {
            /* Registers taken while suspended, reported after resuming: the
             * report writes files, and the main thread may hold the C
             * runtime's locks. */
            stall_reported = 1;
            if (SuspendThread(main_thread) != (DWORD)-1) {
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                GetThreadContext(main_thread, &context);
                ResumeThread(main_thread);
                stall_report(&context);
                write_dump("hang", NULL, main_id);
            }
        }
        if (SuspendThread(main_thread) == (DWORD)-1) continue;
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS | CONTEXT_EXCEPTION_REQUEST;
        if (GetThreadContext(main_thread, &context)) {
            /* The registers of a thread in the kernel for an exception or a
             * system call are not the ones it will resume with: WoW64 hands
             * back its saved 32-bit context, and a context set now can be
             * dropped, or half-applied, by the exception's own NtContinue.
             * The guest-call and low-memory faults (image.c) make this
             * common, and the MIPS effect bridge makes it constant: in a
             * 32-bit test with such faults in a loop, half the redirects
             * were lost, and in the game one left Memories_VSync with a
             * function address for its frame pointer. Such a tick waits for
             * the next sample or wait. */
            int unreliable = (context.ContextFlags & CONTEXT_EXCEPTION_REPORTING) &&
                             (context.ContextFlags & (CONTEXT_EXCEPTION_ACTIVE | CONTEXT_SERVICE_ACTIVE));
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            /* A redirect made while an exception was being delivered can be
             * dropped: the exception resumes the thread with the registers
             * it captured. The tick then never runs and would hold in_tick
             * for ever. While it does run, the thread is below the slot the
             * redirect pushed; above it, the redirect was lost. */
            if (!unreliable && redirected && context.Esp > pushed_at) release_lost_redirect();
            if (unreliable) {
                InterlockedIncrement(&skipped_unreliable);
                pending = 1;
            } else if (!held && !in_tick && !(context.EFlags & 0x100) && context.Eip >= image_low &&
                       context.Eip < image_high) {
                interrupted = context;
                in_tick = 1;
                pending = 0;
                context.Esp -= 4;
                *(DWORD *)(uintptr_t)context.Esp = context.Eip;
                context.Eip = (DWORD)(uintptr_t)Win32_InterruptEntry;
                pushed_at = context.Esp;
                redirected = 1;
                if (!SetThreadContext(main_thread, &context)) {
                    redirected = 0;
                    in_tick = 0;
                    pending = 1;
                }
            } else {
                pending = 1;
            }
        }
        ResumeThread(main_thread);
    }
}

int Win32_UndoInterruptedFault(void *context)
{
    CONTEXT *registers = context;
    /* Another thread's exception says nothing about the main thread's redirect. */
    if (!redirected || GetCurrentThreadId() != main_id) return 0;
    if (registers->Eip != (DWORD)(uintptr_t)Win32_InterruptEntry) {
        /* The exception's registers were captured before the redirect, and
         * are what the thread resumes with: the redirect is already lost.
         * Noticed here rather than by the clock, whose test (the stack
         * pointer back above the pushed slot) never fires while the game
         * waits in a deeper frame. */
        release_lost_redirect();
        return 0;
    }
    /* The clock pushed the interrupted EIP before redirecting. */
    registers->Eip = *(const DWORD *)(uintptr_t)registers->Esp;
    registers->Esp += 4;
    redirected = 0;
    pending = 1;
    in_tick = 0;
    undone_faults++;
    return 1;
}

void Win32_SetStallReporter(void (*report)(void *context), unsigned seconds)
{
    stall_ms = seconds * 1000u;
    stall_report = report;
}

void Win32_Heartbeat(void)
{
    InterlockedIncrement(&heartbeat);
}

void Win32_ClockRepairs(unsigned *lost, unsigned *undone, unsigned *skipped)
{
    *lost = (unsigned)lost_redirects;
    *undone = (unsigned)undone_faults;
    *skipped = (unsigned)skipped_unreliable;
}

void Win32_ServiceInterrupt(void)
{
    if (held || GetCurrentThreadId() != main_id) return;
    if (redirected) release_lost_redirect(); /* see there: this code is not the tick */
    if (!pending) return;
    if (InterlockedCompareExchange(&in_tick, 1, 0) != 0) return;
    pending = 0;
    memset(&interrupted, 0, sizeof(interrupted));
    interrupted.Eip = (DWORD)(uintptr_t)__builtin_return_address(0);
    interrupted.Ebp = (DWORD)(uintptr_t)__builtin_frame_address(0);
    tick_handler(interrupted.Eip, &interrupted);
    in_tick = 0;
}

int Win32_StartInterrupt(void (*tick)(uintptr_t eip, void *context))
{
    HANDLE thread;
    tick_handler = tick;
    main_id = GetCurrentThreadId();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, 0)) {
        return -1;
    }
    Win32_ImageRange(&image_low, &image_high);
    thread = CreateThread(NULL, 0, run_clock, NULL, 0, NULL);
    if (!thread) return -1;
    SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
    CloseHandle(thread);
    return 0;
}

void Win32_ContextRegisters(const void *context, uintptr_t *eip, uintptr_t *esp, uintptr_t *ebp)
{
    const CONTEXT *registers = context;
    *eip = registers->Eip;
    *esp = registers->Esp;
    *ebp = registers->Ebp;
}

void Win32_ImageRange(uintptr_t *low, uintptr_t *high)
{
    const unsigned char *base = (const unsigned char *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS *headers = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    *low = (uintptr_t)base;
    *high = (uintptr_t)base + headers->OptionalHeader.SizeOfImage;
}

int Win32_ModuleName(uintptr_t address, char *out, unsigned size, uintptr_t *offset)
{
    HMODULE module;
    char path[MAX_PATH];
    const char *name;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &module) ||
        !GetModuleFileNameA(module, path, sizeof(path))) {
        return 0;
    }
    name = strrchr(path, '\\');
    snprintf(out, size, "%s", name ? name + 1 : path);
    *offset = address - (uintptr_t)module;
    return 1;
}

int Win32_Restart(void)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return -1;
    SetEnvironmentVariableW(L"MEMORIES_LOAD_STATE", NULL);
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(path, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "memories-pc: restart failed (error %lu)\n", GetLastError());
        return -1;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), 0); /* not exit(): atexit handlers could re-enter game code */
    return -1;
}

const char *Win32_FontPath(int japanese)
{
    static const char *const sans[] = {"segoeui.ttf", "arial.ttf", "tahoma.ttf", NULL};
    static const char *const cjk[] = {"msgothic.ttc", "YuGothM.ttc", "meiryo.ttc", "segoeui.ttf", "arial.ttf", NULL};
    static char path[MAX_PATH];
    const char *const *name;
    char directory[MAX_PATH];
    UINT length = GetWindowsDirectoryA(directory, sizeof(directory));
    if (!length || length >= sizeof(directory)) return NULL;
    for (name = japanese ? cjk : sans; *name; name++) {
        snprintf(path, sizeof(path), "%s\\Fonts\\%s", directory, *name);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return NULL;
}

void Win32_StackRange(uintptr_t *low, uintptr_t *high)
{
    ULONG_PTR bottom, top;
    GetCurrentThreadStackLimits(&bottom, &top);
    *low = bottom;
    *high = top;
}

/* Beside every crash and hang report, a minidump with every thread's stack
 * (tmp/pc/<kind>-<pid>.dmp; lldb -c reads it). */
static void write_dump(const char *kind, EXCEPTION_POINTERS *pointers, DWORD thread)
{
    char path[128];
    HANDLE file;
    MINIDUMP_EXCEPTION_INFORMATION exception;
    snprintf(path, sizeof(path), "tmp/pc/%s-%lu.dmp", kind, GetCurrentProcessId());
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    exception.ThreadId = thread;
    exception.ExceptionPointers = pointers;
    exception.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                      pointers ? &exception : NULL, NULL, NULL);
    CloseHandle(file);
}

/* A stack overflow leaves the faulting thread a page or so of stack, too
 * little for the report and the minidump: they run on a thread of their own
 * while the faulting one waits. */
typedef struct {
    EXCEPTION_POINTERS *pointers;
    uintptr_t fault;
    DWORD thread;
} OverflowReport;

static DWORD WINAPI report_overflow(void *argument)
{
    const OverflowReport *job = argument;
    const CONTEXT *context = job->pointers->ContextRecord;
    if (crash_report) {
        crash_report(job->pointers->ExceptionRecord->ExceptionCode, job->fault, context->Eip, context->Esp,
                     context->Ebp);
    }
    write_dump("crash", job->pointers, job->thread);
    return 0;
}

/* Code in the executable installs no exception handlers of its own, so an
 * exception raised there that the guest fault handler (image.c) did not take
 * is fatal. Other modules' exceptions are theirs to handle. */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    const CONTEXT *context = pointers->ContextRecord;
    uintptr_t fault = 0, address;
    Win32_UndoInterruptedFault(pointers->ContextRecord);
    switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
        fault = record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0;
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_BREAKPOINT:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    address = (uintptr_t)record->ExceptionAddress;
    /* The executable, or a call into guest RAM that nothing resolved, or
     * anywhere at all while the thread has no exception handler to try:
     * the game stack runs with an empty chain (state.c), so an exception
     * raised there with none installed is the end of the process. */
    if ((address < image_low || address >= image_high) && !(address >= 0x80000000u && address < 0x80200000u) &&
        !(address >= 0xa0000000u && address < 0xa0200000u) && address >= 0x00200000u &&
        __readfsdword(0) != 0xffffffffu) {
        /* Except the main thread running where no module is: a jump
         * through a bad pointer or return address, which nothing handles. */
        HMODULE owner;
        if (GetCurrentThreadId() != main_id ||
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)address, &owner)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        static OverflowReport job;
        HANDLE reporter;
        job.pointers = pointers;
        job.fault = fault;
        job.thread = GetCurrentThreadId();
        reporter = CreateThread(NULL, 0, report_overflow, &job, 0, NULL);
        if (reporter) WaitForSingleObject(reporter, 30000);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    if (crash_report) crash_report(record->ExceptionCode, fault, context->Eip, context->Esp, context->Ebp);
    write_dump("crash", pointers, GetCurrentThreadId());
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Any other thread's exception that nothing handled (audio, drivers). */
static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    const CONTEXT *context = pointers->ContextRecord;
    fprintf(stderr, "memories-pc: unhandled exception 0x%08lx at %p on thread %lu\n", record->ExceptionCode,
            record->ExceptionAddress, GetCurrentThreadId());
    if (crash_report) {
        crash_report(record->ExceptionCode, record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0,
                     context->Eip, context->Esp, context->Ebp);
    }
    write_dump("crash", pointers, GetCurrentThreadId());
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Last in line: note each exception nobody took, once per address, so a
 * process that ends without a report still says what it saw. */
static LONG CALLBACK on_unclaimed(EXCEPTION_POINTERS *pointers)
{
    static void *seen[64];
    static LONG count;
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    LONG i, n = count;
    if (record->ExceptionCode == 0x406d1388u || record->ExceptionCode == 0x40010006u ||
        record->ExceptionCode == 0x4001000au) {
        return EXCEPTION_CONTINUE_SEARCH; /* thread names and debug output */
    }
    for (i = 0; i < n && i < 64; i++) {
        if (seen[i] == record->ExceptionAddress) return EXCEPTION_CONTINUE_SEARCH;
    }
    if (n < 64) seen[InterlockedIncrement(&count) - 1] = record->ExceptionAddress;
    fprintf(stderr, "memories-pc: exception 0x%08lx at %p (data 0x%08lx) on thread %lu%s\n", record->ExceptionCode,
            record->ExceptionAddress, record->NumberParameters >= 2 ? (unsigned long)record->ExceptionInformation[1] : 0ul,
            GetCurrentThreadId(), GetCurrentThreadId() == main_id ? " (main)" : "");
    return EXCEPTION_CONTINUE_SEARCH;
}

void Win32_SetCrashReporter(Win32CrashReport report)
{
    SetUnhandledExceptionFilter(on_unhandled);
    crash_report = report;
    Win32_ImageRange(&image_low, &image_high);
    AddVectoredExceptionHandler(0, on_exception);
    AddVectoredExceptionHandler(0, on_unclaimed);
}
#endif
