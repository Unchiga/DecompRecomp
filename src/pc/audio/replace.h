#ifndef MEMORIES_PC_AUDIO_REPLACE_H
#define MEMORIES_PC_AUDIO_REPLACE_H
/* Audio replacement: a mod's "audio" object (notes/modding.md, "Audio")
 * swaps the game's songs, XA streams and sound effects for WAV or Ogg
 * Vorbis files, with no code.
 *
 * The files are decoded when the mod is applied, to 44.1 kHz s16 stereo in
 * memory. Three kinds of caller meet here, and none of them waits on
 * another:
 *
 *  - the mod system (main thread): AudioReplace_Load/Unload. The id tables
 *    change only with the game clock held off (SIGALRM blocked) and the
 *    mixer held out of the channels, so a clip is never freed while
 *    something reads it;
 *  - the sound driver (game thread, much of it on the VBlank interrupt):
 *    the Music/Xa/Sfx calls below. They read the tables, post requests into
 *    fixed channel slots and log with Log_Signal; nothing allocates, locks
 *    or prints;
 *  - the mixer (audio thread, inside Spu_Mix): MixBegin/MixFrame/MixEnd.
 *    It takes posted requests through a sequence word per slot and mixes.
 *
 * The game's own sound keeps running underneath: a replaced song's
 * sequence still plays (its voices are muted in the mix), a replaced XA
 * clip is still read at the drive's pace (its decoded sectors are dropped),
 * so everything the game times on them is unchanged. */
#include <stddef.h>
#include <stdint.h>

struct JsonValue;

typedef enum { AUDIO_MUSIC, AUDIO_XA, AUDIO_SFX, AUDIO_KINDS } AudioKind;

/* Decoded sound: interleaved stereo s16 at 44.1 kHz. */
typedef struct AudioClip {
    int16_t *frames;
    uint32_t count;     /* frames */
} AudioClip;

#define AUDIO_RATE 44100
/* The longest clip decoded, in frames at 44.1 kHz: 12 minutes (127 MB). */
#define AUDIO_CLIP_MAX_FRAMES (12u * 60u * AUDIO_RATE)

/* WAV (PCM 8/16/24/32-bit, 32/64-bit float, any channel count and rate) or
 * Ogg Vorbis, told apart by their contents. Returns 0 and fills `clip`
 * (free clip->frames), or -1 with the reason in `error`. */
int AudioReplace_Decode(const unsigned char *data, size_t size, AudioClip *clip, char *error, size_t error_size);
/* Interleaved s16 at any rate and 1 to 32 channels to 44.1 kHz stereo
 * (linear resampling). `speakers` names each channel's speaker by its bit
 * in WAVE_FORMAT_EXTENSIBLE's channel mask (0 front left, 1 front right,
 * 2 centre, 3 LFE, 4 and 5 back, 9 and 10 side; replace.c), so that more
 * than two channels fold to stereo as a downmix does: centres on both
 * sides at -3 dB, the rest on their own side, the LFE dropped. NULL: the
 * first two are the front pair. Returns 0, or -1 when out of memory or too
 * long. */
int AudioReplace_Convert(const int16_t *samples, size_t frames, int channels, const unsigned char *speakers,
                         unsigned rate, AudioClip *clip);

/* A mod's "audio" object, when it is applied (mods.c). Every entry that
 * decodes is added; each one that does not is skipped, and the first reason
 * is written to `error` (empty when all loaded). Returns the number of
 * entries added, or -1 when `audio` itself is malformed. `mod_id` must live
 * until exit: the trace names the mod by it from interrupt context. Of
 * several applied mods that replace one id, the one applied last wins. */
int AudioReplace_Load(int mod, const char *mod_id, const char *directory, const struct JsonValue *audio,
                      char *error, size_t error_size);
/* Drop what a mod added, stopping whatever of it is playing. */
void AudioReplace_Unload(int mod);
/* How many ids are replaced now, of one kind, and whether `id` is. */
int AudioReplace_Count(AudioKind kind);
/* The mod index replacing `id`, or -1. */
int AudioReplace_Owner(AudioKind kind, int id);
/* "0x2D0" (hexadecimal) or "720" (decimal) to 0-0xFFFF; -1 if neither. */
long AudioReplace_ParseId(const char *text);

/* --- the sound driver's hooks (src/game, #ifdef MEMORIES_PC) ---------- */
/* A song (the driver's id, 0x2D0 for SD_BGMPlay(0x2D0)) started playing. */
void AudioReplace_MusicStart(int id);
/* The song stopped, was reset or ended. */
void AudioReplace_MusicStop(void);
/* The driver's music level (SD_SetSecondaryMasterLevels), fades included,
 * and `full`, what it is with no fade (the driver's music volume). A
 * replacement plays at its own volume at `full` and follows the fades. */
void AudioReplace_MusicLevel(int left, int right, int full);
/* A sound effect is about to be given a voice. `volume` 0-255, `pan`
 * -128 (left) to 128 (right). Nonzero when it was replaced and played
 * here, and the voice should not be allocated. */
int AudioReplace_Sfx(int id, int volume, int pan);
/* Every sound-effect voice was keyed off. */
void AudioReplace_StopSfx(void);
/* An XA clip (id 0x8xxx, 0x9xxx or 0xAxxx) is requested from `sectors`
 * sectors at `lba`. */
void AudioReplace_XaStart(int id, int lba, int sectors);
/* The driver stopped XA playback. */
void AudioReplace_XaStop(void);
/* From the drive model: whether a decoded XA sector at `lba` belongs to a
 * replaced clip and should not reach the mix. */
int AudioReplace_XaSectorMuted(int lba);
/* A save state was loaded: every replacement channel stops. `music_id` is
 * the song the loaded game is playing, or -1; it starts again from its top. */
void AudioReplace_Reset(int music_id);
/* The same, with the song and level read from the loaded game's driver:
 * defined beside SD_ResetMusicState (src/game/func_80049010.c). */
void AudioReplace_StateLoaded(void);

/* --- the mixer (Spu_Mix) ---------------------------------------------- */
/* Once per mix period, before its frames. Returns a mask: AUDIO_MIX_ACTIVE
 * when MixFrame has anything to add, AUDIO_MIX_MUTE_MUSIC when the game's
 * music voices are to be silent. MixEnd must follow. */
#define AUDIO_MIX_ACTIVE 1
#define AUDIO_MIX_MUTE_MUSIC 2
int AudioReplace_MixBegin(void);
/* One output frame: adds music, sfx and stream left/right into out[0..5],
 * full scale 32767, before any bus, master or CD gain. */
void AudioReplace_MixFrame(int out[6]);
void AudioReplace_MixEnd(void);
#endif
