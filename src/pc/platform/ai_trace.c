/* AI trace: does the opponent's thinking time change what it decides?
 *
 * AiScript_Run (src/game/ai_script_vm.c) yields to the next frame once
 * VSync(1) reports 240 lines, so a slower machine spreads a decision over
 * more frames. The decision could only change if other code drew from rand()
 * in those frames, between two of the AI's own draws.
 * MEMORIES_AI_TRACE=<file> writes one line per event:
 *   A <address> Memories_Rand   where Memories_Rand is loaded, for symbols
 *   F <frame>                   a frame's VSync(0)
 *   V <offset> <base> <id>      a VSync(1) by AiScript_Run, after a command
 *                               that did not end the run: the command's
 *                               script offset, the script base and the
 *                               opponent id
 *   R <address>                 a rand() call and its caller
 * MEMORIES_AI_YIELD=N makes every Nth of those VSync(1) calls report a full
 * frame, so the interpreter yields there as a slow console would.
 * tools/pc/ai_trace_check.py reads the file; notes/pc-build.md has the
 * result. */
#include "pc/compat/fs.h"
#include "pc/platform/ai_trace.h"
#include "pc/rng.h"
#include <stdio.h>
#include <stdlib.h>

extern struct { /* AiScriptState, src/game/ai.h */
    unsigned char enabled, pad01[3];
    unsigned char *script_base;
    unsigned char *script_cursor;
    unsigned char *previous_cursor;
} gAiScript_State;
extern signed char gDuel_bOpponentID;
extern int AiScript_Run(void);

static FILE *out;
static unsigned yield_every, calls;

static void on_rand(void *caller)
{
    fprintf(out, "R %p\n", caller);
}

static void close_trace(void)
{
    if (out) fclose(out);
    out = NULL;
}

static void init(void)
{
    const char *path = getenv("MEMORIES_AI_TRACE"), *yield = getenv("MEMORIES_AI_YIELD");
    if (yield) yield_every = (unsigned)strtoul(yield, NULL, 10);
    if (!path || !*path) return;
    out = fopen(path, "w");
    if (!out) return;
    setvbuf(out, NULL, _IOFBF, 1 << 20);
    fprintf(out, "A %p Memories_Rand\n", (void *)Memories_Rand);
    Memories_RandHook = on_rand;
    atexit(close_trace);
}

void AiTrace_Frame(unsigned frame)
{
    static int started;
    if (!started) {
        started = 1;
        init();
    }
    if (out) fprintf(out, "F %u\n", frame);
}

int AiTrace_VSync1(int elapsed, void *caller)
{
    /* The interpreter's own query only: the port's wait loops ask too. No
     * other game function calls VSync(1), so the functions linked after
     * AiScript_Run cannot be mistaken for it. */
    if ((char *)caller < (char *)AiScript_Run || (char *)caller >= (char *)AiScript_Run + 0x400) return elapsed;
    if (out) {
        fprintf(out, "V %ld %p %d\n", (long)(gAiScript_State.previous_cursor - gAiScript_State.script_base),
                (void *)gAiScript_State.script_base, gDuel_bOpponentID);
    }
    if (yield_every && ++calls % yield_every == 0 && elapsed < 263) return 263;
    return elapsed;
}
