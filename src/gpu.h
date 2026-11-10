/* Zero-copy frames: the game's picture reaches RetroArch without the CPU
 * reading it back. The core makes three GPU buffers (dmabuf, through gbm),
 * the game inherits them and the shim copies each frame into one on the
 * GPU (shim.c), and the core draws from them into RetroArch's OpenGL
 * frame. Where any of that is missing, frames come the old way, read back
 * by the shim. */
#ifndef ATARIVCS_GPU_H
#define ATARIVCS_GPU_H

#include <stdbool.h>
#include <stdint.h>

#include "libretro.h"
#include "shared.h"

/* Before the game starts: the buffers, w x h at most. False: none (no
 * render node, no gbm), frames come read back. */
bool gpu_alloc(unsigned w, unsigned h, retro_log_printf_t log);
/* For the game: "W:H:SIZE:fd0,fd1,fd2", and in the child, the descriptors
 * kept open across exec */
const char *gpu_env(void);
void gpu_child_keep_fds(void);
/* RetroArch's OpenGL context for the core: false when the frontend has
 * none, frames then go to it as before (software) */
bool gpu_request_hw(retro_environment_t environ_cb);
bool gpu_active(void);
/* retro_run: the newest frame - a GPU one, else the read-back one passed
 * (top down XRGB8888, or NULL when there is none new) - drawn and handed
 * to RetroArch; *w and *h are the picture's size */
void gpu_run(atarivcs_shared *shared, const uint32_t *cpu_frame, unsigned cpu_w, unsigned cpu_h,
             retro_video_refresh_t video_cb, unsigned *w, unsigned *h);
/* The picture last drawn, top down XRGB8888, for /tmp/atarivcs-dump */
bool gpu_read_shown(uint32_t *out, unsigned max_pixels, unsigned *w, unsigned *h);
void gpu_free(void);

#endif
