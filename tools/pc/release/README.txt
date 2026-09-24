YFM Re-Decomp
=============

A native PC version of Yu-Gi-Oh! Forbidden Memories (PlayStation, USA),
rebuilt from the decompiled game. It is a work in progress.

No part of the game's disc comes with it. You need your own copy.


Getting started
---------------

1. Make a raw image of your Yu-Gi-Oh! Forbidden Memories disc (USA,
   SLUS-01411): the .bin file of a .bin/.cue pair.

2. Put the .bin file in the "game" folder next to the program. Any file
   name ending in .bin will do.

3. Start the game:
     Windows: memories-pc.exe
     Linux:   ./memories-pc   (see "Linux" below)

If the disc image is missing, the game says so and tells you which folder
it looked in.


Controls
--------

Keyboard (change these, or set up a controller, in Game > Controls on the
menu bar):

  Arrow keys   D-pad            X   Cross        S   Circle
  Enter        Start            Z   Square       A   Triangle
  Right Shift  Select           Q/W L1/R1        E/R L2/R2
                                T/Y L3/R3

  Esc          close an open menu; with none open, quit
  F5 / F7      save / load a state      F1, F2, F4  choose the state slot

Controllers (Xbox, PlayStation and most others) work out of the box.


Your files
----------

Settings, controls, memory cards, save states and your own mods are kept
in your user folder, not next to the program:

  Windows: Documents\My Games\YFM Re-Decomp
  Linux:   ~/.local/share/YFM Re-Decomp

A copy of the program can be replaced by a newer one without losing
anything.


Mods
----

Game > Mods lists the mods the game found and lets you turn them on and
off. Two come with it:

  3D Monsters  face-up monsters stand on their cards as 3D models
  Hand Camera  L1/R1 turn and L3/R3 zoom the duel camera while the
               hand is up

To install someone else's mod, put its folder in the "mods" folder of your
user folder. A mod that contains code runs as part of the game, so only
install mods from people you trust. Mod authors: see sdk/ and
sdk/tools/build_mod.py.


Linux
-----

This is a 32-bit program, like the game it comes from. It needs the 32-bit
graphics driver (and, for sound, the 32-bit PulseAudio or ALSA library).
If you have Steam installed, you already have them. If not:

  Debian/Ubuntu:  sudo dpkg --add-architecture i386 && sudo apt update
                  sudo apt install libgl1:i386 libgl1-mesa-dri:i386 libpulse0:i386
  Fedora:         sudo dnf install mesa-libGL.i686 mesa-dri-drivers.i686 pulseaudio-libs.i686
  Arch:           enable [multilib], then: sudo pacman -S lib32-mesa lib32-libpulse

It runs on Debian 11, Ubuntu 20.04 and anything newer.


Problems
--------

If the game crashes or stops responding, it writes a report to the
"reports" folder in your user folder (crash-*.txt, hang-*.txt, and on
Windows a .dmp file) and shows where. Please include them, and
last-session.log from the same folder, when you report a problem.
