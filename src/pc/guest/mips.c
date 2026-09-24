/* A small MIPS I interpreter for the overlays that have no C source: the WA
 * duel-effect bank at 0x80146000 and the per-monster MODEL control modules
 * swapped into 0x8013A000/0x8013B000 (slot A) and 0x8017A000/0x8017B000
 * (slot B). The retail bytes are loaded by the game's own loader at their
 * original addresses; the interpreter runs them there and bridges every
 * call that leaves the overlay to the native function of that address.
 *
 * Scope: integer MIPS I plus COP2 through the software GTE. `break` is
 * GCC's divide-by-zero trap, which only follows a zero divisor the `div`
 * case already skips, so it is a no-op. A scan of the retail modules
 * (notes/pc-build.md, "MIPS-only effects") found nothing else.
 *
 * Calls nest: overlay -> native -> overlay callback (through the guest-call
 * fault handler, Memories_MipsThunk) -> native. Every level shares one
 * guest-visible stack mapped at a negative address, because model code
 * tells a pointer from a small number by its sign; each level starts below
 * the innermost live frame. Failures (an instruction outside the subset, a
 * call to an address with no native function) unwind to the bridge that
 * started the level, which can fall back instead of stopping the game. */
#define _GNU_SOURCE
#include "mips.h"
#include "image.h"
#include "pc/compat/gte.h"
#include "pc/rng.h"
#include "pc/debug/log.h"
#include "pc/debug/crash.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/compat/mman.h"

#define STACK_BASE 0x9FF00000u
#define STACK_SIZE 0x40000u
#define LIMIT 20000000u

typedef struct { uint32_t r[32], hi, lo, pc; unsigned steps; jmp_buf *escape; } State;

extern int rsin(int angle);
extern int rcos(int angle);
extern int Psx_csin(int angle);
extern int Psx_ccos(int angle);

static uint32_t stack_top;   /* 0 until the stack is mapped */
static uint32_t current_sp;  /* the innermost interpreted frame, or 0 */
uint32_t Memories_MipsThunkTarget;

static uint32_t l32(uint32_t a) { return *(uint32_t *)(uintptr_t)a; }
static uint16_t l16(uint32_t a) { return *(uint16_t *)(uintptr_t)a; }
static uint8_t l8(uint32_t a) { return *(uint8_t *)(uintptr_t)a; }
static void s32(uint32_t a, uint32_t v) { *(uint32_t *)(uintptr_t)a = v; }
static void s16(uint32_t a, uint16_t v) { *(uint16_t *)(uintptr_t)a = v; }
static void s8(uint32_t a, uint8_t v) { *(uint8_t *)(uintptr_t)a = v; }

/* The bank at 0x80180000 holds the natively linked main-menu overlay, but
 * the credits (func_800507D0) load 16 SU sectors of MIPS there and call
 * into them: interpret that region whenever no native module is resident. */
static int native_module_resident_at(uint32_t bank)
{
    unsigned i;
    for (i = 0; i < Memories_ModuleCount; i++) {
        if (Memories_Modules[i].bank == bank && Memories_ModuleIsResident(bank, Memories_Modules[i].identifier)) {
            return 1;
        }
    }
    return 0;
}

int Memories_MipsInOverlay(uint32_t address)
{
    if (address >= 0x80180000u && address < 0x80188000u) {
        return !native_module_resident_at(0x80180000u);
    }
    return (address >= 0x8013A000u && address < 0x80168000u) || (address >= 0x8017A000u && address < 0x80180000u);
}

static int ensure_stack(void)
{
    void *wanted = (void *)(uintptr_t)STACK_BASE;
    if (stack_top) {
        return 0;
    }
    if (mmap(wanted, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) !=
        wanted) {
        static uint32_t fallback[STACK_SIZE / 4] __attribute__((aligned(16)));
        fprintf(stderr, "memories-pc: cannot map the overlay stack at 0x%08x; using host memory\n", STACK_BASE);
        stack_top = (uint32_t)(uintptr_t)(fallback + STACK_SIZE / 4) - 16;
        return 0;
    }
    stack_top = STACK_BASE + STACK_SIZE - 16;
    return 0;
}

