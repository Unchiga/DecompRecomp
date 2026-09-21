#define _GNU_SOURCE
#include "image.h"
#include "mips.h"
#include "pc/debug/crash.h"
#include "pc/debug/log.h"
#include "pc/debug/profile.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

_Static_assert(sizeof(void *) == 4, "the guest image model requires an ILP32 build");

static int map_at(uint32_t address, size_t length, int fd, off_t offset)
{
    void *wanted = (void *)(uintptr_t)address;
    int flags = MAP_FIXED_NOREPLACE | (fd < 0 ? MAP_PRIVATE | MAP_ANONYMOUS : MAP_SHARED);
    if (mmap(wanted, length, PROT_READ | PROT_WRITE, flags, fd, offset) != wanted) {
        fprintf(stderr, "cannot map guest memory at 0x%08x\n", (unsigned)address);
        return -1;
    }
    return 0;
}

/* The first 64 KiB. On the console that is kernel RAM, and retail code reaches
 * it through null pointers: CardList_CreateSlotTextBox clears a flag in
 * box->field_28 one call before that object exists, a read-modify-write of
 * address 8 that nothing notices. Hosts do not let a process map page zero,
 * so such an access faults; the handler then points the instruction's base
 * register at `low_memory` (the same pages guest RAM has at 0x80000000),
 * single-steps it, and puts the register back unless the instruction itself
 * replaced it. Each site is reported once. */
static unsigned char *low_memory;
static struct {
    int active, reg;
    greg_t original, patched;
} low_fixup;

/* ModRM/SIB register numbers to gregs[]. */
static const int register_slot[8] = {REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI};

static void report_low_access(uint32_t eip, uint32_t address)
{
    static uint32_t seen[32];
    static unsigned count;
    char text[128];
    unsigned i;
    int length;
    for (i = 0; i < count; i++) {
        if (seen[i] == eip) {
            return;
        }
    }
    if (count < sizeof(seen) / sizeof(seen[0])) {
        seen[count++] = eip;
    }
    length = snprintf(text, sizeof(text), "memories-pc: null-pointer access to 0x%04x at eip 0x%08x goes to kernel RAM, as on the console\n",
                      (unsigned)address, (unsigned)eip);
    (void)!write(2, text, (size_t)length);
}

/* Which register does the faulting instruction address memory through? */
static int low_access_register(const ucontext_t *user)
{
    const unsigned char *code = (const unsigned char *)(uintptr_t)user->uc_mcontext.gregs[REG_EIP];
    unsigned modrm, base;
    while (*code == 0x66 || *code == 0xf2 || *code == 0xf3 || *code == 0x2e || *code == 0x36 || *code == 0x3e ||
           *code == 0x26) {
        code++;
    }
    if ((*code >= 0xa4 && *code <= 0xa7) || *code == 0xaa || *code == 0xab) { /* string moves and stores */
        if (*code != 0xaa && *code != 0xab && (uint32_t)user->uc_mcontext.gregs[REG_ESI] < 0x10000u) {
            return REG_ESI;
        }
        return REG_EDI;
    }
    code += *code == 0x0f ? 2 : 1;
    modrm = *code++;
    if (modrm >> 6 == 3 || ((modrm >> 6) == 0 && (modrm & 7) == 5)) {
        return -1; /* register operand, or an absolute address */
    }
    base = modrm & 7;
    if (base == 4) {
        unsigned sib = *code;
        base = sib & 7;
        if (base == 5 && modrm >> 6 == 0) {
            return -1;
        }
    }
    return register_slot[base];
}

static void on_step(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    (void)number; (void)info;
    if (low_fixup.active) {
        low_fixup.active = 0;
        if (user->uc_mcontext.gregs[low_fixup.reg] == low_fixup.patched) {
            user->uc_mcontext.gregs[low_fixup.reg] = low_fixup.original;
        }
    } else {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigaction(SIGTRAP, &action, NULL);
        raise(SIGTRAP);
        return;
    }
    user->uc_mcontext.gregs[REG_EFL] &= ~0x100; /* trap flag */
}

/* Guest RAM is mapped without execute permission, so a call through a MIPS
 * address faults with EIP equal to that address. Tables in the retail data
 * image hold such addresses; the build generates Memories_FunctionMap (guest
 * address -> native function, sorted) and the handler resumes in the native
 * function. The caller's return address and cdecl arguments are already on
 * the stack, so the redirect is transparent. Anything else is fatal. */
