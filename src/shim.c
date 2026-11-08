/* Loaded into the game (LD_PRELOAD, or BOX64_LD_PRELOAD under box64): hands
 * each frame the game swaps to the core, and gives the game the core's pad as
 * an SDL joystick. Built for x86-64, the games' architecture. No SDL, GL or
 * X11 headers: the few types and constants it needs are declared here. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shared.h"

#define EXPORT __attribute__((visibility("default")))

static atarivcs_shared *shared;

__attribute__((constructor)) static void shim_init(void)
{
    const char *name = getenv("ATARIVCS_SHM");
    if (!name)
        return;
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "atarivcs shim: cannot open %s\n", name);
        return;
    }
    void *p = mmap(NULL, sizeof(atarivcs_shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED || ((atarivcs_shared *)p)->magic != ATARIVCS_MAGIC) {
        fprintf(stderr, "atarivcs shim: %s is not the core's\n", name);
        return;
    }
    shared = p;
    fprintf(stderr, "atarivcs shim: attached to %s\n", name);
}

static void *next(const char *symbol)
{
    void *f = dlsym(RTLD_NEXT, symbol);
    if (!f)
        fprintf(stderr, "atarivcs shim: no %s\n", symbol);
    return f;
}

/* ---- Video: the back buffer, read just before the swap ---- */

typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
#define GL_BACK 0x0405
#define GL_READ_BUFFER 0x0C02
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GLX_WIDTH 0x801D
#define GLX_HEIGHT 0x801E

static void capture(void *dpy, unsigned long drawable)
{
    static void (*query)(void *, unsigned long, int, unsigned int *);
    static void (*get_integerv)(GLenum, GLint *);
    static void (*read_buffer)(GLenum);
    static void (*pixel_storei)(GLenum, GLint);
    static void (*read_pixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    static void (*bind_buffer)(GLenum, unsigned int);
    static void (*bind_framebuffer)(GLenum, unsigned int);
    static int resolved;

    if (!shared)
        return;
    if (!resolved) {
        resolved = 1;
        query = dlsym(RTLD_DEFAULT, "glXQueryDrawable");
        get_integerv = dlsym(RTLD_DEFAULT, "glGetIntegerv");
        read_buffer = dlsym(RTLD_DEFAULT, "glReadBuffer");
        pixel_storei = dlsym(RTLD_DEFAULT, "glPixelStorei");
        read_pixels = dlsym(RTLD_DEFAULT, "glReadPixels");
        void *(*proc)(const unsigned char *) = dlsym(RTLD_DEFAULT, "glXGetProcAddress");
        if (proc) {
            bind_buffer = proc((const unsigned char *)"glBindBuffer");
            bind_framebuffer = proc((const unsigned char *)"glBindFramebuffer");
        }
    }
    if (!query || !get_integerv || !read_buffer || !pixel_storei || !read_pixels)
        return;

    unsigned int w = 0, h = 0;
    query(dpy, drawable, GLX_WIDTH, &w);
    query(dpy, drawable, GLX_HEIGHT, &h);
    if (!w || !h || w > ATARIVCS_MAX_WIDTH || h > ATARIVCS_MAX_HEIGHT)
        return;

    /* What the game had set, put back after */
    GLint read_buf = 0, align = 4, row = 0, skip_rows = 0, skip_pixels = 0, pbo = 0, read_fbo = 0;
    get_integerv(GL_READ_BUFFER, &read_buf);
    get_integerv(GL_PACK_ALIGNMENT, &align);
    get_integerv(GL_PACK_ROW_LENGTH, &row);
    get_integerv(GL_PACK_SKIP_ROWS, &skip_rows);
    get_integerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    if (bind_buffer)
        get_integerv(GL_PIXEL_PACK_BUFFER_BINDING, &pbo);
    if (bind_framebuffer)
        get_integerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);

    if (pbo)
        bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    if (read_fbo)
        bind_framebuffer(GL_READ_FRAMEBUFFER, 0);
    read_buffer(GL_BACK);
    pixel_storei(GL_PACK_ALIGNMENT, 4);
    pixel_storei(GL_PACK_ROW_LENGTH, 0);
    pixel_storei(GL_PACK_SKIP_ROWS, 0);
    pixel_storei(GL_PACK_SKIP_PIXELS, 0);

    __atomic_add_fetch(&shared->frame_seq, 1, __ATOMIC_ACQ_REL); /* odd: writing */
    read_pixels(0, 0, (GLsizei)w, (GLsizei)h, GL_BGRA, GL_UNSIGNED_BYTE, shared->pixels);
    shared->width = w;
    shared->height = h;
    __atomic_add_fetch(&shared->frame_seq, 1, __ATOMIC_ACQ_REL); /* even: done */

    /* Every two seconds or so: what the reads give */
    static unsigned swaps;
    if (++swaps % 120 == 1) {
        static GLenum (*get_error)(void);
        if (!get_error)
            get_error = dlsym(RTLD_DEFAULT, "glGetError");
        unsigned nonzero = 0;
        for (size_t i = 0; i < (size_t)w * h * 4; i += 4093)
            nonzero += shared->pixels[i] != 0;
        fprintf(stderr, "atarivcs shim: swap %u, %ux%u, read buffer was 0x%x, fbo %d, error 0x%x, nonzero samples %u\n",
                swaps, w, h, (unsigned)read_buf, read_fbo, get_error ? get_error() : 0u, nonzero);
    }

    read_buffer((GLenum)read_buf);
    pixel_storei(GL_PACK_ALIGNMENT, align);
    pixel_storei(GL_PACK_ROW_LENGTH, row);
    pixel_storei(GL_PACK_SKIP_ROWS, skip_rows);
    pixel_storei(GL_PACK_SKIP_PIXELS, skip_pixels);
    if (pbo)
        bind_buffer(GL_PIXEL_PACK_BUFFER, (unsigned int)pbo);
    if (read_fbo)
        bind_framebuffer(GL_READ_FRAMEBUFFER, (unsigned int)read_fbo);
}

