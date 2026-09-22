#define _GNU_SOURCE
#include "image.h"
#include "mips.h"
#include "pc/debug/crash.h"
#include "pc/debug/log.h"
#include "pc/debug/profile.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif

_Static_assert(sizeof(void *) == 4, "the guest image model requires an ILP32 build");

/* The first 64 KiB. On the console that is kernel RAM, and retail code reaches
 * it through null pointers: CardList_CreateSlotTextBox clears a flag in
 * box->field_28 one call before that object exists, a read-modify-write of
 * address 8 that nothing notices. Hosts do not let a process map page zero,
 * so such an access faults; the handler then points the instruction's base
 * register at `low_memory` (the same pages guest RAM has at 0x80000000),
 * single-steps it, and puts the register back unless the instruction itself
 * replaced it. Each site is reported once. */
#ifndef _WIN32
static unsigned char *low_memory;
#endif
static struct {
    int active, reg; /* reg: the ModRM register number, 0 (EAX) to 7 (EDI) */
    uint32_t original, patched;
} low_fixup;

/* Every instruction that has faulted into the low-address repair, appended
 * on its first fault and read by the Windows clock thread: a thread it
 * suspends at one of these may be on its way into the fault's dispatch,
 * whose frames a redirect would land on (win32.c). */
static uint32_t fault_sites[256];
static volatile unsigned fault_site_count;

static int is_fault_site(uintptr_t eip)
{
    unsigned i, count = fault_site_count;
    for (i = 0; i < count; i++) {
        if (fault_sites[i] == (uint32_t)eip) return 1;
    }
    return 0;
}

static int low_access_register(const unsigned char *code, uint32_t esi);
#ifdef _WIN32
static DWORD *context_register(CONTEXT *context, int number);

/* For the clock: would the instruction the thread is at fault into the
 * repair? Either it has before, or it addresses memory through a register
 * holding a low address, which always faults here. The thread is suspended,
 * so its code and registers can be read. */
static int fault_imminent(void *context)
{
    CONTEXT *registers = context;
    int reg;
    if (is_fault_site(registers->Eip)) return 1;
    reg = low_access_register((const unsigned char *)(uintptr_t)registers->Eip, registers->Esi);
    return reg >= 0 && *context_register(registers, reg) < MEMORIES_GUEST_RAM_SIZE;
}
#endif

static void report_low_access(uint32_t eip, uint32_t address)
{
    char text[128];
    int length;
    if (is_fault_site(eip)) return;
    if (fault_site_count < sizeof(fault_sites) / sizeof(fault_sites[0])) {
        fault_sites[fault_site_count] = eip;
        fault_site_count++;
    }
    length = snprintf(text, sizeof(text), "memories-pc: null-pointer access to 0x%04x at eip 0x%08x goes to kernel RAM, as on the console\n",
                      (unsigned)address, (unsigned)eip);
    (void)!write(2, text, (size_t)length);
}

/* Which register (ModRM number) does the faulting instruction address memory
 * through? -1 for none. */
#define REGISTER_ESI 6
#define REGISTER_EDI 7
static int low_access_register(const unsigned char *code, uint32_t esi)
{
    unsigned modrm, base;
    while (*code == 0x66 || *code == 0xf2 || *code == 0xf3 || *code == 0x2e || *code == 0x36 || *code == 0x3e ||
           *code == 0x26) {
        code++;
    }
    if ((*code >= 0xa4 && *code <= 0xa7) || *code == 0xaa || *code == 0xab) { /* string moves and stores */
        if (*code != 0xaa && *code != 0xab && esi < 0x10000u) {
            return REGISTER_ESI;
        }
        return REGISTER_EDI;
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
    return (int)base;
}

/* Guest RAM is mapped without execute permission, so a call through a MIPS
 * address faults with EIP equal to that address. Tables in the retail data
 * image hold such addresses; the build generates Memories_FunctionMap (guest
 * address -> native function, sorted) and the handler resumes in the native
 * function. The caller's return address and cdecl arguments are already on
 * the stack, so the redirect is transparent. Anything else is fatal.
 * Returns where to resume, or NULL. */
static void *guest_call_target(uint32_t address)
{
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
            return (void *)(uintptr_t)entry->host;
        }
    }
    if (Memories_MipsInOverlay(address)) {
        /* A callback into a loaded overlay: run it interpreted. */
        Memories_MipsThunkTarget = address;
        return (void *)(uintptr_t)Memories_MipsThunk;
    }
    return NULL;
}

