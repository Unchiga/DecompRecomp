#define _GNU_SOURCE
#include "state.h"
#include "pc/mods/mods.h"
#include "image.h"
#include "pc/audio/spu.h"
#include "pc/compat/gte.h"
#include "pc/render/soft_gpu.h"
#include "pc/debug/crash.h"
#include "pc/debug/log.h"
#include "pc/compat/signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/compat/mman.h"
#include <sys/stat.h>
#include <unistd.h>
#include "pc/compat/posix.h"
#ifndef _WIN32
#include <ucontext.h>
#endif

#ifdef _WIN32
#define STACK_BASE 0x90000000u /* 32-bit Windows loads system DLLs around 0x70000000 */
#else
#define STACK_BASE 0x70000000u
#endif
#define STACK_SIZE 0x00800000u
#define STACK_TOP (STACK_BASE + STACK_SIZE)
#define SCRATCHPAD 0x1f800000u
#define SCRATCHPAD_SIZE 0x400u
#define VERSION 2u /* 1: the header word was the game-source fingerprint; 2: the build id */

/* Provided by the link: the fixed-address sections of the game objects. */
extern char __start_game_text[], __stop_game_text[];
extern char __start_game_data[], __stop_game_data[];
extern char __start_game_bss[] __attribute__((weak)), __stop_game_bss[] __attribute__((weak));
extern const unsigned Memories_GameFingerprint; /* generated: hash of the game sources */
void Memories_StateReturn(const MemoriesStateEntry *entry, int value) __attribute__((noreturn));

MemoriesStateEntry Memories_StateEntry;

typedef struct Region {
    const char *name;
    char *data, *data_end, *bss, *bss_end;
    char *startup; /* the data as linked, before the game ran */
} Region;

struct MemoriesState {
    int loading;
    FILE *file;              /* saving */
    const uint8_t *image;    /* loading: the whole file */
    size_t image_size;
};

static Region *regions;
static unsigned region_count;
#ifdef _WIN32
/* Windows has no ucontext. A context is the stack pointer of a suspended
 * Memories_ContextSwitch (state_i386.S), which keeps the callee-saved
 * registers on that stack. The thread's stack bounds and exception-handler
 * chain live in its TEB and must follow the stack, as fibers do: exceptions
 * raised on a stack outside those bounds cannot be dispatched. Bounds are
 * the TEB's first three words: handler chain, stack base, stack limit. */
void Memories_ContextSwitch(uint32_t *from_esp, const uint32_t *to_esp);
static uint32_t service_context, game_context;
static uint32_t process_bounds[3];
static const uint32_t game_bounds[3] = {0xffffffffu, STACK_TOP, STACK_BASE}; /* no handlers yet */

static void save_stack_bounds(uint32_t *bounds)
{
    __asm__ volatile("movl %%fs:0, %0\n\tmovl %%fs:4, %1\n\tmovl %%fs:8, %2"
                     : "=r"(bounds[0]), "=r"(bounds[1]), "=r"(bounds[2]));
}

static void set_stack_bounds(const uint32_t *bounds)
{
    __asm__ volatile("movl %0, %%fs:0\n\tmovl %1, %%fs:4\n\tmovl %2, %%fs:8"
                     :
                     : "r"(bounds[0]), "r"(bounds[1]), "r"(bounds[2])
                     : "memory");
}

static void leave_game_stack(void)
{
    set_stack_bounds(process_bounds);
    Memories_ContextSwitch(&game_context, &service_context);
}
#else
static ucontext_t service_context, game_context;
#endif
static int (*game_entry)(void);
static int game_result;
static volatile int requested, requested_slot = 1;
static volatile int last_loaded_slot;
static uint8_t *pending_image;
static uint32_t build_id; /* from the `buildid` file beside the executable */
static size_t pending_size;

int Memories_StateLoading(const MemoriesState *state) { return state->loading; }
int Memories_LastStateSlot(void) { return last_loaded_slot; }

