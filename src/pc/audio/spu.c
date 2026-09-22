#include "spu.h"
#include "spu_gauss.h"
#include "game/sound_voice_constants.h"
#include <string.h>
#include "pc/guest/state.h"
#include "pc/debug/log.h"
#include <time.h>

enum { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

typedef struct Voice {
    volatile int16_t left, right;
    volatile uint16_t pitch, adsr1, adsr2;
    volatile uint32_t start, loop;
    volatile int phase, level, keyed;
    int loop_set;
    uint32_t address, counter;
    int32_t wait;
    int16_t block[3 + 28], history[2]; /* three samples of the previous block, then this one */
    int position, stop;
} Voice;

uint8_t Spu_Ram[SPU_RAM_SIZE];
static Voice voices[SPU_VOICES];
static volatile uint32_t pending_on, pending_off, pending_late_off;
/* What the game has keyed, as of its last SpuSetKey. The hardware answers
 * status and envelope reads at once, and the sound driver relies on that:
 * it picks a free voice by envelope == 0 and spins on status after a
 * key-off. The mixer applies keys a period later, so those reads come from
 * the request, not the mixer, or two sounds land on one voice in one
 * period and the first never plays (common at 200%+ game speed). */
static volatile uint32_t keyed_mask;
static volatile int16_t master_left = 0x3fff, master_right = 0x3fff, cd_left = 0x7fff, cd_right = 0x7fff;
static volatile int cd_enabled = 1;
static volatile int hold, mixing;
/* The port's own output control (the menu bar's Audio > Volume), 0-100.
 * Kept apart from the game's master volume so a save state cannot carry it.
 * The mixer walks its gain to the target instead of jumping: a step in the
 * middle of a waveform is a click, and dragging the slider made a burst of
 * them. GAIN_ONE is 100% in the mixer's fixed point; GAIN_STEP crosses the
 * whole range in about 36 ms, fast enough to feel instant. */
#define GAIN_ONE (100 << 8)
#define GAIN_STEP 16
#define SPU_SFX_FIRST_VOICE 20
_Static_assert(SPU_SFX_FIRST_VOICE == SD_VOICE_SLOT_FIRST_VOICE, "SFX voice split changed");
static volatile int output_volume = 100;
static volatile int output_muted;
static volatile int interpolation; /* SpuInterpolation */
static volatile int bus_volume[SPU_BUS_COUNT] = {100, 100, 100};

#define CD_RING 65536u /* frames; power of two */
static int16_t cd_ring[CD_RING * 2];
static volatile uint32_t cd_head, cd_tail, cd_rate = 37800;
static uint32_t cd_phase;
static int16_t cd_last[2], cd_next[2];

void Spu_Reset(void)
{
    memset(voices, 0, sizeof(voices));
    pending_on = pending_off = pending_late_off = keyed_mask = 0;
    Spu_CdFlush();
}

/* The mixer applies a period's key-offs before its key-ons, which is the
 * order the sound-effect path issues them in. A key-off that follows a
 * key-on still waiting for the mixer is kept apart and applied after it, so
 * a note shorter than one mix period is released instead of left hanging. */
void Spu_KeyOn(uint32_t bits)
{
    bits &= 0xffffff;
    if (bits & pending_on) {
        /* the earlier key-on will never sound: the voice was reused before the mixer ran */
        LOG(LOG_SPU, "key-on %06x replaces a key-on not yet mixed", (unsigned)(bits & pending_on));
    }
    __atomic_fetch_and(&pending_late_off, ~bits, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&pending_on, bits, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&keyed_mask, bits, __ATOMIC_SEQ_CST);
}

void Spu_KeyOff(uint32_t bits)
{
    uint32_t late;
    bits &= 0xffffff;
    late = bits & pending_on;
    __atomic_fetch_or(&pending_late_off, late, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&pending_off, bits & ~late, __ATOMIC_SEQ_CST);
    __atomic_fetch_and(&keyed_mask, ~bits, __ATOMIC_SEQ_CST);
}
void Spu_SetVolume(unsigned v, int16_t l, int16_t r) { voices[v].left = l; voices[v].right = r; }
void Spu_SetPitch(unsigned v, uint16_t pitch) { voices[v].pitch = pitch > 0x3fff ? 0x3fff : pitch; }
void Spu_SetStart(unsigned v, uint32_t address) { voices[v].start = address & (SPU_RAM_SIZE - 8); }
void Spu_SetLoop(unsigned v, uint32_t address) { voices[v].loop = address & (SPU_RAM_SIZE - 8); voices[v].loop_set = 1; }
void Spu_SetAdsr(unsigned v, uint16_t a1, uint16_t a2) { voices[v].adsr1 = a1; voices[v].adsr2 = a2; }
void Spu_GetAdsr(unsigned v, uint16_t *a1, uint16_t *a2) { *a1 = voices[v].adsr1; *a2 = voices[v].adsr2; }
void Spu_SetMaster(int16_t l, int16_t r) { master_left = l; master_right = r; }
void Spu_SetOutputVolume(int percent) { output_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
int Spu_GetOutputVolume(void) { return output_volume; }
void Spu_SetMuted(int muted) { output_muted = !!muted; }
void Spu_SetInterpolation(SpuInterpolation mode) { interpolation = mode; }
int Spu_Muted(void) { return output_muted; }
void Spu_SetBusVolume(SpuBus bus, int percent)
{
    if (bus < 0 || bus >= SPU_BUS_COUNT) return;
    bus_volume[bus] = percent < 0 ? 0 : percent > 100 ? 100 : percent;
}
int Spu_GetBusVolume(SpuBus bus) { return bus >= 0 && bus < SPU_BUS_COUNT ? bus_volume[bus] : 0; }
void Spu_SetCd(int16_t l, int16_t r, int enabled) { cd_left = l; cd_right = r; cd_enabled = enabled; }
/* A voice keyed on but not yet mixed reads as in full attack: the driver
 * takes envelope 0 as a free voice. */
int16_t Spu_Envelope(unsigned v) { return pending_on >> v & 1 ? 0x7fff : (int16_t)voices[v].level; }

int Spu_KeyStatus(unsigned v)
{
    int keyed = keyed_mask >> v & 1, sounding = voices[v].level > 0 || (pending_on >> v & 1);
    return keyed ? (sounding ? 1 : 3) : (sounding ? 2 : 0);
}

/* Register volume: bit 15 selects sweep (unsupported, treated as full scale
 * of its sign); otherwise a signed 15-bit level doubled by the hardware. */
static int volume(int16_t value)
{
    if (value & 0x8000) {
        return value & 0x2000 ? -0x7fff : 0x7fff; /* sweep: approximate */
    }
    return (int16_t)((uint16_t)value << 1);
}

static void decode_block(Voice *voice)
{
    static const int k0[5] = {0, 60, 115, 98, 122}, k1[5] = {0, 0, -52, -55, -60};
    const uint8_t *data = Spu_Ram + (voice->address & (SPU_RAM_SIZE - 1));
    int shift = data[0] & 15, filter = (data[0] >> 4) > 4 ? 4 : data[0] >> 4, flags = data[1], i;
    if (shift > 12) {
        shift = 9;
    }
    if ((flags & 4) && !voice->loop_set) {
        voice->loop = voice->address;
    }
    voice->block[0] = voice->block[28];
    voice->block[1] = voice->block[29];
    voice->block[2] = voice->block[30];
    for (i = 0; i < 28; i++) {
        int nibble = (data[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int sample = ((int16_t)(nibble << 12) >> shift) +
                     ((voice->history[0] * k0[filter] + voice->history[1] * k1[filter] + 32) >> 6);
        sample = sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample;
        voice->history[1] = voice->history[0];
        voice->history[0] = (int16_t)sample;
        voice->block[3 + i] = (int16_t)sample;
    }
    voice->address = (voice->address + 16) & (SPU_RAM_SIZE - 1);
    if (flags & 1) {
        voice->address = voice->loop;
        voice->stop = !(flags & 2);
    }
}

static void envelope(Voice *voice)
{
    int rate, exponential, decreasing, shift, step, cycles;
    switch (voice->phase) {
    case ATTACK: rate = (voice->adsr1 >> 8) & 0x7f; exponential = voice->adsr1 >> 15; decreasing = 0; break;
    case DECAY: rate = ((voice->adsr1 >> 4) & 15) * 4; exponential = 1; decreasing = 1; break;
    case SUSTAIN: rate = (voice->adsr2 >> 6) & 0x7f; exponential = voice->adsr2 >> 15; decreasing = (voice->adsr2 >> 14) & 1; break;
    case RELEASE: rate = (voice->adsr2 & 31) * 4; exponential = (voice->adsr2 >> 5) & 1; decreasing = 1; break;
    default: return;
    }
    if (--voice->wait > 0) {
        return;
    }
    shift = rate >> 2;
    step = decreasing ? -8 + (rate & 3) : 7 - (rate & 3);
    cycles = 1 << (shift > 11 ? shift - 11 : 0);
    step <<= shift < 11 ? 11 - shift : 0;
    if (exponential && !decreasing && voice->level > 0x6000) {
        cycles *= 4;
    }
    if (exponential && decreasing) {
        step = (step * voice->level) >> 15;
    }
    voice->wait = cycles;
    voice->level += step;
    if (voice->level < 0) {
        voice->level = 0;
    }
    if (voice->phase == ATTACK && voice->level >= 0x7fff) {
        voice->level = 0x7fff;
        voice->phase = DECAY;
        voice->wait = 0;
    } else if (voice->phase == DECAY && voice->level <= (((voice->adsr1 & 15) + 1) * 0x800)) {
        voice->phase = SUSTAIN;
        voice->wait = 0;
    } else if (voice->phase == RELEASE && voice->level == 0) {
        voice->phase = OFF;
    }
    if (voice->level > 0x7fff) {
        voice->level = 0x7fff;
    }
}

size_t Spu_CdSpace(void) { return CD_RING - 1 - ((cd_head - cd_tail) & (CD_RING - 1)); }

size_t Spu_CdWrite(const int16_t *frames, size_t count, unsigned rate)
{
    size_t space = Spu_CdSpace(), i;
    if (count > space) {
        count = space;
    }
    cd_rate = rate;
    for (i = 0; i < count; i++) {
        uint32_t at = (cd_head + i) & (CD_RING - 1);
        cd_ring[at * 2] = frames[i * 2];
        cd_ring[at * 2 + 1] = frames[i * 2 + 1];
    }
    cd_head = (cd_head + count) & (CD_RING - 1);
    return count;
}

void Spu_CdFlush(void) { cd_tail = cd_head; }

void Spu_Mix(int16_t *out, size_t frames)
{
    static int output_gain = GAIN_ONE; /* the audio thread's own; no other writer */
    static int bus_gain[SPU_BUS_COUNT] = {GAIN_ONE, GAIN_ONE, GAIN_ONE};
    const int output_target = (output_muted ? 0 : output_volume) << 8;
    const int bus_target[SPU_BUS_COUNT] = {bus_volume[SPU_BUS_MUSIC] << 8,
                                           bus_volume[SPU_BUS_SFX] << 8,
                                           bus_volume[SPU_BUS_STREAM] << 8};
    size_t n;
    unsigned v;
    uint32_t on;
    /* A save state is being taken or applied: stay out of the voices. */
    __atomic_store_n(&mixing, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&hold, __ATOMIC_SEQ_CST)) {
        memset(out, 0, frames * 2 * sizeof(*out));
        __atomic_store_n(&mixing, 0, __ATOMIC_SEQ_CST);
        return;
    }
    on = __atomic_exchange_n(&pending_on, 0, __ATOMIC_SEQ_CST);
    uint32_t off = __atomic_exchange_n(&pending_off, 0, __ATOMIC_SEQ_CST);
    uint32_t late_off = __atomic_exchange_n(&pending_late_off, 0, __ATOMIC_SEQ_CST);
    for (v = 0; v < SPU_VOICES; v++) {
        Voice *voice = &voices[v];
        if (off >> v & 1) {
            voice->keyed = 0;
            if (voice->phase != OFF) {
                voice->phase = RELEASE;
                voice->wait = 0;
            }
        }
        if (on >> v & 1) {
            voice->address = voice->start;
            voice->counter = 0;
            voice->position = 28;
            voice->history[0] = voice->history[1] = 0;
            voice->block[28] = voice->block[29] = voice->block[30] = 0;
            voice->level = 0;
            voice->wait = 0;
            voice->stop = 0;
            voice->loop_set = 0;
            voice->phase = ATTACK;
            voice->keyed = 1;
        }
        if (late_off >> v & 1) {
            voice->keyed = 0;
            voice->phase = RELEASE;
            voice->wait = 0;
        }
    }
    for (n = 0; n < frames; n++) {
        int music_l = 0, music_r = 0, sfx_l = 0, sfx_r = 0, left, right;
        int bus;
        for (bus = 0; bus < SPU_BUS_COUNT; bus++) {
            if (bus_gain[bus] != bus_target[bus]) {
                int step = bus_target[bus] - bus_gain[bus];
                step = step > GAIN_STEP ? GAIN_STEP : step < -GAIN_STEP ? -GAIN_STEP : step;
                bus_gain[bus] += step;
            }
        }
        for (v = 0; v < SPU_VOICES; v++) {
            Voice *voice = &voices[v];
            const int16_t *taps;
            int sample, fraction;
            if (voice->phase == OFF) {
                continue;
            }
            while (voice->position >= 28) {
                if (voice->stop) {
                    voice->phase = OFF;
                    voice->level = 0;
                    break;
                }
                decode_block(voice);
                voice->position -= 28;
            }
            if (voice->phase == OFF) {
                continue;
            }
            /* Four-tap Gaussian interpolation over the newest samples, as the
             * console's SPU does; it rolls off the top octave. The cubic
             * option (Catmull-Rom through the same four, between taps[1] and
             * taps[2] like the Gaussian) keeps it. */
            fraction = (int)(voice->counter >> 4 & 0xff);
            taps = &voice->block[voice->position];
            if (interpolation == SPU_INTERPOLATION_CUBIC) {
                int a = 3 * (taps[1] - taps[2]) + taps[3] - taps[0];
                int b = 2 * taps[0] - 5 * taps[1] + 4 * taps[2] - taps[3];
                int c = taps[2] - taps[0];
                sample = taps[1] + ((((((a * fraction) >> 8) + b) * fraction >> 8) + c) * fraction >> 9);
                sample = sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample;
            } else {
                sample = (Spu_Gauss[0x0ff - fraction] * taps[0] + Spu_Gauss[0x1ff - fraction] * taps[1] +
                          Spu_Gauss[0x100 + fraction] * taps[2] + Spu_Gauss[fraction] * taps[3]) >> 15;
            }
            voice->counter += voice->pitch;
            voice->position += (int)(voice->counter >> 12);
            voice->counter &= 0xfff;
            envelope(voice);
            sample = (sample * voice->level) >> 15;
            if (v < SPU_SFX_FIRST_VOICE) {
                music_l += (sample * volume(voice->left)) >> 15;
                music_r += (sample * volume(voice->right)) >> 15;
            } else {
                sfx_l += (sample * volume(voice->left)) >> 15;
                sfx_r += (sample * volume(voice->right)) >> 15;
            }
        }
        left = (int)(((int64_t)music_l * bus_gain[SPU_BUS_MUSIC] +
                      (int64_t)sfx_l * bus_gain[SPU_BUS_SFX]) / GAIN_ONE);
        right = (int)(((int64_t)music_r * bus_gain[SPU_BUS_MUSIC] +
                       (int64_t)sfx_r * bus_gain[SPU_BUS_SFX]) / GAIN_ONE);
        left = (left * volume(master_left)) >> 15;
        right = (right * volume(master_right)) >> 15;
        if (cd_enabled) {
            cd_phase += cd_rate;
            while (cd_phase >= 44100) {
                cd_phase -= 44100;
                cd_last[0] = cd_next[0];
                cd_last[1] = cd_next[1];
                if (cd_tail != cd_head) {
                    cd_next[0] = cd_ring[cd_tail * 2];
                    cd_next[1] = cd_ring[cd_tail * 2 + 1];
                    cd_tail = (cd_tail + 1) & (CD_RING - 1);
                } else {
                    cd_next[0] = cd_next[1] = 0;
                }
            }
            int stream_l = ((cd_last[0] + (int)((int64_t)(cd_next[0] - cd_last[0]) * cd_phase / 44100)) * cd_left) >> 15;
            int stream_r = ((cd_last[1] + (int)((int64_t)(cd_next[1] - cd_last[1]) * cd_phase / 44100)) * cd_right) >> 15;
            left += (int)((int64_t)stream_l * bus_gain[SPU_BUS_STREAM] / GAIN_ONE);
            right += (int)((int64_t)stream_r * bus_gain[SPU_BUS_STREAM] / GAIN_ONE);
        }
        if (output_gain != output_target) {
            int step = output_target - output_gain;
            step = step > GAIN_STEP ? GAIN_STEP : step < -GAIN_STEP ? -GAIN_STEP : step;
            output_gain += step;
        }
        if (output_gain != GAIN_ONE) {
            left = (int)((int64_t)left * output_gain / GAIN_ONE);
            right = (int)((int64_t)right * output_gain / GAIN_ONE);
        }
        out[n * 2] = (int16_t)(left < -32768 ? -32768 : left > 32767 ? 32767 : left);
        out[n * 2 + 1] = (int16_t)(right < -32768 ? -32768 : right > 32767 ? 32767 : right);
    }
    __atomic_store_n(&mixing, 0, __ATOMIC_SEQ_CST);
}

void Spu_Hold(int on)
{
    struct timespec nap = {0, 200000};
    __atomic_store_n(&hold, on, __ATOMIC_SEQ_CST);
    while (on && __atomic_load_n(&mixing, __ATOMIC_SEQ_CST)) {
        nanosleep(&nap, NULL);
    }
}

void Spu_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {
        {Spu_Ram, sizeof(Spu_Ram)}, {voices, sizeof(voices)}, {(void *)&pending_on, sizeof(pending_on)},
        {(void *)&pending_off, sizeof(pending_off)}, {(void *)&pending_late_off, sizeof(pending_late_off)},
        {(void *)&master_left, sizeof(master_left)}, {(void *)&master_right, sizeof(master_right)},
        {(void *)&cd_left, sizeof(cd_left)}, {(void *)&cd_right, sizeof(cd_right)},
        {(void *)&cd_enabled, sizeof(cd_enabled)}};
    if (Memories_StateChunk(state, "spu", fields, sizeof(fields) / sizeof(fields[0]))) {
        unsigned v;
        uint32_t keyed = pending_on;
        for (v = 0; v < SPU_VOICES; v++) if (voices[v].keyed) keyed |= 1u << v;
        keyed_mask = keyed & ~pending_off & ~pending_late_off;
        Spu_CdFlush(); /* streamed audio restarts from the disc position */
        cd_phase = 0;
        cd_last[0] = cd_last[1] = cd_next[0] = cd_next[1] = 0;
    }
}
