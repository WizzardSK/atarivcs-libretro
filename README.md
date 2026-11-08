# atarivcs-libretro

A libretro core for the native games of the Atari VCS (2021) store: Linux x86-64 programs. The core starts the game as a child process (directly on x86-64, through [box64](https://github.com/ptitSeb/box64) elsewhere) with a small library preloaded into it, which hands each frame the game draws to the core and gives the game RetroArch's pad as an SDL joystick. So a game is played with whatever RetroArch maps to its RetroPad, the keyboard included.

Prototype. Bloo Kid (X11, GLX, SDL joystick) runs in RetroArch on the aarch64 Chromebook through box64: its picture in RetroArch, at RetroArch's pace, and the RetroPad driving its menus. Its window is not shown (the game is told it was mapped and focused; `ATARIVCS_SHOW_WINDOW=1` shows it). OpenAL games (openal-soft) play through RetroArch: the core points the game's OpenAL at a FIFO with openal-soft's wave backend and passes the stream on. Unity games (their own SDL inside, libGL and libX11 dlopened) show their picture and get the pad as an evdev Xbox 360 pad the shim makes up. Games built with Atari's Unity SDK also need AtariOS's bundle bridge daemon, a JSON-RPC socket in `$XDG_RUNTIME_DIR` that hands them the user and the system: the core serves a stand-in in a runtime folder of the game's own (linked to the real one's entries). Unity games' sound (PulseAudio) does not come through RetroArch yet. Savestates and rewind cannot work, and while RetroArch is paused the game slows to 10 fps rather than stopping.

What the shim does to the game: it reads the back buffer at each `glXSwapBuffers`, waits there for RetroArch's next frame, answers SDL's joystick calls with the RetroPad, leaves keyboard and pointer grabs undone (RetroArch keeps the keyboard), does not map the game's window but hands the game a `MapNotify`, `Expose` and `FocusIn` for it, and drops its `FocusOut` - Bloo Kid stops while it is not focused.

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
