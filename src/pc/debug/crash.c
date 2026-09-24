#define _GNU_SOURCE
#include "pc/compat/fs.h"
#include "crash.h"
#include "log.h"
#include "monitor.h"
#include "symbols.h"
#include "pc/guest/state.h"
#include "pc/platform/platform.h"
#include "pc/sdk/display.h"
#include "pc/platform/paths.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#else
#include <ucontext.h>
#endif

#ifdef _WIN32
#define GAME_STACK_LOW 0xB0000000u /* state.c */
#define GAME_STACK_HIGH 0xB0800000u
#else
#define GAME_STACK_LOW 0x70000000u
#define GAME_STACK_HIGH 0x70800000u
#endif

static unsigned char alternate_stack[64 * 1024];
static uintptr_t main_stack_low, main_stack_high;
static volatile sig_atomic_t reporting;
static int report_fd = -1;

static void output(const char *text, size_t length)
{
    if (length) {
        (void)!write(2, text, length);
        if (report_fd >= 0) (void)!write(report_fd, text, length);
    }
}

static void line(const char *format, uintptr_t a, uintptr_t b, uintptr_t c)
{
    char text[512];
    int length = snprintf(text, sizeof(text), format, a, b, c);
    if (length > 0) output(text, (size_t)(length < (int)sizeof(text) ? length : (int)sizeof(text) - 1));
}

static const char *region(uintptr_t address)
{
#ifdef _WIN32
    uintptr_t image_low, image_high;
    Win32_ImageRange(&image_low, &image_high);
#else
    extern char __executable_start[], etext[];
    uintptr_t image_low = (uintptr_t)__executable_start, image_high = (uintptr_t)etext;
#endif
    if ((address >= 0x80000000u && address < 0x80200000u) ||
        (address >= 0xa0000000u && address < 0xa0200000u) || address < 0x00200000u) return "guest RAM";
    if (address >= 0x1f800000u && address < 0x1f801000u) return "scratchpad";
    if (address >= 0x01000000u && address < 0x0a000000u) return "game section";
    if (address >= GAME_STACK_LOW && address < GAME_STACK_HIGH) return "game stack";
    if (address >= image_low && address < image_high) return "native text";
    return "native/unmapped";
}

static int valid_frame(uintptr_t address)
{
    return (address >= GAME_STACK_LOW && address + 2 * sizeof(uintptr_t) <= GAME_STACK_HIGH) ||
           (address >= main_stack_low && address + 2 * sizeof(uintptr_t) <= main_stack_high);
}

static void symbol_line(int index, uintptr_t address)
{
    uintptr_t offset = 0;
    const char *name = Symbols_Lookup(address, &offset);
    char text[256];
    int length;
#ifdef _WIN32
    char module[64];
    if (!name && Win32_ModuleName(address, module, sizeof(module), &offset)) name = module;
#endif
    if (name) length = snprintf(text, sizeof(text), "  #%d 0x%08lx %s+0x%lx\n",
                                index, (unsigned long)address, name, (unsigned long)offset);
    else length = snprintf(text, sizeof(text), "  #%d 0x%08lx\n", index, (unsigned long)address);
    if (length > 0) output(text, (size_t)length);
}

static void walk(uintptr_t eip, uintptr_t ebp)
{
    int depth = 0;
    symbol_line(depth++, eip);
    while (depth < 32 && valid_frame(ebp)) {
        const uintptr_t *frame = (const uintptr_t *)ebp;
        uintptr_t next = frame[0], return_address = frame[1];
        if (!return_address) break;
        symbol_line(depth++, return_address);
        if (next <= ebp || !valid_frame(next)) break;
        ebp = next;
    }
}

char Crash_ReportDir[512] = "tmp/pc";

