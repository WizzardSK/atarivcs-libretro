/* The memory the core and the shim in the game's process share: the game's
 * frames one way, the pad the other. The core creates it (shm_open) and
 * names it in ATARIVCS_SHM for the game. */
#ifndef ATARIVCS_SHARED_H
#define ATARIVCS_SHARED_H

#include <stdint.h>

#define ATARIVCS_MAGIC 0x53435641u /* "AVCS" */
#define ATARIVCS_VERSION 4u

#define ATARIVCS_MAX_WIDTH 3840
#define ATARIVCS_MAX_HEIGHT 2160
#define ATARIVCS_GPU_NEW 4u

/* The pad as an Xbox 360 controller reads through SDL's joystick API on
 * Linux (xpad): the layout the games made for the VCS also take */
enum {
    ATARIVCS_BTN_A, ATARIVCS_BTN_B, ATARIVCS_BTN_X, ATARIVCS_BTN_Y,
    ATARIVCS_BTN_LB, ATARIVCS_BTN_RB, ATARIVCS_BTN_BACK, ATARIVCS_BTN_START,
    ATARIVCS_BTN_GUIDE, ATARIVCS_BTN_LS, ATARIVCS_BTN_RS,
    ATARIVCS_BTN_COUNT
};
enum {
    ATARIVCS_AXIS_LX, ATARIVCS_AXIS_LY, ATARIVCS_AXIS_LT,
    ATARIVCS_AXIS_RX, ATARIVCS_AXIS_RY, ATARIVCS_AXIS_RT,
    ATARIVCS_AXIS_COUNT
};
/* SDL_HAT_* bits */
enum { ATARIVCS_HAT_UP = 1, ATARIVCS_HAT_RIGHT = 2, ATARIVCS_HAT_DOWN = 4, ATARIVCS_HAT_LEFT = 8 };

typedef struct atarivcs_shared {
    uint32_t magic;
    uint32_t version;

    /* Pad, written by the core every retro_run */
    volatile uint32_t buttons; /* bit per ATARIVCS_BTN_* */
    volatile int16_t axes[ATARIVCS_AXIS_COUNT];
    volatile uint8_t hat;
    uint8_t pad0[3];
    /* Where the mouse pointer would be, x only and unbounded: the core adds
     * RetroArch's mouse and the Classic Controller's ring turning (the right
     * stick's X, an angle that wraps) to it, the shim moves the game's
     * pointer by what it changed */
    volatile int32_t mouse_x;

    /* One more each retro_run: the game's swaps wait for it (at most 100 ms),
     * so the game runs at RetroArch's pace - its window, behind RetroArch's,
     * is not held by vsync, and Bloo Kid ran at ~200 fps - and slows to
     * 10 fps while RetroArch is paused */
    volatile uint32_t run_seq;

    /* Frame, written by the shim at each buffer swap. frame_seq is odd while
     * it is being written; a reader copies while it is even and the same
     * before and after. Rows bottom up (glReadPixels), XRGB8888 in memory
     * order B, G, R, X. */
    volatile uint32_t frame_seq;
    volatile uint32_t width;
    volatile uint32_t height;
    uint32_t pad1;

    /* Zero-copy frames (gpu.c, shim.c): three GPU buffers the core made
     * (dmabuf, passed in ATARIVCS_DMABUF) and the shim copies the game's
     * picture into, on the GPU. A triple buffer: the shim draws into its
     * own, the core shows its own, and the third is exchanged through
     * gpu_middle - bits 0-1 its index, ATARIVCS_GPU_NEW set when the shim
     * put a new frame there. gpu_w/gpu_h: the picture's size in each, rows
     * bottom up from the buffer's start. */
    volatile uint32_t gpu_middle;
    volatile uint32_t gpu_w[3], gpu_h[3];
    volatile uint32_t gpu_frames; /* handed over this way so far */
    /* Set by the core while it can show the GPU buffers (imported into
     * RetroArch's OpenGL); without it the shim reads frames back */
    volatile uint32_t gpu_ready;
    uint8_t pixels[ATARIVCS_MAX_WIDTH * ATARIVCS_MAX_HEIGHT * 4];
} atarivcs_shared;

#endif
