#include "image.h"
#include "state.h"
#include "pc/platform/platform.h"
#include "pc/debug/log.h"
#include "pc/debug/symbols.h"
#include "pc/debug/crash.h"
#include "pc/mods/exports.h"
#include "pc/platform/game_files.h"
#include "pc/cards/cards.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef _WIN32
#include "pc/platform/win32.h"
#include <fcntl.h>

int Platform_RestartGame(void)
{
    return Win32_Restart();
}
#else
#include <signal.h>
#include <sys/time.h>
#endif

static char **launch_argv;

#ifndef _WIN32
int Platform_RestartGame(void)
{
    struct itimerval stopped = {0}, previous;
    struct sigaction ignored = {0}, old_action;
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    sigaction(SIGALRM, &ignored, &old_action);
    setitimer(ITIMER_REAL, &stopped, &previous);
    /* A restart must boot the game, not auto-load an old launch state. */
    unsetenv("MEMORIES_LOAD_STATE");
    execv("/proc/self/exe", launch_argv);
    perror("memories-pc: restart");
    sigaction(SIGALRM, &old_action, NULL);
    setitimer(ITIMER_REAL, &previous, NULL);
    return -1;
}
#endif

extern int Main_Init(void);

/* The game's executable: the file named on the command line (development),
 * or SLUS_014.11 read out of the player's disc image. */
static int load_game(const char *named)
{
    char why[768];
    const char *disc;
    unsigned char *data;
    size_t size = 0;
    int result;
    if (named) return Memories_GuestLoadExe(named);
    disc = GameFiles_Disc(why, sizeof(why));
    if (!disc) {
        Platform_ShowError("Yu-Gi-Oh! Forbidden Memories", why);
        return -1;
    }
    data = GameFiles_ReadExecutable(disc, &size);
    if (!data) {
        snprintf(why, sizeof(why), "Could not read the game's executable from %s.", disc);
        Platform_ShowError("Yu-Gi-Oh! Forbidden Memories", why);
        return -1;
    }
    result = Memories_GuestLoadExeData(data, size, disc);
    free(data);
    return result;
}

/* GCC's MIPS `main` prologue hook; nothing to construct natively. */
void Psx___main(void)
{
}

int main(int argc, char **argv)
{
    const char *exe = argc > 1 ? argv[1] : NULL;
    launch_argv = argv;
    if (getenv("MEMORIES_MOD_EXPORTS")) {
        return Mods_PrintExports();   /* tools/pc/check_mod_exports.py */
    }
#ifdef _WIN32
    _set_fmode(_O_BINARY); /* disc images and states: no newline translation */
#endif
    Log_Init();
    Symbols_Load();
    Crash_Init();
    /* Guest globals are linked at fixed addresses: map before touching any. */
    if (Memories_GuestMap() != 0 || load_game(exe) != 0 || Memories_ModulesInit() != 0) {
        return 1;
    }
    if (Platform_Open("Yu-Gi-Oh! Forbidden Memories (native port, work in progress)") != 0) {
        return 1;
    }
    /* The mods are applied by now (Platform_Open reads the settings), and
     * the executable is in place: the cards they add come after its own. */
    Cards_Build();
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* The game runs on a stack at a fixed address; see state.h. */
    return Memories_StateRunGame(Main_Init);
}
