/* The object the loader's test loads and runs (tests/pc/object_loader_test.c):
 * a call into the host, a host variable read and written (as a mod does a
 * pinned guest variable), .rodata, .data, .bss, a pointer to a host function
 * in .data, a call into the object's other file, 64-bit division (a compiler
 * helper), C library calls, a stack frame larger than a page, and narrow
 * results from host functions built by the host's compiler. */
#include <stdio.h>
#include <string.h>

extern int host_add(int a, int b);
extern int host_value;
extern short host_narrow(const short *value);
extern signed char host_narrow_char(const signed char *value);
int other_file(int x);
int deep(int n);

static const char greeting[] = "hello";
static int counter = 5;
static int zeroes[64];
static int (*callback)(int, int) = host_add;

static int twice(int x)
{
    return x * 2;
}

long long divide(long long a, long long b)
{
    return a / b;
}

int (*get_twice(void))(int)
{
    return twice;
}

int run(void)
{
    char buffer[32];
    int i, sum = 0;
    for (i = 0; i < 64; i++) sum += zeroes[i];
    if (sum) return 1;
    if (callback(2, 3) != 5) return 2;
    if (host_value != 1234) return 3;
    host_value = 4321;
    counter++;
    if (counter != 6) return 4;
    snprintf(buffer, sizeof(buffer), "%s %d", greeting, twice(21));
    if (strcmp(buffer, "hello 42")) return 5;
    if (other_file(3) != 9) return 6;
    if (divide(10000000000LL, 3) != 3333333333LL) return 7;
    if (deep(7) != 14) return 8;
    {
        short minus_one = -1, minus_two = -2, top = 0x7fff;
        signed char c_minus_two = -2;
        volatile int wide;
        wide = host_narrow(&minus_one);
        if (wide != 0) return 9;
        wide = host_narrow(&minus_two);
        if (wide != -1) return 10;
        wide = host_narrow(&top);
        if (wide != -32768) return 11;
        wide = host_narrow_char(&c_minus_two);
        if (wide != -1) return 12;
    }
    zeroes[3] = 1;
    return 0;
}
