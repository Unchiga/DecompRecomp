/* The crash monitor (monitor.h). The game half: the shared block, the facts,
 * the system description. The monitor half: start the game, pass its
 * console output through, watch its heartbeat, and write the report when
 * it freezes or ends badly.
 *
 * Linux: the monitor forks before the game has done anything; the block is
 * a memfd the game maps again after a restart (execv keeps the descriptor,
 * and MEMORIES_MONITOR_FD names it). A freeze is looked at through ptrace
 * (a parent may trace its child under Yama's default) and /proc.
 * Windows: the monitor starts the executable again as its child, in a job
 * that ends it with the monitor; MEMORIES_MONITOR_HANDLE names the inherited
 * mapping. A freeze is looked at with GetThreadContext and a minidump,
 * taken from outside, so a game thread holding the heap or loader lock
 * cannot stop them. A restart (Win32_Restart) is the monitor starting the
 * game again. */
#define _GNU_SOURCE
#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#define PSAPI_VERSION 2
#endif
#include "monitor.h"
#include "crash.h"
#include "symbols.h"
#include "pc/platform/paths.h"
#include <cpuid.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* platform.h brings the game's types, which clash with <windows.h>. */
void Platform_ShowError(const char *title, const char *message);

#define MAGIC 0x314E4F4Du /* "MON1" */
#define VERSION 1u
#define STARTUP_SECONDS 30        /* before the first VSync: the disc, the window, the GL driver */
#define CONSOLE_LINES 200
#define CONSOLE_LINE_SIZE 320
#define SESSION_LIMIT (16u << 20) /* a trace run can print without end */

static MonitorShared own_block;
static MonitorShared *shared = &own_block;
static int active;

MonitorShared *Monitor_Shared(void)
{
    return shared;
}

int Monitor_Active(void)
{
    return active;
}

void Monitor_Modal(int on)
{
    if (on) __atomic_add_fetch(&shared->modal, 1, __ATOMIC_SEQ_CST);
    else if (shared->modal) __atomic_sub_fetch(&shared->modal, 1, __ATOMIC_SEQ_CST);
}

void Monitor_Fact(const char *key, const char *format, ...)
{
    char value[1024], rebuilt[MONITOR_FACTS_SIZE];
    size_t key_length = strlen(key), used = 0;
    const char *at = shared->facts;
    char *scan;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(value, sizeof(value), format, arguments);
    va_end(arguments);
    for (scan = value; *scan; scan++) {
        if (*scan == '\n' || *scan == '\r') *scan = ' ';
    }
    while (*at) {
        const char *end = strchr(at, '\n');
        size_t length = end ? (size_t)(end - at) + 1 : strlen(at);
        int same = length > key_length && !strncmp(at, key, key_length) && at[key_length] == ':';
        if (!same && used + length < sizeof(rebuilt)) {
            memcpy(rebuilt + used, at, length);
            used += length;
        }
        at += length;
    }
    used += (size_t)snprintf(rebuilt + used, sizeof(rebuilt) - used, "%s: %s\n", key, value);
    if (used >= sizeof(rebuilt)) used = sizeof(rebuilt) - 1;
    rebuilt[used] = '\0';
    __atomic_add_fetch(&shared->facts_sequence, 1, __ATOMIC_SEQ_CST);
    memcpy(shared->facts, rebuilt, used + 1);
    __atomic_add_fetch(&shared->facts_sequence, 1, __ATOMIC_SEQ_CST);
}

static void read_file_line(const char *relative, char *out, size_t size)
{
    char path[640];
    FILE *file;
    out[0] = '\0';
    if (Paths_Program(path, sizeof(path), relative)) return;
    file = fopen(path, "r");
    if (!file) return;
    if (fgets(out, (int)size, file)) out[strcspn(out, "\r\n")] = '\0';
    fclose(file);
}

static void cpu_name(char *out, size_t size)
{
    unsigned words[12], highest = 0, unused, i;
    char *start;
    snprintf(out, size, "unknown");
    if (!__get_cpuid(0x80000000u, &highest, &unused, &unused, &unused) || highest < 0x80000004u) return;
    for (i = 0; i < 3; i++) __get_cpuid(0x80000002u + i, &words[4 * i], &words[4 * i + 1], &words[4 * i + 2], &words[4 * i + 3]);
    start = (char *)words;
    start[47] = '\0';
    while (*start == ' ') start++;
    snprintf(out, size, "%s", start);
}

