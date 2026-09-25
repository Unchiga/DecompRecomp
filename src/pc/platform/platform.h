#ifndef MEMORIES_PC_PLATFORM_H
#define MEMORIES_PC_PLATFORM_H
#include <stdint.h>

/* Window, keyboard and frame clock. Everything except Platform_Pad and
 * Platform_VBlankCount must be called from the main thread only.
 * MEMORIES_HEADLESS=1 skips the window; MEMORIES_SCALE picks the zoom. */
int Platform_Open(const char *title);
/* A problem the player has to fix before the game can start, such as the
 * missing disc image: a message box where there is a window system to show
 * one (and not MEMORIES_HEADLESS), and standard error always. */
void Platform_ShowError(const char *title, const char *message);
void Platform_OpenMods(void);
void Platform_OpenControls(void);
/* Show a folder in the system's file manager; 0 on success. */
int Platform_OpenFolder(const char *path);
/* Re-exec with the original arguments; returns only on failure. */
int Platform_RestartGame(void);
/* Show a VRAM rectangle (15-bit, or packed 24-bit RGB bytes) and pump events. */
void Platform_Present(const uint16_t *vram, int stride, int x, int y, int w, int h, int rgb24);
/* Nonzero when the window shows a 16:9 picture, which the game then draws
 * into wider buffers (SoftGpu_SetWidescreen). */
int Platform_Widescreen(void);
/* The scaled picture instead (soft_gpu.h): 0x00RRGGBB pixels, `scale` of
 * them per game pixel each way, so the window is laid out for w/scale by
 * h/scale. Returns 0 where the backend cannot show it. */
int Platform_PresentPicture(const uint32_t *pixels, int stride, int x, int y, int w, int h, int scale);
/* With pixels NULL the picture is the one the backend's own renderer drew
 * from the GPU's record (gl_picture.h); x, y, w and h are picture pixels
 * still. Platform_ReadPicture reads w x h of that picture as 0x00RRGGBB
 * (frame dumps); 0 where there is no such renderer. */
int Platform_ReadPicture(uint32_t *out, int x, int y, int w, int h);
/* Widescreen: the widened picture of display area x,y,w,h (words), wide_w
 * words across, that the backend's own renderer drew (gl_picture.h,
 * GlPicture_WideTexture); 0 where there is none, and the caller shows
 * another. Platform_ReadWidePicture reads it whole (wide_w x h words at the
 * scale) as 0x00RRGGBB, for frame dumps; 0 when the backend's picture is
 * not that size. */
int Platform_PresentWidePicture(int x, int y, int w, int h, int wide_w, int scale);
int Platform_ReadWidePicture(uint32_t *out, int x, int y, int w, int h, int wide_w, int scale);
int Platform_ShouldQuit(void);
int Platform_StateSlot(void);
void Platform_SetStateSlot(int slot);
/* Display settings are applied at the next present. The SDL backend supports
 * resizable/window-mode controls; legacy X11 deliberately reports no support. */
void Platform_ApplyDisplaySettings(void);
int Platform_HasWindowModes(void);
/* Save the source picture, or the composed window when `window_image` is set. */
void Platform_Screenshot(int window_image);
/* PS1 digital pad bits, active high (Select 0x0001 ... Square 0x8000).
 * Async-signal-safe: it only reads a word written by Platform_Present. */
uint16_t Platform_Pad(int port);
/* Whether the port has a pad: port 0 always (the keyboard), port 1 when a
 * second controller is connected. Async-signal-safe. */
int Platform_PadConnected(int port);
/* Controllers (gamepad_evdev.c): polled once a frame on the main thread. */
void Gamepad_Poll(unsigned frame);
uint16_t Gamepad_Bits(int port);
int Gamepad_Connected(int port);
/* Advance scripted test input (MEMORIES_INPUT) to this presented frame. */
void Platform_Frame(unsigned frame);
/* Poll only window/input events while a paused VBlank wait owns the main thread. */
void Platform_PumpEvents(void);
/* The scripted pad bits in force at a frame (platform_common.c), for the
 * first pad (MEMORIES_INPUT) and the second (MEMORIES_INPUT2), and whether
 * the second is scripted at all. */
