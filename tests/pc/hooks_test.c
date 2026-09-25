/* Function hooks (src/pc/mods/hooks.c) on functions laid out as the game's
 * are: built 32-bit, the targets with -fpatchable-function-entry=8,6
 * (tools/pc/test_mods_lifecycle.py builds it for Linux and Windows). */
#include "pc/mods/hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int active[4] = {1, 1, 1, 1};
int Mods_Active(int owner) { return owner >= 0 && owner < 4 && active[owner]; }

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

__attribute__((noinline, patchable_function_entry(8, 6))) int Game_Add(int a, int b) { return a + b; }
__attribute__((noinline, patchable_function_entry(8, 6))) int Game_Other(int a) { return a * 3; }
__attribute__((noinline)) int Port_Function(int a) { return a - 1; }
/* Called through a volatile pointer, as the game calls through its tables:
 * the compiler must not fold the call away. */
static int (*volatile call_add)(int, int) = Game_Add;

typedef int (*AddFunction)(int, int);
static void *first_original, *second_original;
static int doubled(int a, int b) { return ((AddFunction)first_original)(a, b) * 2; }
static int plus_hundred(int a, int b) { return ((AddFunction)second_original)(a, b) + 100; }
static int replaced(int a, int b) { (void)a; (void)b; return -7; }

int main(void)
{
    unsigned char entry[2];
    int first, second, third;
    memcpy(entry, (void *)Game_Add, 2);
    CHECK(call_add(2, 3) == 5);
    CHECK(!Hooks_IsHooked((const void *)Game_Add));
    CHECK(!Hooks_Add(0, (void *)Port_Function, (void *)replaced, NULL));   /* no room: refused */

    first = Hooks_Add(0, (void *)Game_Add, (void *)doubled, &first_original);
    CHECK(first);
    CHECK(Hooks_IsHooked((const void *)Game_Add));
    CHECK(call_add(2, 3) == 10);
    second = Hooks_Add(1, (void *)Game_Add, (void *)plus_hundred, &second_original);
    CHECK(second && second != first);
    CHECK(call_add(2, 3) == 110);        /* the later hook runs first */

    active[0] = 0; Hooks_Relink();        /* mod 0 removed: its hook steps aside */
    CHECK(call_add(2, 3) == 105);
    CHECK(((AddFunction)first_original)(2, 3) == 5);
    active[0] = 1; Hooks_Relink();
    CHECK(call_add(2, 3) == 110);

    Hooks_Remove(1, second);
    CHECK(call_add(2, 3) == 10);
    Hooks_Clear(0);
    CHECK(call_add(2, 3) == 5);
    CHECK(!Hooks_IsHooked((const void *)Game_Add));
    CHECK(!memcmp(entry, (void *)Game_Add, 2));   /* its own nops are back */

    third = Hooks_Add(2, (void *)Game_Other, (void *)replaced, NULL);
    CHECK(third && Game_Other(4) == -7);
    active[2] = 0; Hooks_Relink();
    CHECK(Game_Other(4) == 12);
    CHECK(!Hooks_IsHooked((const void *)Game_Other));
    printf("hooks: passed\n");
    return 0;
}