void Monitor_NoteSystem(void)
{
    char build[64], commit[96], cpu[64];
    time_t now = time(NULL);
    char started[64];
    read_file_line("buildid", build, sizeof(build));
    read_file_line("commit", commit, sizeof(commit));
    strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S %z", localtime(&now));
    Monitor_Fact("build", "%s (commit %s)", build[0] ? build : "unknown", commit[0] ? commit : "unknown");
    Monitor_Fact("started", "%s", started);
    cpu_name(cpu, sizeof(cpu));
#ifdef _WIN32
    {
        typedef LONG(WINAPI * GetVersionFunction)(OSVERSIONINFOW *);
        typedef const char *(CDECL * WineVersionFunction)(void);
        typedef void(CDECL * WineHostFunction)(const char **, const char **);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        GetVersionFunction get_version = (GetVersionFunction)(void *)GetProcAddress(ntdll, "RtlGetVersion");
        WineVersionFunction wine = (WineVersionFunction)(void *)GetProcAddress(ntdll, "wine_get_version");
        WineHostFunction host = (WineHostFunction)(void *)GetProcAddress(ntdll, "wine_get_host_version");
        OSVERSIONINFOW version;
        MEMORYSTATUSEX memory;
        SYSTEM_INFO system;
        BOOL wow64 = FALSE;
        memset(&version, 0, sizeof(version));
        version.dwOSVersionInfoSize = sizeof(version);
        if (get_version) get_version(&version);
        IsWow64Process(GetCurrentProcess(), &wow64);
        if (wine) {
            const char *sysname = "?", *release = "?";
            if (host) host(&sysname, &release);
            Monitor_Fact("os", "Wine %s on %s %s (as Windows %lu.%lu.%lu)", wine(), sysname, release,
                         version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
        } else {
            Monitor_Fact("os", "Windows %lu.%lu build %lu%s", version.dwMajorVersion, version.dwMinorVersion,
                         version.dwBuildNumber, wow64 ? ", 64-bit" : ", 32-bit");
        }
        GetNativeSystemInfo(&system);
        Monitor_Fact("cpu", "%s, %lu threads", cpu, system.dwNumberOfProcessors);
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            Monitor_Fact("memory", "%llu MB, %llu MB free", (unsigned long long)(memory.ullTotalPhys >> 20),
                         (unsigned long long)(memory.ullAvailPhys >> 20));
        }
    }
#else
    {
        struct utsname name;
        struct sysinfo memory;
        char pretty[160] = "", line[256];
        const char *session = getenv("XDG_SESSION_TYPE"), *desktop = getenv("XDG_CURRENT_DESKTOP");
        FILE *release = fopen("/etc/os-release", "r");
        while (release && fgets(line, sizeof(line), release)) {
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *value = line + 12;
                value[strcspn(value, "\r\n")] = '\0';
                if (*value == '"') {
                    value++;
                    if (*value && value[strlen(value) - 1] == '"') value[strlen(value) - 1] = '\0';
                }
                snprintf(pretty, sizeof(pretty), "%s", value);
            }
        }
        if (release) fclose(release);
        if (!uname(&name)) {
            Monitor_Fact("os", "%s, %s %s %s; session %s, desktop %s", pretty[0] ? pretty : "Linux", name.sysname,
                         name.release, name.machine, session ? session : "?", desktop ? desktop : "?");
        }
        Monitor_Fact("cpu", "%s, %ld threads", cpu, sysconf(_SC_NPROCESSORS_ONLN));
        if (!sysinfo(&memory)) {
            Monitor_Fact("memory", "%llu MB, %llu MB free",
                         (unsigned long long)memory.totalram * memory.mem_unit >> 20,
                         (unsigned long long)memory.freeram * memory.mem_unit >> 20);
        }
    }
#endif
}

/* A new game in the block: the first start, or a restart. */
static void begin_generation(void)
{
    shared->running = 0;
    shared->paused = 0;
    shared->modal = 0;
    shared->exiting = 0;
    shared->restart = 0;
    shared->error_shown = 0;
    shared->reported = 0;
    shared->report_path[0] = '\0';
    shared->module[0] = '\0';
    shared->facts[0] = '\0';
    __atomic_add_fetch(&shared->generation, 1, __ATOMIC_SEQ_CST);
}

static int wanted(void)
{
    const char *off = getenv("MEMORIES_NO_MONITOR");
    if (off && *off && strcmp(off, "0")) return 0;
#ifdef _WIN32
    if (IsDebuggerPresent()) return 0;
#else
    {
        char line[128];
        int traced = 0;
        FILE *status = fopen("/proc/self/status", "r");
        while (status && fgets(line, sizeof(line), status)) {
            if (!strncmp(line, "TracerPid:", 10)) traced = atoi(line + 10) != 0;
        }
        if (status) fclose(status);
        if (traced) return 0;
    }
#endif
    return 1;
}

/* ---- The monitor ---------------------------------------------------- */

typedef struct {
    int generation;
    uint32_t beat;
    uint64_t beat_ms, started_ms;
    int hang_reported, recovered;
    uint64_t hang_at_ms;
    char hang_path[640];
} Watch;

static Watch watch;
static unsigned long game_pid;
static char console[CONSOLE_LINES][CONSOLE_LINE_SIZE];
static unsigned console_head;
static char partial[CONSOLE_LINE_SIZE];
static size_t partial_length;
static FILE *session;
static size_t session_bytes;
static FILE *out;
#ifdef _WIN32
static HANDLE game_process, console_read, original_stderr, job;
static CRITICAL_SECTION console_lock;
#else
static pid_t game;
static int console_fd = -1;
static int stashed_status, have_stashed;
#endif

