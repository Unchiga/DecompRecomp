/* The mod loader (src/pc/mods/modload.c) against libraries built the way a
 * player builds one (tools/pc/build_mod.py, fixtures in tests/pc/mod_fixtures):
 * the relocations a compiled mod carries, the C library it is given, the
 * game's names resolving to the game's own addresses, and the refusals --
 * a library that reaches for files by path or for the network, and files
 * that are not libraries at all. argv[1] is the directory the fixtures were
 * built into. */
#include "pc/mods/modload.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game, as far as the fixtures know it. */
int Memories_TestValue = 7;
int Memories_TestTwice(int value) { return value * 2; }

const ModSymbol Memories_ModSymbols[] = {
    {"Memories_TestTwice", (void *)(size_t)&Memories_TestTwice},
    {"Memories_TestValue", &Memories_TestValue},
};
const unsigned Memories_ModSymbolCount = sizeof(Memories_ModSymbols) / sizeof(Memories_ModSymbols[0]);

static char directory[1024];

static void *open_fixture(const char *name, char *error, size_t size)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    return ModLoad_Open(path, error, size);
}

/* A symbol from a loaded library, as the function type the test calls. */
#define FIXTURE(library, type, name) ((type)(size_t)ModLoad_Symbol(library, name))

static void write_bytes(const char *name, const void *bytes, size_t size)
{
    char path[1100];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(bytes, 1, size, file) == size);
    assert(!fclose(file));
}

int main(int argc, char **argv)
{
    char error[256], text[64];
    void *good, *library;
    unsigned i;
    assert(argc == 2);
    snprintf(directory, sizeof(directory), "%s", argv[1]);

    /* Both tables are searched by halves, so both must be in order. */
    for (i = 1; i < Memories_ModLibcCount; i++)
        assert(strcmp(Memories_ModLibc[i - 1].name, Memories_ModLibc[i].name) < 0);
    assert(ModLoad_Resolve("memcpy") && ModLoad_Resolve("Memories_TestValue") == &Memories_TestValue);
    assert(!ModLoad_Resolve("fopen") && !ModLoad_Resolve("remove") && !ModLoad_Resolve("socket"));

    error[0] = 0;
    good = open_fixture("good.mod", error, sizeof(error));
    if (!good) fprintf(stderr, "good.mod: %s\n", error);
    assert(good);
    assert(!ModLoad_Symbol(good, "no_such_function"));
    /* Its constructor ran before anything was asked of it. */
    assert(FIXTURE(good, int (*)(void), "fixture_constructed")() == 42);
    /* Pointers in its own data were moved with it. */
    assert(!strcmp(FIXTURE(good, const char *(*)(int), "fixture_word")(2), "gamma"));
    /* The game's names are the game's addresses, data and functions alike:
     * a mod can compare a pointer the game stored against one it names. */
    assert(FIXTURE(good, int *(*)(void), "fixture_value_address")() == &Memories_TestValue);
    assert(FIXTURE(good, int (*(*)(void))(int), "fixture_twice_address")() == Memories_TestTwice);
    assert(FIXTURE(good, int (*)(int), "fixture_call_twice")(20) == 47);
    /* The C library it is given. */
    assert(FIXTURE(good, long long (*)(long long, long long), "fixture_divide")(123456789012345ll, 7) ==
           17636684144620ll);
    assert(FIXTURE(good, unsigned long long (*)(unsigned long long, unsigned long long),
                   "fixture_modulo")(18000000000000000000ull, 1000003ull) == 999517ull);
    assert(FIXTURE(good, int (*)(char *, int), "fixture_format")(text, sizeof(text)) > 0);
    assert(!strcmp(text, "beta 4 1.41 1099511627776"));
    assert(FIXTURE(good, int (*)(void), "fixture_clock")());
    assert(FIXTURE(good, int (*)(void), "fixture_mmap")());

    /* Files by path and the network are refused, and every such name is
     * reported at once. */
    error[0] = 0;
    assert(!open_fixture("forbidden.mod", error, sizeof(error)));
    assert(strstr(error, "fopen") && strstr(error, "remove") && strstr(error, "socket"));

    /* Not libraries: nothing, text, and a real library cut short. A cut
     * that only loses the debug information at the end may still load; any
     * other is refused with a reason, and none is read past its end. */
    error[0] = 0;
    assert(!open_fixture("missing.mod", error, sizeof(error)) && error[0]);
    write_bytes("text.mod", "not a library at all", 20);
    error[0] = 0;
    assert(!open_fixture("text.mod", error, sizeof(error)) && error[0]);
    {
        char path[1100];
        FILE *file;
        long size = 0;
        unsigned char *bytes;
        snprintf(path, sizeof(path), "%s/good.mod", directory);
        file = fopen(path, "rb");
        assert(file && !fseek(file, 0, SEEK_END) && (size = ftell(file)) > 0 && !fseek(file, 0, SEEK_SET));
        bytes = malloc((size_t)size);
        assert(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
        fclose(file);
        for (i = 1; i < 8; i++) {
            write_bytes("cut.mod", bytes, (size_t)size * i / 8);
            error[0] = 0;
            library = open_fixture("cut.mod", error, sizeof(error));
            assert(library || error[0]);
            if (i == 1) assert(!library); /* not even the segments are whole */
        }
        free(bytes);
    }
    printf("modload: ok\n");
    return 0;
}
