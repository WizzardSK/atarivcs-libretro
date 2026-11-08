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
#include <time.h>
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

/* The dynamic linker's dlsym: this library exports its own (below), for the
 * games that dlopen their libraries (Unity: libGL, libX11) */
static void *(*real_dlsym)(void *, const char *);

static void find_real_dlsym(void)
{
    if (real_dlsym)
        return;
    real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!real_dlsym)
        real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    if (!real_dlsym)
        fprintf(stderr, "atarivcs shim: no real dlsym\n");
}

/* The library a game took its GL functions from; NULL for the global scope */
static void *gl_lib;

static void *lookup(void *lib, const char *symbol)
{
    find_real_dlsym();
    if (!real_dlsym)
        return NULL;
    void *f = lib ? real_dlsym(lib, symbol) : NULL;
    if (!f)
        f = real_dlsym(RTLD_NEXT, symbol);
    if (!f)
        f = real_dlsym(RTLD_DEFAULT, symbol);
    return f;
}

static void *real_of(const char *symbol);

static void *next(const char *symbol)
{
    void *f = real_of(symbol);
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
        query = lookup(gl_lib, "glXQueryDrawable");
        get_integerv = lookup(gl_lib, "glGetIntegerv");
        read_buffer = lookup(gl_lib, "glReadBuffer");
        pixel_storei = lookup(gl_lib, "glPixelStorei");
        read_pixels = lookup(gl_lib, "glReadPixels");
        void *(*proc)(const unsigned char *) = lookup(gl_lib, "glXGetProcAddress");
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
            get_error = lookup(gl_lib, "glGetError");
        unsigned nonzero = 0;
        for (size_t i = 0; i < (size_t)w * h * 4; i += 4093)
            nonzero += shared->pixels[i] != 0;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        static double last;
        const double t = now.tv_sec + now.tv_nsec / 1e9;
        fprintf(stderr, "atarivcs shim: swap %u, %ux%u, %.1f fps, read buffer was 0x%x, fbo %d, error 0x%x, nonzero samples %u\n",
                swaps, w, h, last ? 120.0 / (t - last) : 0.0, (unsigned)read_buf, read_fbo, get_error ? get_error() : 0u, nonzero);
        last = t;
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

/* Until RetroArch has run another frame; at most 100 ms, so a core that
 * went away does not hold the game for good */
static void wait_for_run(void)
{
    static uint32_t seen;
    if (!shared)
        return;
    for (int i = 0; i < 200; i++) {
        uint32_t now = __atomic_load_n(&shared->run_seq, __ATOMIC_ACQUIRE);
        if (now != seen) {
            seen = now;
            return;
        }
        usleep(500);
    }
}

EXPORT void glXSwapBuffers(void *dpy, unsigned long drawable)
{
    static void (*real)(void *, unsigned long);
    if (!real)
        real = next("glXSwapBuffers");
    capture(dpy, drawable);
    if (real)
        real(dpy, drawable);
    wait_for_run();
}

/* ---- Keyboard and pointer: left to RetroArch, which plays the pad ---- */

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


/* ---- The game's window. It is not mapped: the picture comes to RetroArch,
 * and the window would only stand beside it (ATARIVCS_SHOW_WINDOW=1 maps it
 * anyway). The game is told it was - MapNotify, Expose - and that it has the
 * focus, which RetroArch's window has: games that stop while not focused
 * (Bloo Kid waits in XNextEvent) go on. Its FocusOut events are dropped. ---- */

#define XEVENT_SIZE 192 /* sizeof(XEvent) on x86-64 */
#define MapNotify 19
#define Expose 12
#define FocusIn 9
#define FocusOut 10
#define NotifyNonlinear 3

typedef struct { int type; unsigned long serial; int send_event; void *display; unsigned long window; } any_event;
typedef struct { int type; unsigned long serial; int send_event; void *display; unsigned long event, window; int override_redirect; } map_event;
typedef struct { int type; unsigned long serial; int send_event; void *display; unsigned long window; int x, y, width, height, count; } expose_event;
typedef struct { int type; unsigned long serial; int send_event; void *display; unsigned long window; int mode, detail; } focus_event;

static union { char bytes[XEVENT_SIZE]; long align; } owed[32];
static int owed_count;

static void owe(int type, void *dpy, unsigned long w)
{
    if (owed_count == (int)(sizeof(owed) / sizeof(owed[0])))
        return;
    void *ev = owed[owed_count++].bytes;
    memset(ev, 0, XEVENT_SIZE);
    if (type == MapNotify) {
        map_event *m = ev;
        m->event = w;
        m->window = w;
    } else if (type == Expose) {
        ((expose_event *)ev)->window = w;
    } else {
        focus_event *f = ev;
        f->window = w;
        f->detail = NotifyNonlinear;
    }
    any_event *a = ev;
    a->type = type;
    a->send_event = 1;
    a->display = dpy;
}

static void pop_owed(int i, void *out)
{
    memcpy(out, owed[i].bytes, XEVENT_SIZE);
    memmove(&owed[i], &owed[i + 1], (size_t)(owed_count - i - 1) * sizeof(owed[0]));
    owed_count--;
}

static int show_window(void)
{
    const char *v = getenv("ATARIVCS_SHOW_WINDOW");
    return v && *v == '1';
}

/* The game's window, as far as focus goes */
static unsigned long game_window;

static int map_window(const char *name, void *dpy, unsigned long w)
{
    game_window = w;
    if (show_window()) {
        int (*real)(void *, unsigned long) = next(name);
        owe(FocusIn, dpy, w);
        return real ? real(dpy, w) : 0;
    }
    owe(MapNotify, dpy, w);
    owe(Expose, dpy, w);
    owe(FocusIn, dpy, w);
    return 1;
}

EXPORT int XMapWindow(void *dpy, unsigned long w) { return map_window("XMapWindow", dpy, w); }
EXPORT int XMapRaised(void *dpy, unsigned long w) { return map_window("XMapRaised", dpy, w); }

/* ---- The pad as keys too. Rewired in Breakout Recharged never moved the
 * paddle with the pad, and the game has a keyboard map: the D-pad (or the
 * left stick) comes as the arrow keys and A as space, KeyPress and
 * KeyRelease for the game's window as the pad changes. Read where the game
 * reads its events, so on its own thread. ---- */

#define KeyPress 2
#define KeyRelease 3
#define MotionNotify 6

typedef struct {
    int type; unsigned long serial; int send_event; void *display; unsigned long window, root, subwindow, time;
    int x, y, x_root, y_root; unsigned int state, keycode; int same_screen;
} key_event;

static const struct { unsigned long keysym; } pad_key_syms[] = {
    { 0xff51 /* Left */ }, { 0xff52 /* Up */ }, { 0xff53 /* Right */ }, { 0xff54 /* Down */ }, { 0x20 /* space */ },
};
#define PAD_KEY_COUNT (sizeof(pad_key_syms) / sizeof(pad_key_syms[0]))

static unsigned pad_keys_down(void)
{
    if (!shared)
        return 0;
    int hat = shared->hat, lx = shared->axes[ATARIVCS_AXIS_LX], ly = shared->axes[ATARIVCS_AXIS_LY];
    unsigned down = 0;
    if ((hat & ATARIVCS_HAT_LEFT) || lx < -16384) down |= 1u << 0;
    if ((hat & ATARIVCS_HAT_UP) || ly < -16384) down |= 1u << 1;
    if ((hat & ATARIVCS_HAT_RIGHT) || lx > 16384) down |= 1u << 2;
    if ((hat & ATARIVCS_HAT_DOWN) || ly > 16384) down |= 1u << 3;
    if (shared->buttons & (1u << ATARIVCS_BTN_A)) down |= 1u << 4;
    return down;
}

/* The pointer: x follows the core's mouse_x (RetroArch's mouse, the
 * Classic Controller's ring), kept in the window; y stays low in it, where
 * a paddle is */
static int pointer_x = 640, pointer_y = 600;

static void send_motion(void *dpy)
{
    static int32_t last;
    static int started;
    if (!shared)
        return;
    int32_t now = shared->mouse_x;
    if (!started) {
        last = now;
        started = 1;
        return;
    }
    if (now == last || owed_count == (int)(sizeof(owed) / sizeof(owed[0])))
        return;
    pointer_x += now - last;
    last = now;
    if (pointer_x < 0)
        pointer_x = 0;
    if (pointer_x > 1279)
        pointer_x = 1279;
    unsigned long (*root_of)(void *) = real_of("XDefaultRootWindow");
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    key_event *m = (key_event *)owed[owed_count++].bytes;
    memset(m, 0, XEVENT_SIZE);
    m->type = MotionNotify;
    m->display = dpy;
    m->window = game_window;
    m->root = root_of ? root_of(dpy) : 0;
    m->time = (unsigned long)ts.tv_sec * 1000 + (unsigned long)ts.tv_nsec / 1000000;
    m->x = m->x_root = pointer_x;
    m->y = m->y_root = pointer_y;
    m->same_screen = 1; /* is_hint stays 0, in keycode's place */
}

EXPORT int XQueryPointer(void *dpy, unsigned long w, unsigned long *root, unsigned long *child, int *root_x,
                         int *root_y, int *win_x, int *win_y, unsigned int *mask)
{
    int (*real)(void *, unsigned long, unsigned long *, unsigned long *, int *, int *, int *, int *, unsigned int *) =
        real_of("XQueryPointer");
    int r = real ? real(dpy, w, root, child, root_x, root_y, win_x, win_y, mask) : 1;
    if (game_window) {
        if (root_x) *root_x = pointer_x;
        if (root_y) *root_y = pointer_y;
        if (win_x) *win_x = pointer_x;
        if (win_y) *win_y = pointer_y;
        if (child) *child = 0;
    }
    return r;
}

static void send_keys(void *dpy)
{
    static unsigned was;
    static unsigned char codes[PAD_KEY_COUNT];
    static unsigned long root;
    if (!game_window || !dpy)
        return;
    send_motion(dpy);
    unsigned now = pad_keys_down(), changed = now ^ was;
    if (!changed)
        return;
    if (!codes[0]) {
        unsigned char (*to_code)(void *, unsigned long) = real_of("XKeysymToKeycode");
        unsigned long (*root_of)(void *) = real_of("XDefaultRootWindow");
        for (unsigned i = 0; i < PAD_KEY_COUNT && to_code; i++)
            codes[i] = to_code(dpy, pad_key_syms[i].keysym);
        root = root_of ? root_of(dpy) : 0;
        if (!codes[0]) {
            was = now;
            return;
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    for (unsigned i = 0; i < PAD_KEY_COUNT; i++) {
        if (!(changed & (1u << i)) || !codes[i] || owed_count == (int)(sizeof(owed) / sizeof(owed[0])))
            continue;
        key_event *k = (key_event *)owed[owed_count++].bytes;
        memset(k, 0, XEVENT_SIZE);
        k->type = (now & (1u << i)) ? KeyPress : KeyRelease;
        k->display = dpy;
        k->window = game_window;
        k->root = root;
        k->time = (unsigned long)ts.tv_sec * 1000 + (unsigned long)ts.tv_nsec / 1000000;
        k->keycode = codes[i];
        k->same_screen = 1;
        was ^= 1u << i;
    }
}

EXPORT int XNextEvent(void *dpy, void *ev)
{
    static int (*real)(void *, void *);
    if (!real)
        real = next("XNextEvent");
    send_keys(dpy);
    if (owed_count) {
        pop_owed(0, ev);
        return 0;
    }
    for (;;) {
        int r = real(dpy, ev);
        if (((any_event *)ev)->type != FocusOut)
            return r;
    }
}

EXPORT int XPeekEvent(void *dpy, void *ev)
{
    static int (*real)(void *, void *);
    if (!real)
        real = next("XPeekEvent");
    if (owed_count) {
        memcpy(ev, owed[0].bytes, XEVENT_SIZE);
        return 0;
    }
    return real ? real(dpy, ev) : 0;
}

typedef int (*predicate_fn)(void *, void *, char *);

EXPORT int XPeekIfEvent(void *dpy, void *ev, predicate_fn pred, char *arg)
{
    static int (*real)(void *, void *, predicate_fn, char *);
    if (!real)
        real = next("XPeekIfEvent");
    for (int i = 0; i < owed_count; i++)
        if (pred(dpy, owed[i].bytes, arg)) {
            memcpy(ev, owed[i].bytes, XEVENT_SIZE);
            return 0;
        }
    return real ? real(dpy, ev, pred, arg) : 0;
}

EXPORT int XIfEvent(void *dpy, void *ev, predicate_fn pred, char *arg)
{
    static int (*real)(void *, void *, predicate_fn, char *);
    if (!real)
        real = next("XIfEvent");
    for (int i = 0; i < owed_count; i++)
        if (pred(dpy, owed[i].bytes, arg)) {
            pop_owed(i, ev);
            return 0;
        }
    return real ? real(dpy, ev, pred, arg) : 0;
}

EXPORT int XPending(void *dpy)
{
    static int (*real)(void *);
    if (!real)
        real = next("XPending");
    send_keys(dpy);
    return owed_count + (real ? real(dpy) : 0);
}

EXPORT int XEventsQueued(void *dpy, int mode)
{
    static int (*real)(void *, int);
    if (!real)
        real = next("XEventsQueued");
    send_keys(dpy);
    return owed_count + (real ? real(dpy, mode) : 0);
}

/* The other ways a game asks about focus: XGetInputFocus, and the window
 * manager's _NET_ACTIVE_WINDOW (on the root) and _NET_WM_STATE_FOCUSED (on
 * its window). Unity asks them, and Rewired drops its pads' input while
 * Unity's window is not focused - Breakout Recharged's menus took the pad
 * through Unity's own input, its paddle (Rewired) never did. */
EXPORT int XGetInputFocus(void *dpy, unsigned long *focus, int *revert)
{
    int (*real)(void *, unsigned long *, int *) = real_of("XGetInputFocus");
    int r = real ? real(dpy, focus, revert) : 1;
    if (game_window && focus)
        *focus = game_window;
    return r;
}

EXPORT int XGetWindowProperty(void *dpy, unsigned long w, unsigned long property, long offset, long length,
                              int del, unsigned long req_type, unsigned long *actual_type, int *actual_format,
                              unsigned long *nitems, unsigned long *bytes_after, unsigned char **prop)
{
    int (*real)(void *, unsigned long, unsigned long, long, long, int, unsigned long, unsigned long *, int *,
                unsigned long *, unsigned long *, unsigned char **) = real_of("XGetWindowProperty");
    unsigned long (*intern)(void *, const char *, int) = real_of("XInternAtom");
    if (game_window && intern) {
        const unsigned long active = intern(dpy, "_NET_ACTIVE_WINDOW", 0);
        const unsigned long state = intern(dpy, "_NET_WM_STATE", 0);
        if ((property == active && w != game_window) || (property == state && w == game_window)) {
            unsigned long *value = malloc(sizeof(unsigned long));
            if (value) {
                *value = property == active ? game_window : intern(dpy, "_NET_WM_STATE_FOCUSED", 0);
                *actual_type = property == active ? 33 /* XA_WINDOW */ : 4 /* XA_ATOM */;
                *actual_format = 32;
                *nitems = 1;
                *bytes_after = 0;
                *prop = (unsigned char *)value;
                return 0; /* Success */
            }
        }
    }
    return real ? real(dpy, w, property, offset, length, del, req_type, actual_type, actual_format, nitems,
                       bytes_after, prop)
                : 1;
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

/* ---- The pad for games with their own SDL inside (Unity): an evdev device
 * at a path no real one has, named to their SDL in SDL_JOYSTICK_DEVICE (the
 * core sets it). It answers stat, open, ioctl and read as an Xbox 360 pad
 * (xpad) would; the fd behind it is /dev/null's. ---- */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/input.h>

#include "shim_pad.h"

static const unsigned pad_keys[ATARIVCS_BTN_COUNT] = {
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR,
};
static const unsigned pad_abs[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y };

typedef struct {
    int fd;
    int flags;
    unsigned reads, with_events;
    uint32_t buttons;
    int32_t abs[ABS_MAX + 1];
} pad_fd;
static pad_fd pad_fds[8];

static pad_fd *pad_of(int fd)
{
    if (fd < 0)
        return NULL;
    for (size_t i = 0; i < sizeof(pad_fds) / sizeof(pad_fds[0]); i++)
        if (pad_fds[i].fd == fd + 1)
            return &pad_fds[i];
    return NULL;
}

/* The pad now, as evdev values */
static int32_t pad_abs_value(unsigned code)
{
    if (!shared)
        return code == ABS_Z || code == ABS_RZ ? 0 : 0;
    switch (code) {
    case ABS_X: return shared->axes[ATARIVCS_AXIS_LX];
    case ABS_Y: return shared->axes[ATARIVCS_AXIS_LY];
    case ABS_RX: return shared->axes[ATARIVCS_AXIS_RX];
    case ABS_RY: return shared->axes[ATARIVCS_AXIS_RY];
    case ABS_Z: return (shared->axes[ATARIVCS_AXIS_LT] + 32768) >> 8;
    case ABS_RZ: return (shared->axes[ATARIVCS_AXIS_RT] + 32768) >> 8;
    case ABS_HAT0X: return (shared->hat & ATARIVCS_HAT_RIGHT) ? 1 : (shared->hat & ATARIVCS_HAT_LEFT) ? -1 : 0;
    case ABS_HAT0Y: return (shared->hat & ATARIVCS_HAT_DOWN) ? 1 : (shared->hat & ATARIVCS_HAT_UP) ? -1 : 0;
    }
    return 0;
}

static int is_pad_path(const char *path)
{
    if (!shared || !path || strcmp(path, PAD_PATH))
        return 0;
    static int logged;
    if (logged++ < 8)
        fprintf(stderr, "atarivcs shim: the game looks at %s\n", path);
    return 1;
}

static void fill_pad_stat(struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0660;
    st->st_rdev = makedev(PAD_MAJOR, PAD_MINOR);
    st->st_nlink = 1;
}

EXPORT int __xstat(int ver, const char *path, struct stat *st)
{
    static int (*real)(int, const char *, struct stat *);
    if (is_pad_path(path)) {
        fill_pad_stat(st);
        return 0;
    }
    if (!real)
        real = lookup(NULL, "__xstat");
    return real ? real(ver, path, st) : -1;
}

EXPORT int __xstat64(int ver, const char *path, struct stat *st)
{
    static int (*real)(int, const char *, struct stat *);
    if (is_pad_path(path)) {
        fill_pad_stat(st);
        return 0;
    }
    if (!real)
        real = lookup(NULL, "__xstat64");
    return real ? real(ver, path, st) : -1;
}

EXPORT int stat(const char *path, struct stat *st)
{
    static int (*real)(const char *, struct stat *);
    if (is_pad_path(path)) {
        fill_pad_stat(st);
        return 0;
    }
    if (!real)
        real = lookup(NULL, "stat");
    return real ? real(path, st) : -1;
}

static int open_pad(int flags)
{
    static int (*real)(const char *, int, ...);
    if (!real)
        real = lookup(NULL, "open");
    int fd = real ? real("/dev/null", (flags & ~(O_CREAT | O_TRUNC)) | O_CLOEXEC) : -1;
    if (fd < 0)
        return fd;
    for (size_t i = 0; i < sizeof(pad_fds) / sizeof(pad_fds[0]); i++)
        if (!pad_fds[i].fd) {
            memset(&pad_fds[i], 0, sizeof(pad_fds[i]));
            pad_fds[i].fd = fd + 1;
            pad_fds[i].flags = flags;
            fprintf(stderr, "atarivcs shim: pad opened as fd %d (slot %zu, flags 0x%x)\n", fd, i, (unsigned)flags);
            for (size_t a = 0; a < sizeof(pad_abs) / sizeof(pad_abs[0]); a++)
                pad_fds[i].abs[pad_abs[a]] = pad_abs_value(pad_abs[a]);
            pad_fds[i].buttons = shared->buttons;
            return fd;
        }
    return fd;
}

EXPORT int open(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    if (is_pad_path(path))
        return open_pad(flags);
    if (!real)
        real = lookup(NULL, "open");
    return real ? real(path, flags, mode) : -1;
}

EXPORT int open64(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    if (is_pad_path(path))
        return open_pad(flags);
    if (!real)
        real = lookup(NULL, "open64");
    return real ? real(path, flags, mode) : -1;
}

EXPORT int close(int fd)
{
    static int (*real)(int);
    pad_fd *p = pad_of(fd);
    if (p) {
        fprintf(stderr, "atarivcs shim: pad fd %d closed after %u reads\n", fd, p->reads);
        p->fd = 0;
    }
    if (!real)
        real = lookup(NULL, "close");
    return real ? real(fd) : -1;
}

static void set_bit(unsigned char *bits, size_t len, unsigned bit)
{
    if (bit / 8 < len)
        bits[bit / 8] |= (unsigned char)(1u << (bit % 8));
}

static int pad_ioctl(unsigned long req, void *arg)
{
    const unsigned nr = _IOC_NR(req), size = _IOC_SIZE(req);
    static int logged;
    if (logged++ < 40)
        fprintf(stderr, "atarivcs shim: pad ioctl type %c nr 0x%x size %u\n", (char)_IOC_TYPE(req), nr, size);
    if (_IOC_TYPE(req) != 'E')
        goto unsupported;
    if (req == EVIOCGVERSION) {
        *(int *)arg = EV_VERSION;
        return 0;
    }
    if (req == EVIOCGID) {
        struct input_id *id = arg;
        id->bustype = BUS_USB;
        id->vendor = 0x045e;
        id->product = 0x028e;
        id->version = 0x0114;
        return 0;
    }
    if (nr == _IOC_NR(EVIOCGNAME(0))) {
        const char *name = "Microsoft X-Box 360 pad";
        size_t n = strlen(name) + 1 < size ? strlen(name) + 1 : size;
        memcpy(arg, name, n);
        return (int)n;
    }
    if (nr == _IOC_NR(EVIOCGPHYS(0)) || nr == _IOC_NR(EVIOCGUNIQ(0))) {
        if (size)
            *(char *)arg = '\0';
        return 0;
    }
    if (nr == _IOC_NR(EVIOCGPROP(0)) || nr == _IOC_NR(EVIOCGLED(0)) || nr == _IOC_NR(EVIOCGSND(0)) ||
        nr == _IOC_NR(EVIOCGSW(0))) {
        memset(arg, 0, size);
        return (int)size;
    }
    if (nr == _IOC_NR(EVIOCGKEY(0))) {
        memset(arg, 0, size);
        for (unsigned b = 0; b < ATARIVCS_BTN_COUNT; b++)
            if (shared && (shared->buttons >> b) & 1)
                set_bit(arg, size, pad_keys[b]);
        return (int)size;
    }
    if (nr >= 0x20 && nr < 0x40) { /* EVIOCGBIT(ev, len) */
        const unsigned ev = nr - 0x20;
        memset(arg, 0, size);
        if (ev == 0) {
            set_bit(arg, size, EV_SYN);
            set_bit(arg, size, EV_KEY);
            set_bit(arg, size, EV_ABS);
        } else if (ev == EV_KEY) {
            for (unsigned b = 0; b < ATARIVCS_BTN_COUNT; b++)
                set_bit(arg, size, pad_keys[b]);
        } else if (ev == EV_ABS) {
            for (size_t a = 0; a < sizeof(pad_abs) / sizeof(pad_abs[0]); a++)
                set_bit(arg, size, pad_abs[a]);
        }
        return (int)size;
    }
    if (nr >= 0x40 && nr < 0x40 + ABS_CNT && _IOC_DIR(req) == _IOC_READ) { /* EVIOCGABS(abs) */
        const unsigned code = nr - 0x40;
        struct input_absinfo *info = arg;
        memset(info, 0, sizeof(*info));
        info->value = pad_abs_value(code);
        if (code == ABS_Z || code == ABS_RZ) {
            info->maximum = 255;
        } else if (code == ABS_HAT0X || code == ABS_HAT0Y) {
            info->minimum = -1;
            info->maximum = 1;
        } else {
            info->minimum = -32768;
            info->maximum = 32767;
            info->fuzz = 16;
            info->flat = 128;
        }
        return 0;
    }
    if (req == EVIOCGRAB)
        return 0;
    if (req == EVIOCGEFFECTS) {
        *(int *)arg = 0;
        return 0;
    }
unsupported:
    errno = ENOTTY;
    return -1;
}

EXPORT int ioctl(int fd, unsigned long req, ...)
{
    static int (*real)(int, unsigned long, ...);
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (pad_of(fd))
        return pad_ioctl(req, arg);
    if (!real)
        real = lookup(NULL, "ioctl");
    return real ? real(fd, req, arg) : -1;
}

/* What changed since the last read, as input events with a SYN_REPORT */
EXPORT ssize_t read(int fd, void *buf, size_t count)
{
    static ssize_t (*real)(int, void *, size_t);
    pad_fd *p = pad_of(fd);
    if (!p) {
        if (!real)
            real = lookup(NULL, "read");
        return real ? real(fd, buf, count) : -1;
    }
    struct input_event ev[ATARIVCS_BTN_COUNT + 8 + 1];
    size_t n = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint32_t buttons = shared ? shared->buttons : 0;
    for (unsigned b = 0; b < ATARIVCS_BTN_COUNT; b++)
        if (((buttons ^ p->buttons) >> b) & 1) {
            memset(&ev[n], 0, sizeof(ev[n]));
            ev[n].type = EV_KEY;
            ev[n].code = (uint16_t)pad_keys[b];
            ev[n].value = (buttons >> b) & 1;
            n++;
        }
    p->buttons = buttons;
    for (size_t a = 0; a < sizeof(pad_abs) / sizeof(pad_abs[0]); a++) {
        const int32_t v = pad_abs_value(pad_abs[a]);
        if (v != p->abs[pad_abs[a]]) {
            memset(&ev[n], 0, sizeof(ev[n]));
            ev[n].type = EV_ABS;
            ev[n].code = (uint16_t)pad_abs[a];
            ev[n].value = v;
            n++;
            p->abs[pad_abs[a]] = v;
        }
    }
    p->reads++;
    p->with_events += n != 0;
    if (p->reads % 600 == 1 || (n && p->with_events <= 3))
        fprintf(stderr, "atarivcs shim: pad fd %d: reads %u, with events %u\n", fd, p->reads, p->with_events);
    if (!n) {
        errno = EAGAIN;
        return -1;
    }
    memset(&ev[n], 0, sizeof(ev[n]));
    ev[n].type = EV_SYN;
    ev[n].code = SYN_REPORT;
    n++;
    for (size_t i = 0; i < n; i++) {
        ev[i].input_event_sec = now.tv_sec;
        ev[i].input_event_usec = now.tv_nsec / 1000;
    }
    size_t bytes = n * sizeof(ev[0]);
    if (bytes > count)
        bytes = count - count % sizeof(ev[0]); /* the rest comes with the next read */
    memcpy(buf, ev, bytes);
    return (ssize_t)bytes;
}

/* ---- A smaller screen: the game is told the display is ATARIVCS_SCREEN
 * (WxH, set by the core) wide, through XRandR (Unity's "native desktop
 * resolution") and the root window's attributes. It then draws, and the
 * shim reads back, that many pixels: at 1080p Breakout Recharged's menus
 * and play ran at 13 fps under box64 on a Chromebook. ---- */

typedef struct {
    unsigned long id;
    unsigned int width, height;
    unsigned long dotClock;
    unsigned int hSyncStart, hSyncEnd, hTotal, hSkew, vSyncStart, vSyncEnd, vTotal;
    char *name;
    unsigned int nameLength;
    unsigned long modeFlags;
} rr_mode_info;

typedef struct {
    unsigned long timestamp, configTimestamp;
    int ncrtc;
    unsigned long *crtcs;
    int noutput;
    unsigned long *outputs;
    int nmode;
    rr_mode_info *modes;
} rr_screen_resources;

typedef struct {
    unsigned long timestamp;
    int x, y;
    unsigned int width, height;
    unsigned long mode;
} rr_crtc_info_head;

typedef struct {
    int width, height, mwidth, mheight;
} rr_screen_size;

typedef struct {
    int x, y, width, height, border_width, depth;
    void *visual;
    unsigned long root;
} window_attributes_head;

/* The size to tell, 0 for the real one */
static void screen_size(unsigned *w, unsigned *h)
{
    static int parsed;
    static unsigned sw, sh;
    if (!parsed) {
        parsed = 1;
        const char *v = getenv("ATARIVCS_SCREEN");
        if (!v || sscanf(v, "%ux%u", &sw, &sh) != 2)
            sw = sh = 0;
    }
    *w = sw;
    *h = sh;
}

static void shrink_resources(rr_screen_resources *r)
{
    unsigned w, h;
    screen_size(&w, &h);
    if (!r || !w)
        return;
    for (int i = 0; i < r->nmode; i++) {
        r->modes[i].width = w;
        r->modes[i].height = h;
    }
}

EXPORT void *XRRGetScreenResources(void *dpy, unsigned long window)
{
    void *(*real)(void *, unsigned long) = real_of("XRRGetScreenResources");
    void *r = real ? real(dpy, window) : NULL;
    shrink_resources(r);
    return r;
}

EXPORT void *XRRGetScreenResourcesCurrent(void *dpy, unsigned long window)
{
    void *(*real)(void *, unsigned long) = real_of("XRRGetScreenResourcesCurrent");
    void *r = real ? real(dpy, window) : NULL;
    shrink_resources(r);
    return r;
}

EXPORT void *XRRGetCrtcInfo(void *dpy, void *resources, unsigned long crtc)
{
    void *(*real)(void *, void *, unsigned long) = real_of("XRRGetCrtcInfo");
    rr_crtc_info_head *c = real ? real(dpy, resources, crtc) : NULL;
    unsigned w, h;
    screen_size(&w, &h);
    if (c && w && c->width && c->height) {
        static int told;
        if (!told++)
            fprintf(stderr, "atarivcs shim: the screen is %ux%u to the game, not %ux%u\n", w, h, c->width, c->height);
        c->width = w;
        c->height = h;
    }
    return c;
}

EXPORT rr_screen_size *XRRConfigSizes(void *config, int *nsizes)
{
    rr_screen_size *(*real)(void *, int *) = real_of("XRRConfigSizes");
    rr_screen_size *sizes = real ? real(config, nsizes) : NULL;
    unsigned w, h;
    screen_size(&w, &h);
    if (sizes && nsizes && w)
        for (int i = 0; i < *nsizes; i++) {
            sizes[i].width = (int)w;
            sizes[i].height = (int)h;
        }
    return sizes;
}

EXPORT int XGetWindowAttributes(void *dpy, unsigned long window, void *attrs)
{
    int (*real)(void *, unsigned long, void *) = real_of("XGetWindowAttributes");
    int r = real ? real(dpy, window, attrs) : 0;
    window_attributes_head *a = attrs;
    unsigned w, h;
    screen_size(&w, &h);
    if (r && a && w && a->root == window) {
        a->width = (int)w;
        a->height = (int)h;
    }
    return r;
}

/* ---- dlsym: the functions above, also for games that look them up in the
 * libraries they dlopen (Unity). The real ones are kept for the hooks. ---- */

EXPORT void *glXGetProcAddress(const unsigned char *name);
EXPORT void *glXGetProcAddressARB(const unsigned char *name);

static struct {
    const char *name;
    void *mine;
    void *real;
    int gl;
} hooks[] = {
    { "glXSwapBuffers", (void *)glXSwapBuffers, NULL, 1 },
    { "glXGetProcAddress", (void *)glXGetProcAddress, NULL, 1 },
    { "glXGetProcAddressARB", (void *)glXGetProcAddressARB, NULL, 1 },
    { "XGrabKeyboard", (void *)XGrabKeyboard, NULL, 0 },
    { "XGrabPointer", (void *)XGrabPointer, NULL, 0 },
    { "XMapWindow", (void *)XMapWindow, NULL, 0 },
    { "XMapRaised", (void *)XMapRaised, NULL, 0 },
    { "XNextEvent", (void *)XNextEvent, NULL, 0 },
    { "XPeekEvent", (void *)XPeekEvent, NULL, 0 },
    { "XPeekIfEvent", (void *)XPeekIfEvent, NULL, 0 },
    { "XIfEvent", (void *)XIfEvent, NULL, 0 },
    { "XPending", (void *)XPending, NULL, 0 },
    { "XEventsQueued", (void *)XEventsQueued, NULL, 0 },
    /* The pad's files, for code that looks libc's functions up (Mono's
     * P/Invoke) */
    { "open", (void *)open, NULL, 0 },
    { "open64", (void *)open64, NULL, 0 },
    { "close", (void *)close, NULL, 0 },
    { "read", (void *)read, NULL, 0 },
    { "ioctl", (void *)ioctl, NULL, 0 },
    { "__xstat", (void *)__xstat, NULL, 0 },
    { "__xstat64", (void *)__xstat64, NULL, 0 },
    { "stat", (void *)stat, NULL, 0 },
    { "dlopen", (void *)dlopen, NULL, 0 },
    { "dlclose", (void *)dlclose, NULL, 0 },
    { "XRRGetScreenResources", (void *)XRRGetScreenResources, NULL, 0 },
    { "XRRGetScreenResourcesCurrent", (void *)XRRGetScreenResourcesCurrent, NULL, 0 },
    { "XRRGetCrtcInfo", (void *)XRRGetCrtcInfo, NULL, 0 },
    { "XRRConfigSizes", (void *)XRRConfigSizes, NULL, 0 },
    { "XGetWindowAttributes", (void *)XGetWindowAttributes, NULL, 0 },
    { "XGetInputFocus", (void *)XGetInputFocus, NULL, 0 },
    { "XQueryPointer", (void *)XQueryPointer, NULL, 0 },
    { "XGetWindowProperty", (void *)XGetWindowProperty, NULL, 0 },
    { "XInternAtom", NULL, NULL, 0 },
};

static void *real_of(const char *symbol)
{
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
        if (!strcmp(hooks[i].name, symbol)) {
            if (!hooks[i].real)
                hooks[i].real = lookup(NULL, symbol);
            return hooks[i].real;
        }
    return lookup(NULL, symbol);
}

/* A looked-up function the shim has its own of: the real one kept, the
 * shim's returned (entries without one of the shim's only keep the real one,
 * for the shim's own use) */
static void *swap_in(void *lib, const char *name, void *found)
{
    if (!found || !name)
        return found;
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++)
        if (!strcmp(hooks[i].name, name)) {
            if (found == hooks[i].mine)
                return found;
            if (!hooks[i].real)
                hooks[i].real = found;
            if (hooks[i].gl && lib)
                gl_lib = lib;
            /* No function of the shim's: only the real one is wanted */
            return hooks[i].mine ? hooks[i].mine : found;
        }
    return found;
}

/* libudev, when the core asks (ATARIVCS_FAKE_UDEV), is the shim's own
 * (shim_udev.c), which knows the shim's pad only: Rewired's native Linux
 * input finds its pads through udev alone, and the real one knows nothing
 * of the shim's */
EXPORT void *dlopen(const char *file, int flags)
{
    static void *(*real)(const char *, int);
    if (!real) {
        real = dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.34");
        if (!real)
            real = dlvsym(RTLD_NEXT, "dlopen", "GLIBC_2.2.5");
    }
    if (file && strstr(file, "libudev") && getenv("ATARIVCS_FAKE_UDEV")) {
        static int told;
        if (!told++)
            fprintf(stderr, "atarivcs shim: the shim's udev for %s\n", file);
        return &atarivcs_fake_udev_handle;
    }
    /* Rewired asks for libudev.so.0 by name, which today's systems (and
     * box64's wrappers) only have as libudev.so.1 */
    if (file && strstr(file, "libudev.so.0")) {
        void *h = real ? real(file, flags) : NULL;
        if (!h && real)
            h = real("libudev.so.1", flags);
        return h;
    }
    return real ? real(file, flags) : NULL;
}

EXPORT int dlclose(void *lib)
{
    static int (*real)(void *);
    if (lib == &atarivcs_fake_udev_handle)
        return 0;
    if (!real) {
        real = dlvsym(RTLD_NEXT, "dlclose", "GLIBC_2.34");
        if (!real)
            real = dlvsym(RTLD_NEXT, "dlclose", "GLIBC_2.2.5");
    }
    return real ? real(lib) : 0;
}

EXPORT void *dlsym(void *lib, const char *name)
{
    if (lib == &atarivcs_fake_udev_handle)
        return atarivcs_fake_udev_symbol(name);
    find_real_dlsym();
    if (!real_dlsym)
        return NULL;
    return swap_in(lib, name, real_dlsym(lib, name));
}

EXPORT void *glXGetProcAddress(const unsigned char *name)
{
    void *(*real)(const unsigned char *) = real_of("glXGetProcAddress");
    return swap_in(NULL, (const char *)name, real ? real(name) : NULL);
}

EXPORT void *glXGetProcAddressARB(const unsigned char *name)
{
    void *(*real)(const unsigned char *) = real_of("glXGetProcAddressARB");
    return swap_in(NULL, (const char *)name, real ? real(name) : NULL);
}
