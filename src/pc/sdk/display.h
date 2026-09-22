#ifndef MEMORIES_PC_SDK_DISPLAY_H
#define MEMORIES_PC_SDK_DISPLAY_H
/* Show the area selected by the last PutDispEnv. Main thread only. */
void Memories_PresentDisplay(void);
unsigned Memories_PresentedFrames(void);
/* Frames that reached the window: presentation is paced apart from the game. */
unsigned Memories_ShownFrames(void);
typedef struct FrameStats {
    unsigned fps_tenths;       /* game frames per second */
    unsigned shown_tenths;     /* presented frames per second */
    unsigned game_us, present_us, game_max_us, present_max_us;
    unsigned missed_vblanks;
    unsigned draw_words, draw_us;
} FrameStats;
const FrameStats *Memories_FrameStats(void);
void Memories_SetDrawStats(unsigned words, unsigned us);
/* Write the current display rectangle, or the entire 1024x512 VRAM, as PPM. */
void Memories_DumpFrame(const char *path, int full_vram);
#endif
