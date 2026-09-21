/* Routines that are reached during bring-up but only affect output that is
 * not visible yet. Each says so once and returns; none fakes a result a
 * caller could depend on. Replace them with real ports, do not grow this. */
#include <unistd.h>

static void once(int *said, const char *text, unsigned length)
{
    if (!*said) {
        *said = 1;
        (void)!write(2, text, length); /* async-signal-safe */
    }
}

#define DEFERRED(text) do { static int said; \
    once(&said, "memories-pc: deferred: " text "\n", sizeof("memories-pc: deferred: " text "\n") - 1); } while (0)

/* LIBGPU debug font: on-screen diagnostics only. */
void FntLoad(int x, int y) { (void)x; (void)y; DEFERRED("FntLoad (debug font)"); }
int FntOpen(int x, int y, int w, int h, int isbg, int n) { (void)x; (void)y; (void)w; (void)h; (void)isbg; (void)n; return 0; }
void SetDumpFnt(int id) { (void)id; }
int FntPrint(const char *format, ...) { (void)format; return 0; }
void *FntFlush(int id) { (void)id; return 0; }
