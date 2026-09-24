/* Audio replacement (src/pc/audio/replace.h): the decoders, the resampler,
 * the manifest's "audio" object, which mod wins an id, and what the mixer
 * makes of the sound driver's calls. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* M_PI */
#include "pc/audio/replace.h"
#include "pc/mods/json.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"
#include "scratch.h"

/* --- the port around it --------------------------------------------------- */

static int signals_logged;
void Log_Signal(int channel, const char *format, long a, long b, long c, long d, long e, long f)
{
    (void)channel; (void)format; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    signals_logged++;
}

/* --- building WAV files ------------------------------------------------- */

static unsigned char wav[1 << 20];

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* A sine of `frequency` in every channel (channel c scaled by 1/(c+1)). */
static size_t make_wav(int format, int bits, int channels, unsigned rate, size_t frames, double frequency,
                       int extensible)
{
    size_t bytes = (size_t)bits / 8, data = frames * (size_t)channels * bytes, at, i;
    size_t fmt_size = extensible ? 40 : 16;
    int c;
    memcpy(wav, "RIFF", 4);
    put32(wav + 4, (uint32_t)(4 + 8 + fmt_size + 8 + data));
    memcpy(wav + 8, "WAVE", 4);
    /* A chunk the reader must step over. */
    memcpy(wav + 12, "LIST", 4);
    put32(wav + 16, 3);
    memcpy(wav + 20, "abc\0", 4); /* three bytes and the pad byte */
    at = 24;
    memcpy(wav + at, "fmt ", 4);
    put32(wav + at + 4, (uint32_t)fmt_size);
    put16(wav + at + 8, extensible ? 0xFFFE : (unsigned)format);
    put16(wav + at + 10, (unsigned)channels);
    put32(wav + at + 12, rate);
    put32(wav + at + 16, (uint32_t)(rate * channels * bytes));
    put16(wav + at + 20, (unsigned)(channels * bytes));
    put16(wav + at + 22, (unsigned)bits);
    if (extensible) {
        memset(wav + at + 24, 0, 24);
        put16(wav + at + 24, 22);
        put16(wav + at + 32, (unsigned)format);
    }
    at += 8 + fmt_size;
    memcpy(wav + at, "data", 4);
    put32(wav + at + 4, (uint32_t)data);
    at += 8;
    assert(at + data <= sizeof(wav));
    for (i = 0; i < frames; i++) {
        for (c = 0; c < channels; c++) {
            double value = 0.5 * sin(2 * M_PI * frequency * (double)i / rate) / (c + 1);
            unsigned char *p = wav + at + (i * (size_t)channels + (size_t)c) * bytes;
            if (format == 3 && bits == 32) {
                union { float f; uint32_t w; } u;
                u.f = (float)value;
                put32(p, u.w);
            } else if (format == 3) {
                union { double f; uint64_t w; } u;
                u.f = value;
                put32(p, (uint32_t)u.w);
                put32(p + 4, (uint32_t)(u.w >> 32));
            } else if (bits == 8) {
                p[0] = (unsigned char)(128 + (int)lround(127 * value));
            } else {
                int32_t s = (int32_t)lround(value * 2147483647.0);
                if (bits == 16) put16(p, (unsigned)(uint16_t)(s >> 16));
                if (bits == 24) { p[0] = (unsigned char)(s >> 8); put16(p + 1, (unsigned)(uint16_t)(s >> 16)); }
                if (bits == 32) put32(p, (uint32_t)s);
            }
        }
    }
    return at + data;
}

/* The frequency of a clip's channel, from its rising zero crossings. */
static double frequency_of(const AudioClip *clip, int side)
{
    uint32_t i, first = 0, last = 0, crossings = 0;
    for (i = 1; i < clip->count; i++) {
        if (clip->frames[(i - 1) * 2 + side] < 0 && clip->frames[i * 2 + side] >= 0) {
            if (!crossings) first = i;
            last = i;
            crossings++;
        }
    }
    return crossings > 1 ? (double)(crossings - 1) * AUDIO_RATE / (last - first) : 0;
}

static int peak_of(const AudioClip *clip, int side)
{
    uint32_t i;
    int peak = 0;
    for (i = 0; i < clip->count; i++) {
        int v = abs(clip->frames[i * 2 + side]);
        if (v > peak) peak = v;
    }
    return peak;
}

