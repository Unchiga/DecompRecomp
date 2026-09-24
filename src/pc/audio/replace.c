/* Audio replacement (replace.h): a mod's WAV and Ogg Vorbis files in place
 * of the game's songs, XA clips and sound effects.
 *
 * How the three threads meet, without a lock anywhere:
 *
 *  - Every replacement is a Sound: its decoded clip, loop and gain. A Sound
 *    never changes after it is made, and it is freed only by Unload, with
 *    the game clock held off (block_clock) and the mixer held out of the
 *    channels (hold_mixer), after every reference to it has been dropped.
 *  - The table of Sounds by id is read by the sound driver (on the VBlank
 *    interrupt, or the game thread) and written only by Load/Unload on the
 *    game thread with the clock held off, so a reader never sees it half
 *    changed.
 *  - Each mixer channel has a request slot: the Sound to start (NULL stops),
 *    a packed gain, and a serial the poster bumps last. The mixer starts
 *    whatever the slot holds when the serial moves. Each word is written
 *    whole, so a poster cannot tear it, and the sound-effect slots are
 *    handed out by an atomic counter, so two posters never share one. */
#define _POSIX_C_SOURCE 200809L
#include "pc/compat/fs.h"
#include "replace.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/debug/log.h"
#include "pc/compat/signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "pc/third_party/stb_vorbis.c" /* the declarations; vorbis.c compiles the decoder */

#define PATH_MAX_ 1024
#define FILE_MAX (256u << 20)  /* the largest file read */
#define GAIN_ONE 4096          /* Q12 */
#define VOLUME_MAX 400         /* percent */

/* --- decoding ---------------------------------------------------------- */

static void say_error(char *error, size_t size, const char *text)
{
    if (error && size) snprintf(error, size, "%s", text);
}

static uint32_t le16(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | le16(p + 2) << 16; }

/* Where a channel plays: the bit numbers of WAVE_FORMAT_EXTENSIBLE's
 * channel mask, which the decoders translate their own orders into. */
enum {
    FL, FR, FC, LFE, BL, BR, FLC, FRC, BC, SL, SR, TC, TFL, TFC, TFR, TBL, TBC, TBR,
    SPEAKERS, UNPLACED = SPEAKERS /* past a layout's end: both sides, as a centre */
};

/* Each speaker's share of the left and right output, Q12: the front pair
 * whole, a centre at -3 dB on both sides, surrounds, backs and heights at
 * -3 dB on their own side, the low-frequency channel left out (a
 * downmix's usual weights). */
static const int16_t fold_gain[SPEAKERS + 1][2] = {
    [FL] = {4096, 0},    [FR] = {0, 4096},    [FC] = {2896, 2896}, [LFE] = {0, 0},
    [BL] = {2896, 0},    [BR] = {0, 2896},    [FLC] = {3784, 1567}, [FRC] = {1567, 3784},
    [BC] = {2896, 2896}, [SL] = {2896, 0},    [SR] = {0, 2896},    [TC] = {2896, 2896},
    [TFL] = {2896, 0},   [TFC] = {2896, 2896}, [TFR] = {0, 2896},  [TBL] = {2896, 0},
    [TBC] = {2896, 2896}, [TBR] = {0, 2896},  [UNPLACED] = {2896, 2896},
};

