# atarivcs-libretro

A libretro core for the native games of the Atari VCS (2021) store: Linux x86-64 programs. The core starts the game as a child process (directly on x86-64, through [box64](https://github.com/ptitSeb/box64) elsewhere) with a small library preloaded into it, which hands each frame the game draws to the core and gives the game RetroArch's pad as an SDL joystick. So a game is played with whatever RetroArch maps to its RetroPad, the keyboard included.

Prototype: tried first with Bloo Kid (X11, GLX and SDL joystick). Unity games, sound through RetroArch and hiding the game's own window are not done yet. The game keeps its own clock and sound; pause, savestates and rewind cannot work.

## Use

- Put `atarivcs_libretro.so` and `atarivcs_shim_x86_64.so` in RetroArch's cores folder (or the shim in `system/atarivcs/`).
- Load a game's `bundle.ini` (from the unpacked store package).
- On aarch64, box64 is taken from `ATARIVCS_BOX64`, else `~/.cache/gameflix/box64` (with the x86-64 libraries gameflix sets up beside it), else `PATH`.

## Controls

The RetroPad is the game's Xbox 360 layout: B = A, A = B, Y = X, X = Y, L/R = LB/RB, Select = Back, Start, L2/R2 triggers, sticks, D-pad as the hat.
