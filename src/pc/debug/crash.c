#define _GNU_SOURCE
#include "crash.h"
#include "log.h"
#include "symbols.h"
#include "pc/guest/state.h"
#include "pc/platform/platform.h"
#include "pc/sdk/display.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

#define GAME_STACK_LOW 0x70000000u
#define GAME_STACK_HIGH 0x70800000u

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
    extern char __executable_start[], etext[];
    if ((address >= 0x80000000u && address < 0x80200000u) ||
        (address >= 0xa0000000u && address < 0xa0200000u) || address < 0x00200000u) return "guest RAM";
    if (address >= 0x1f800000u && address < 0x1f801000u) return "scratchpad";
    if (address >= 0x01000000u && address < 0x0a000000u) return "game section";
    if (address >= GAME_STACK_LOW && address < GAME_STACK_HIGH) return "game stack";
    if (address >= (uintptr_t)__executable_start && address < (uintptr_t)etext) return "native text";
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

static void open_report(void)
{
    char path[128];
    snprintf(path, sizeof(path), "tmp/pc/crash-%ld.txt", (long)getpid());
    report_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

void Crash_HandleSignal(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    uintptr_t eip = (uintptr_t)user->uc_mcontext.gregs[REG_EIP];
    uintptr_t esp = (uintptr_t)user->uc_mcontext.gregs[REG_ESP];
    uintptr_t ebp = (uintptr_t)user->uc_mcontext.gregs[REG_EBP];
    uintptr_t fault = info ? (uintptr_t)info->si_addr : 0;
    const char *tail_lines[32];
    int count, i;
    struct sigaction action;
    if (reporting++) _exit(128 + number);
    open_report();
    line("memories-pc: fatal signal %lu at 0x%08lx (%s)\n", (uintptr_t)number, fault, (uintptr_t)region(fault));
    line("registers: EIP=0x%08lx ESP=0x%08lx EBP=0x%08lx\n", eip, esp, ebp);
    walk(eip, ebp);
    line("frame=%lu vblank=%lu clock=%ld%%\n", Memories_PresentedFrames(), Platform_VBlankCount(),
         (uintptr_t)(long)Platform_ClockRate());
    line("last loaded state slot=%ld\n", (uintptr_t)(long)Memories_LastStateSlot(), 0, 0);
    count = Log_Tail(32, tail_lines);
    if (count) output("log tail:\n", 10);
    for (i = 0; i < count; i++) {
        output(tail_lines[i], strnlen(tail_lines[i], 512));
        if (!strchr(tail_lines[i], '\n')) output("\n", 1);
    }
    if (report_fd >= 0) close(report_fd);
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

void Crash_ReportSoft(const char *kind, const char *detail)
{
    const char *tail_lines[32];
    int count, i;
    fprintf(stderr, "memories-pc: %s: %s\n", kind, detail ? detail : "");
    count = Log_Tail(32, tail_lines);
    for (i = 0; i < count; i++) fprintf(stderr, "  %s%s", tail_lines[i], strchr(tail_lines[i], '\n') ? "" : "\n");
}