static void check_wav(int format, int bits, int channels, unsigned rate, int extensible)
{
    AudioClip clip;
    char error[128];
    size_t size = make_wav(format, bits, channels, rate, rate / 4, 441.0, extensible);
    int left, right;
    assert(!AudioReplace_Decode(wav, size, &clip, error, sizeof(error)));
    /* A quarter second at 44.1 kHz, whatever the source rate. */
    assert(clip.count == ((uint64_t)(rate / 4) * AUDIO_RATE + rate - 1) / rate);
    assert(clip.count >= AUDIO_RATE / 4 - 6 && clip.count <= AUDIO_RATE / 4 + 1);
    assert(fabs(frequency_of(&clip, 0) - 441.0) < 3.0);
    left = peak_of(&clip, 0);
    right = peak_of(&clip, 1);
    if (channels <= 2) assert(abs(left - 16383) < 600);
    if (channels == 1) {
        assert(left == right);
    } else if (channels == 2) {
        assert(abs(right - 8191) < 400); /* the second channel is half */
    } else {
        /* Folded: the even channels average left, the odd ones right
         * (channel c is at 1/(c+1): (1 + 1/3 + 1/5) / 3 and (1/2 + 1/4 + 1/6) / 3). */
        assert(abs(left - 8373) < 300 && abs(right - 5005) < 300);
    }
    free(clip.frames);
}

static void test_decode(void)
{
    AudioClip clip;
    char error[128];
    size_t size;
    int16_t samples[8] = {0, 0, 1000, -1000, 2000, -2000, 3000, -3000};
    check_wav(1, 16, 1, 22050, 0);
    check_wav(1, 16, 2, 44100, 0);
    check_wav(1, 8, 1, 11025, 0);
    check_wav(1, 24, 2, 48000, 0);
    check_wav(1, 32, 1, 32000, 0);
    check_wav(3, 32, 2, 96000, 0);
    check_wav(3, 64, 1, 8000, 0);
    check_wav(1, 16, 2, 48000, 1);   /* WAVE_FORMAT_EXTENSIBLE */
    check_wav(1, 16, 6, 44100, 0);   /* 5.1 folds to stereo */
    /* 44.1 kHz stereo passes through untouched. */
    assert(!AudioReplace_Convert(samples, 4, 2, AUDIO_RATE, &clip));
    assert(clip.count == 4 && !memcmp(clip.frames, samples, sizeof(samples)));
    free(clip.frames);
    /* Doubling the rate puts the midpoints between. */
    assert(!AudioReplace_Convert(samples, 4, 2, AUDIO_RATE / 2, &clip));
    assert(clip.count == 8 && clip.frames[2] == 500 && clip.frames[3] == -500 && clip.frames[4] == 1000);
    free(clip.frames);
    /* Broken files say why and give nothing back. */
    size = make_wav(1, 16, 1, 22050, 100, 441.0, 0);
    assert(AudioReplace_Decode(wav, 20, &clip, error, sizeof(error)) && error[0] && !clip.frames);
    assert(AudioReplace_Decode((const unsigned char *)"not audio at all", 16, &clip, error, sizeof(error)));
    assert(strstr(error, "neither"));
    put16(wav + 24 + 8, 2); /* MS ADPCM */
    assert(AudioReplace_Decode(wav, size, &clip, error, sizeof(error)) && strstr(error, "format 2"));
    size = make_wav(1, 16, 1, 22050, 0, 441.0, 0);
    assert(AudioReplace_Decode(wav, size, &clip, error, sizeof(error)) && strstr(error, "no samples"));
    memcpy(wav, "OggS", 4);
    assert(AudioReplace_Decode(wav, 200, &clip, error, sizeof(error)) && strstr(error, "Vorbis"));
}