static void fail(State *s, uint32_t pc, const char *what, uint32_t detail)
{
    char text[160];
    fprintf(stderr, "memories-pc: MIPS overlay: %s (0x%08x at 0x%08x)\n", what, detail, pc);
    if (s->escape) {
        longjmp(*s->escape, 1);
    }
    snprintf(text, sizeof(text), "%s (0x%08x at 0x%08x)", what, detail, pc);
    Crash_ReportFatal("MIPS overlay", text);
    exit(71);
}

static const MemoriesGuestFunction *find_native(uint32_t address)
{
    unsigned lo = 0, hi = Memories_FunctionMapCount;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (Memories_FunctionMap[mid].guest < address) lo = mid + 1;
        else hi = mid;
    }
    while (lo < Memories_FunctionMapCount && Memories_FunctionMap[lo].guest == address) {
        if (Memories_ModuleIsResident(Memories_FunctionMap[lo].bank, Memories_FunctionMap[lo].identifier))
            return &Memories_FunctionMap[lo];
        lo++;
    }
    return NULL;
}

static uint32_t call_native(State *s, uint32_t address)
{
    const MemoriesGuestFunction *e;
    typedef uint32_t (*Call)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t, uint32_t, uint32_t, uint32_t);
    uint32_t sp = s->r[29], keep, result;

    /* libc and libmath routines linked as SDK assembly in the original and
     * therefore absent from the generated native function map. */
    switch (address) {
    case 0x8008E360u: memset((void *)(uintptr_t)s->r[4], 0, s->r[5]); return s->r[4];
    case 0x8008E3D0u: memset((void *)(uintptr_t)s->r[4], s->r[5], s->r[6]); return s->r[4];
    case 0x800866A0u: return (uint32_t)rsin((int)s->r[4]);
    case 0x80086770u: return (uint32_t)rcos((int)s->r[4]);
    case 0x80086920u: return (uint32_t)Psx_ccos((int)s->r[4]);
    case 0x80086BB0u: return (uint32_t)Psx_csin((int)s->r[4]);
    case 0x8008E590u: return Memories_Rand();
    /* The string routines, on guest pointers (guest RAM is mapped at its own address). */
#define G(reg) ((void *)(uintptr_t)s->r[reg])
#define GS(reg) ((const char *)(uintptr_t)s->r[reg])
    case 0x8008E320u: memmove(G(5), G(4), s->r[6]); return 0; /* bcopy(src, dst, n) */
    case 0x8008E390u: memcpy(G(4), G(5), s->r[6]); return s->r[4];
    case 0x8008FA80u: memmove(G(4), G(5), s->r[6]); return s->r[4];
    case 0x8008E5D0u: strcat(G(4), GS(5)); return s->r[4];
    case 0x8008E680u: return (uint32_t)strcmp(GS(4), GS(5));
    case 0x8008E6F0u: strcpy(G(4), GS(5)); return s->r[4];
    case 0x8008E740u: return (uint32_t)strlen(GS(4));
    case 0x8008E780u: return (uint32_t)strncmp(GS(4), GS(5), s->r[6]);
    case 0x8008E800u: strncpy(G(4), GS(5), s->r[6]); return s->r[4];
#undef G
#undef GS
    case 0x8008E870u: /* printf: the modules' debug prints */
        LOG(LOG_MIPS_PRINTF, "overlay printf: %s", (const char *)(uintptr_t)s->r[4]);
        return 0;
    default: break;
    }
    e = find_native(address);
    if (!e) {
        fail(s, s->pc, "call to an address with no native function", address);
    }
    keep = current_sp;
    current_sp = sp;
    result = ((Call)e->host)(s->r[4], s->r[5], s->r[6], s->r[7], l32(sp + 16), l32(sp + 20), l32(sp + 24),
                             l32(sp + 28), l32(sp + 32), l32(sp + 36), l32(sp + 40), l32(sp + 44));
    current_sp = keep;
    return result;
}

