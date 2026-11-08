/* The shim's pad (shim.c) as the fake libudev (shim_udev.c) describes it */
#ifndef ATARIVCS_SHIM_PAD_H
#define ATARIVCS_SHIM_PAD_H

/* An event node no real device has; SDL_JOYSTICK_DEVICE names it too */
#define PAD_PATH "/dev/input/event250"
#define PAD_SYSNAME "event250"
#define PAD_MAJOR 13
#define PAD_MINOR 1023

/* What dlopen hands out for libudev, and what dlsym answers for it */
extern char atarivcs_fake_udev_handle;
void *atarivcs_fake_udev_symbol(const char *name);

#endif