int AudioReplace_Convert(const int16_t *samples, size_t frames, int channels, const unsigned char *speakers,
                         unsigned rate, AudioClip *clip)
{
    uint64_t out_frames;
    int16_t *out;
    int32_t weight[2][32];
    int64_t scale = GAIN_ONE;
    uint32_t i;
    int c, side;
    memset(clip, 0, sizeof(*clip));
    if (channels < 1 || channels > 32 || !rate || !frames) return -1;
    /* Mono plays on both sides; stereo, and a wider source without speakers
     * named, as if its first two channels were the front pair. Wider
     * sources fold to the pair, scaled down so that every channel at full
     * level together cannot clip. */
    for (c = 0; c < channels; c++) {
        int speaker = channels == 1 ? FC : speakers ? speakers[c] : c < 2 ? c : UNPLACED;
        if (speaker > UNPLACED) speaker = UNPLACED;
        for (side = 0; side < 2; side++) weight[side][c] = channels == 1 ? GAIN_ONE : fold_gain[speaker][side];
    }
    for (side = 0; side < 2; side++) {
        int64_t sum = 0;
        for (c = 0; c < channels; c++) sum += weight[side][c];
        if (sum > scale) scale = sum;
    }
    out_frames = ((uint64_t)frames * AUDIO_RATE + rate - 1) / rate;
    if (out_frames > AUDIO_CLIP_MAX_FRAMES) return -1;
    out = malloc((size_t)out_frames * 2 * sizeof(*out));
    if (!out) return -1;
    for (i = 0; i < (uint32_t)out_frames; i++) {
        /* Source position i * rate / 44100, as a frame and a fraction. */
        uint64_t position = (uint64_t)i * rate;
        size_t at = (size_t)(position / AUDIO_RATE), next;
        int64_t fraction = (int64_t)(position % AUDIO_RATE);
        if (at >= frames) at = frames - 1;
        next = at + 1 < frames ? at + 1 : at;
        for (side = 0; side < 2; side++) {
            int64_t a = 0, b = 0;
            for (c = 0; c < channels; c++) {
                a += (int64_t)samples[at * (size_t)channels + (size_t)c] * weight[side][c];
                b += (int64_t)samples[next * (size_t)channels + (size_t)c] * weight[side][c];
            }
            a /= scale;
            b /= scale;
            out[(size_t)i * 2 + (size_t)side] = (int16_t)(a + (b - a) * fraction / AUDIO_RATE);
        }
    }
    clip->frames = out;
    clip->count = (uint32_t)out_frames;
    return 0;
}

/* A WAV's speakers: the channel mask's, lowest bit first, when the file
 * has one, else the layouts WAVE_FORMAT_EXTENSIBLE takes as the default
 * for the count (L R C LFE, then backs, then sides). Channels past them
 * are unplaced. */
static void wav_speakers(int channels, uint32_t mask, unsigned char *speakers)
{
    static const unsigned char defaults[9][8] = {
        [3] = {FL, FR, FC},
        [4] = {FL, FR, BL, BR},
        [5] = {FL, FR, FC, BL, BR},
        [6] = {FL, FR, FC, LFE, BL, BR},
        [7] = {FL, FR, FC, LFE, BC, SL, SR},
        [8] = {FL, FR, FC, LFE, BL, BR, SL, SR},
    };
    int c, bit = 0;
    for (c = 0; c < channels; c++) {
        if (mask) {
            while (bit < SPEAKERS && !(mask & (1u << bit))) bit++;
            speakers[c] = bit < SPEAKERS ? (unsigned char)bit++ : UNPLACED;
        } else {
            speakers[c] = channels < 3 ? (unsigned char)c : channels <= 8 ? defaults[channels][c] : c < 8 ? defaults[8][c] : UNPLACED;
        }
    }
}

/* A Vorbis stream's speakers, in the order its specification fixes for one
 * to eight channels (the centre between the front pair, the LFE last);
 * past eight the order is the application's, so they are unplaced. */
static void vorbis_speakers(int channels, unsigned char *speakers)
{
    static const unsigned char orders[9][8] = {
        [1] = {FC},
        [2] = {FL, FR},
        [3] = {FL, FC, FR},
        [4] = {FL, FR, BL, BR},
        [5] = {FL, FC, FR, BL, BR},
        [6] = {FL, FC, FR, BL, BR, LFE},
        [7] = {FL, FC, FR, SL, SR, BC, LFE},
        [8] = {FL, FC, FR, SL, SR, BL, BR, LFE},
    };
    int c;
    for (c = 0; c < channels; c++) speakers[c] = channels <= 8 ? orders[channels][c] : UNPLACED;
}