static uint64_t now_ms(void)
{
#ifdef _WIN32
    /* Not the tick count, which runs on while the computer sleeps: waking
     * would look like a freeze. */
    ULONGLONG interrupt_time;
    QueryUnbiasedInterruptTime(&interrupt_time);
    return (uint64_t)interrupt_time / 10000u;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

/* Reports get posted where anyone can read them: the home folder, which
 * names the player, is written "~". */
static void put(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void put(const char *format, ...)
{
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
    size_t home_length = home ? strlen(home) : 0;
    char text[4096];
    const char *at, *found;
    va_list arguments;
    if (!out) return;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    at = text;
    while (home_length > 3 && (found = strstr(at, home))) {
        fwrite(at, 1, (size_t)(found - at), out);
        fputc('~', out);
        at = found + home_length;
    }
    fputs(at, out);
}

static void lock_console(int on)
{
#ifdef _WIN32
    if (on) EnterCriticalSection(&console_lock);
    else LeaveCriticalSection(&console_lock);
#else
    (void)on; /* one thread */
#endif
}

static void keep_line(void)
{
    partial[partial_length] = '\0';
    snprintf(console[console_head % CONSOLE_LINES], CONSOLE_LINE_SIZE, "%s", partial);
    console_head++;
    partial_length = 0;
}

/* The game's console output: on to the monitor's own, into the session log,
 * and its last lines kept for a report. */
static void relay(const char *data, size_t size)
{
    size_t i;
#ifdef _WIN32
    DWORD written;
    if (original_stderr && original_stderr != INVALID_HANDLE_VALUE) WriteFile(original_stderr, data, (DWORD)size, &written, NULL);
#else
    size_t done = 0;
    while (done < size) {
        ssize_t written = write(2, data + done, size - done);
        if (written <= 0) break;
        done += (size_t)written;
    }
#endif
    lock_console(1);
    if (session && session_bytes < SESSION_LIMIT) {
        fwrite(data, 1, size, session);
        session_bytes += size;
        if (session_bytes >= SESSION_LIMIT) fputs("\n[monitor: the session log stops here, 16 MB]\n", session);
        fflush(session);
    }
    for (i = 0; i < size; i++) {
        if (data[i] == '\n') keep_line();
        else if (data[i] != '\r' && partial_length < CONSOLE_LINE_SIZE - 1) partial[partial_length++] = data[i];
    }
    lock_console(0);
}

static void open_session(void)
{
    char last[640], previous[640];
    time_t now = time(NULL);
    char when[64];
    snprintf(last, sizeof(last), "%s/last-session.log", Crash_ReportDir);
    snprintf(previous, sizeof(previous), "%s/previous-session.log", Crash_ReportDir);
    remove(previous);
    rename(last, previous);
    session = fopen(last, "w");
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %z", localtime(&now));
    if (session) fprintf(session, "[monitor: session started %s]\n", when);
}

static void note_session(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void note_session(const char *format, ...)
{
    va_list arguments;
    if (!session) return;
    lock_console(1);
    fputs("[monitor: ", session);
    va_start(arguments, format);
    vfprintf(session, format, arguments);
    va_end(arguments);
    fputs("]\n", session);
    fflush(session);
    lock_console(0);
}

static int dialogs_allowed(void)
{
    const char *dialog = getenv("MEMORIES_CRASH_DIALOG"), *headless = getenv("MEMORIES_HEADLESS");
    const char *script = getenv("MEMORIES_SDL_SCRIPT");
    if (dialog && *dialog) return strcmp(dialog, "0") != 0;
    if (headless && *headless && strcmp(headless, "0")) return 0;
    return !(script && *script);
}

/* Remote memory and names --------------------------------------------- */

static int remote_read(uintptr_t address, void *buffer, size_t size)
{
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(game_process, (LPCVOID)address, buffer, size, &got) && got == size ? 0 : -1;
#else
    struct iovec local = {buffer, size}, remote = {(void *)address, size};
    return process_vm_readv(game, &local, 1, &remote, 1, 0) == (ssize_t)size ? 0 : -1;
#endif
}

typedef struct {
    uintptr_t low, high, offset;
    int executable;
    char name[64];
} Mapping;

static Mapping mappings[512];
static int mapping_count;

static void load_mappings(void)
{
    mapping_count = 0;
#ifdef _WIN32
    {
        HMODULE modules[256];
        DWORD needed = 0, i;
        if (!EnumProcessModules(game_process, modules, sizeof(modules), &needed)) return;
        for (i = 0; i < needed / sizeof(HMODULE) && i < 256 && mapping_count < 512; i++) {
            MODULEINFO information;
            Mapping *mapping = &mappings[mapping_count];
            if (!GetModuleInformation(game_process, modules[i], &information, sizeof(information))) continue;
            mapping->low = (uintptr_t)information.lpBaseOfDll;
            mapping->high = mapping->low + information.SizeOfImage;
            mapping->offset = 0;
            mapping->executable = 1;
            if (!GetModuleBaseNameA(game_process, modules[i], mapping->name, sizeof(mapping->name))) {
                snprintf(mapping->name, sizeof(mapping->name), "module");
            }
            mapping_count++;
        }
    }
#else
    {
        char path[64], line[512];
        FILE *maps;
        snprintf(path, sizeof(path), "/proc/%d/maps", (int)game);
        maps = fopen(path, "r");
        while (maps && fgets(line, sizeof(line), maps) && mapping_count < 512) {
            unsigned long low, high, offset;
            char permissions[8], file[400] = "";
            Mapping *mapping = &mappings[mapping_count];
            const char *base;
            if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %399[^\n]", &low, &high, permissions, &offset, file) < 4) continue;
            base = strrchr(file, '/');
            mapping->low = low;
            mapping->high = high;
            mapping->offset = offset;
            mapping->executable = permissions[2] == 'x';
            snprintf(mapping->name, sizeof(mapping->name), "%s", base ? base + 1 : file[0] ? file : "anonymous");
            mapping_count++;
        }
        if (maps) fclose(maps);
    }
#endif
}

static const Mapping *mapping_at(uintptr_t address)
{
    int i;
    for (i = 0; i < mapping_count; i++) {
        if (address >= mappings[i].low && address < mappings[i].high) return &mappings[i];
    }
    return NULL;
}

/* Whether an address is in code: an executable mapping (Linux), or an
 * executable section of this executable, which is the game's, or any DLL
 * (Windows). The symbol table has data symbols too, and on Windows a
 * symbol's size runs to the next one. */
static int in_code(uintptr_t address)
{
#ifdef _WIN32
    static uintptr_t ranges[16][2];
    static int count = -1;
    const Mapping *mapping;
    int i;
    if (count < 0) {
        const unsigned char *base = (const unsigned char *)GetModuleHandleW(NULL);
        const IMAGE_NT_HEADERS *headers = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
        const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(headers);
        count = 0;
        for (i = 0; i < headers->FileHeader.NumberOfSections && count < 16; i++, section++) {
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            ranges[count][0] = (uintptr_t)base + section->VirtualAddress;
            ranges[count][1] = ranges[count][0] + section->Misc.VirtualSize;
            count++;
        }
    }
    for (i = 0; i < count; i++) {
        if (address >= ranges[i][0] && address < ranges[i][1]) return 1;
    }
    mapping = mapping_at(address);
    return mapping && mapping->low != (uintptr_t)GetModuleHandleW(NULL);
#else
    const Mapping *mapping = mapping_at(address);
    return mapping && mapping->executable;
#endif
}

/* "name+0x12" for code: a function of the executable, else a library's
 * name and the offset into its file. Empty when it is neither. */
static int name_code(uintptr_t address, char *text, size_t size)
{
    uintptr_t offset = 0;
    const char *name;
    const Mapping *mapping;
    text[0] = '\0';
    if (!in_code(address)) return 0;
    name = Symbols_Lookup(address, &offset);
    if (name) {
        snprintf(text, size, "%s+0x%lx", name, (unsigned long)offset);
        return 1;
    }
    mapping = mapping_at(address);
    if (mapping) {
        snprintf(text, size, "%s+0x%lx", mapping->name, (unsigned long)(address - mapping->low + mapping->offset));
        return 1;
    }
    return 0;
}

static void walk_remote(uintptr_t eip, uintptr_t esp, uintptr_t ebp)
{
    char name[160];
    uint32_t frame[2];
    int depth = 1, i;
    name_code(eip, name, sizeof(name));
    put("    #0  0x%08lx %s\n", (unsigned long)eip, name);
    while (depth < 40 && ebp && !(ebp & 3) && !remote_read(ebp, frame, sizeof(frame)) && frame[1]) {
        name_code(frame[1], name, sizeof(name));
        put("    #%-2d 0x%08lx %s\n", depth++, (unsigned long)frame[1], name);
        if (frame[0] <= ebp) break;
        ebp = frame[0];
    }
    if (depth >= 4 || !esp) return;
    /* A short chain (a library without frame pointers): list what on the
     * stack looks like return addresses into code, as a hint. */
    put("    code addresses on the stack (some are stale):\n");
    for (i = 0; i < 2048 && depth < 40; i++) {
        uint32_t word;
        if (remote_read(esp + 4u * (unsigned)i, &word, sizeof(word))) break;
        if (word > 0x10000u && name_code(word, name, sizeof(name))) {
            put("      [esp+0x%x] 0x%08lx %s\n", 4 * i, (unsigned long)word, name);
            depth++;
        }
    }
}

#ifndef _WIN32
static const char *syscall_name(long number)
{
    switch (number) { /* i386 */
    case 3: return "read";
    case 4: return "write";
    case 54: return "ioctl";
    case 142: return "select";
    case 158: return "sched_yield";
    case 162: return "nanosleep";
    case 168: return "poll";
    case 240: return "futex";
    case 256: return "epoll_wait";
    case 265: return "clock_gettime";
    case 267: return "clock_nanosleep";
    case 308: return "pselect6";
    case 309: return "ppoll";
    case 319: return "epoll_pwait";
    case 372: return "recvmsg";
    case 407: return "clock_nanosleep_time64";
    case 414: return "ppoll_time64";
    case 422: return "futex_time64";
    default: return "";
    }
}

static int thread_registers(pid_t tid, uintptr_t *eip, uintptr_t *esp, uintptr_t *ebp)
{
    struct user_regs_struct registers;
    int status;
    if (ptrace(PTRACE_SEIZE, tid, 0, 0)) return -1;
    if (ptrace(PTRACE_INTERRUPT, tid, 0, 0)) {
        ptrace(PTRACE_DETACH, tid, 0, 0);
        return -1;
    }
    for (;;) {
        pid_t got = waitpid(tid, &status, __WALL);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tid == game) {
                stashed_status = status;
                have_stashed = 1;
            }
            return -1;
        }
        if (WIFSTOPPED(status)) {
            /* An interrupt stop, or a signal on its way (the 1 kHz clock
             * makes that likely), which detaching hands back. */
            int inject = (status >> 16) == PTRACE_EVENT_STOP ? 0 : WSTOPSIG(status);
            int result = ptrace(PTRACE_GETREGS, tid, 0, &registers) ? -1 : 0;
            ptrace(PTRACE_DETACH, tid, 0, (void *)(uintptr_t)inject);
            if (result) return -1;
            *eip = (uintptr_t)registers.eip;
            *esp = (uintptr_t)registers.esp;
            *ebp = (uintptr_t)registers.ebp;
            return 0;
        }
    }
}

static void proc_text(pid_t tid, const char *what, char *text, size_t size)
{
    char path[96];
    FILE *file;
    snprintf(path, sizeof(path), "/proc/%d/task/%d/%s", (int)game, (int)tid, what);
    text[0] = '\0';
    file = fopen(path, "r");
    if (!file) return;
    if (!fgets(text, (int)size, file)) text[0] = '\0';
    text[strcspn(text, "\n")] = '\0';
    fclose(file);
}

static void dump_threads(void)
{
    char path[64];
    DIR *tasks;
    struct dirent *entry;
    snprintf(path, sizeof(path), "/proc/%d/task", (int)game);
    tasks = opendir(path);
    if (!tasks) {
        put("  (the game's threads could not be listed)\n");
        return;
    }
    while ((entry = readdir(tasks))) {
        pid_t tid = (pid_t)atoi(entry->d_name);
        char comm[64], stat[512], syscall_text[256];
        const char *state = "?";
        uintptr_t eip, esp, ebp;
        long number;
        if (tid <= 0) continue;
        proc_text(tid, "comm", comm, sizeof(comm));
        proc_text(tid, "stat", stat, sizeof(stat));
        proc_text(tid, "syscall", syscall_text, sizeof(syscall_text));
        if (strrchr(stat, ')') && strrchr(stat, ')')[1]) state = strrchr(stat, ')') + 2;
        put("  thread %d \"%s\"%s, state %c", (int)tid, comm, tid == game ? " (main: the game's)" : "", *state);
        if (sscanf(syscall_text, "%ld", &number) == 1) put(", in system call %ld %s", number, syscall_name(number));
        else if (!strncmp(syscall_text, "running", 7)) put(", running");
        put("\n");
        if (!thread_registers(tid, &eip, &esp, &ebp)) {
            put("    EIP=0x%08lx ESP=0x%08lx EBP=0x%08lx\n", (unsigned long)eip, (unsigned long)esp, (unsigned long)ebp);
            walk_remote(eip, esp, ebp);
        } else {
            /* No ptrace (Yama level 2 and up): the system call's stack and
             * program counters are still in /proc for a thread in one. */
            unsigned long values[9];
            if (sscanf(syscall_text, "%lx %lx %lx %lx %lx %lx %lx %lx %lx", &values[0], &values[1], &values[2],
                       &values[3], &values[4], &values[5], &values[6], &values[7], &values[8]) == 9) {
                put("    (no ptrace: registers from /proc) ESP=0x%08lx EIP=0x%08lx\n", values[7], values[8]);
                walk_remote(values[8], values[7], 0);
            } else {
                put("    (its registers could not be read: %s)\n", strerror(errno));
            }
        }
    }
    closedir(tasks);
}
#else
static void dump_threads(void)
{
    typedef HRESULT(WINAPI * DescriptionFunction)(HANDLE, PWSTR *);
    DescriptionFunction describe =
        (DescriptionFunction)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry;
    int first = 1;
    if (snapshot == INVALID_HANDLE_VALUE) {
        put("  (the game's threads could not be listed: error %lu)\n", GetLastError());
        return;
    }
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        HANDLE thread;
        CONTEXT context;
        char name[96] = "";
        int got = 0;
        if (entry.th32OwnerProcessID != (DWORD)game_pid) continue;
        thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                            entry.th32ThreadID);
        if (!thread) {
            put("  thread %lu: cannot open (error %lu)\n", entry.th32ThreadID, GetLastError());
            continue;
        }
        if (describe) {
            PWSTR wide = NULL;
            if (SUCCEEDED(describe(thread, &wide)) && wide) {
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), NULL, NULL);
                LocalFree(wide);
            }
        }
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(thread) != (DWORD)-1) {
            got = GetThreadContext(thread, &context) != 0;
            ResumeThread(thread);
        }
        /* Toolhelp lists a process's threads oldest first: the main one. */
        put("  thread %lu%s%s%s%s\n", entry.th32ThreadID, name[0] ? " \"" : "", name, name[0] ? "\"" : "",
            first ? " (main: the game's)" : "");
        first = 0;
        if (got) {
            put("    EIP=0x%08lx ESP=0x%08lx EBP=0x%08lx\n", (unsigned long)context.Eip, (unsigned long)context.Esp,
                (unsigned long)context.Ebp);
            walk_remote(context.Eip, context.Esp, context.Ebp);
        } else {
            put("    (its registers could not be read: error %lu)\n", GetLastError());
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

static int write_minidump(const char *path)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL written;
    if (file == INVALID_HANDLE_VALUE) return -1;
    written = MiniDumpWriteDump(game_process, (DWORD)game_pid, file,
                                (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                                MiniDumpWithUnloadedModules),
                                NULL, NULL, NULL);
    CloseHandle(file);
    if (!written) remove(path);
    return written ? 0 : -1;
}
#endif