EXPORT void glXSwapBuffers(void *dpy, unsigned long drawable)
{
    static void (*real)(void *, unsigned long);
    if (!real)
        real = next("glXSwapBuffers");
    capture(dpy, drawable);
    if (real)
        real(dpy, drawable);
}

/* ---- Keyboard and pointer: left to RetroArch, which plays the pad. Focus
 * is let through: Bloo Kid waits for its FocusIn after setting it ---- */

#define GrabSuccess 0

EXPORT int XGrabKeyboard(void *dpy, unsigned long w, int owner, int pmode, int kmode, unsigned long time)
{
    (void)dpy; (void)w; (void)owner; (void)pmode; (void)kmode; (void)time;
    return GrabSuccess;
}

EXPORT int XGrabPointer(void *dpy, unsigned long w, int owner, unsigned int mask, int pmode, int kmode,
                        unsigned long confine, unsigned long cursor, unsigned long time)
{
    (void)dpy; (void)w; (void)owner; (void)mask; (void)pmode; (void)kmode; (void)confine; (void)cursor; (void)time;
    return GrabSuccess;
}


/* ---- Focus: RetroArch's window has it, the game's never does. Games that
 * stop while they are not focused (Bloo Kid waits in XNextEvent) are told
 * they have it: one FocusIn for each window they map, their FocusOut
 * dropped. ---- */

typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    void *display;
    unsigned long window;
    int mode;
    int detail;
} focus_event;
#define FocusIn 9
#define FocusOut 10
#define NotifyNormal 0
#define NotifyNonlinear 3

static unsigned long focus_window;
static int focus_owed;

