#include "image.h"
#include "state.h"
#include "pc/platform/platform.h"
#include "pc/debug/log.h"
#include <stdio.h>

extern int Main_Init(void);

/* GCC's MIPS `main` prologue hook; nothing to construct natively. */
void Psx___main(void)
{
}

int main(int argc, char **argv)
{
    const char *exe = argc > 1 ? argv[1] : "game/SLUS_014.11";
    Log_Init();
    /* Guest globals are linked at fixed addresses: map before touching any. */
    if (Memories_GuestMap() != 0 || Memories_GuestLoadExe(exe) != 0 || Memories_ModulesInit() != 0) {
        return 1;
    }
    if (Platform_Open("Yu-Gi-Oh! Forbidden Memories (native port, work in progress)") != 0) {
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* The game runs on a stack at a fixed address; see state.h. */
    return Memories_StateRunGame(Main_Init);
}