void Crash_ChooseReportDir(void)
{
    struct stat info;
    if (!stat("tmp/pc", &info) && S_ISDIR(info.st_mode)) return;   /* running from a checkout */
    if (Paths_User(Crash_ReportDir, sizeof(Crash_ReportDir), "reports") || Paths_MakeDirs(Crash_ReportDir)) {
        snprintf(Crash_ReportDir, sizeof(Crash_ReportDir), ".");
    }
}

static void open_report(void)
{
    MonitorShared *shared = Monitor_Shared();
    char path[640];
    snprintf(path, sizeof(path), "%s/crash-%ld.txt", Crash_ReportDir, (long)getpid());
    report_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (report_fd >= 0) {
        memcpy(shared->report_path, path, sizeof(shared->report_path));
        shared->reported = 1;
    }
}

/* What the game knew: the system and its last log lines. With a monitor
 * those come in its section of the same file, with more of the log. */
static void context(void)
{
    const char *tail_lines[32];
    int count, i;
    if (Monitor_Active()) {
        static const char note[] = "(the system, the log and the console output follow in the monitor's section)\n";
        output(note, sizeof(note) - 1);
        return;
    }
    output(Monitor_Shared()->facts, strnlen(Monitor_Shared()->facts, sizeof(Monitor_Shared()->facts)));
    count = Log_Tail(32, tail_lines);
    if (count) output("log tail:\n", 10);
    for (i = 0; i < count; i++) {
        output(tail_lines[i], strnlen(tail_lines[i], MONITOR_LINE_SIZE));
        if (!strchr(tail_lines[i], '\n')) output("\n", 1);
    }
}

/* `what` names the kind of number: a signal, or a Windows exception code. */
static void report_fatal(const char *what, unsigned long number, uintptr_t fault, uintptr_t eip, uintptr_t esp,
                         uintptr_t ebp)
{
    char text[128];
    open_report();
    snprintf(text, sizeof(text), strcmp(what, "signal") ? "fatal %s 0x%08lx" : "fatal %s %lu", what, number);
    line("memories-pc: %s at 0x%08lx (%s)\n", (uintptr_t)text, fault, (uintptr_t)region(fault));
    line("registers: EIP=0x%08lx ESP=0x%08lx EBP=0x%08lx\n", eip, esp, ebp);
    walk(eip, ebp);
    line("frame=%lu vblank=%lu clock=%ld%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
         (uintptr_t)(long)Platform_ClockRate());
    line("last loaded state slot=%ld\n", (uintptr_t)(long)Memories_LastStateSlot(), 0, 0);
    context();
    if (report_fd >= 0) close(report_fd);
    report_fd = -1;
}

#ifdef _WIN32
static void report_exception(unsigned long code, uintptr_t fault, uintptr_t eip, uintptr_t esp, uintptr_t ebp)
{
    if (reporting++) return;
    report_fatal("exception", code, fault, eip, esp, ebp);
}

/* abort() ends the process without an exception the handlers above would
 * see. */
static void on_abort(int number)
{
    (void)number;
    if (reporting++) _exit(3);
    Crash_ReportFatal("abort()", "the C runtime was asked to abort");
    _exit(3);
}

/* A bad argument to a C runtime function: the function fails and returns,
 * as with MinGW's own handler, which this replaces; the game has always
 * gone on past them. Noted, so a report shows them. */
static void on_invalid_parameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file,
                                 unsigned int at, uintptr_t reserved)
{
    static volatile long noted;
    (void)expression;
    (void)function;
    (void)file;
    (void)at;
    (void)reserved;
    if (__atomic_fetch_add(&noted, 1, __ATOMIC_RELAXED) < 8) {
        fprintf(stderr, "memories-pc: a C runtime function was given an invalid parameter; it failed (called from %p)\n",
                __builtin_return_address(0));
    }
}

