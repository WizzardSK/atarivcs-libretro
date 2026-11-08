# atarivcs-libretro

A libretro core for the native games of the Atari VCS (2021) store: Linux x86-64 programs. The core starts the game as a child process (directly on x86-64, through [box64](https://github.com/ptitSeb/box64) elsewhere) with a small library preloaded into it, which hands each frame the game draws to the core and gives the game RetroArch's pad as an SDL joystick. So a game is played with whatever RetroArch maps to its RetroPad, the keyboard included.

Prototype. Bloo Kid (X11, GLX, SDL joystick) runs in RetroArch on the aarch64 Chromebook through box64: its picture in RetroArch, at RetroArch's pace, and the RetroPad driving its menus. Unity games, sound through RetroArch and hiding the game's own window are not done yet. The game plays its own sound; savestates and rewind cannot work, and while RetroArch is paused the game slows to 10 fps rather than stopping.

What the shim does to the game: it reads the back buffer at each `glXSwapBuffers`, waits there for RetroArch's next frame, answers SDL's joystick calls with the RetroPad, leaves keyboard and pointer grabs undone (RetroArch keeps the keyboard), and tells the game it has the focus (a `FocusIn` when its window is mapped, its `FocusOut` dropped) - Bloo Kid stops while it is not focused.

## Debugging

- `touch /tmp/atarivcs-dump`: the frame shown is written to `/tmp/atarivcs-frame.ppm`.
- `echo down > /tmp/atarivcs-press`: a RetroPad button (up, down, left, right, a, b, x, y, start, select) held for 10 frames.
- The shim writes a line to the game's stderr every 120 swaps.

## Use

- Put `atarivcs_libretro.so` and `atarivcs_shim_x86_64.so` in RetroArch's cores folder (or the shim in `system/atarivcs/`).
- Load a game's `bundle.ini` (from the unpacked store package).
- On aarch64, box64 is taken from `ATARIVCS_BOX64`, else `~/.cache/gameflix/box64` (with the x86-64 libraries gameflix sets up beside it), else `PATH`.

## Controls

The RetroPad is the game's Xbox 360 layout: B = A, A = B, Y = X, X = Y, L/R = LB/RB, Select = Back, Start, L2/R2 triggers, sticks, D-pad as the hat.