uint16_t Platform_ScriptedBits(unsigned frame);
uint16_t Platform_ScriptedBits2(unsigned frame);
int Platform_ScriptedPad2(void);

/* The frame clock, in three independent parts (platform_common.c):
 *
 * 1. The game clock. A 1 kHz SIGALRM on the main thread stands in for the
 *    console's interrupts; handlers run between instructions of the game,
 *    like the originals. `tick` runs every millisecond with the accelerated
 *    game clock and monotonic real time (time-based game services use
 *    game_us, music sequencing uses real_us so the score keeps its tempo);
 *    `vblank` fires at 59.94 Hz of game time. Handlers must be
 *    async-signal-safe. The speed setting scales game time: percent of real
 *    time, 0 paused, -1 uncapped (a VBlank whenever the game waits for one).
 * 2. Presentation. Independently of the game clock, at most one game frame
 *    per present period reaches the window: the cap is a frame rate, 0 for
 *    the display's refresh rate, -1 for every game frame.
 * 3. Display vsync (backend). A blocking vsync present can only pace the game
 *    while game frames come no faster than the display refreshes; above that
 *    the backend presents without blocking and the cap alone limits presents.
 */
int Platform_StartTimers(void (*tick)(uint64_t game_us, uint64_t real_us), void (*vblank)(void));
/* Wait while the VBlank count still equals `count` (the caller's count at entry). */
void Platform_WaitVBlank(unsigned count);
/* No more ticks or VBlanks: called before the game exits, so that none
 * interrupts the C runtime while it shuts down. */
void Platform_StopTimers(void);
/* A VSync that does not wait (the running count): in a deterministic run it
 * lets 1 ms of game time pass, since a loop polling it is waiting for time
 * (movie playback waits for its strips that way). Nothing otherwise. */
void Platform_PollTime(void);
unsigned Platform_VBlankCount(void);
void Platform_SetClockRate(int percent);
int Platform_ClockRate(void);
/* Game frames per second at the current speed; 0 when paused or uncapped. */
float Platform_GameHz(void);
void Platform_StepFrame(void);
void Platform_SetPresentCap(int fps);
int Platform_PresentCap(void);
/* Present period from the cap and the display refresh, in microseconds; 0 for every frame. */
unsigned Platform_PresentPeriodUs(void);
/* Whether the game frame about to be shown should reach the window. Main thread. */
int Platform_PresentDue(void);
/* Whether a blocking vsync present may pace the game at the current speed. */
int Platform_VSyncPacesGame(void);
/* The backend reports each vsynced present; at 100% on a 60 Hz display the
 * game's VBlank is re-phased to the display so the two rates do not beat. */
void Platform_NotifyPresent(uint64_t real_now_us, int vsynced);
void Platform_SetVBlankPeriod(unsigned us);
void Platform_SetPresentRefresh(float hz);
float Platform_PresentRefresh(void);
void Platform_VSyncHeartbeat(void);

/* Start a 44.1 kHz stereo output thread that pulls from `mix`. Failure is not
 * fatal; MEMORIES_NO_AUDIO=1 or headless mode skips it. */
#include <stddef.h>
int Platform_StartAudio(void (*mix)(int16_t *frames, size_t count));
/* The device-less mixer thread (platform_common.c): silent, or dumping to a
 * file. Backends use it for MEMORIES_NO_AUDIO, MEMORIES_DUMP_AUDIO and when
 * no device opens. */
int Platform_StartSilentAudio(void (*mix)(int16_t *frames, size_t count), const char *dump_path);
void Platform_AudioStats(int *queued_frames, unsigned *underruns);
#endif
