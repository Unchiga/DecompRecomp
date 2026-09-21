/* LIBSPU over the software SPU (src/pc/audio/spu.c). Reverb calls are
 * accepted and have no effect. Transfers complete immediately. */
#include "types.h"
#include "psyq/libspu.h"
#include "pc/audio/spu.h"
#include "pc/platform/platform.h"
#include "pc/debug/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/guest/state.h"

static unsigned long transfer_address;
static long reverb_on, reverb_reserved;
static unsigned long reverb_voices;
static int started;
static u16 sample_notes[SPU_VOICES];

void SpuInit(void)
{
    Spu_Reset();
    if (!started) {
        started = 1;
        Platform_StartAudio(Spu_Mix);
    }
}

void SpuQuit(void) {}
long SpuSetReverb(long on_off) { return reverb_on = on_off; }
long SpuGetReverb(void) { return reverb_on; }
long SpuSetReverbModeParam(SpuReverbAttr *attr) { (void)attr; return 0; }
long SpuSetReverbModeType(long type) { (void)type; return 0; }
long SpuReserveReverbWorkArea(long on_off) { return reverb_reserved = on_off; }
long SpuIsReverbWorkAreaReserved(long on_off) { (void)on_off; return reverb_reserved; }

unsigned long SpuSetReverbVoice(long on_off, unsigned long voice_bit)
{
    reverb_voices = on_off ? reverb_voices | voice_bit : reverb_voices & ~voice_bit;
    return reverb_voices;
}

unsigned long SpuGetReverbVoice(void) { return reverb_voices; }
long SpuSetTransferMode(long transfer_mode) { return transfer_mode; }

unsigned long SpuSetTransferStartAddr(unsigned long address)
{
    if (address >= SPU_RAM_SIZE) {
        return 0;
    }
    return transfer_address = address & ~7ul;
}

unsigned long SpuWrite(unsigned char *address, unsigned long size)
{
    if (size > SPU_RAM_SIZE - transfer_address) {
        size = SPU_RAM_SIZE - transfer_address;
    }
    memcpy(Spu_Ram + transfer_address, address, size);
    transfer_address += size;
    return size;
}

long SpuIsTransferCompleted(long flag) { (void)flag; return 1; }

long SpuReadDecodedData(SpuDecodedData *data, long flag)
{
    (void)flag;
    memset(data, 0, sizeof(*data));
    return 0;
}

long SpuSetIRQ(long on_off) { return on_off; }

/* The library's integer note-to-pitch: notes are semitone in the high byte
 * and 1/128 semitone below; an octave is 1536 units. Within the octave it
 * steps a 1/32-octave ratio (0x103B / 0x1000) and interpolates linearly. */
static u32 octave_pitch(u32 base, u32 units)
{
    u32 low = base << 12, factor = 0x103b, high = base * factor, i;
    for (i = 0; i < units / 32; i++) {
        low = base * factor;
        factor = (factor * 0x103b) >> 12;
        high = base * factor;
    }
    return (low + ((high - low) >> 5) * (units % 32)) >> 12;
}

static u16 note_to_pitch(u16 note, u16 sample_note)
{
    s32 difference = (((note >> 8) << 7) + (note & 0xff)) - (((sample_note >> 8) << 7) + (sample_note & 0xff));
    s32 distance = difference < 0 ? -difference : difference;
    s32 octave = distance / 1536, units = distance % 1536;
    u32 base, pitch;
    if (difference >= 0) {
        base = 0x1000u << octave;
    } else {
        if (units) {
            octave++;
            units = 1536 - units;
        }
        base = 0x1000u >> octave;
    }
    pitch = octave_pitch(base & 0xffff, (u32)units);
    return pitch >= 0x4000 ? 0x3fff : (u16)pitch;
}