/* One sample of a WAV data chunk as s16. */
static int16_t wav_sample(const unsigned char *p, int format, int bits)
{
    if (format == 3) {
        double value;
        if (bits == 32) {
            union { uint32_t word; float value; } u;
            u.word = le32(p);
            value = u.value;
        } else {
            union { uint64_t word; double value; } u;
            u.word = (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
            value = u.value;
        }
        if (!(value == value)) value = 0; /* NaN */
        value *= 32767.0;
        return (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
    }
    switch (bits) {
    case 8: return (int16_t)(((int)p[0] - 128) << 8);
    case 16: return (int16_t)le16(p);
    case 24: return (int16_t)(le16(p + 1));
    default: return (int16_t)(le16(p + 2)); /* 32 */
    }
}

static int decode_wav(const unsigned char *data, size_t size, AudioClip *clip, char *error, size_t error_size)
{
    size_t at = 12, data_at = 0, data_size = 0, frames, i;
    int format = 0, channels = 0, bits = 0, have_format = 0, bytes, block;
    unsigned rate = 0;
    uint32_t mask = 0;
    unsigned char speakers[32];
    int16_t *samples;
    int result;
    while (at + 8 <= size) {
        uint32_t length = le32(data + at + 4);
        const unsigned char *body = data + at + 8;
        size_t room = size - at - 8;
        if (!memcmp(data + at, "fmt ", 4) && length >= 16 && length <= room) {
            format = (int)le16(body);
            channels = (int)le16(body + 2);
            rate = le32(body + 4);
            bits = (int)le16(body + 14);
            if (format == 0xFFFE && length >= 26) { /* WAVE_FORMAT_EXTENSIBLE */
                mask = le32(body + 20);
                format = (int)le16(body + 24);
            }
            have_format = 1;
        } else if (!memcmp(data + at, "data", 4)) {
            data_at = at + 8;
            data_size = length > room ? room : length; /* a streamed file may not know its length */
            if (have_format) break;
        }
        if (length > room) break;
        at += 8 + (size_t)length + (length & 1);
    }
    if (!have_format || !data_at) {
        say_error(error, error_size, "is not a WAV file with a format and data");
        return -1;
    }
    if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (format == 3 && (bits == 32 || bits == 64)))) {
        char text[96];
        snprintf(text, sizeof(text), "is a WAV of format %d, %d bits; PCM 8/16/24/32 or float 32/64 plays", format, bits);
        say_error(error, error_size, text);
        return -1;
    }
    if (channels < 1 || channels > 32 || rate < 1000 || rate > 768000) {
        say_error(error, error_size, "has an unusable channel count or sample rate");
        return -1;
    }
    bytes = bits / 8;
    block = bytes * channels;
    frames = data_size / (size_t)block;
    if (!frames) {
        say_error(error, error_size, "holds no samples");
        return -1;
    }
    if ((uint64_t)frames * AUDIO_RATE / rate > AUDIO_CLIP_MAX_FRAMES) {
        say_error(error, error_size, "is longer than 12 minutes");
        return -1;
    }
    samples = malloc(frames * (size_t)channels * sizeof(*samples));
    if (!samples) {
        say_error(error, error_size, "does not fit in memory");
        return -1;
    }
    for (i = 0; i < frames * (size_t)channels; i++) {
        samples[i] = wav_sample(data + data_at + i * (size_t)bytes, format, bits);
    }
    wav_speakers(channels, mask, speakers);
    result = AudioReplace_Convert(samples, frames, channels, speakers, rate, clip);
    free(samples);
    if (result) say_error(error, error_size, "does not fit in memory");
    return result;
}

/* The stream's length is asked first, from its last page, so an overlong
 * file is refused before it is decoded; one that does not say (or lies) is
 * stopped where the limit falls. */
static int decode_vorbis(const unsigned char *data, size_t size, AudioClip *clip, char *error, size_t error_size)
{
    int channels, rate, status = 0, result;
    unsigned length;
    size_t frames = 0, room = 4096, limit;
    short *samples, *more;
    unsigned char speakers[32];
    stb_vorbis *stream;
    stb_vorbis_info info;
    if (size > 0x7fffffff) return -1;
    stream = stb_vorbis_open_memory(data, (int)size, &status, NULL);
    if (!stream) {
        say_error(error, error_size, "is not a playable Ogg Vorbis file");
        return -1;
    }
    info = stb_vorbis_get_info(stream);
    channels = info.channels;
    rate = (int)info.sample_rate;
    if (channels < 1 || channels > 32 || rate <= 0) {
        stb_vorbis_close(stream);
        say_error(error, error_size, "is not a playable Ogg Vorbis file");
        return -1;
    }
    limit = (size_t)((uint64_t)AUDIO_CLIP_MAX_FRAMES * (unsigned)rate / AUDIO_RATE) + 1;
    length = stb_vorbis_stream_length_in_samples(stream);
    if (length == 0xffffffffu) length = 0; /* no last page to read it from */
    if (length > limit) {
        stb_vorbis_close(stream);
        say_error(error, error_size, "is longer than 12 minutes");
        return -1;
    }
    if (length) room = (size_t)length + 4096; /* a frame's worth past it: the last frame may overrun */
    samples = malloc(room * (size_t)channels * sizeof(*samples));
    for (;;) {
        int got;
        if (!samples) {
            stb_vorbis_close(stream);
            say_error(error, error_size, "does not fit in memory");
            return -1;
        }
        if (room - frames < 4096) {
            room *= 2;
            more = realloc(samples, room * (size_t)channels * sizeof(*samples));
            if (!more) free(samples);
            samples = more;
            continue;
        }
        got = stb_vorbis_get_frame_short_interleaved(stream, channels, samples + frames * (size_t)channels,
                                                     (int)((room - frames) * (size_t)channels));
        if (got <= 0) break;
        frames += (size_t)got;
        if (frames > limit) {
            free(samples);
            stb_vorbis_close(stream);
            say_error(error, error_size, "is longer than 12 minutes");
            return -1;
        }
    }
    stb_vorbis_close(stream);
    if (!frames) {
        free(samples);
        say_error(error, error_size, "is not a playable Ogg Vorbis file");
        return -1;
    }
    vorbis_speakers(channels, speakers);
    result = AudioReplace_Convert(samples, frames, channels, speakers, (unsigned)rate, clip);
    free(samples);
    if (result) say_error(error, error_size, "is too long or does not fit in memory");
    return result;
}

