#ifndef MEMORIES_PC_PLATFORM_H
#define MEMORIES_PC_PLATFORM_H
#include <stdint.h>

/* Window, keyboard and frame clock. Everything except Platform_Pad and
 * Platform_VBlankCount must be called from the main thread only.
 * MEMORIES_HEADLESS=1 skips the window; MEMORIES_SCALE picks the zoom. */
int Platform_Open(const char *title);
/* Show a VRAM rectangle (15-bit, or packed 24-bit RGB bytes) and pump events. */
void Platform_Present(const uint16_t *vram, int stride, int x, int y, int w, int h, int rgb24);
int Platform_ShouldQuit(void);
/* Display settings are applied at the next present. The SDL backend supports
 * resizable/window-mode controls; legacy X11 deliberately reports no support. */
void Platform_ApplyDisplaySettings(void);
int Platform_HasWindowModes(void);
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
/* The scripted pad bits in force at a frame (platform_common.c). */
uint16_t Platform_ScriptedBits(unsigned frame);

/* A 1 kHz SIGALRM on the main thread stands in for the hardware interrupts;
 * handlers run between instructions of the game, like the originals.
 * `tick` runs every millisecond with a monotonic microsecond clock, `vblank`
 * at 59.94 Hz. Handlers must be async-signal-safe. */
int Platform_StartTimers(void (*tick)(uint64_t now_us), void (*vblank)(void));
void Platform_WaitVBlank(unsigned count_at_entry);
unsigned Platform_VBlankCount(void);

/* Start a 44.1 kHz stereo output thread that pulls from `mix`. Failure is not
 * fatal; MEMORIES_NO_AUDIO=1 or headless mode skips it. */
#include <stddef.h>
int Platform_StartAudio(void (*mix)(int16_t *frames, size_t count));
/* The device-less mixer thread (platform_common.c): silent, or dumping to a
 * file. Backends use it for MEMORIES_NO_AUDIO, MEMORIES_DUMP_AUDIO and when
 * no device opens. */
int Platform_StartSilentAudio(void (*mix)(int16_t *frames, size_t count), const char *dump_path);
#endif