static void report_guest_fault(uint32_t address, uint32_t eip)
{
    char text[128];
    int length;
    if (eip == address) {
        length = snprintf(text, sizeof(text), "memories-pc: call into guest code at 0x%08x, which has no native function\n",
                          (unsigned)address);
    } else {
        length = snprintf(text, sizeof(text), "memories-pc: bad memory access at 0x%08x (eip 0x%08x)\n",
                          (unsigned)address, (unsigned)eip);
    }
    (void)!write(2, text, (size_t)length);
}

#ifdef _WIN32
static DWORD *context_register(CONTEXT *context, int number)
{
    switch (number) {
    case 0: return &context->Eax;
    case 1: return &context->Ecx;
    case 2: return &context->Edx;
    case 3: return &context->Ebx;
    case 4: return &context->Esp;
    case 5: return &context->Ebp;
    case 6: return &context->Esi;
    default: return &context->Edi;
    }
}

/* The common low access, a plain 32-bit MOV to or from [base + disp], is
 * done here through guest RAM instead of the rebase-and-single-step path:
 * 32-bit processes on 64-bit Windows can mishandle the trap when the
 * instruction's destination is its own base register. Returns 1 if done. */
static int emulate_low_mov(CONTEXT *context, uint32_t address)
{
    const unsigned char *code = (const unsigned char *)(uintptr_t)context->Eip;
    unsigned modrm, mod, reg, rm;
    uint32_t *guest;
    if ((code[0] != 0x8b && code[0] != 0x89) || address > MEMORIES_GUEST_RAM_SIZE - 4) {
        return 0;
    }
    modrm = code[1];
    mod = modrm >> 6;
    reg = (modrm >> 3) & 7;
    rm = modrm & 7;
    if (mod == 3 || rm == 4 || (mod == 0 && rm == 5)) {
        return 0; /* register operand, SIB byte or absolute address */
    }
    guest = (uint32_t *)(uintptr_t)(MEMORIES_GUEST_RAM + address);
    if (code[0] == 0x8b) {
        *context_register(context, (int)reg) = *guest;
    } else {
        *guest = *context_register(context, (int)reg);
    }
    context->Eip += 2u + (mod == 1 ? 1u : mod == 2 ? 4u : 0u);
    return 1;
}

/* First in line for every exception in the process: take the guest's own
 * faults, leave everything else to the next handler (win32.c reports what
 * the executable raised). */