void Memories_StateRequest(int what, int slot)
{
    if (slot > 0) {
        requested_slot = slot;
    }
    requested = what;
}

/* Chunk: 16-byte tag, 32-bit size, payload. */
static const uint8_t *find_chunk(const MemoriesState *state, const char *tag, size_t *size)
{
    size_t at = 16;
    char padded[16];
    memset(padded, 0, sizeof(padded));
    strncpy(padded, tag, sizeof(padded) - 1);
    while (at + 20 <= state->image_size) {
        uint32_t length;
        memcpy(&length, state->image + at + 16, 4);
        if (length > state->image_size - at - 20) {
            return NULL;
        }
        if (!memcmp(state->image + at, padded, 16)) {
            *size = length;
            return state->image + at + 20;
        }
        at += 20 + length;
    }
    return NULL;
}

int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    size_t total = 0, i, size;
    const uint8_t *from;
    for (i = 0; i < count; i++) {
        total += fields[i].size;
    }
    if (!state->loading) {
        char padded[16];
        uint32_t length = (uint32_t)total;
        memset(padded, 0, sizeof(padded));
        strncpy(padded, tag, sizeof(padded) - 1);
        fwrite(padded, 1, 16, state->file);
        fwrite(&length, 4, 1, state->file);
        for (i = 0; i < count; i++) {
            fwrite(fields[i].data, 1, fields[i].size, state->file);
        }
        return 0;
    }
    from = find_chunk(state, tag, &size);
    if (!from || size != total) {
        fprintf(stderr, "memories-pc: state: %s '%s'; that part keeps its current state\n",
                from ? "layout changed for" : "no chunk", tag);
        return 0;
    }
    for (i = 0; i < count; i++) {
        memcpy(fields[i].data, from, fields[i].size);
        from += fields[i].size;
    }
    return 1;
}

static void subsystems(MemoriesState *state)
{
    MemoriesStateField gpu[2], gte[1];
    unsigned gte_size;
    gpu[0].data = SoftGpu_StateData(0, &gpu[0].size);
    gpu[1].data = SoftGpu_StateData(1, &gpu[1].size);
    gte[0].data = Gte_StateData(&gte_size);
    gte[0].size = gte_size;
    Memories_StateChunk(state, "soft_gpu", gpu, 2);
    Memories_StateChunk(state, "gte", gte, 1);
    Spu_State(state);
    LibSpu_State(state);
    LibDs_State(state);
    LibEtc_State(state);
    LibGpu_State(state);
    LibGte_State(state);
    LibPress_State(state);
    LibMcrd_State(state);
    Platform_State(state);
}

static void tagged(char *out, size_t size, const char *kind, const char *name)
{
    snprintf(out, size, "%s:%s", kind, name);
}