static void on_fault(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    uint32_t address = (uint32_t)(uintptr_t)info->si_addr;
    char text[128];
    int length;
    (void)number;
    if (address < 0x10000u && (uint32_t)user->uc_mcontext.gregs[REG_EIP] != address && low_memory &&
        !low_fixup.active) {
        int reg = low_access_register(user);
        if (reg >= 0 && (uint32_t)user->uc_mcontext.gregs[reg] < 0x10000u) {
            report_low_access((uint32_t)user->uc_mcontext.gregs[REG_EIP], address);
            low_fixup.active = 1;
            low_fixup.reg = reg;
            low_fixup.original = user->uc_mcontext.gregs[reg];
            low_fixup.patched = low_fixup.original + (greg_t)(uintptr_t)low_memory;
            user->uc_mcontext.gregs[reg] = low_fixup.patched;
            user->uc_mcontext.gregs[REG_EFL] |= 0x100;
            return;
        }
    }
    if ((uint32_t)user->uc_mcontext.gregs[REG_EIP] == address) {
        size_t low = 0, high = Memories_FunctionMapCount;
        while (low < high) {
            size_t middle = (low + high) / 2;
            if (Memories_FunctionMap[middle].guest < address) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        for (; low < Memories_FunctionMapCount && Memories_FunctionMap[low].guest == address; low++) {
            const MemoriesGuestFunction *entry = &Memories_FunctionMap[low];
            if (Memories_ModuleIsResident(entry->bank, entry->identifier)) {
                user->uc_mcontext.gregs[REG_EIP] = (greg_t)(uintptr_t)entry->host;
                return;
            }
        }
        if (Memories_MipsInOverlay(address)) {
            /* A callback into a loaded overlay: run it interpreted. */
            Memories_MipsThunkTarget = address;
            user->uc_mcontext.gregs[REG_EIP] = (greg_t)(uintptr_t)Memories_MipsThunk;
            return;
        }
        length = snprintf(text, sizeof(text), "memories-pc: call into guest code at 0x%08x, which has no native function\n",
                          (unsigned)address);
    } else {
        length = snprintf(text, sizeof(text), "memories-pc: bad memory access at 0x%08x (eip 0x%08x)\n",
                          (unsigned)address, (unsigned)user->uc_mcontext.gregs[REG_EIP]);
    }
    (void)!write(2, text, (size_t)length);
    Crash_HandleSignal(number, info, context);
}

int Memories_GuestMap(void)
{
    struct sigaction action;
    int fd, result;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, NULL);
    action.sa_sigaction = on_step;
    sigaction(SIGTRAP, &action, NULL);
    fd = memfd_create("memories-ram", 0);
    if (fd < 0 || ftruncate(fd, MEMORIES_GUEST_RAM_SIZE) != 0) {
        perror("guest RAM");
        return -1;
    }
    result = map_at(MEMORIES_GUEST_RAM, MEMORIES_GUEST_RAM_SIZE, fd, 0) ||
             map_at(0xa0000000u, MEMORIES_GUEST_RAM_SIZE, fd, 0) ||
             map_at(0x00010000u, MEMORIES_GUEST_RAM_SIZE - 0x10000u, fd, 0x10000) ||
             map_at(0x1f800000u, 0x1000, -1, 0);
    low_memory = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (low_memory == MAP_FAILED) {
        low_memory = NULL;
    }
    close(fd);
    return result ? -1 : 0;
}

static uint32_t le32(const unsigned char *bytes)
{
    return bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

int Memories_GuestLoadExe(const char *path)
{
    unsigned char header[0x800];
    uint32_t address, size;
    FILE *file = fopen(path, "rb");
    if (!file || fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "%s: not a readable PS-X executable\n", path);
        if (file) {
            fclose(file);
        }
        return -1;
    }
    address = le32(header + 0x18);
    size = le32(header + 0x1c);
    if (address < MEMORIES_GUEST_RAM + 0x10000u || size > MEMORIES_GUEST_RAM_SIZE ||
        address - MEMORIES_GUEST_RAM > MEMORIES_GUEST_RAM_SIZE - size ||
        fread((void *)(uintptr_t)address, 1, size, file) != size) {
        fprintf(stderr, "%s: image does not fit guest RAM or is truncated\n", path);
        fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

typedef struct StubCount { const char *name; unsigned count; } StubCount;
static StubCount stub_calls[512];
static unsigned stub_call_count;

static int compare_stub_counts(const void *left, const void *right)
{
    const StubCount *a = left, *b = right;
    return a->count < b->count ? 1 : a->count > b->count ? -1 : strcmp(a->name, b->name);
}

static void print_stub_summary(void)
{
    unsigned at;
    qsort(stub_calls, stub_call_count, sizeof(stub_calls[0]), compare_stub_counts);
    for (at = 0; at < stub_call_count; at++) {
        LOG(LOG_STUB, "%s: %u calls", stub_calls[at].name, stub_calls[at].count);
    }
    Log_Drain();
}

void Memories_Unimplemented(const char *name)
{
    static int registered;
    const char *break_name = getenv("MEMORIES_STUB_BREAK");
    unsigned i;
    if (break_name && !strcmp(break_name, name)) raise(SIGTRAP);
    /* Survey aid only: results after the first line are not meaningful,
     * because the missing routine returned garbage. */
    if (getenv("MEMORIES_STUB_TRACE")) {
        for (i = 0; i < stub_call_count && strcmp(stub_calls[i].name, name); i++) {}
        if (i == stub_call_count && stub_call_count < sizeof(stub_calls) / sizeof(stub_calls[0])) {
            stub_calls[stub_call_count].name = name;
            stub_calls[stub_call_count++].count = 0;
        }
        if (i < stub_call_count) stub_calls[i].count++;
        if (!registered) {
            registered = 1;
            Log_Enable(LOG_STUB, 1);
            atexit(print_stub_summary);
        }
        LOG(LOG_STUB, "%s", name);
        return;
    }
    fflush(stdout);
    Crash_ReportSoft("unimplemented routine", name);
    Profile_Flush();
    _exit(70); /* not exit(): atexit handlers could re-enter game code */
}