int AudioReplace_Decode(const unsigned char *data, size_t size, AudioClip *clip, char *error, size_t error_size)
{
    memset(clip, 0, sizeof(*clip));
    if (size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4))
        return decode_wav(data, size, clip, error, error_size);
    if (size >= 4 && !memcmp(data, "OggS", 4)) return decode_vorbis(data, size, clip, error, error_size);
    say_error(error, error_size, "is neither WAV nor Ogg Vorbis");
    return -1;
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data = NULL;
    long length;
    if (!file) return NULL;
    if (!fseek(file, 0, SEEK_END) && (length = ftell(file)) > 0 && (unsigned long)length <= FILE_MAX &&
        !fseek(file, 0, SEEK_SET) && (data = malloc((size_t)length))) {
        if (fread(data, 1, (size_t)length, file) != (size_t)length) {
            free(data);
            data = NULL;
        } else {
            *size = (size_t)length;
        }
    }
    fclose(file);
    return data;
}

/* --- the table ----------------------------------------------------------- */

typedef struct Sound {
    AudioClip clip;
    int kind, id, mod;
    int loop;
    uint32_t loop_start;
    int gain;               /* Q12, from "volume" */
    const char *mod_id;     /* the mod's, alive until exit */
} Sound;

static Sound **table;       /* in the order they were added: the last match wins */
static int table_count;

static const char *const kind_names[AUDIO_KINDS] = {"music", "xa", "sfx"};

static const Sound *find(int kind, int id)
{
    int i;
    for (i = table_count - 1; i >= 0; i--) {
        if (table[i]->kind == kind && table[i]->id == id) return table[i];
    }
    return NULL;
}

int AudioReplace_Count(AudioKind kind)
{
    int i, j, count = 0;
    for (i = 0; i < table_count; i++) {
        if ((int)table[i]->kind != (int)kind) continue;
        for (j = i + 1; j < table_count; j++) {
            if (table[j]->kind == table[i]->kind && table[j]->id == table[i]->id) break;
        }
        count += j == table_count;  /* each id once */
    }
    return count;
}

int AudioReplace_Owner(AudioKind kind, int id)
{
    const Sound *sound = find((int)kind, id);
    return sound ? sound->mod : -1;
}

long AudioReplace_ParseId(const char *text)
{
    char *end;
    long value;
    if (!text || !*text) return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        if (!text[2]) return -1;
        value = strtol(text + 2, &end, 16);
    } else {
        value = strtol(text, &end, 10);
    }
    if (*end || value < 0 || value > 0xFFFF || text[0] == '-' || text[0] == '+' || text[0] == ' ') return -1;
    return value;
}

/* The game clock (SIGALRM, or the Windows timer thread through the same
 * call) is the only other reader or writer of the table and of the
 * driver-side state below; holding it off makes them the caller's. */
static void block_clock(int on, sigset_t *previous)
{
    sigset_t set;
    if (on) {
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_BLOCK, &set, previous);
    } else {
        sigprocmask(SIG_SETMASK, previous, NULL);
    }
}

/* --- channels and requests ---------------------------------------------- */

enum { CHANNEL_MUSIC, CHANNEL_XA, CHANNEL_SFX, SFX_CHANNELS = 8, CHANNELS = CHANNEL_SFX + SFX_CHANNELS };