/* The report -------------------------------------------------------------- */

static void put_facts(void)
{
    char copy[MONITOR_FACTS_SIZE];
    int tries;
    for (tries = 0; tries < 5; tries++) {
        uint32_t before = __atomic_load_n(&shared->facts_sequence, __ATOMIC_SEQ_CST);
        memcpy(copy, shared->facts, sizeof(copy));
        if (!(before & 1) && before == __atomic_load_n(&shared->facts_sequence, __ATOMIC_SEQ_CST)) break;
    }
    copy[sizeof(copy) - 1] = '\0';
    put("%s", copy[0] ? copy : "(the game had not described the system yet)\n");
}

static void put_game_state(void)
{
    char module[sizeof(shared->module)];
    memcpy(module, shared->module, sizeof(module));
    module[sizeof(module) - 1] = '\0';
    put("game: frame %u, vblank %u, runtime module %s%s%s\n", shared->frame, shared->vblank,
        module[0] ? module : "none yet", shared->paused ? ", paused" : "", shared->running ? "" : ", before its first frame");
}

static void put_log(void)
{
    uint32_t head = __atomic_load_n(&shared->tail_head, __ATOMIC_ACQUIRE), i;
    uint32_t count = head < MONITOR_TAIL_LINES ? head : MONITOR_TAIL_LINES;
    put("\nthe game's log, newest last (%u lines):\n", count);
    for (i = head - count; i != head; i++) {
        char line[MONITOR_LINE_SIZE];
        memcpy(line, shared->tail[i % MONITOR_TAIL_LINES], sizeof(line));
        line[sizeof(line) - 1] = '\0';
        line[strcspn(line, "\n")] = '\0';
        put("  %s\n", line);
    }
}