void SpuSetVoiceAttr(SpuVoiceAttr *attr)
{
    unsigned long mask = attr->mask ? attr->mask : 0xfffffffful;
    unsigned v;
    for (v = 0; v < SPU_VOICES; v++) {
        u16 adsr1, adsr2;
        if (!(attr->voice >> v & 1)) {
            continue;
        }
        if (mask & (SPU_VOICE_VOLL | SPU_VOICE_VOLR)) {
            /* Direct mode only: 14-bit magnitude plus sign. */
            Spu_SetVolume(v, (int16_t)(attr->volume.left & 0x7fff), (int16_t)(attr->volume.right & 0x7fff));
        }
        /* Library order: a note request overrides a pitch in the same call
         * (the sound-effect voice sends both with mask 0xFFFF), and the
         * sample note is per-voice state that outlives the call. */
        if (mask & SPU_VOICE_PITCH) {
            Spu_SetPitch(v, attr->pitch);
        }
        if (mask & SPU_VOICE_SAMPLE_NOTE) {
            sample_notes[v] = attr->sample_note;
        }
        if (mask & SPU_VOICE_NOTE) {
            Spu_SetPitch(v, note_to_pitch(attr->note, sample_notes[v]));
        }
        if (mask & SPU_VOICE_WDSA) {
            Spu_SetStart(v, attr->addr);
        }
        if (mask & SPU_VOICE_LSAX) {
            Spu_SetLoop(v, attr->loop_addr);
        }
        Spu_GetAdsr(v, &adsr1, &adsr2);
        if (mask & SPU_VOICE_ADSR_ADSR1) { adsr1 = attr->adsr1; }
        if (mask & SPU_VOICE_ADSR_ADSR2) { adsr2 = attr->adsr2; }
        /* As in the library, a mode is only written together with its rate;
         * a mode bit on its own (the music path sets AMODE beside ADSR1)
         * changes nothing. */
        if (mask & SPU_VOICE_ADSR_AR) {
            u16 exponential = (mask & SPU_VOICE_ADSR_AMODE) && attr->a_mode == SPU_VOICE_EXPIncN ? 0x80 : 0;
            adsr1 = (u16)((adsr1 & 0x00ff) | (((attr->ar > 0x7f ? 0x7f : attr->ar) | exponential) << 8));
        }
        if (mask & SPU_VOICE_ADSR_DR) { adsr1 = (u16)((adsr1 & 0xff0f) | ((attr->dr > 15 ? 15 : attr->dr) << 4)); }
        if (mask & SPU_VOICE_ADSR_SL) { adsr1 = (u16)((adsr1 & 0xfff0) | (attr->sl > 15 ? 15 : attr->sl)); }
        if (mask & SPU_VOICE_ADSR_SR) {
            u16 mode = 0;
            if (mask & SPU_VOICE_ADSR_SMODE) {
                mode = attr->s_mode == SPU_VOICE_LINEARIncN ? 0 : attr->s_mode == SPU_VOICE_EXPIncN ? 0x200
                     : attr->s_mode == SPU_VOICE_EXPDec ? 0x300 : 0x100;
            }
            adsr2 = (u16)((adsr2 & 0x003f) | (((attr->sr > 0x7f ? 0x7f : attr->sr) | mode) << 6));
        }
        if (mask & SPU_VOICE_ADSR_RR) {
            u16 exponential = (mask & SPU_VOICE_ADSR_RMODE) && attr->r_mode == SPU_VOICE_EXPDec ? 0x20 : 0;
            adsr2 = (u16)((adsr2 & 0xffc0) | (attr->rr > 0x1f ? 0x1f : attr->rr) | exponential);
        }
        Spu_SetAdsr(v, adsr1, adsr2);
    }
}

void SpuSetKey(long on_off, unsigned long voice_bit)
{
    if (on_off) {
        Spu_KeyOn(voice_bit);
    } else {
        Spu_KeyOff(voice_bit);
    }
}

void SpuSetKeyOnWithAttr(SpuVoiceAttr *attr)
{
    Log_Signal(LOG_SPU, "key on %06lx mask %05lx vol %04lx/%04lx pitch %04lx note %04lx",
               attr->voice, attr->mask, (u16)attr->volume.left, (u16)attr->volume.right, attr->pitch, attr->note);
    Log_Signal(LOG_SPU, "key detail sample_note %04lx addr %05lx adsr %04lx/%04lx",
               attr->sample_note, attr->addr, attr->adsr1, attr->adsr2, 0, 0);
    SpuSetVoiceAttr(attr);
    Spu_KeyOn(attr->voice);
}

long SpuGetKeyStatus(unsigned long voice_bit)
{
    unsigned v;
    for (v = 0; v < SPU_VOICES; v++) {
        if (voice_bit >> v & 1) {
            return Spu_KeyStatus(v);
        }
    }
    return -1;
}

void SpuGetAllKeysStatus(char *status)
{
    unsigned v;
    for (v = 0; v < SPU_VOICES; v++) {
        status[v] = (char)Spu_KeyStatus(v);
    }
}

void SpuSetCommonAttr(SpuCommonAttr *attr)
{
    static SpuCommonAttr current = {0, {0x3fff, 0x3fff}, {0, 0}, {0, 0}, {{0x7fff, 0x7fff}, 0, 1}, {{0, 0}, 0, 0}};
    unsigned long mask = attr->mask ? attr->mask : 0xfffffffful;
    if (mask & SPU_COMMON_MVOLL) { current.mvol.left = attr->mvol.left; }
    if (mask & SPU_COMMON_MVOLR) { current.mvol.right = attr->mvol.right; }
    if (mask & SPU_COMMON_CDVOLL) { current.cd.volume.left = attr->cd.volume.left; }
    if (mask & SPU_COMMON_CDVOLR) { current.cd.volume.right = attr->cd.volume.right; }
    if (mask & SPU_COMMON_CDMIX) { current.cd.mix = attr->cd.mix; }
    Spu_SetMaster((int16_t)(current.mvol.left & 0x7fff), (int16_t)(current.mvol.right & 0x7fff));
    Spu_SetCd(current.cd.volume.left, current.cd.volume.right, current.cd.mix != 0);
}

void SpuGetVoiceEnvelope(int voice, short *envelope)
{
    *envelope = voice >= 0 && voice < SPU_VOICES ? Spu_Envelope((unsigned)voice) : 0;
}

void LibSpu_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {{&transfer_address, sizeof(transfer_address)}, {&reverb_on, sizeof(reverb_on)},
                                         {&reverb_reserved, sizeof(reverb_reserved)},
                                         {&reverb_voices, sizeof(reverb_voices)}, {sample_notes, sizeof(sample_notes)}};
    Memories_StateChunk(state, "libspu", fields, sizeof(fields) / sizeof(fields[0]));
}