static void hold_signals(int hold)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigprocmask(hold ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

static void slot_path(char *out, size_t size, int slot)
{
    const char *directory = getenv("MEMORIES_STATE_DIR");
    if (!directory) {
        directory = "tmp/pc/states";
        mkdir("tmp", 0777);
        mkdir("tmp/pc", 0777);
    }
    mkdir(directory, 0777);
    snprintf(out, size, "%s/slot%d.state", directory, slot);
}

static int save(const char *path)
{
    MemoriesState state = {0, NULL, NULL, 0};
    MemoriesStateEntry entry = Memories_StateEntry;
    char partial[600], tag[32];
    uint32_t header[2] = {VERSION, build_id};
    unsigned i;
    snprintf(partial, sizeof(partial), "%s.partial", path);
    state.file = fopen(partial, "wb");
    if (!state.file) {
        perror(partial);
        return -1;
    }
    hold_signals(1);
    Spu_Hold(1);
    fwrite("YFMSTATE", 1, 8, state.file);
    fwrite(header, 4, 2, state.file);
    {
        MemoriesStateField fields[] = {{&entry, sizeof(entry)}};
        Memories_StateChunk(&state, "entry", fields, 1);
    }
    {
        MemoriesStateField fields[] = {{(void *)(uintptr_t)entry.esp, STACK_TOP - entry.esp}};
        Memories_StateChunk(&state, "stack", fields, 1);
    }
    {
        MemoriesStateField fields[] = {{(void *)(uintptr_t)MEMORIES_GUEST_RAM, MEMORIES_GUEST_RAM_SIZE},
                                       {(void *)(uintptr_t)SCRATCHPAD, SCRATCHPAD_SIZE}};
        Memories_StateChunk(&state, "memory", fields, 2);
    }
    for (i = 0; i < region_count; i++) {
        Region *region = &regions[i];
        MemoriesStateField data[] = {{region->startup, (size_t)(region->data_end - region->data)},
                                     {region->data, (size_t)(region->data_end - region->data)}};
        MemoriesStateField bss[] = {{region->bss, (size_t)(region->bss_end - region->bss)}};
        tagged(tag, sizeof(tag), "data", region->name);
        Memories_StateChunk(&state, tag, data, 2);
        tagged(tag, sizeof(tag), "bss", region->name);
        Memories_StateChunk(&state, tag, bss, 1);
    }
    subsystems(&state);
    Spu_Hold(0);
    hold_signals(0);
    if (fclose(state.file) != 0 || rename(partial, path) != 0) {
        perror(path);
        return -1;
    }
    fprintf(stderr, "memories-pc: state saved to %s\n", path);
    return 0;
}

/* Runs on the service (process) stack: the game stack is about to be replaced. */
static void apply(void)
{
    MemoriesState state = {1, NULL, pending_image, pending_size};
    static MemoriesStateEntry entry; /* not on a stack that a handler may share */
    const uint8_t *chunk;
    size_t size;
    char tag[32];
    unsigned i;
    hold_signals(1);
    Spu_Hold(1);
    chunk = find_chunk(&state, "memory", &size);
    memcpy((void *)(uintptr_t)MEMORIES_GUEST_RAM, chunk, MEMORIES_GUEST_RAM_SIZE);
    memcpy((void *)(uintptr_t)SCRATCHPAD, chunk + MEMORIES_GUEST_RAM_SIZE, SCRATCHPAD_SIZE);
    for (i = 0; i < region_count; i++) {
        Region *region = &regions[i];
        size_t length = (size_t)(region->data_end - region->data), word;
        tagged(tag, sizeof(tag), "data", region->name);
        chunk = find_chunk(&state, tag, &size);
        if (chunk && size == length * 2) {
            memcpy(region->data, chunk + length, length);
            /* Relocated words the game never changed follow this build. */
            for (word = 0; word + 4 <= length; word += 4) {
                if (!memcmp(chunk + word, chunk + length + word, 4) && memcmp(chunk + word, region->startup + word, 4)) {
                    memcpy(region->data + word, region->startup + word, 4);
                }
            }
        } else {
            fprintf(stderr, "memories-pc: state: variables of '%s' do not match this build\n", region->name);
        }
        tagged(tag, sizeof(tag), "bss", region->name);
        chunk = find_chunk(&state, tag, &size);
        if (chunk && size == (size_t)(region->bss_end - region->bss)) {
            memcpy(region->bss, chunk, size);
        }
    }
    subsystems(&state);
    chunk = find_chunk(&state, "entry", &size);
    memcpy(&entry, chunk, sizeof(entry));
    chunk = find_chunk(&state, "stack", &size);
    memcpy((void *)(uintptr_t)entry.esp, chunk, size);
    free(pending_image);
    pending_image = NULL;
    Spu_Hold(0);
    Mods_Reset(); /* another game: whatever the mods were holding is not it */
    fprintf(stderr, "memories-pc: state loaded\n");
    hold_signals(0);
#ifdef _WIN32
    set_stack_bounds(game_bounds);
#endif
    Memories_StateReturn(&entry, 263); /* one field, as VSync(0) reports it */
}

/* Relocation across game-source changes. A state holds addresses of game
 * code: return addresses on the stack, and callbacks stored in guest RAM, in
 * game variables and in a few native chunks. The build files every game
 * build's symbols under its fingerprint (tmp/pc/game32/symbols/), so a state
 * from another build can be carried over by name:
 *
 * - a word equal to the start of an old function becomes the new start,
 *   wherever function pointers live (aligned words of guest RAM and game
 *   data, and the callback-bearing part of the native chunks);
 * - on the stack any address inside an old function moves with it, which is
 *   only right if that function is unchanged, so a function on the stack
 *   whose size differs refuses the load;
 * - game variables must not have moved at all, or the load is refused.
 *
 * A value that merely looks like a function's first byte would be rewritten
 * too; with about a thousand functions that is improbable, and the count of
 * rewritten words is reported. */
typedef struct Symbol {
    uint32_t address, size;
    char name[72];
} Symbol;

static int executable_directory(char *out, size_t size)
{
    ssize_t length;
    char *slash;
    if (!size) return -1;
    length = readlink("/proc/self/exe", out, size - 1);
    if (length <= 0 || (size_t)length >= size) return -1;
    out[length] = 0;
    slash = strrchr(out, '/');
    if (!slash) return -1;
    *slash = 0;
    return 0;
}

int Memories_SymbolTablePath(char *out, size_t size)
{
    char directory[512], path[640], text[32] = "";
    FILE *file;
    if (executable_directory(directory, sizeof(directory))) return -1;
    snprintf(path, sizeof(path), "%s/buildid", directory);
    file = fopen(path, "r");
    if (!file) return -1;
    if (fgets(text, sizeof(text), file)) build_id = (uint32_t)strtoul(text, NULL, 16);
    fclose(file);
    if (!build_id || snprintf(out, size, "%s/symbols/%08x.txt", directory, (unsigned)build_id) >= (int)size) {
        return -1;
    }
    return 0;
}

static Symbol *read_symbols(uint32_t fingerprint, size_t *count)
{
    char path[640], exe[512];
    Symbol *table = NULL;
    FILE *file;
    size_t used = 0, room = 0;
    *count = 0;
    if (executable_directory(exe, sizeof(exe))) return NULL;
    snprintf(path, sizeof(path), "%s/symbols/%08x.txt", exe, (unsigned)fingerprint);
    file = fopen(path, "r");
    if (!file) {
        return NULL;
    }
    for (;;) {
        Symbol symbol;
        unsigned address, size;
        if (fscanf(file, "%x %x %71s", &address, &size, symbol.name) != 3) {
            break;
        }
        symbol.address = address;
        symbol.size = size;
        if (used == room) {
            room = room ? room * 2 : 2048;
            table = realloc(table, room * sizeof(*table));
            if (!table) {
                fclose(file);
                return NULL;
            }
        }
        table[used++] = symbol;
    }
    fclose(file);
    *count = used;
    return table;
}

static const Symbol *by_name(const Symbol *table, size_t count, const char *name)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (!strcmp(table[i].name, name)) {
            return &table[i];
        }
    }
    return NULL;
}

