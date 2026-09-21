#define _GNU_SOURCE
#include "platform.h"
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PERIOD 256 /* 5.8 ms: key-ons land on mix periods, so keep them well under a sequencer tick */

static void (*mixer)(int16_t *, size_t);
static snd_pcm_t *pcm;

static void *run(void *unused)
{
    static int16_t buffer[PERIOD * 2];
    (void)unused;
    for (;;) {
        snd_pcm_sframes_t written;
        mixer(buffer, PERIOD);
        written = snd_pcm_writei(pcm, buffer, PERIOD);
        if (written < 0 && snd_pcm_recover(pcm, (int)written, 1) < 0) {
            fprintf(stderr, "memories-pc: audio output stopped: %s\n", snd_strerror((int)written));
            return NULL;
        }
    }
}

int Platform_StartAudio(void (*mix)(int16_t *, size_t))
{
    pthread_t thread;
    sigset_t all, previous;
    int error;
    const char *dump = getenv("MEMORIES_DUMP_AUDIO");
    int silent = dump || getenv("MEMORIES_NO_AUDIO") || getenv("MEMORIES_HEADLESS");
    if (!silent) {
        error = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (error >= 0) {
            error = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 44100, 1,
                                       40000);
        }
        if (error < 0) {
            fprintf(stderr, "memories-pc: no audio device (%s); continuing silently\n", snd_strerror(error));
            silent = 1;
        }
    }
    mixer = mix;
    /* The timer signal is the game's interrupt and must stay on the main thread. */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &previous);
    error = silent ? Platform_StartSilentAudio(mix, dump) : pthread_create(&thread, NULL, run, NULL);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return error ? -1 : 0;
}