static void put_console(void)
{
    unsigned count, i;
    lock_console(1);
    count = console_head < CONSOLE_LINES ? console_head : CONSOLE_LINES;
    put("\nthe game's console output, newest last (%u lines; all of it in last-session.log):\n", count);
    for (i = console_head - count; i != console_head; i++) put("  %s\n", console[i % CONSOLE_LINES]);
    if (partial_length) {
        partial[partial_length] = '\0';
        put("  %s\n", partial);
    }
    lock_console(0);
}

static void put_heading(const char *what)
{
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %z", localtime(&now));
    put("\n==== monitor: %s ====\n", what);
    put("at %s, %.1f s after the game started (pid %lu)\n", when, (double)(now_ms() - watch.started_ms) / 1000.0,
        game_pid);
}

static void report_hang(uint64_t quiet_ms)
{
    char what[160], dump[640];
    snprintf(watch.hang_path, sizeof(watch.hang_path), "%s/hang-%lu.txt", Crash_ReportDir, game_pid);
    out = fopen(watch.hang_path, "a");
    if (!out) return;
    snprintf(what, sizeof(what), shared->running ? "no frame for %.1f s" : "no first frame after %.1f s",
             (double)quiet_ms / 1000.0);
    put_heading(what);
    put_facts();
    put_game_state();
    load_mappings();
    put("\nthreads:\n");
    dump_threads();
#ifdef _WIN32
    snprintf(dump, sizeof(dump), "%s/hang-%lu.dmp", Crash_ReportDir, game_pid);
    if (!write_minidump(dump)) put("\nminidump: %s\n", dump);
#else
    (void)dump;
#endif
    put_log();
    put_console();
    fclose(out);
    out = NULL;
    note_session("no frame for %.1f s: %s", (double)quiet_ms / 1000.0, watch.hang_path);
    fprintf(stderr, "memories-pc: the game has not finished a frame for %.0f s; report in %s\n",
            (double)quiet_ms / 1000.0, watch.hang_path);
}