/* Tables are in address order. */
static const Symbol *containing(const Symbol *table, size_t count, uint32_t address)
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if (table[middle].address + (table[middle].size ? table[middle].size : 1) <= address) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low < count && table[low].address <= address ? &table[low] : NULL;
}

/* The tables hold functions and the game objects' variables; the variables
 * are the ones linked between these addresses. */
static int is_text(uint32_t address) { return address < 0x03000000u || address >= 0x08000000u; }
static int is_game_text(uint32_t address) { return address >= 0x01000000u && address < 0x01400000u; }

static void read_build_id(void)
{
    char path[640];
    Memories_SymbolTablePath(path, sizeof(path));
}

/* Returns the number of words rewritten, or -1 when the state cannot move. */
static long relocate_words(uint8_t *bytes, size_t size, size_t step, int whole_functions, const Symbol *old,
                           size_t old_count, const Symbol *new, size_t new_count)
{
    long changed = 0;
    size_t at;
    for (at = 0; at + 4 <= size; at += step) {
        const Symbol *from, *to;
        uint32_t value;
        memcpy(&value, bytes + at, 4);
        if (!is_text(value) || !(from = containing(old, old_count, value)) || !is_text(from->address)) {
            continue;
        }
        if (value != from->address && !whole_functions) {
            continue;
        }
        to = by_name(new, new_count, from->name);
        if (!to || (value != from->address && to->size != from->size)) {
            /* A native frame below the game's entry is never returned to. */
            if (whole_functions && is_game_text(from->address)) {
                fprintf(stderr, "memories-pc: state: %s was running when the state was saved and has changed since\n",
                        from->name);
                return -1;
            }
            continue;
        }
        value = to->address + (value - from->address);
        memcpy(bytes + at, &value, 4);
        changed += to->address != from->address;
    }
    return changed;
}

