#ifndef MEMORIES_PC_COMPAT_SIGNAL_H
#define MEMORIES_PC_COMPAT_SIGNAL_H
/* <signal.h> for the port's one use of signal masks: holding off the game
 * clock (SIGALRM, see platform_common.c) around code that shares state with
 * it. Windows has no signals; its clock interrupts the main thread from a
 * timer thread (platform/win32_interrupt.c) and skips a tick while the main
 * thread holds SIGALRM through these calls. Other threads are never
 * interrupted, so their masks do not matter and are not kept. */
#include <signal.h>
#ifdef _WIN32
#include <pthread.h>

#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

typedef unsigned long memories_sigset_t;
#define sigset_t memories_sigset_t
#define sigemptyset(set) (*(set) = 0, 0)
#define sigfillset(set) (*(set) = ~0ul, 0)
#define sigaddset(set, number) (*(set) |= 1ul << (number), 0)
#define sigdelset(set, number) (*(set) &= ~(1ul << (number)), 0)
#define sigismember(set, number) ((*(set) >> (number)) & 1)

int Memories_SigProcMask(int how, const sigset_t *set, sigset_t *previous);
#define sigprocmask Memories_SigProcMask
#undef pthread_sigmask
#define pthread_sigmask Memories_SigProcMask
#endif
#endif