typedef struct {
    const Sound *volatile sound;   /* to start; NULL stops */
    volatile uint32_t gain;        /* left | right << 16, Q12; sound effects only */
    volatile unsigned serial;      /* bumped last */
} Request;

typedef struct {
    const Sound *sound;            /* playing, or NULL */
    uint32_t position;
    int gain_left, gain_right;     /* the request's, Q12 */
    unsigned seen;
} Channel;

static Request requests[CHANNELS];
static Channel channels[CHANNELS]; /* the mixer's */
static volatile unsigned sfx_next;
static volatile uint32_t sfx_busy;       /* written by the mixer: which sfx channels play */
static volatile int music_level[2] = {GAIN_ONE, GAIN_ONE}; /* Q12 */
static volatile int music_muted;
static volatile int mixer_hold, mixer_busy;

/* The driver's side: the song and XA clip in hand. */
static int music_id = -1;
static const Sound *music_sound;
static const Sound *xa_sound;
static int xa_mute_lba, xa_mute_end;     /* the replaced clip's sectors, while xa_mute_on */
static volatile int xa_mute_on;

static void post(int channel, const Sound *sound, uint32_t gain)
{
    Request *request = &requests[channel];
    request->gain = gain;
    request->sound = sound;
    __atomic_fetch_add(&request->serial, 1, __ATOMIC_SEQ_CST);
}

/* Wait until the mixer is outside the channels, and keep it out. */
static void hold_mixer(int on)
{
    struct timespec nap = {0, 200000};
    __atomic_store_n(&mixer_hold, on, __ATOMIC_SEQ_CST);
    while (on && __atomic_load_n(&mixer_busy, __ATOMIC_SEQ_CST)) nanosleep(&nap, NULL);
}

/* The trace names the replacing mod by its id, a string that lives until
 * exit; Log_Signal carries it as a long, which holds a pointer on every
 * system the port builds for. */
#define TRACE(format, id, sound)                                                                   \
    do {                                                                                           \
        if (!(sound)) Log_Signal(LOG_MODS, "audio: " format, (long)(id), 0, 0, 0, 0, 0);            \
        else if (sizeof(long) >= sizeof(const char *))                                             \
            Log_Signal(LOG_MODS, "audio: " format " (replaced by %s)", (long)(id),                   \
                       (long)(intptr_t)(sound)->mod_id, 0, 0, 0, 0);                               \
        else Log_Signal(LOG_MODS, "audio: " format " (replaced by mod %ld)", (long)(id),             \
                        (long)(sound)->mod, 0, 0, 0, 0);                                           \
    } while (0)

static void music_start(int id, int traced)
{
    const Sound *sound = find(AUDIO_MUSIC, id);
    music_id = id;
    if (traced) TRACE("music 0x%lX starts", id, sound);
    music_sound = sound;
    post(CHANNEL_MUSIC, sound, 0);
    music_muted = sound != NULL;
}

void AudioReplace_MusicStart(int id)
{
    music_start(id & 0xFFFF, 1);
}

void AudioReplace_MusicStop(void)
{
    if (music_id < 0 && !music_sound) return;
    Log_Signal(LOG_MODS, "audio: music 0x%lX stops", (long)music_id, 0, 0, 0, 0, 0);
    music_id = -1;
    music_sound = NULL;
    music_muted = 0;
    post(CHANNEL_MUSIC, NULL, 0);
}

void AudioReplace_MusicLevel(int left, int right, int full)
{
    if (full <= 0) full = 127;
    left = left < 0 ? 0 : left > 2 * full ? 2 * full : left;
    right = right < 0 ? 0 : right > 2 * full ? 2 * full : right;
    music_level[0] = left * GAIN_ONE / full;
    music_level[1] = right * GAIN_ONE / full;
}

