#ifndef MEMORIES_PC_SPU_H
#define MEMORIES_PC_SPU_H
#include <stddef.h>
#include <stdint.h>

/* Software SPU: 24 ADPCM voices with hardware ADSR, 512 KiB sound RAM, and a
 * CD/XA input. The game thread (including its interrupt handlers) sets
 * parameters; one audio thread calls Spu_Mix. Key on/off cross threads as
 * atomic bit sets, everything else is single-word state, so no locks are
 * taken anywhere a signal handler can run.
 * Samples pass through the hardware's four-tap Gaussian interpolation.
 * Not modelled: reverb, noise, pitch modulation, volume sweeps, IRQ address. */
#define SPU_RAM_SIZE 0x80000u
#define SPU_VOICES 24

extern uint8_t Spu_Ram[SPU_RAM_SIZE];

void Spu_Reset(void);
void Spu_KeyOn(uint32_t voices);
void Spu_KeyOff(uint32_t voices);
void Spu_SetVolume(unsigned voice, int16_t left, int16_t right); /* register values */
void Spu_SetPitch(unsigned voice, uint16_t pitch);              /* 0x1000 = 44.1 kHz */
void Spu_SetStart(unsigned voice, uint32_t address);
void Spu_SetLoop(unsigned voice, uint32_t address);
void Spu_SetAdsr(unsigned voice, uint16_t adsr1, uint16_t adsr2);
void Spu_GetAdsr(unsigned voice, uint16_t *adsr1, uint16_t *adsr2);
void Spu_SetMaster(int16_t left, int16_t right);
void Spu_SetCd(int16_t left, int16_t right, int enabled);
/* 0 off, 1 on, 2 key off but still releasing, 3 keyed on with a silent envelope */
int Spu_KeyStatus(unsigned voice);
int16_t Spu_Envelope(unsigned voice);

/* CD/XA input: interleaved stereo at `rate` Hz. Single producer (the disc
 * service); returns frames accepted. */
size_t Spu_CdWrite(const int16_t *frames, size_t count, unsigned rate);
size_t Spu_CdSpace(void);
void Spu_CdFlush(void);

/* Output volume over the whole mix, 0-100: the port's own control, not the
 * game's. Set from the main thread, read by the audio thread. */
void Spu_SetOutputVolume(int percent);
int Spu_GetOutputVolume(void);
void Spu_SetMuted(int muted);
int Spu_Muted(void);

typedef enum { SPU_BUS_MUSIC, SPU_BUS_SFX, SPU_BUS_STREAM, SPU_BUS_COUNT } SpuBus;
/* Port gains outside save-state data. Voices 0-19 are music, 20-23 are the
 * game's dedicated SFX slots, and CD/XA is the stream bus. */
void Spu_SetBusVolume(SpuBus bus, int percent);
int Spu_GetBusVolume(SpuBus bus);

/* Save states: make the audio thread output silence, and wait until it is
 * outside Spu_Mix, so the voices can be read or replaced. */
void Spu_Hold(int on);

/* Audio thread: interleaved stereo at 44.1 kHz. */
void Spu_Mix(int16_t *out, size_t frames);
#endif