static void check(void)
{
    uint64_t now = now_ms();
    uint32_t beat = __atomic_load_n(&shared->heartbeat, __ATOMIC_SEQ_CST);
    const char *setting = getenv("MEMORIES_WATCHDOG");
    unsigned seconds = setting && *setting ? (unsigned)strtoul(setting, NULL, 10) : 5;
    int generation = (int)__atomic_load_n(&shared->generation, __ATOMIC_SEQ_CST);
    if (generation != watch.generation) {
        watch.generation = generation;
        watch.beat = beat;
        watch.beat_ms = watch.started_ms = now;
        watch.hang_reported = 0;
        return;
    }
    if (beat != watch.beat || shared->paused || shared->modal || shared->exiting) {
        if (watch.hang_reported && !watch.recovered && beat != watch.beat) {
            FILE *file = fopen(watch.hang_path, "a");
            double seconds_stuck = (double)(now - watch.beat_ms) / 1000.0;
            watch.recovered = 1;
            if (file) {
                fprintf(file, "\nmonitor: the game went on after %.1f s (a long wait, not a freeze)\n", seconds_stuck);
                fclose(file);
            }
            note_session("frames again after %.1f s", seconds_stuck);
        }
        if (beat != watch.beat) watch.hang_reported = 0;
        watch.beat = beat;
        watch.beat_ms = now;
        return;
    }
    if (!seconds || watch.hang_reported) return;
    /* A second after the game's own watchdog, which names mod code. */
    if (now - watch.beat_ms >= 1000u * (shared->running ? seconds + 1 : (seconds > STARTUP_SECONDS ? seconds : STARTUP_SECONDS))) {
        watch.hang_reported = 1;
        watch.recovered = 0;
        watch.hang_at_ms = now;
        report_hang(now - watch.beat_ms);
    }
}

#ifndef _WIN32
static void put_core_dump(pid_t pid)
{
    char command[160], line[512];
    int attempt, found = 0;
    if (access("/usr/bin/coredumpctl", X_OK) && access("/bin/coredumpctl", X_OK)) {
        put("\ncore dump: systemd-coredump is not installed; none was kept\n");
        return;
    }
    /* systemd-coredump takes a moment to file it. */
    for (attempt = 0; attempt < 20 && !found; attempt++) {
        FILE *pipe;
        struct timespec pause = {0, 500000000};
        int lines = 0;
        snprintf(command, sizeof(command), "timeout 10 coredumpctl --no-pager info %d 2>/dev/null", (int)pid);
        if (attempt) nanosleep(&pause, NULL);
        pipe = popen(command, "r");
        if (!pipe) break;
        while (fgets(line, sizeof(line), pipe)) {
            if (!found) {
                if (!strstr(line, "PID:")) continue;
                found = 1;
                put("\ncore dump (coredumpctl info %d; coredumpctl debug %d opens it in gdb):\n", (int)pid, (int)pid);
            }
            /* Nothing that names the machine or the account. */
            if (strstr(line, "Hostname:") || strstr(line, "Machine ID:") || strstr(line, "Boot ID:") ||
                strstr(line, " UID:") || strstr(line, " GID:") || strstr(line, "Control Group:") ||
                strstr(line, "Unit:") || strstr(line, "Slice:")) {
                continue;
            }
            if (lines++ < 400) put("%s", line);
        }
        pclose(pipe);
    }
    if (!found) put("\ncore dump: systemd-coredump has none for pid %d\n", (int)pid);
}
#endif

static const char *exception_name(unsigned long code)
{
    switch (code) {
    case 0xC0000005u: return "access violation";
    case 0xC000001Du: return "illegal instruction";
    case 0xC00000FDu: return "stack overflow";
    case 0xC0000094u: return "integer divide by zero";
    case 0xC0000409u: return "fail fast (stack buffer overrun or a fatal runtime check)";
    case 0xC0000417u: return "invalid parameter to the C runtime";
    case 0xC0000374u: return "heap corruption";
    case 0xC0000096u: return "privileged instruction";
    case 0xC0000006u: return "in-page error (reading the executable or a file failed)";
    case 0xC0000017u: return "out of memory";
    case 0x80000003u: return "breakpoint";
    case 0xCFFFFFFFu: return "ended by Windows as not responding";
    default: return "";
    }
}

/* How the game ended, in words; "" for an ending that needs no report. */
static void describe_end(int status, char *text, size_t size, int *crashed)
{
    *crashed = 0;
    text[0] = '\0';
#ifdef _WIN32
    {
        unsigned long code = (unsigned long)(unsigned)status;
        if (code == 0 || code == 0xC000013Au || code == 0x40010004u) {
            if (watch.hang_reported && !watch.recovered) snprintf(text, size, "the game was closed while not responding");
            return;
        }
        if ((code & 0xC0000000u) == 0xC0000000u || (code & 0xC0000000u) == 0x80000000u) {
            *crashed = code != 0xCFFFFFFFu;
            snprintf(text, size, "the game crashed: exception 0x%08lx %s", code, exception_name(code));
            if (!*crashed) snprintf(text, size, "the game was closed by Windows while not responding (0x%08lx)", code);
        } else if (code == 3) {
            *crashed = 1;
            snprintf(text, size, "the game stopped: abort() (exit code 3)");
        } else if (watch.hang_reported && !watch.recovered) {
            snprintf(text, size, "the game was closed while not responding (exit code %lu)", code);
        } else {
            /* Also what Wine gives a process Windows could not deliver an
             * exception to (no stack left): it just ends. */
            *crashed = !shared->error_shown;
            snprintf(text, size, "the game stopped with exit code %lu%s", code,
                     code == 70 || code == 71 ? " (an internal error it could not go on from)" : "");
        }
    }
#else
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (!code) {
            if (watch.hang_reported && !watch.recovered) snprintf(text, size, "the game was closed while not responding");
            return;
        }
        *crashed = !shared->error_shown;
        snprintf(text, size, "the game stopped with exit code %d%s", code,
                 code == 70 || code == 71 ? " (an internal error it could not go on from)" : "");
    } else if (WIFSIGNALED(status)) {
        int number = WTERMSIG(status);
        const char *name = strsignal(number);
        if (number == SIGINT || number == SIGTERM || number == SIGHUP) {
            if (watch.hang_reported && !watch.recovered)
                snprintf(text, size, "the game was closed while not responding (signal %d, %s)", number, name);
            return;
        }
        if (number == SIGKILL) {
            snprintf(text, size, "the game was killed (SIGKILL): by the out-of-memory killer, or force-closed%s",
                     watch.hang_reported && !watch.recovered ? " while not responding" : "");
        } else {
            *crashed = 1;
            snprintf(text, size, "the game crashed: signal %d (%s)%s", number, name,
                     WCOREDUMP(status) ? ", core dumped" : "");
        }
    }
#endif
}