static unsigned char *read_whole(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;
    long length;
    assert(file);
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc((size_t)length);
    assert(data && fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void test_vorbis(void)
{
    AudioClip clip;
    char error[128];
    size_t size;
    unsigned char *data = read_whole(AUDIO_FIXTURES "/tone880.ogg", &size);
    /* Half a second of 880 Hz, mono at 22.05 kHz (ffmpeg's libvorbis). */
    assert(!AudioReplace_Decode(data, size, &clip, error, sizeof(error)));
    assert(clip.count > AUDIO_RATE * 45 / 100 && clip.count < AUDIO_RATE * 55 / 100);
    assert(fabs(frequency_of(&clip, 0) - 880.0) < 5.0);
    assert(peak_of(&clip, 0) > 1000 && peak_of(&clip, 0) == peak_of(&clip, 1));
    free(clip.frames);
    /* A damaged stream is refused, not played as noise. */
    memset(data + 60, 0x55, size - 60);
    assert(AudioReplace_Decode(data, size, &clip, error, sizeof(error)) && strstr(error, "Vorbis"));
    free(data);
}

static void test_ids(void)
{
    assert(AudioReplace_ParseId("0x2D0") == 0x2D0);
    assert(AudioReplace_ParseId("0X012") == 0x12);
    assert(AudioReplace_ParseId("720") == 720);
    assert(AudioReplace_ParseId("0") == 0);
    assert(AudioReplace_ParseId("0x10000") == -1);
    assert(AudioReplace_ParseId("2D0") == -1);
    assert(AudioReplace_ParseId("0x") == -1);
    assert(AudioReplace_ParseId("-1") == -1);
    assert(AudioReplace_ParseId("") == -1 && AudioReplace_ParseId(NULL) == -1);
}

/* --- manifests and playback ----------------------------------------------- */

static char root[SCRATCH_MAX];

static void write_file(const char *relative, const void *data, size_t size)
{
    char path[SCRATCH_MAX + 64];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "wb");
    assert(file && fwrite(data, 1, size, file) == size);
    fclose(file);
}

/* One MixBegin..MixEnd period of `frames`, summed per bus. */
static int mix(int frames, int sums[6], int last[6])
{
    int flags = AudioReplace_MixBegin(), n, i;
    memset(sums, 0, 6 * sizeof(int));
    for (n = 0; n < frames; n++) {
        int out[6] = {0, 0, 0, 0, 0, 0};
        if (flags & AUDIO_MIX_ACTIVE) AudioReplace_MixFrame(out);
        for (i = 0; i < 6; i++) sums[i] += abs(out[i]);
        if (last) memcpy(last, out, sizeof(out));
    }
    AudioReplace_MixEnd();
    return flags;
}

static void test_manifest(void)
{
    static const char *manifest_a =
        "{ \"music\": { \"0x2D0\": \"song.wav\", \"0x350\": { \"file\": \"flat.wav\", \"loop\": true,"
        "                \"loop_start\": 2, \"volume\": 50 }, \"0x351\": \"missing.wav\","
        "                \"bogus\": \"song.wav\" },"
        "  \"xa\": { \"0x8020\": \"flat.wav\" },"
        "  \"sfx\": { \"0x012\": { \"file\": \"flat.wav\", \"volume\": 100 }, \"18\": \"../escape.wav\" } }";
    static const char *manifest_b = "{ \"music\": { \"720\": { \"file\": \"flat.wav\", \"volume\": 25 } } }";
    char error[256];
    JsonDocument *a = Json_Parse(manifest_a, error, sizeof(error));
    JsonDocument *b = Json_Parse(manifest_b, error, sizeof(error));
    JsonDocument *bad = Json_Parse("{ \"music\": [] }", error, sizeof(error));
    int16_t flat[16];
    int sums[6], last[6], flags, i;
    size_t size;
    assert(a && b && bad);
    /* flat.wav: 8 frames of a constant 8000 left, -8000 right at 44.1 kHz. */
    for (i = 0; i < 8; i++) { flat[i * 2] = 8000; flat[i * 2 + 1] = -8000; }
    size = make_wav(1, 16, 2, AUDIO_RATE, 8, 0, 0);
    memcpy(wav + size - sizeof(flat), flat, sizeof(flat));
    write_file("flat.wav", wav, size);
    size = make_wav(1, 16, 1, 22050, 2205, 441.0, 0);
    write_file("song.wav", wav, size);

    /* Mod 0: the bad entries are skipped with a reason, the rest load. */
    assert(AudioReplace_Load(0, "first", root, Json_Root(a), error, sizeof(error)) == 4);
    assert(strstr(error, "missing.wav") && strstr(error, "2 more"));
    assert(AudioReplace_Count(AUDIO_MUSIC) == 2 && AudioReplace_Count(AUDIO_XA) == 1 &&
           AudioReplace_Count(AUDIO_SFX) == 1);
    assert(AudioReplace_Owner(AUDIO_MUSIC, 0x2D0) == 0 && AudioReplace_Owner(AUDIO_MUSIC, 0x351) == -1);
    assert(AudioReplace_Load(2, "bad", root, Json_Root(bad), error, sizeof(error)) == 0 && error[0]);
    assert(AudioReplace_Load(3, "bad", root, Json_Root(Json_Parse("[]", error, sizeof(error))), error,
                             sizeof(error)) == -1);

    /* Nothing plays until the driver asks. */
    flags = mix(16, sums, NULL);
    assert(!(flags & AUDIO_MIX_ACTIVE) && !(flags & AUDIO_MIX_MUTE_MUSIC));

    /* A replaced song: the game's voices are muted and the file plays on
     * the music bus, at half volume, following the driver's level. */
    AudioReplace_MusicLevel(150, 150, 150);
    AudioReplace_MusicStart(0x350);
    flags = mix(1, sums, last);
    assert((flags & AUDIO_MIX_ACTIVE) && (flags & AUDIO_MIX_MUTE_MUSIC));
    assert(last[0] == 4000 && last[1] == -4000 && !last[2] && !last[4]);
    AudioReplace_MusicLevel(75, 75, 150); /* a fade half way */
    mix(1, sums, last);
    assert(last[0] == 2000 && last[1] == -2000);
    /* It loops from frame 2: 8 frames played, still going. */
    mix(20, sums, NULL);
    assert(sums[0] == 20 * 2000);
    /* Stopped: silent, and the game's voices are heard again. */
    AudioReplace_MusicStop();
    flags = mix(4, sums, NULL);
    assert(!(flags & AUDIO_MIX_MUTE_MUSIC) && !sums[0]);
    /* An id no mod replaces leaves the game's music alone. */
    AudioReplace_MusicStart(0x123);
    assert(!(mix(4, sums, NULL) & AUDIO_MIX_MUTE_MUSIC));

    /* A later mod wins the same id, and applying it mid-song is heard at once. */
    AudioReplace_MusicLevel(150, 150, 150);
    AudioReplace_MusicStart(0x2D0);
    mix(1, sums, last);
    assert(last[0] == 0); /* song.wav starts at a zero crossing */
    mix(10, sums, NULL);
    assert(sums[0] > 0);
    assert(AudioReplace_Load(1, "second", root, Json_Root(b), error, sizeof(error)) == 1 && !error[0]);
    assert(AudioReplace_Owner(AUDIO_MUSIC, 0x2D0) == 1 && AudioReplace_Count(AUDIO_MUSIC) == 2);
    mix(1, sums, last);
    assert(last[0] == 2000); /* flat.wav at 25% */
    /* Removing it brings the first mod's back. */
    AudioReplace_Unload(1);
    assert(AudioReplace_Owner(AUDIO_MUSIC, 0x2D0) == 0);
    mix(1, sums, last);
    assert(last[0] == 0 && (AudioReplace_MixBegin() & AUDIO_MIX_MUTE_MUSIC));
    AudioReplace_MixEnd();

    /* Sound effects: the driver's volume and pan law, a one-shot on the sfx bus. */
    assert(!AudioReplace_Sfx(0x13, 255, 0));
    assert(AudioReplace_Sfx(0x12, 255, 64));
    mix(1, sums, last);
    assert(last[2] == 4000 && last[3] == -8000);
    mix(7, sums, NULL);
    assert(sums[2] == 7 * 4000);
    mix(4, sums, NULL);
    assert(!sums[2]); /* played once */
    assert(AudioReplace_Sfx(0x12, 255, 0) && AudioReplace_Sfx(0x12, 255, 0));
    mix(1, sums, last);
    assert(last[2] == 16000); /* two at once */
    AudioReplace_StopSfx();
    mix(1, sums, NULL);
    assert(!sums[2]);

    /* XA: the replacement rides the stream bus; the clip's own sectors are
     * dropped until the driver stops it or plays another. */
    AudioReplace_XaStart(0x8020, 1000, 16);
    assert(AudioReplace_XaSectorMuted(1000) && AudioReplace_XaSectorMuted(1015));
    assert(!AudioReplace_XaSectorMuted(999) && !AudioReplace_XaSectorMuted(1016));
    mix(1, sums, last);
    assert(last[4] == 8000 && last[5] == -8000);
    AudioReplace_XaStop();
    assert(!AudioReplace_XaSectorMuted(1000));
    mix(1, sums, NULL);
    assert(!sums[4]);
    AudioReplace_XaStart(0x8021, 1000, 16);
    assert(!AudioReplace_XaSectorMuted(1000));
    AudioReplace_XaStart(0x8020, 2000, 16);

    /* A save state: every channel stops, and the loaded game's song starts over. */
    AudioReplace_Reset(0x350);
    mix(1, sums, last);
    assert(!last[4] && last[0] == 8000 * 50 / 100);
    AudioReplace_Reset(-1);
    assert(!(mix(1, sums, NULL) & (AUDIO_MIX_ACTIVE | AUDIO_MIX_MUTE_MUSIC)));

    /* Removing the mod while its sounds play drops them all at once. */
    AudioReplace_MusicStart(0x2D0);
    AudioReplace_Sfx(0x12, 255, 0);
    mix(1, sums, NULL);
    AudioReplace_Unload(0);
    assert(!AudioReplace_Count(AUDIO_MUSIC) && !AudioReplace_Count(AUDIO_SFX));
    flags = mix(4, sums, NULL);
    assert(!(flags & (AUDIO_MIX_ACTIVE | AUDIO_MIX_MUTE_MUSIC)) && !AudioReplace_Sfx(0x12, 255, 0));
    assert(signals_logged > 0);
    Json_Free(a);
    Json_Free(b);
    Json_Free(bad);
}

int main(void)
{
    char *made;
    test_ids();
    test_decode();
    test_vorbis();
    scratch_template(root, sizeof(root), "memories-audio");
    made = mkdtemp(root);
    assert(made);
    test_manifest();
    printf("audio replacement: ok\n");
    return 0;
}