EXPORT int XMapWindow(void *dpy, unsigned long w)
{
    static int (*real)(void *, unsigned long);
    if (!real)
        real = next("XMapWindow");
    focus_window = w;
    focus_owed = 1;
    return real ? real(dpy, w) : 0;
}

EXPORT int XMapRaised(void *dpy, unsigned long w)
{
    static int (*real)(void *, unsigned long);
    if (!real)
        real = next("XMapRaised");
    focus_window = w;
    focus_owed = 1;
    return real ? real(dpy, w) : 0;
}

static int owe_focus(void *dpy, void *ev)
{
    if (!focus_owed)
        return 0;
    focus_owed = 0;
    focus_event *f = ev;
    memset(ev, 0, 192); /* sizeof(XEvent) */
    f->type = FocusIn;
    f->send_event = 1;
    f->display = dpy;
    f->window = focus_window;
    f->mode = NotifyNormal;
    f->detail = NotifyNonlinear;
    return 1;
}

EXPORT int XNextEvent(void *dpy, void *ev)
{
    static int (*real)(void *, void *);
    if (!real)
        real = next("XNextEvent");
    if (owe_focus(dpy, ev))
        return 0;
    for (;;) {
        int r = real(dpy, ev);
        if (((focus_event *)ev)->type != FocusOut)
            return r;
    }
}

EXPORT int XPending(void *dpy)
{
    static int (*real)(void *);
    if (!real)
        real = next("XPending");
    return focus_owed + (real ? real(dpy) : 0);
}

EXPORT int XEventsQueued(void *dpy, int mode)
{
    static int (*real)(void *, int);
    if (!real)
        real = next("XEventsQueued");
    return focus_owed + (real ? real(dpy, mode) : 0);
}

/* ---- The pad: one SDL joystick, the core's ---- */

typedef struct { int dummy; } fake_joystick;
static fake_joystick the_pad;
#define SDL_JOYSTICK_TYPE_GAMECONTROLLER 1

EXPORT int SDL_NumJoysticks(void) { return shared ? 1 : 0; }
EXPORT void *SDL_JoystickOpen(int index) { return shared && index == 0 ? &the_pad : NULL; }
EXPORT void SDL_JoystickClose(void *j) { (void)j; }
EXPORT const char *SDL_JoystickName(void *j) { (void)j; return "Xbox 360 Controller"; }
EXPORT const char *SDL_JoystickNameForIndex(int i) { return i == 0 ? "Xbox 360 Controller" : NULL; }
EXPORT int SDL_JoystickGetType(void *j) { (void)j; return SDL_JOYSTICK_TYPE_GAMECONTROLLER; }
EXPORT int SDL_JoystickGetAttached(void *j) { return j == &the_pad; }
EXPORT int SDL_JoystickInstanceID(void *j) { return j == &the_pad ? 0 : -1; }
EXPORT int SDL_JoystickNumAxes(void *j) { (void)j; return ATARIVCS_AXIS_COUNT; }
EXPORT int SDL_JoystickNumButtons(void *j) { (void)j; return ATARIVCS_BTN_COUNT; }
EXPORT int SDL_JoystickNumHats(void *j) { (void)j; return 1; }
EXPORT void SDL_JoystickUpdate(void) {}
EXPORT void SDL_GameControllerUpdate(void) {}

EXPORT int16_t SDL_JoystickGetAxis(void *j, int axis)
{
    if (j != &the_pad || !shared || axis < 0 || axis >= ATARIVCS_AXIS_COUNT)
        return 0;
    return shared->axes[axis];
}

EXPORT uint8_t SDL_JoystickGetButton(void *j, int button)
{
    if (j != &the_pad || !shared || button < 0 || button >= ATARIVCS_BTN_COUNT)
        return 0;
    return (shared->buttons >> button) & 1;
}

EXPORT uint8_t SDL_JoystickGetHat(void *j, int hat)
{
    if (j != &the_pad || !shared || hat != 0)
        return 0;
    return shared->hat;
}