int AudioReplace_Sfx(int id, int volume, int pan)
{
    static int last_id = -1;
    static unsigned repeats;
    const Sound *sound = find(AUDIO_SFX, id & 0xFFFF);
    int left, right, tries, channel = 0;
    /* Some screens start one sound every frame: the trace says so every 60th time. */
    if ((id & 0xFFFF) != last_id || ++repeats % 60 == 0) {
        if ((id & 0xFFFF) != last_id) repeats = 0;
        last_id = id & 0xFFFF;
        TRACE("sfx 0x%lX plays", id & 0xFFFF, sound);
    }
    if (!sound) return 0;
    /* The driver's own pan law (func_8004803C): one side falls off
     * linearly as the sound moves to the other. */
    volume = volume < 0 ? 0 : volume > 255 ? 255 : volume;
    left = right = volume * GAIN_ONE / 255;
    if (pan > 0 && pan <= 128) left = left * (128 - pan) / 128;
    if (pan < 0 && pan >= -128) right = right * (pan + 128) / 128;
    /* A free channel if there is one, else the oldest-started. */
    for (tries = 0; tries < SFX_CHANNELS; tries++) {
        channel = (int)(__atomic_fetch_add(&sfx_next, 1, __ATOMIC_SEQ_CST) % SFX_CHANNELS);
        if (!(__atomic_load_n(&sfx_busy, __ATOMIC_SEQ_CST) >> channel & 1)) break;
    }
    post(CHANNEL_SFX + channel, sound, (uint32_t)left | (uint32_t)right << 16);
    return 1;
}

void AudioReplace_StopSfx(void)
{
    int i;
    if (!__atomic_load_n(&sfx_busy, __ATOMIC_SEQ_CST)) {
        for (i = 0; i < SFX_CHANNELS; i++) if (requests[CHANNEL_SFX + i].sound) break;
        if (i == SFX_CHANNELS) return;
    }
    for (i = 0; i < SFX_CHANNELS; i++) post(CHANNEL_SFX + i, NULL, 0);
}

void AudioReplace_XaStart(int id, int lba, int sectors)
{
    const Sound *sound = find(AUDIO_XA, id & 0xFFFF);
    TRACE("xa 0x%lX starts", id & 0xFFFF, sound);
    xa_mute_on = 0;
    xa_mute_lba = lba;
    xa_mute_end = lba + (sectors > 0 ? sectors : 0);
    xa_sound = sound;
    xa_mute_on = sound != NULL;
    post(CHANNEL_XA, sound, 0);
}

void AudioReplace_XaStop(void)
{
    if (!xa_sound && !xa_mute_on) return;
    Log_Signal(LOG_MODS, "audio: xa stops", 0, 0, 0, 0, 0, 0);
    xa_sound = NULL;
    xa_mute_on = 0;
    post(CHANNEL_XA, NULL, 0);
}

int AudioReplace_XaSectorMuted(int lba)
{
    return xa_mute_on && lba >= xa_mute_lba && lba < xa_mute_end;
}

/* Every channel silent and every request dropped, with the mixer held. */
static void stop_all(void)
{
    int i;
    for (i = 0; i < CHANNELS; i++) {
        requests[i].sound = NULL;
        channels[i].sound = NULL;
        channels[i].seen = requests[i].serial;
    }
    sfx_busy = 0;
}

void AudioReplace_Reset(int id)
{
    sigset_t previous;
    block_clock(1, &previous);
    hold_mixer(1);
    stop_all();
    hold_mixer(0);
    music_id = -1;
    music_sound = xa_sound = NULL;
    music_muted = 0;
    xa_mute_on = 0;
    if (id >= 0) music_start(id & 0xFFFF, 0);
    block_clock(0, &previous);
}

/* --- the manifest -------------------------------------------------------- */

static void free_sound(Sound *sound)
{
    if (!sound) return;
    free(sound->clip.frames);
    free(sound);
}

/* After the table changed: the song in hand follows it, so a mod applied or
 * removed mid-song is heard at once. An XA clip already playing keeps what
 * it started with. Clock held. */
static void follow_table(void)
{
    if (music_id >= 0 && find(AUDIO_MUSIC, music_id) != music_sound) music_start(music_id, 0);
}