static void plain(State *s, uint32_t pc)
{
    uint32_t ins = l32(pc), a, word;
    unsigned op = ins >> 26, rs = ins >> 21 & 31, rt = ins >> 16 & 31, rd = ins >> 11 & 31, sa = ins >> 6 & 31,
             fn = ins & 63;
    int32_t im = (int16_t)ins;
    uint64_t up;
    int64_t sp;
    if (!ins) return;
    switch (op) {
    case 0:
        switch (fn) {
        case 0: s->r[rd] = s->r[rt] << sa; break;
        case 2: s->r[rd] = s->r[rt] >> sa; break;
        case 3: s->r[rd] = (uint32_t)((int32_t)s->r[rt] >> sa); break;
        case 4: s->r[rd] = s->r[rt] << (s->r[rs] & 31); break;
        case 6: s->r[rd] = s->r[rt] >> (s->r[rs] & 31); break;
        case 7: s->r[rd] = (uint32_t)((int32_t)s->r[rt] >> (s->r[rs] & 31)); break;
        case 0x0c: fail(s, pc, "syscall", ins); break;
        case 0x0d: break; /* break: GCC's divide-by-zero trap, never reached */
        case 0x10: s->r[rd] = s->hi; break;
        case 0x11: s->hi = s->r[rs]; break;
        case 0x12: s->r[rd] = s->lo; break;
        case 0x13: s->lo = s->r[rs]; break;
        case 0x18: sp = (int64_t)(int32_t)s->r[rs] * (int32_t)s->r[rt]; s->lo = (uint32_t)sp; s->hi = (uint32_t)(sp >> 32); break;
        case 0x19: up = (uint64_t)s->r[rs] * s->r[rt]; s->lo = (uint32_t)up; s->hi = (uint32_t)(up >> 32); break;
        case 0x1a:
            if (s->r[rt]) {
                if (s->r[rt] == 0xffffffffu) { s->lo = (uint32_t)-(int32_t)s->r[rs]; s->hi = 0; }
                else { s->lo = (uint32_t)((int32_t)s->r[rs] / (int32_t)s->r[rt]); s->hi = (uint32_t)((int32_t)s->r[rs] % (int32_t)s->r[rt]); }
            } else { s->lo = (int32_t)s->r[rs] < 0 ? 1u : 0xffffffffu; s->hi = s->r[rs]; }
            break;
        case 0x1b:
            if (s->r[rt]) { s->lo = s->r[rs] / s->r[rt]; s->hi = s->r[rs] % s->r[rt]; }
            else { s->lo = 0xffffffffu; s->hi = s->r[rs]; }
            break;
        case 0x20: case 0x21: s->r[rd] = s->r[rs] + s->r[rt]; break;
        case 0x22: case 0x23: s->r[rd] = s->r[rs] - s->r[rt]; break;
        case 0x24: s->r[rd] = s->r[rs] & s->r[rt]; break;
        case 0x25: s->r[rd] = s->r[rs] | s->r[rt]; break;
        case 0x26: s->r[rd] = s->r[rs] ^ s->r[rt]; break;
        case 0x27: s->r[rd] = ~(s->r[rs] | s->r[rt]); break;
        case 0x2a: s->r[rd] = (int32_t)s->r[rs] < (int32_t)s->r[rt]; break;
        case 0x2b: s->r[rd] = s->r[rs] < s->r[rt]; break;
        default: fail(s, pc, "unsupported instruction", ins);
        }
        break;
    case 8: case 9: s->r[rt] = s->r[rs] + (uint32_t)im; break;
    case 0xa: s->r[rt] = (int32_t)s->r[rs] < im; break;
    case 0xb: s->r[rt] = s->r[rs] < (uint32_t)im; break;
    case 0xc: s->r[rt] = s->r[rs] & (ins & 0xffff); break;
    case 0xd: s->r[rt] = s->r[rs] | (ins & 0xffff); break;
    case 0xe: s->r[rt] = s->r[rs] ^ (ins & 0xffff); break;
    case 0xf: s->r[rt] = ins << 16; break;
    case 0x12: /* COP2 */
        if (ins & (1u << 25)) { Memories_GteCommand(ins); break; }
        switch (rs) {
        case 0: s->r[rt] = Memories_GteReadData(rd); break;
        case 2: s->r[rt] = Memories_GteReadControl(rd); break;
        case 4: Memories_GteWriteData(rd, s->r[rt]); break;
        case 6: Memories_GteWriteControl(rd, s->r[rt]); break;
        default: fail(s, pc, "unsupported COP2 form", ins);
        }
        break;
    case 0x20: s->r[rt] = (uint32_t)(int32_t)(int8_t)l8(s->r[rs] + (uint32_t)im); break;
    case 0x21: s->r[rt] = (uint32_t)(int32_t)(int16_t)l16(s->r[rs] + (uint32_t)im); break;
    case 0x22: a = s->r[rs] + (uint32_t)im; s->r[rt] = (s->r[rt] & (0x00ffffffu >> ((a & 3) * 8))) | (l32(a & ~3u) << ((3 - (a & 3)) * 8)); break;
    case 0x23: s->r[rt] = l32(s->r[rs] + (uint32_t)im); break;
    case 0x24: s->r[rt] = l8(s->r[rs] + (uint32_t)im); break;
    case 0x25: s->r[rt] = l16(s->r[rs] + (uint32_t)im); break;
    case 0x26: a = s->r[rs] + (uint32_t)im; s->r[rt] = (s->r[rt] & (0xffffff00u << ((3 - (a & 3)) * 8))) | (l32(a & ~3u) >> ((a & 3) * 8)); break;
    case 0x28: s8(s->r[rs] + (uint32_t)im, (uint8_t)s->r[rt]); break;
    case 0x29: s16(s->r[rs] + (uint32_t)im, (uint16_t)s->r[rt]); break;
    case 0x2a: a = s->r[rs] + (uint32_t)im; word = l32(a & ~3u); s32(a & ~3u, (word & (0xffffff00u << ((a & 3) * 8))) | (s->r[rt] >> ((3 - (a & 3)) * 8))); break;
    case 0x2b: s32(s->r[rs] + (uint32_t)im, s->r[rt]); break;
    case 0x2e: a = s->r[rs] + (uint32_t)im; word = l32(a & ~3u); s32(a & ~3u, (word & (0x00ffffffu >> ((3 - (a & 3)) * 8))) | (s->r[rt] << ((a & 3) * 8))); break;
    case 0x32: Memories_GteLoad(rt, (void *)(uintptr_t)(s->r[rs] + (uint32_t)im)); break;
    case 0x3a: Memories_GteStore(rt, (void *)(uintptr_t)(s->r[rs] + (uint32_t)im)); break;
    default: fail(s, pc, "unsupported instruction", ins);
    }
    s->r[0] = 0;
}