static int relocate(uint8_t *image, size_t image_size, uint32_t saved_fingerprint)
{
    size_t old_count, new_count, at = 16, i;
    Symbol *old = read_symbols(saved_fingerprint, &old_count);
    Symbol *new = read_symbols(build_id, &new_count);
    long total = 0;
    int result = -1;
    if (!old || !new) {
        fprintf(stderr, "memories-pc: state: no symbol table for build %08x or %08x in symbols/; cannot carry the "
                        "state over\n", (unsigned)saved_fingerprint, (unsigned)build_id);
        goto done;
    }
    for (i = 0; i < old_count; i++) {
        const Symbol *now = is_text(old[i].address) ? NULL : by_name(new, new_count, old[i].name);
        if (now && now->address != old[i].address) {
            fprintf(stderr, "memories-pc: state: game variable %s moved; cannot carry the state over\n", old[i].name);
            goto done;
        }
    }
    while (at + 20 <= image_size) {
        const char *tag = (const char *)image + at;
        uint32_t length;
        long changed = 0;
        memcpy(&length, image + at + 16, 4);
        if (length > image_size - at - 20) {
            break;
        }
        if (!strcmp(tag, "stack")) {
            changed = relocate_words(image + at + 20, length, 4, 1, old, old_count, new, new_count);
        } else if (!strcmp(tag, "memory") || !strncmp(tag, "data:", 5) || !strncmp(tag, "bss:", 4)) {
            changed = relocate_words(image + at + 20, length, 4, 0, old, old_count, new, new_count);
        } else if (!strcmp(tag, "libetc") || !strcmp(tag, "libpress") || !strcmp(tag, "libds")) {
            /* Packed fields: callbacks sit at any offset, all within the first kilobyte. */
            changed = relocate_words(image + at + 20, length < 1024 ? length : 1024, 1, 0, old, old_count, new,
                                     new_count);
        }
        if (changed < 0) {
            goto done;
        }
        total += changed;
        at += 20 + length;
    }
    fprintf(stderr, "memories-pc: state from build %08x carried over to %08x: %ld code addresses moved\n",
            (unsigned)saved_fingerprint, (unsigned)build_id, total);
    result = 0;
done:
    free(old);
    free(new);
    return result;
}

