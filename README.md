# atarivcs-libretro

A libretro core for the native games of the Atari VCS (2021) store: Linux x86-64 programs. The core starts the game as a child process (directly on x86-64, through [FEX](https://github.com/FEX-Emu/FEX) or [box64](https://github.com/ptitSeb/box64) elsewhere) with a small library preloaded into it, which hands each frame the game draws to the core and gives the game RetroArch's pad as an SDL joystick. So a game is played with whatever RetroArch maps to its RetroPad, the keyboard included.

Prototype. Bloo Kid (X11, GLX, SDL joystick) runs in RetroArch on the aarch64 Chromebook through box64: its picture in RetroArch, at RetroArch's pace, and the RetroPad driving its menus. Its window is not shown (the game is told it was mapped and focused; `ATARIVCS_SHOW_WINDOW=1` shows it). OpenAL games (openal-soft) play through RetroArch: the core points the game's OpenAL at a FIFO with openal-soft's wave backend and passes the stream on. Unity games (their own SDL inside, libGL and libX11 dlopened) show their picture and get the pad as an evdev Xbox 360 pad the shim makes up (`/dev/input/event250`), which the shim's own libudev reports, for games that find their pads through udev (Rewired). Breakout Recharged gets past its title and plays with the RetroPad. Games built with Atari's Unity SDK also need AtariOS's bundle bridge daemon, a JSON-RPC socket in `$XDG_RUNTIME_DIR` that hands them the user and the system: the core serves a stand-in in a runtime folder of the game's own (linked to the real one's entries). Unity games' sound (PulseAudio) does not come through RetroArch yet. Savestates and rewind cannot work, and while RetroArch is paused the game slows to 10 fps rather than stopping.

Zero-copy: when RetroArch gives the core an OpenGL context (video driver gl or glcore), the core makes three GPU buffers (dmabuf through gbm), the game inherits them, and at each swap the shim copies the picture into one of them on the GPU (`GL_EXT_memory_object_fd`, through FEX's GL thunk too); the core draws it into RetroArch's frame. No pixel passes through the CPU. Where that is not possible (no render node or gbm, another video driver, a GL without the extension, a game larger than the buffers), or with `ATARIVCS_NO_ZEROCOPY=1`, frames are read back as below.

What the shim does to the game: it reads the back buffer at each `glXSwapBuffers` into one of two GPU pixel buffers in the CPU's cached memory and copies out the previous swap's, so the game does not wait for the GPU and RetroArch shows each frame one swap later (`ATARIVCS_PLAIN_PBO=1`: plain, uncached pixel buffers; `ATARIVCS_SYNC_READ=1`: read and waited for at once), waits there for RetroArch's next frame, answers SDL's joystick calls with the RetroPad, leaves keyboard and pointer grabs undone (RetroArch keeps the keyboard), does not map the game's window but hands the game a `MapNotify`, `Expose` and `FocusIn` for it, and drops its `FocusOut` - Bloo Kid stops while it is not focused.

## Debugging

- `touch /tmp/atarivcs-dump`: the frame shown is written to `/tmp/atarivcs-frame.ppm`.
- `echo down > /tmp/atarivcs-press`: a RetroPad button (up, down, left, right, a, b, x, y, l, r, start, select; mleft and mright move the pointer) held for 10 frames.
- The shim writes a line to the game's stderr every 120 swaps: the frame rate and how long reading a frame takes (starting the read, and mapping and copying it out).

## Use

- Put `atarivcs_libretro.so` in RetroArch's cores folder (and `atarivcs_libretro.info` in its info folder). The core carries the x86-64 shim and writes it out for each game; an `atarivcs_shim_x86_64.so` beside the core or in `system/atarivcs/` is taken instead.
- Load a game's `bundle.ini` (from the unpacked store package).
- Core option Game Resolution (1280x720 by default; 960x540, 854x480, 1920x1080): the screen the game is told it has. Games that take the desktop's resolution (the Unity ones) draw at it, and every frame of it is read back to RetroArch, so a smaller one plays faster: the read costs most of a frame at 720p on an ARM Chromebook. Taken when a game starts.
- On aarch64 the game runs through FEX or box64, as the core option x86-64 Emulator says (Auto: FEX when it is in the system folder, else box64). FEX ran Breakout Recharged at 26-28 fps in play where box64 managed 16-25 (Chromebook, Snapdragon 7c).
  - FEX: the aarch64 core built by this repository's CI carries it (about 16 MB), and writes it to `system/atarivcs/fex` (FEX, FEXServer and its GL thunks) and `system/atarivcs/fex-rootfs` (the x86-64 libraries from Debian 12) when a game is loaded and what is there is from another build. `scripts/fetch-fex.py` gets these; `make FEX_ZIP=<zip of them>` builds a core that carries them, a plain `make` one that takes them as they were put there. The games' GL goes to the host's driver through FEX's thunk. A RetroArch in flatpak needs `--allow=multiarch`, without which FEX cannot set its personality.
  - box64 is taken from `ATARIVCS_BOX64`, else `~/.cache/gameflix/box64` (with the x86-64 libraries gameflix sets up beside it), else `PATH`.

## Controls

The RetroPad is the game's Xbox 360 layout: B = A, A = B, Y = X, X = Y, L/R = LB/RB, Select = Back, Start, L2/R2 triggers, sticks, D-pad as the hat.

## Building

`make` builds the x86-64 shim and the core with the shim inside it (`FEX_ZIP=`, see Use: FEX inside it too). On a host other than x86-64 the shim needs an x86-64 cross compiler (`SHIM_CC`, by default `x86_64-linux-gnu-gcc`; Debian and Ubuntu: `gcc-x86-64-linux-gnu`). `.gitlab-ci.yml` is the libretro buildbot's recipe (Linux x86-64 and aarch64).

## License

MIT, see LICENSE. src/libretro.h is the libretro API header, MIT too (The RetroArch team). src/miniz is miniz, MIT (src/miniz/LICENSE). The FEX the aarch64 core carries is FEX-Emu's own build from its Ubuntu PPA (MIT), and its x86-64 libraries are Debian 12's packages unchanged, under their own licenses (glibc and OpenAL LGPL, SDL2 zlib, the X11 libraries MIT); their sources are Debian's, for the versions `scripts/fetch-fex.py` took.