static void delay(State *s, uint32_t pc)
{
    uint32_t i = l32(pc);
    unsigned op = i >> 26, fn = i & 63;
    if ((op == 0 && (fn == 8 || fn == 9)) || (op >= 1 && op <= 7) || op == 2 || op == 3 || (op >= 0x14 && op <= 0x17)) {
        fail(s, pc, "branch in a delay slot", i);
    }
    plain(s, pc);
}

int Memories_MipsTry(uint32_t address, const uint32_t *args, unsigned count, uint32_t *result)
{
    State s;
    jmp_buf escape;
    uint32_t sp, saved_sp = current_sp;
    unsigned i;
    if (ensure_stack()) {
        return -1;
    }
    memset(&s, 0, sizeof(s));
    /* Below the innermost live frame, or the stack's top, with room for the
     * twelve argument slots and the callee's home slots for a0-a3: the
     * credits module stores its arguments at sp+0 on entry. */
    sp = ((current_sp ? current_sp : stack_top) - 64) & ~0xFu;
    s.pc = address;
    s.escape = &escape;
    s.r[29] = sp;
    for (i = 0; i < count && i < 4; i++) {
        s.r[4 + i] = args[i];
    }
    for (i = 4; i < count; i++) {
        s32(sp + i * 4, args[i]);
    }
    if (setjmp(escape)) {
        current_sp = saved_sp;
        return -1;
    }
    while (s.pc) {
        uint32_t i, target, next;
        unsigned op, rs, rt, fn;
        int take = 0, likely = 0;
        int32_t im;
        if (++s.steps > LIMIT) {
            fail(&s, s.pc, "instruction limit", s.steps);
        }
        i = l32(s.pc);
        op = i >> 26;
        rs = i >> 21 & 31;
        rt = i >> 16 & 31;
        fn = i & 63;
        im = (int16_t)i;
        next = s.pc + 8;
        if (op == 0 && (fn == 8 || fn == 9)) { /* jr, jalr */
            target = s.r[rs];
            if (fn == 9) s.r[i >> 11 & 31] = next;
            delay(&s, s.pc + 4);
            if (!target) { s.pc = 0; continue; }
            if (!Memories_MipsInOverlay(target)) { s.r[2] = call_native(&s, target); s.pc = next; }
            else s.pc = target;
            continue;
        }
        if (op == 2 || op == 3) { /* j, jal */
            target = (s.pc & 0xf0000000u) | ((i & 0x3ffffff) << 2);
            if (op == 3) s.r[31] = next;
            delay(&s, s.pc + 4);
            if (op == 3 && !Memories_MipsInOverlay(target)) { s.r[2] = call_native(&s, target); s.pc = next; }
            else s.pc = target;
            continue;
        }
        if (op == 1) { /* bltz, bgez, bltzl, bgezl, bltzal, bgezal */
            unsigned kind = rt & 0x0f;
            if (kind == 0 || kind == 2) take = (int32_t)s.r[rs] < 0;
            else if (kind == 1 || kind == 3) take = (int32_t)s.r[rs] >= 0;
            else fail(&s, s.pc, "unsupported instruction", i);
            likely = kind == 2 || kind == 3;
            if (rt & 0x10) s.r[31] = next;
        } else if (op >= 4 && op <= 7) {
            if (op == 4) take = s.r[rs] == s.r[rt];
            if (op == 5) take = s.r[rs] != s.r[rt];
            if (op == 6) take = (int32_t)s.r[rs] <= 0;
            if (op == 7) take = (int32_t)s.r[rs] > 0;
        } else if (op >= 0x14 && op <= 0x17) {
            likely = 1;
            if (op == 0x14) take = s.r[rs] == s.r[rt];
            if (op == 0x15) take = s.r[rs] != s.r[rt];
            if (op == 0x16) take = (int32_t)s.r[rs] <= 0;
            if (op == 0x17) take = (int32_t)s.r[rs] > 0;
        } else {
            plain(&s, s.pc);
            s.pc += 4;
            continue;
        }
        if (!likely || take) delay(&s, s.pc + 4);
        s.pc = take ? s.pc + 4 + ((uint32_t)im << 2) : next;
    }
    current_sp = saved_sp;
    *result = s.r[2];
    return 0;
}

uint32_t Memories_MipsCall(uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t args[4] = {a0, a1, a2, a3}, result;
    if (Memories_MipsTry(address, args, 4, &result)) {
        char text[96];
        snprintf(text, sizeof(text), "overlay routine 0x%08x failed with no fallback", address);
        Crash_ReportFatal("MIPS overlay", text);
        exit(71);
    }
    return result;
}

/* A native routine called a function pointer that holds an overlay address
 * (a display-object callback an effect installed, for instance). The fault
 * handler parks the address and resumes here; the cdecl arguments are where
 * the caller left them, so this reads as many as any MIPS routine could. */
uint32_t Memories_MipsThunk(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                            uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11)
{
    uint32_t target = Memories_MipsThunkTarget, result;
    uint32_t args[12] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
    if (Memories_MipsTry(target, args, 12, &result)) {
        char text[96];
        snprintf(text, sizeof(text), "guest callback 0x%08x failed with no fallback", target);
        Crash_ReportFatal("MIPS overlay", text);
        exit(71);
    }
    return result;
}