static Sound *load_entry(const char *directory, int kind, const JsonValue *value, const char *key,
                         char *error, size_t error_size)
{
    const char *file = NULL;
    char path[PATH_MAX_], why[160];
    long volume = 100, loop_start = 0;
    int loop = kind == AUDIO_MUSIC;
    unsigned char *data;
    size_t size = 0;
    Sound *sound;
    if (Json_TypeOf(value) == JSON_STRING) {
        file = Json_String(value, NULL);
    } else if (Json_TypeOf(value) == JSON_OBJECT) {
        file = Json_String(Json_Member(value, "file"), NULL);
        loop = Json_Bool(Json_Member(value, "loop"), loop);
        loop_start = Json_Number(Json_Member(value, "loop_start"), 0);
        volume = Json_Number(Json_Member(value, "volume"), 100);
    }
    if (!file || !*file) {
        snprintf(error, error_size, "audio %s %s: needs a file name, or {\"file\": ...}", kind_names[kind], key);
        return NULL;
    }
    if (!Paths_Contained(file)) {
        snprintf(error, error_size, "audio %s %s: %s is outside the mod", kind_names[kind], key, file);
        return NULL;
    }
    if (volume < 0 || volume > VOLUME_MAX || loop_start < 0) {
        snprintf(error, error_size, "audio %s %s: volume is 0-%d, loop_start not negative", kind_names[kind], key,
                 VOLUME_MAX);
        return NULL;
    }
    if (snprintf(path, sizeof(path), "%s/%s", directory, file) >= (int)sizeof(path) ||
        !(data = read_file(path, &size))) {
        snprintf(error, error_size, "audio: cannot read %s", file);
        return NULL;
    }
    sound = calloc(1, sizeof(*sound));
    if (!sound) {
        free(data);
        snprintf(error, error_size, "audio: out of memory for %s", file);
        return NULL;
    }
    if (AudioReplace_Decode(data, size, &sound->clip, why, sizeof(why))) {
        free(data);
        free(sound);
        snprintf(error, error_size, "audio: %s %s", file, why);
        return NULL;
    }
    free(data);
    sound->kind = kind;
    sound->loop = loop;
    sound->loop_start = (uint32_t)loop_start < sound->clip.count ? (uint32_t)loop_start : 0;
    sound->gain = (int)(volume * GAIN_ONE / 100);
    return sound;
}

int AudioReplace_Load(int mod, const char *mod_id, const char *directory, const JsonValue *audio,
                      char *error, size_t error_size)
{
    Sound **added = NULL;
    int added_count = 0, kind, failures = 0;
    char why[256];
    sigset_t previous;
    if (error && error_size) error[0] = '\0';
    if (!audio) return 0;
    if (Json_TypeOf(audio) != JSON_OBJECT) {
        say_error(error, error_size, "\"audio\" is not an object");
        return -1;
    }
    for (kind = 0; kind < AUDIO_KINDS; kind++) {
        const JsonValue *group = Json_Member(audio, kind_names[kind]);
        int i, count = Json_Count(group);
        if (group && Json_TypeOf(group) != JSON_OBJECT) {
            snprintf(why, sizeof(why), "\"audio\": \"%s\" is not an object", kind_names[kind]);
            if (!failures++) say_error(error, error_size, why);
            continue;
        }
        for (i = 0; i < count; i++) {
            const JsonValue *member = Json_At(group, i);
            const char *key = Json_Name(member);
            long id = AudioReplace_ParseId(key);
            Sound *sound, **grown;
            if (id < 0) {
                snprintf(why, sizeof(why), "audio %s: \"%s\" is not an id (0x2D0 or 720)", kind_names[kind],
                         key ? key : "");
                if (!failures++) say_error(error, error_size, why);
                continue;
            }
            sound = load_entry(directory, kind, member, key, why, sizeof(why));
            if (!sound) {
                if (!failures++) say_error(error, error_size, why);
                continue;
            }
            grown = realloc(added, (size_t)(added_count + 1) * sizeof(*added));
            if (!grown) {
                free_sound(sound);
                if (!failures++) say_error(error, error_size, "audio: out of memory");
                continue;
            }
            added = grown;
            sound->id = (int)id;
            sound->mod = mod;
            sound->mod_id = mod_id;
            added[added_count++] = sound;
        }
    }
    if (failures > 1 && error && error_size) {
        char first[256];
        snprintf(first, sizeof(first), "%s", error);
        snprintf(error, error_size, "%s (and %d more skipped)", first, failures - 1);
    }
    if (added_count) {
        Sound **grown = realloc(table, (size_t)(table_count + added_count) * sizeof(*table));
        if (!grown) {
            int i;
            for (i = 0; i < added_count; i++) free_sound(added[i]);
            free(added);
            say_error(error, error_size, "audio: out of memory");
            return 0;
        }
        /* The driver reads the table on the clock: it sees the old one or the new. */
        block_clock(1, &previous);
        table = grown;
        memcpy(table + table_count, added, (size_t)added_count * sizeof(*added));
        table_count += added_count;
        follow_table();
        block_clock(0, &previous);
    }
    free(added);
    return added_count;
}