void Crash_Init(void)
{
    Crash_ChooseReportDir();
    Win32_StackRange(&main_stack_low, &main_stack_high);
    Win32_SetCrashReporter(report_exception);
    signal(SIGABRT, on_abort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(on_invalid_parameter);
}
#else
void Crash_HandleSignal(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    struct sigaction action;
    if (reporting++) _exit(128 + number);
    report_fatal("signal", (unsigned long)number, info ? (uintptr_t)info->si_addr : 0,
                 (uintptr_t)user->uc_mcontext.gregs[REG_EIP], (uintptr_t)user->uc_mcontext.gregs[REG_ESP],
                 (uintptr_t)user->uc_mcontext.gregs[REG_EBP]);
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(number, &action, NULL);
    raise(number);
}

static void installed_handler(int number, siginfo_t *info, void *context)
{
    Crash_HandleSignal(number, info, context);
}

void Crash_Init(void)
{
    static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    stack_t stack;
    struct sigaction action;
    pthread_attr_t attributes;
    void *address;
    size_t size;
    unsigned i;
    Crash_ChooseReportDir();
    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof(alternate_stack);
    stack.ss_flags = 0;
    sigaltstack(&stack, NULL);
    if (!pthread_getattr_np(pthread_self(), &attributes)) {
        if (!pthread_attr_getstack(&attributes, &address, &size)) {
            main_stack_low = (uintptr_t)address;
            main_stack_high = main_stack_low + size;
        }
        pthread_attr_destroy(&attributes);
    }
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = installed_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) sigaction(signals[i], &action, NULL);
}
#endif

void Crash_ReportSoft(const char *kind, const char *detail)
{
    const char *tail_lines[32];
    int count, i;
    fprintf(stderr, "memories-pc: %s: %s\n", kind, detail ? detail : "");
    fprintf(stderr, "frame=%u vblank=%u clock=%d%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
            Platform_ClockRate());
    walk((uintptr_t)__builtin_return_address(0), (uintptr_t)__builtin_frame_address(0));
    count = Log_Tail(32, tail_lines);
    for (i = 0; i < count; i++) fprintf(stderr, "  %s%s", tail_lines[i], strchr(tail_lines[i], '\n') ? "" : "\n");
}

void Crash_ReportFatal(const char *kind, const char *detail)
{
    uintptr_t ebp = (uintptr_t)__builtin_frame_address(0);
    fflush(stdout);
    open_report();
    output("memories-pc: fatal: ", 20);
    output(kind, strlen(kind));
    if (detail) {
        output(": ", 2);
        output(detail, strlen(detail));
    }
    output("\n", 1);
    walk((uintptr_t)__builtin_return_address(0), ebp);
    line("frame=%lu vblank=%lu clock=%ld%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
         (uintptr_t)(long)Platform_ClockRate());
    line("last loaded state slot=%ld\n", (uintptr_t)(long)Memories_LastStateSlot(), 0, 0);
    context();
    if (report_fd >= 0) close(report_fd);
    report_fd = -1;
}

void Crash_ReportHang(void *context_pointer)
{
    char path[640];
    uintptr_t eip, esp, ebp;
#ifdef _WIN32
    Win32_ContextRegisters(context_pointer, &eip, &esp, &ebp);
#else
    ucontext_t *user = context_pointer;
    eip = (uintptr_t)user->uc_mcontext.gregs[REG_EIP];
    esp = (uintptr_t)user->uc_mcontext.gregs[REG_ESP];
    ebp = (uintptr_t)user->uc_mcontext.gregs[REG_EBP];
#endif
    report_fd = -1;
    snprintf(path, sizeof(path), "%s/hang-%ld.txt", Crash_ReportDir, (long)getpid());
    report_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666); /* the monitor may have begun it */
    {
        static const char message[] = "memories-pc: no VSync for 5 s\n";
        output(message, sizeof(message) - 1);
        line("registers: EIP=0x%08lx ESP=0x%08lx EBP=0x%08lx\n", eip, esp, ebp);
        walk(eip, ebp);
        line("frame=%lu vblank=%lu clock=%ld%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
             (uintptr_t)(long)Platform_ClockRate());
        context();
    }
    if (report_fd >= 0) close(report_fd);
    report_fd = -1;
}
