#define _POSIX_C_SOURCE 200809L
#include "pc/debug/log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned Memories_PresentedFrames(void) { return 12; }
unsigned Platform_VBlankCount(void) { return 34; }

int main(void)
{
    char path[] = "/tmp/memories-log-XXXXXX", line[1024];
    int fd = mkstemp(path), count = 0, dropped = 0, i;
    FILE *file;
    assert(fd >= 0);
    close(fd);
    assert(!setenv("MEMORIES_LOG", path, 1));
    Log_Init();
    Log_Enable(LOG_DISC, 1);
    for (i = 0; i < 300; i++) Log_Signal(LOG_DISC, "record %ld", i, 0, 0, 0, 0, 0);
    Log_Drain();
    file = fopen(path, "r");
    assert(file);
    while (fgets(line, sizeof(line), file)) {
        count++;
        if (strstr(line, "dropped 44 records")) dropped = 1;
    }
    fclose(file);
    assert(count == 257);
    assert(dropped);
    unlink(path);
    return 0;
}