/* The game has ended: the report, if it ended badly. */
static void finish(int status)
{
    char what[256], path[640], message[1200];
    int crashed;
    /* Frames may have come again after a freeze report in the moment
     * before the game ended, between two looks. */
    if (watch.hang_reported && !watch.recovered && __atomic_load_n(&shared->heartbeat, __ATOMIC_SEQ_CST) != watch.beat) {
        check();
    }
    describe_end(status, what, sizeof(what), &crashed);
    if (!what[0]) {
        note_session("the game ended normally");
        return;
    }
    note_session("%s", what);
    /* A failure the game already explained (no disc image, no window) is
     * not a crash. */
    if (shared->error_shown && !crashed) return;
    if (shared->reported && shared->report_path[0]) {
        memcpy(path, (const char *)shared->report_path, sizeof(path));
        path[sizeof(path) - 1] = '\0';
    } else if (!crashed && watch.hang_reported && !watch.recovered) {
        snprintf(path, sizeof(path), "%s", watch.hang_path);
    } else {
        snprintf(path, sizeof(path), "%s/crash-%lu.txt", Crash_ReportDir, game_pid);
    }
    out = fopen(path, "a");
    if (out) {
        put_heading(what);
        if (!shared->reported && crashed) put("the game's own crash handler did not run: this is all there is\n");
        if (watch.hang_reported && path[0] && strcmp(path, watch.hang_path)) {
            put("it had stopped responding %.1f s before it ended: %s\n",
                (double)(now_ms() - watch.hang_at_ms) / 1000.0, watch.hang_path);
        }
        put_facts();
        put_game_state();
        put_log();
        put_console();
#ifndef _WIN32
        if (WIFSIGNALED(status) && WCOREDUMP(status)) put_core_dump(game);
#endif
        fclose(out);
        out = NULL;
    }
    note_session("report: %s", path);
    fprintf(stderr, "memories-pc: %s\nmemories-pc: report written to %s\n", what, path);
    if (dialogs_allowed()) {
        snprintf(message, sizeof(message),
                 "%s.\n\nA report was saved to:\n%s\n\nPlease send it (and any .dmp file of the same number next "
                 "to it) with a description of what you were doing.", what, path);
        message[0] = (char)(message[0] >= 'a' && message[0] <= 'z' ? message[0] - 'a' + 'A' : message[0]);
        Platform_ShowError("Yu-Gi-Oh! Forbidden Memories", message);
    }
}

/* ---- Starting and waiting, per system ------------------------------- */

#ifdef _WIN32
static DWORD WINAPI read_console(void *unused)
{
    char buffer[4096];
    DWORD got;
    (void)unused;
    while (ReadFile(console_read, buffer, sizeof(buffer), &got, NULL) && got) relay(buffer, got);
    return 0;
}

static BOOL WINAPI on_console_event(DWORD event)
{
    /* Ctrl+C reaches the game too, which decides; the monitor stays to see
     * how it ended. */
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}

static HANDLE inheritable(DWORD which)
{
    HANDLE handle = GetStdHandle(which), copy = NULL;
    if (!handle || handle == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return handle;
    return copy;
}

static int start_game(HANDLE console_write)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    wchar_t path[MAX_PATH];
    HANDLE input = inheritable(STD_INPUT_HANDLE), output = inheritable(STD_OUTPUT_HANDLE);
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    BOOL started;
    if (!length || length >= MAX_PATH) return -1;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = output;
    startup.hStdError = console_write;
    started = CreateProcessW(path, GetCommandLineW(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &startup, &process);
    if (input && input != GetStdHandle(STD_INPUT_HANDLE)) CloseHandle(input);
    if (output && output != GetStdHandle(STD_OUTPUT_HANDLE)) CloseHandle(output);
    if (!started) return -1;
    if (job) AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    game_process = process.hProcess;
    game_pid = process.dwProcessId;
    return 0;
}

/* -1: no monitor, go on as the game. 0: the game ran and ended, *status. */
static int run_monitor(int *status)
{
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    HANDLE mapping, console_write, reader;
    char text[32];
    DWORD code = 1;
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(MonitorShared), NULL);
    if (!mapping) return -1;
    shared = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MonitorShared));
    if (!shared) {
        shared = &own_block;
        CloseHandle(mapping);
        return -1;
    }
    if (!CreatePipe(&console_read, &console_write, &attributes, 0)) {
        UnmapViewOfFile(shared);
        shared = &own_block;
        CloseHandle(mapping);
        return -1;
    }
    SetHandleInformation(console_read, HANDLE_FLAG_INHERIT, 0);
    shared->magic = MAGIC;
    shared->version = VERSION;
    snprintf(text, sizeof(text), "%lu", (unsigned long)(uintptr_t)mapping);
    SetEnvironmentVariableA("MEMORIES_MONITOR_HANDLE", text);
    /* The game ends with the monitor (a test's timeout, the task manager). */
    job = CreateJobObjectW(NULL, NULL);
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job && !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        job = NULL;
    }
    original_stderr = GetStdHandle(STD_ERROR_HANDLE);
    InitializeCriticalSection(&console_lock);
    Crash_ChooseReportDir();
    Symbols_Load();
    if (start_game(console_write)) {
        SetEnvironmentVariableA("MEMORIES_MONITOR_HANDLE", NULL);
        CloseHandle(console_write);
        CloseHandle(console_read);
        UnmapViewOfFile(shared);
        shared = &own_block;
        CloseHandle(mapping);
        return -1;
    }
    CloseHandle(console_write);
    SetConsoleCtrlHandler(on_console_event, TRUE);
    open_session();
    reader = CreateThread(NULL, 0, read_console, NULL, 0, NULL);
    watch.started_ms = watch.beat_ms = now_ms();
    for (;;) {
        if (WaitForSingleObject(game_process, 100) == WAIT_OBJECT_0) {
            GetExitCodeProcess(game_process, &code);
            if (shared->restart && code == 0) {
                /* Win32_Restart: the same command line, without the state
                 * load it may have started with. */
                HANDLE again_write;
                CloseHandle(game_process);
                note_session("restart");
                SetEnvironmentVariableA("MEMORIES_LOAD_STATE", NULL);
                shared->restart = 0;
                /* The first game's pipe end is gone with it: a new pipe. */
                if (reader) {
                    WaitForSingleObject(reader, 1000);
                    CloseHandle(reader);
                    reader = NULL;
                }
                CloseHandle(console_read);
                if (!CreatePipe(&console_read, &again_write, &attributes, 0)) break;
                SetHandleInformation(console_read, HANDLE_FLAG_INHERIT, 0);
                if (start_game(again_write)) {
                    CloseHandle(again_write);
                    break;
                }
                CloseHandle(again_write);
                reader = CreateThread(NULL, 0, read_console, NULL, 0, NULL);
                watch.started_ms = watch.beat_ms = now_ms();
                continue;
            }
            break;
        }
        check();
    }
    if (reader) WaitForSingleObject(reader, 1000);
    finish((int)code);
    if (session) fclose(session);
    *status = (int)code;
    return 0;
}