void AudioReplace_Unload(int mod)
{
    Sound *gone[64];
    int i, kept = 0, gone_count = 0, more = 1;
    sigset_t previous;
    while (more) {
        int music_gone = 0;
        more = 0;
        gone_count = 0;
        block_clock(1, &previous);
        for (i = 0, kept = 0; i < table_count; i++) {
            if (table[i]->mod == mod && gone_count < (int)(sizeof(gone) / sizeof(gone[0]))) {
                gone[gone_count++] = table[i];
                continue;
            }
            if (table[i]->mod == mod) more = 1;
            table[kept++] = table[i];
        }
        table_count = kept;
        if (!gone_count) {
            block_clock(0, &previous);
            break;
        }
        /* Nothing may hold a Sound that is about to be freed: not a channel,
         * not a request the mixer has yet to take, not the driver's side. */
        hold_mixer(1);
        for (i = 0; i < CHANNELS; i++) {
            int g;
            for (g = 0; g < gone_count; g++) {
                if (channels[i].sound == gone[g]) channels[i].sound = NULL;
                if (requests[i].sound == gone[g]) requests[i].sound = NULL;
            }
        }
        for (i = 0; i < gone_count; i++) {
            if (music_sound == gone[i]) music_gone = 1;
            if (xa_sound == gone[i]) {
                /* The clip's own audio comes back for the rest of it. */
                xa_sound = NULL;
                xa_mute_on = 0;
            }
        }
        hold_mixer(0);
        if (music_gone) {
            /* Another mod's replacement of the song, or the game's own. */
            music_sound = NULL;
            music_muted = 0;
            if (music_id >= 0) music_start(music_id, 0);
        }
        follow_table();
        block_clock(0, &previous);
        for (i = 0; i < gone_count; i++) free_sound(gone[i]);
    }
}

/* --- the mixer ------------------------------------------------------------- */

static int mix_gain[CHANNELS][2]; /* this period's, Q12 */

int AudioReplace_MixBegin(void)
{
    int i, active = 0;
    uint32_t busy = 0;
    __atomic_store_n(&mixer_busy, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&mixer_hold, __ATOMIC_SEQ_CST)) return 0;
    for (i = 0; i < CHANNELS; i++) {
        Channel *channel = &channels[i];
        unsigned serial = __atomic_load_n(&requests[i].serial, __ATOMIC_SEQ_CST);
        if (serial != channel->seen) {
            uint32_t gain = requests[i].gain;
            channel->seen = serial;
            channel->sound = requests[i].sound;
            channel->position = 0;
            channel->gain_left = (int)(gain & 0xFFFF);
            channel->gain_right = (int)(gain >> 16);
        }
        if (!channel->sound) continue;
        if (i == CHANNEL_MUSIC) {
            mix_gain[i][0] = channel->sound->gain * music_level[0] / GAIN_ONE;
            mix_gain[i][1] = channel->sound->gain * music_level[1] / GAIN_ONE;
        } else if (i == CHANNEL_XA) {
            mix_gain[i][0] = mix_gain[i][1] = channel->sound->gain;
        } else {
            mix_gain[i][0] = channel->sound->gain * channel->gain_left / GAIN_ONE;
            mix_gain[i][1] = channel->sound->gain * channel->gain_right / GAIN_ONE;
            busy |= 1u << (i - CHANNEL_SFX);
        }
        active = 1;
    }
    __atomic_store_n(&sfx_busy, busy, __ATOMIC_SEQ_CST);
    return (active ? AUDIO_MIX_ACTIVE : 0) | (music_muted ? AUDIO_MIX_MUTE_MUSIC : 0);
}

void AudioReplace_MixFrame(int out[6])
{
    int i;
    for (i = 0; i < CHANNELS; i++) {
        Channel *channel = &channels[i];
        const Sound *sound = channel->sound;
        const int16_t *frame;
        int bus;
        if (!sound) continue;
        frame = sound->clip.frames + (size_t)channel->position * 2;
        bus = i == CHANNEL_MUSIC ? 0 : i == CHANNEL_XA ? 4 : 2;
        out[bus] += frame[0] * mix_gain[i][0] / GAIN_ONE;
        out[bus + 1] += frame[1] * mix_gain[i][1] / GAIN_ONE;
        if (++channel->position >= sound->clip.count) {
            if (sound->loop) channel->position = sound->loop_start;
            else channel->sound = NULL;
        }
    }
}

void AudioReplace_MixEnd(void)
{
    __atomic_store_n(&mixer_busy, 0, __ATOMIC_SEQ_CST);
}