static LONG guest_exception(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;
    uint32_t address;
    void *target;
    Win32_UndoInterruptedFault(context); /* then handled as the faulting instruction's own */
    /* A 32-bit process on 64-bit Windows may see the trap as WoW64's own
     * STATUS_WX86_SINGLE_STEP. */
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP || record->ExceptionCode == 0x4000001eu) {
        DWORD *reg;
        if (!low_fixup.active) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        low_fixup.active = 0;
        reg = context_register(context, low_fixup.reg);
        if (*reg == low_fixup.patched) {
            *reg = low_fixup.original;
        }
        context->EFlags &= ~0x100; /* trap flag */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    address = (uint32_t)record->ExceptionInformation[1];
    /* The first 64 KiB and the parts of the physical mirror Windows holds
     * (see Memories_GuestMap) are reached the same way: through guest RAM
     * at 0x80000000, which the same offsets address. */
    if (address < MEMORIES_GUEST_RAM_SIZE && context->Eip != address && !low_fixup.active) {
        int reg = low_access_register((const unsigned char *)(uintptr_t)context->Eip, context->Esi);
        if (reg >= 0 && *context_register(context, reg) < MEMORIES_GUEST_RAM_SIZE) {
            report_low_access(context->Eip, address);
            if (emulate_low_mov(context, address)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            low_fixup.active = 1;
            low_fixup.reg = reg;
            low_fixup.original = *context_register(context, reg);
            low_fixup.patched = low_fixup.original + MEMORIES_GUEST_RAM;
            *context_register(context, reg) = low_fixup.patched;
            context->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (context->Eip == address && (target = guest_call_target(address)) != NULL) {
        context->Eip = (DWORD)(uintptr_t)target;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (context->Eip == address || address < 0x10000u ||
        (address >= MEMORIES_GUEST_RAM && address < MEMORIES_GUEST_RAM + 0x00800000u)) {
        report_guest_fault(address, context->Eip);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The clock must not redirect the main thread into a tick while a handler
 * runs: the tick would nest an exception on the dispatch stack, and the
 * fixup below keeps state between the fault and its single step. */
static LONG CALLBACK on_guest_exception(EXCEPTION_POINTERS *pointers)
{
    LONG disposition;
    Win32_EnterHandler();
    disposition = guest_exception(pointers);
    Win32_LeaveHandler();
    return disposition;
}

/* The Linux layout as far as Windows allows: one pagefile-backed section
 * holds guest RAM and is viewed at 0x80000000 and 0xA0000000. The physical
 * mirror (0x10000..0x200000) competes with what Windows puts there before
 * the program starts (process parameters, locale tables, the WoW64 stack),
 * so it is mapped in 64 KiB pieces where the address space is free; an
 * access to a piece Windows holds faults and goes through guest RAM
 * instead (on_guest_exception). Such accesses into pages Windows has
 * mapped readable would not fault; the sites that fault are reported. */
static int view_at(HANDLE section, uint32_t address, size_t length, DWORD offset)
{
    void *wanted = (void *)(uintptr_t)address;
    if (MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, offset, length, wanted) != wanted) {
        fprintf(stderr, "cannot map guest memory at 0x%08x (error %lu)\n", (unsigned)address,
                GetLastError());
        return -1;
    }
    return 0;
}

int Memories_GuestMap(void)
{
    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                        MEMORIES_GUEST_RAM_SIZE, NULL);
    int result;
    AddVectoredExceptionHandler(1, on_guest_exception);
    Win32_SetFaultSites(fault_imminent);
    if (section == NULL) {
        fprintf(stderr, "guest RAM: CreateFileMapping failed (error %lu)\n", GetLastError());
        return -1;
    }
    /* The mirror first: tested on Windows 11, a view below 0x200000 fails
     * with ERROR_INVALID_ADDRESS once the high views exist. */
    {
        uint32_t piece, held = 0;
        for (piece = 0x10000u; piece < MEMORIES_GUEST_RAM_SIZE; piece += 0x10000u) {
            if (MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, piece, 0x10000u, (void *)(uintptr_t)piece) == NULL) {
                held += 0x10000u;
            }
        }
        if (held) {
            fprintf(stderr, "memories-pc: %u KiB of the physical RAM mirror are taken by Windows; accesses there fault into guest RAM\n",
                    (unsigned)(held / 1024));
        }
    }
    result = view_at(section, MEMORIES_GUEST_RAM, MEMORIES_GUEST_RAM_SIZE, 0) ||
             view_at(section, 0xa0000000u, MEMORIES_GUEST_RAM_SIZE, 0);
    if (!result && VirtualAlloc((void *)0x1f800000u, 0x1000, MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE) != (void *)0x1f800000u) {
        fprintf(stderr, "cannot map the scratchpad at 0x1f800000 (error %lu)\n", GetLastError());
        result = -1;
    }
    /* The views keep the section alive. */
    CloseHandle(section);
    return result ? -1 : 0;
}
#else
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

/* ModRM/SIB register numbers to gregs[]. */
static const int register_slot[8] = {REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI};

static void on_step(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    greg_t *reg;
    (void)number; (void)info;
    if (low_fixup.active) {
        low_fixup.active = 0;
        reg = &user->uc_mcontext.gregs[register_slot[low_fixup.reg]];
        if ((uint32_t)*reg == low_fixup.patched) {
            *reg = (greg_t)low_fixup.original;
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

static void on_fault(int number, siginfo_t *info, void *context)
{
    ucontext_t *user = context;
    uint32_t address = (uint32_t)(uintptr_t)info->si_addr;
    uint32_t eip = (uint32_t)user->uc_mcontext.gregs[REG_EIP];
    void *target;
    if (address < 0x10000u && eip != address && low_memory && !low_fixup.active) {
        int reg = low_access_register((const unsigned char *)(uintptr_t)eip, (uint32_t)user->uc_mcontext.gregs[REG_ESI]);
        if (reg >= 0 && (uint32_t)user->uc_mcontext.gregs[register_slot[reg]] < 0x10000u) {
            report_low_access(eip, address);
            low_fixup.active = 1;
            low_fixup.reg = reg;
            low_fixup.original = (uint32_t)user->uc_mcontext.gregs[register_slot[reg]];
            low_fixup.patched = low_fixup.original + (uint32_t)(uintptr_t)low_memory;
            user->uc_mcontext.gregs[register_slot[reg]] = (greg_t)low_fixup.patched;
            user->uc_mcontext.gregs[REG_EFL] |= 0x100;
            return;
        }
    }
    if (eip == address && (target = guest_call_target(address)) != NULL) {
        user->uc_mcontext.gregs[REG_EIP] = (greg_t)(uintptr_t)target;
        return;
    }
    report_guest_fault(address, eip);
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
#endif /* _WIN32 */

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
#ifdef _WIN32
    if (break_name && !strcmp(break_name, name)) DebugBreak();
#else
    if (break_name && !strcmp(break_name, name)) raise(SIGTRAP);
#endif
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
