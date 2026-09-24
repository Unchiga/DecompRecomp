/* The code mod loader (src/pc/mods/object_loader.c), in a 32-bit process on
 * Linux and on Windows: tools/pc/test_object_loader.py builds the fixtures in
 * tests/pc/mod_fixtures with build_mod.py's flags, builds this for each
 * system and runs it with the fixture directory as its argument.
 *
 * The good object must load and run, reaching the host both ways. Every
 * broken one must fail with its own message. And the good object, damaged a
 * byte at a time, must never crash the loader: the file is untrusted. */
#include "pc/compat/fs.h"
#include "pc/mods/exports.h"
#include "pc/mods/object_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(condition, ...) do { \
    checks++; \
    if (!(condition)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* What the game would lend: a function and a variable of its own, and the
 * C library list. The Linux game is SSE2 code that needs the stack 16-byte
 * aligned at every call into it; the Windows one promises a mod only 4. So
 * the host calls the mod from a misaligned stack (call_misaligned) and the
 * mod must still call back in aligned (build_mod.py: -mstackrealign). */
static int misaligned_calls;

int host_add(int a, int b)
{
#ifndef _WIN32
    if ((uintptr_t)&a & 15) misaligned_calls++;
#endif
    return a + b;
}

static int call_misaligned(int (*function)(void))
{
    int result;
    __asm__ volatile("mov %%esp, %%esi\n\t"
                     "and $-16, %%esp\n\t"
                     "sub $4, %%esp\n\t"
                     "call *%1\n\t"
                     "mov %%esi, %%esp"
                     : "=a"(result) : "r"(function) : "esi", "ecx", "edx", "memory", "cc");
    return result;
}

int host_value = 1234;

/* Narrow returns: compiled -O2, both hosts' compilers leave the upper bits
 * of a short or char result as they fall (0xFFFF + 1 is 0x10000 here), so
 * the mod, built by another compiler, must extend what it reads. */
short host_narrow(const short *value) { return (short)(*value + 1); }
signed char host_narrow_char(const signed char *value) { return (signed char)(*value + 1); }

static void *resolve(const char *name, void *context)
{
    union { void (*function)(void); void *pointer; } libc;
    (void)context;
    if (!strcmp(name, "host_add")) {
        union { int (*function)(int, int); void *pointer; } add;
        add.function = host_add;
        return add.pointer;
    }
    if (!strcmp(name, "host_value")) return &host_value;
    if (!strcmp(name, "host_narrow")) {
        union { short (*function)(const short *); void *pointer; } narrow;
        narrow.function = host_narrow;
        return narrow.pointer;
    }
    if (!strcmp(name, "host_narrow_char")) {
        union { signed char (*function)(const signed char *); void *pointer; } narrow;
        narrow.function = host_narrow_char;
        return narrow.pointer;
    }
    libc.function = Mods_LibcLookup(name);
    return libc.function ? libc.pointer : NULL;
}

static unsigned char *read_file(const char *directory, const char *name, size_t *size)
{
    char path[1024];
    FILE *file;
    unsigned char *data;
    long length;
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    file = fopen(path, "rb");
    if (!file) {
        printf("FAIL cannot open %s\n", path);
        exit(1);
    }
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc(length > 0 ? (size_t)length : 1);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) exit(1);
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void expect_failure(const char *directory, const char *name, const char *wanted)
{
    size_t size;
    unsigned char *data = read_file(directory, name, &size);
    LoadedObject object;
    char error[256];
    int result = ObjectLoader_Load(data, size, resolve, NULL, &object, error, sizeof(error));
    CHECK(result == -1, "%s loaded", name);
    CHECK(strstr(error, wanted) != NULL, "%s: \"%s\" does not say \"%s\"", name, error, wanted);
    CHECK(!object.image && !object.symbols, "%s: a failed load left memory behind", name);
    free(data);
}

static void good(const char *directory)
{
    size_t size;
    unsigned char *data = read_file(directory, "good.o", &size);
    LoadedObject object;
    char error[256];
    union { void *pointer; int (*run)(void); int (*(*get)(void))(int); } entry;
    int (*twice)(int);
    size_t i;
    int found_twice = 0;
    CHECK(!ObjectLoader_Load(data, size, resolve, NULL, &object, error, sizeof(error)), "good.o: %s", error);
    if (!object.image) return;
    entry.pointer = ObjectLoader_Symbol(&object, "run");
    CHECK(entry.pointer != NULL, "good.o has no run");
    if (entry.pointer) {
        int result = call_misaligned(entry.run);
        CHECK(result == 0, "good.o's run returned %d", result);
        CHECK(!misaligned_calls, "good.o called the host with a misaligned stack %d times", misaligned_calls);
        CHECK(host_value == 4321, "good.o did not write the host's variable");
        /* A second run sees what the first left in .data and .bss. */
        CHECK(entry.run() == 1, "good.o's .bss was not kept between calls");
    }
    entry.pointer = ObjectLoader_Symbol(&object, "get_twice");
    CHECK(entry.pointer != NULL, "good.o has no get_twice");
    if (entry.pointer) {
        twice = entry.get();
        CHECK(twice && twice(5) == 10, "the host could not call back into good.o");
    }
    /* A local (static) function is kept for crash reports, but is not
     * something the host can ask for by name. */
    for (i = 0; i < object.symbol_count; i++) {
        if (!strcmp(object.symbols[i].name, "twice")) {
            found_twice = object.symbols[i].function && !object.symbols[i].global &&
                          object.symbols[i].address >= (uintptr_t)object.image &&
                          object.symbols[i].address < (uintptr_t)object.image + object.code_size;
        }
    }
    CHECK(found_twice, "good.o's static function is not in its symbol list");
    CHECK(!ObjectLoader_Symbol(&object, "twice"), "a static function was found by name");
    CHECK(!ObjectLoader_Symbol(&object, "missing"), "a missing name was found");
    ObjectLoader_Free(&object);
    free(data);
}

/* Every byte of the headers, the section table and the symbol and
 * relocation tables set to a few values in turn. The loader may accept a
 * damaged file (a changed byte of code is still code) but must not crash on
 * one; nothing it accepts here is run. */
static void damaged(const char *directory)
{
    static const unsigned char values[] = {0x00, 0xff, 0x80, 0x7f, 0x01};
    size_t size, at;
    unsigned char *data = read_file(directory, "good.o", &size);
    unsigned char *copy = malloc(size);
    unsigned v, loaded = 0, refused = 0;
    for (at = 0; at < size; at++) {
        for (v = 0; v < sizeof(values); v++) {
            LoadedObject object;
            char error[256];
            memcpy(copy, data, size);
            if (copy[at] == values[v]) continue;
            copy[at] = values[v];
            if (ObjectLoader_Load(copy, size, resolve, NULL, &object, error, sizeof(error))) {
                refused++;
            } else {
                loaded++;
                ObjectLoader_Free(&object);
            }
        }
    }
    /* And cut short at every length. */
    for (at = 0; at < size; at++) {
        LoadedObject object;
        char error[256];
        if (!ObjectLoader_Load(data, at, resolve, NULL, &object, error, sizeof(error))) {
            loaded++;
            ObjectLoader_Free(&object);
        } else {
            refused++;
        }
    }
    CHECK(refused > 0, "no damaged copy was refused");
    printf("object_loader: %u damaged copies refused, %u loaded without running\n", refused, loaded);
    free(copy);
    free(data);
}

int main(int argc, char **argv)
{
    const char *directory = argc > 1 ? argv[1] : ".";
    CHECK(Mods_LibcSorted(), "the C library list is out of order");
    good(directory);
    expect_failure(directory, "pic.o", "position-independent");
    expect_failure(directory, "unknown.o", "needs missing_function, which this game does not provide");
    expect_failure(directory, "common.o", "COMMON");
    expect_failure(directory, "ctor.o", "constructors");
    expect_failure(directory, "protected.o", "stack protector");
    expect_failure(directory, "truncated.o", "cut short");
    expect_failure(directory, "garbage.o", "not an ELF");
    expect_failure(directory, "empty.o", "not an ELF");
    damaged(directory);
    printf("object_loader: %d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