static int load(const char *path)
{
    MemoriesState state = {1, NULL, NULL, 0};
    MemoriesStateEntry entry;
    FILE *file = fopen(path, "rb");
    const uint8_t *chunk;
    uint8_t *image;
    uint32_t header[2];
    size_t size;
    long length;
    if (!file) {
        perror(path);
        return -1;
    }
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    rewind(file);
    image = malloc(length > 0 ? (size_t)length : 1);
    if (!image || length < 16 || fread(image, 1, (size_t)length, file) != (size_t)length ||
        memcmp(image, "YFMSTATE", 8)) {
        fprintf(stderr, "memories-pc: %s is not a save state\n", path);
        fclose(file);
        free(image);
        return -1;
    }
    fclose(file);
    memcpy(header, image + 8, 8);
    state.image = image;
    state.image_size = (size_t)length;
    chunk = find_chunk(&state, "entry", &size);
    if ((header[0] != 1 && header[0] != VERSION) || !chunk || size != sizeof(entry)) {
        fprintf(stderr, "memories-pc: %s: unsupported state version\n", path);
        free(image);
        return -1;
    }
    memcpy(&entry, chunk, sizeof(entry));
    chunk = find_chunk(&state, "stack", &size);
    if (!chunk || entry.esp < STACK_BASE || entry.esp >= STACK_TOP || size != STACK_TOP - entry.esp ||
        !find_chunk(&state, "memory", &size) || size != MEMORIES_GUEST_RAM_SIZE + SCRATCHPAD_SIZE) {
        fprintf(stderr, "memories-pc: %s: damaged state\n", path);
        free(image);
        return -1;
    }
    if (header[0] == 1) {
        /* Tables from before build ids list game code only: pointers to
         * native routines held by the game (the town map's HMD drivers) are
         * not carried over, and such a state can fail where it uses them. */
        if (header[1] == Memories_GameFingerprint) {
            header[1] = build_id; /* same game code: nothing to move */
        } else {
            fprintf(stderr, "memories-pc: %s predates build ids; only game code addresses can be carried over\n", path);
        }
    }
    if (header[1] != build_id && relocate(image, (size_t)length, header[1]) != 0) {
        fprintf(stderr, "memories-pc: %s was saved by another build and was not loaded\n", path);
        free(image);
        return -1;
    }
    pending_image = image;
    pending_size = (size_t)length;
    /* Leave the game stack; the service context applies the state. */
#ifdef _WIN32
    leave_game_stack();
#else
    swapcontext(&game_context, &service_context);
#endif
    return 0; /* not reached: the state resumes in its own VSync caller */
}

static int from_game_code(void)
{
    uint32_t caller;
    if (Memories_StateEntry.esp < STACK_BASE || Memories_StateEntry.esp >= STACK_TOP) {
        return 0;
    }
    caller = *(const uint32_t *)(uintptr_t)Memories_StateEntry.esp;
    return caller >= (uintptr_t)__start_game_text && caller < (uintptr_t)__stop_game_text;
}

void Memories_StatePoint(unsigned presented_frames)
{
    static int startup_done, scripted_done;
    static unsigned scripted_frame;
    static const char *scripted_path;
    char path[512];
    int what;
    if (!from_game_code()) {
        return; /* a native caller's frame would not mean anything to another build */
    }
    if (!startup_done && presented_frames >= 30) {
        /* MEMORIES_LOAD_STATE=<slot number or path>, once the boot has
         * initialized every subsystem the state will fill in. */
        const char *wanted = getenv("MEMORIES_LOAD_STATE");
        const char *script = getenv("MEMORIES_SAVE_STATE"); /* "<frame>:<path>", for tests */
        startup_done = 1;
        if (script && strchr(script, ':')) {
            scripted_frame = (unsigned)atoi(script);
            scripted_path = strchr(script, ':') + 1;
        }
        if (wanted && *wanted) {
            int load_result;
            if (strspn(wanted, "0123456789") == strlen(wanted)) {
                slot_path(path, sizeof(path), atoi(wanted));
            } else {
                snprintf(path, sizeof(path), "%s", wanted);
            }
            load_result = load(path);
            if (!load_result && strspn(wanted, "0123456789") == strlen(wanted)) last_loaded_slot = atoi(wanted);
            if (load_result) Crash_ReportSoft("state load failed", path);
        }
    }
    if (scripted_path && !scripted_done && presented_frames >= scripted_frame) {
        scripted_done = 1;
        save(scripted_path);
    }
    {
        /* MEMORIES_AUTOSAVE=<seconds>: a rolling state every so many seconds
         * of presented frames, in slots auto1..auto3 of the state folder, so
         * that a problem report comes with a state from shortly before it. */
        static unsigned autosave_every, autosave_next, autosave_index;
        static int autosave_read;
        if (!autosave_read) {
            const char *every = getenv("MEMORIES_AUTOSAVE");
            autosave_read = 1;
            autosave_every = every ? (unsigned)atoi(every) * 60u : 0;
            autosave_next = presented_frames + autosave_every;
        }
        if (autosave_every && presented_frames >= autosave_next) {
            char folder[512];
            const char *slash;
            autosave_next = presented_frames + autosave_every;
            slot_path(folder, sizeof(folder), 0); /* creates the folder */
            slash = strrchr(folder, '/');
            snprintf(path, sizeof(path), "%.*s/auto%u.state", slash ? (int)(slash - folder) : 1,
                     slash ? folder : ".", autosave_index % 3 + 1);
            autosave_index++;
            if (!save(path)) LOG(LOG_STATE, "autosave %s at frame %u", path, presented_frames);
        }
    }
    what = __atomic_exchange_n(&requested, 0, __ATOMIC_SEQ_CST);
    if (what) {
        slot_path(path, sizeof(path), requested_slot);
        if (what == 1) {
            save(path);
        } else {
            if (!load(path)) last_loaded_slot = requested_slot;
            else Crash_ReportSoft("state load failed", path);
        }
    }
}

