#!/usr/bin/env python3
"""Write the example's sounds: plain sine tones, so the mod carries no
recording and needs nothing but Python to make.

    python3 make_tone.py        # writes title.wav, menu.wav and click.wav here

title.wav   1000 Hz, 2 s, 22050 Hz mono 16-bit  (the title song; loops)
menu.wav     660 Hz, 2 s, 44100 Hz stereo 8-bit (the main menu's song)
click.wav   2000 Hz, 0.08 s, 44100 Hz mono     (the "press Start" sound)

Any WAV (PCM 8/16/24/32-bit or float, any rate and channel count) or Ogg
Vorbis file works in their place; see notes/modding.md, "Audio"."""
import math, os, struct, wave

HERE = os.path.dirname(os.path.abspath(__file__))

def tone(name, frequency, seconds, rate, channels, width, level=0.5, fade=0.0):
    frames = int(seconds * rate)
    fade = int(rate * fade)  # in and out, so a one-shot does not click
    data = bytearray()
    for i in range(frames):
        gain = min(1.0, i / fade, (frames - 1 - i) / fade) if fade else 1.0
        value = level * gain * math.sin(2 * math.pi * frequency * i / rate)
        if width == 1:
            sample = struct.pack("<B", int(128 + 127 * value))
        else:
            sample = struct.pack("<h", int(32767 * value))
        data += sample * channels
    with wave.open(os.path.join(HERE, name), "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(width)
        out.setframerate(rate)
        out.writeframes(bytes(data))
    print(f"{name}: {frequency} Hz, {seconds} s, {rate} Hz, {channels} channel(s), {8 * width}-bit")

if __name__ == "__main__":
    # Songs: a whole number of cycles and no fade, so the loop is seamless.
    tone("title.wav", 1000, 2.0, 22050, 1, 2)
    tone("menu.wav", 660, 2.0, 44100, 2, 1)
    tone("click.wav", 2000, 0.08, 44100, 1, 2, level=0.4, fade=0.005)