static int attach(void)
{
    const char *text = getenv("MEMORIES_MONITOR_HANDLE");
    HANDLE mapping;
    MonitorShared *block;
    if (!text || !*text) return 0;
    mapping = (HANDLE)(uintptr_t)strtoul(text, NULL, 10);
    block = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MonitorShared));
    if (!block || block->magic != MAGIC || block->version != VERSION) {
        if (block) UnmapViewOfFile(block);
        return 1; /* monitored, but not usable: run alone */
    }
    shared = block;
    active = 1;
    begin_generation();
    return 1;
}
#else
static void forward(int number)
{
    if (game > 0) kill(game, number);
}

/* -1: no monitor, or this is the game: go on as the game. 0: the game ran
 * and ended, *exit_status. */
static int run_monitor(int *exit_status)
{
    int fd = memfd_create("memories-monitor", 0), pipe_fds[2], status = 0;
    MonitorShared *block;
    struct sigaction action;
    char text[16];
    pid_t monitor = getpid();
    if (fd < 0) return -1;
    if (ftruncate(fd, sizeof(MonitorShared)) ||
        (block = mmap(NULL, sizeof(MonitorShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        close(fd);
        return -1;
    }
    if (pipe2(pipe_fds, O_CLOEXEC)) {
        munmap(block, sizeof(MonitorShared));
        close(fd);
        return -1;
    }
    shared = block;
    shared->magic = MAGIC;
    shared->version = VERSION;
    game = fork();
    if (game < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (game == 0) {
        dup2(pipe_fds[1], 2); /* dup2's copy is not close-on-exec */
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        snprintf(text, sizeof(text), "%d", fd);
        setenv("MEMORIES_MONITOR_FD", text, 1);
        /* The game ends with the monitor (a test's timeout, a kill). */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != monitor) raise(SIGKILL);
        /* With the monitor gone its console pipe breaks: a write then fails
         * rather than killing the game. */
        signal(SIGPIPE, SIG_IGN);
        active = 1;
        begin_generation();
        return -1;
    }
    close(pipe_fds[1]);
    console_fd = pipe_fds[0];
    game_pid = (unsigned long)game;
    /* Ctrl+C and Ctrl+\ reach the game too, which decides; the monitor
     * stays to see how it ended. What is sent to the monitor alone goes on. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    memset(&action, 0, sizeof(action));
    action.sa_handler = forward;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGUSR2, &action, NULL);
    Crash_ChooseReportDir();
    Symbols_Load();
    open_session();
    watch.started_ms = watch.beat_ms = now_ms();
    for (;;) {
        struct pollfd poll_fd = {console_fd, POLLIN, 0};
        pid_t got;
        if (console_fd >= 0 && poll(&poll_fd, 1, 100) > 0) {
            char buffer[4096];
            ssize_t size = read(console_fd, buffer, sizeof(buffer));
            if (size > 0) relay(buffer, (size_t)size);
            else if (size == 0) {
                close(console_fd);
                console_fd = -1;
            }
        } else if (console_fd < 0) {
            struct timespec pause = {0, 100000000};
            nanosleep(&pause, NULL);
        }
        if (have_stashed) {
            status = stashed_status;
            break;
        }
        got = waitpid(game, &status, WNOHANG);
        if (got == game && (WIFEXITED(status) || WIFSIGNALED(status))) break;
        if (got < 0 && errno == ECHILD) break;
        check();
    }
    /* What the game printed last, unless something it started keeps the
     * pipe open. */
    if (console_fd >= 0) {
        uint64_t until = now_ms() + 1000;
        while (now_ms() < until) {
            struct pollfd poll_fd = {console_fd, POLLIN, 0};
            char buffer[4096];
            ssize_t size;
            if (poll(&poll_fd, 1, 100) <= 0) continue;
            size = read(console_fd, buffer, sizeof(buffer));
            if (size <= 0) break;
            relay(buffer, (size_t)size);
        }
        close(console_fd);
    }
    finish(status);
    if (session) fclose(session);
    if (WIFSIGNALED(status)) {
        /* End the way the game did, for whoever started the monitor; a
         * second core file would only be the monitor's. */
        struct rlimit none = {0, 0};
        setrlimit(RLIMIT_CORE, &none);
        signal(WTERMSIG(status), SIG_DFL);
        raise(WTERMSIG(status));
        *exit_status = 128 + WTERMSIG(status);
        return 0;
    }
    *exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return 0;
}

static int attach(void)
{
    const char *text = getenv("MEMORIES_MONITOR_FD");
    MonitorShared *block;
    if (!text || !*text) return 0;
    block = mmap(NULL, sizeof(MonitorShared), PROT_READ | PROT_WRITE, MAP_SHARED, atoi(text), 0);
    if (block == MAP_FAILED || block->magic != MAGIC || block->version != VERSION) {
        if (block != MAP_FAILED) munmap(block, sizeof(MonitorShared));
        return 1; /* monitored, but not usable: run alone */
    }
    shared = block;
    active = 1;
    signal(SIGPIPE, SIG_IGN);
    begin_generation();
    return 1;
}
#endif

int Monitor_Main(int argc, char **argv, int *status)
{
    (void)argc;
    (void)argv;
    if (attach() || !wanted()) return 0;
    return !run_monitor(status);
}