static void run_game(void)
{
    game_result = game_entry();
#ifdef _WIN32
    leave_game_stack(); /* what uc_link does on Linux */
#endif
}

static int add_region(const char *name, char *data, char *data_end, char *bss, char *bss_end)
{
    Region *region = &regions[region_count++];
    size_t size = (size_t)(data_end - data);
    region->name = name;
    region->data = data;
    region->data_end = data_end;
    region->bss = bss;
    region->bss_end = bss_end;
    region->startup = malloc(size ? size : 1);
    if (!region->startup) {
        return -1;
    }
    memcpy(region->startup, data, size);
    return 0;
}

int Memories_StateRunGame(int (*entry)(void))
{
    void *stack = mmap((void *)(uintptr_t)STACK_BASE, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned i;
    if (stack != (void *)(uintptr_t)STACK_BASE) {
        perror("game stack");
        return 1;
    }
    read_build_id();
    regions = calloc(Memories_ModuleCount + 1, sizeof(*regions));
    if (!regions || add_region("game", __start_game_data, __stop_game_data, __start_game_bss, __stop_game_bss)) {
        return 1;
    }
    for (i = 0; i < Memories_ModuleCount; i++) {
        const MemoriesModule *module = &Memories_Modules[i];
        if (add_region(module->name, module->data, module->data_end, module->bss, module->bss_end)) {
            return 1;
        }
    }
    game_entry = entry;
#ifdef _WIN32
    {
        /* What Memories_ContextSwitch pops: EDI ESI EBX EBP, then the return
         * into run_game, whose own return address is never used. */
        uint32_t *top = (uint32_t *)(uintptr_t)(STACK_TOP - 64);
        top[0] = top[1] = top[2] = top[3] = 0;
        top[4] = (uint32_t)(uintptr_t)run_game;
        top[5] = 0;
        game_context = (uint32_t)(uintptr_t)top;
        save_stack_bounds(process_bounds);
        set_stack_bounds(game_bounds);
        /* Every load request re-enters here, on the process stack. */
        Memories_ContextSwitch(&service_context, &game_context);
    }
#else
    getcontext(&game_context);
    game_context.uc_stack.ss_sp = stack;
    game_context.uc_stack.ss_size = STACK_SIZE;
    game_context.uc_link = &service_context;
    makecontext(&game_context, run_game, 0);
    /* Every load request re-enters here, on the process stack. */
    swapcontext(&service_context, &game_context);
#endif
    if (pending_image) {
        apply();
    }
    return game_result;
}
