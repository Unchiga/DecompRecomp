/* The clock hold the controls runtime takes (pc/compat/signal.h) is answered
 * by platform/win32.c in the game executable, which brings the timer thread,
 * the exception handler and the guest image with it. A unit test only needs
 * the mask to behave, so this keeps the flag on its own. */
#include "pc/compat/signal.h"

static unsigned long held;

int Memories_SigProcMask(int how, const sigset_t *set, sigset_t *previous)
{
    unsigned long bit = 1ul << SIGALRM;
    if (previous) *previous = held;
    if (set) {
        if (how == SIG_SETMASK) held = *set & bit;
        else if (*set & bit) held = how == SIG_BLOCK ? bit : 0;
    }
    return 0;
}
