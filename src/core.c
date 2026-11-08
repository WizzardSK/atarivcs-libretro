/* Atari VCS (2021) games in RetroArch. The game is a Linux x86-64 program;
 * the core starts it as a child process - directly on x86-64, through box64
 * elsewhere - with the shim preloaded, shows the frames the shim hands over
 * and gives the game RetroArch's pad. The game keeps its own time and sound:
 * no pause of its own, no savestates. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "libretro.h"
#include "shared.h"

#define SHIM_NAME "atarivcs_shim_x86_64.so"
#define SAMPLE_RATE 48000
#define FPS 60

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static atarivcs_shared *shared;
static char shm_name[64];
static pid_t child = -1;
static uint32_t last_seq;
static unsigned cur_w = 1280, cur_h = 720;
static uint32_t *frame; /* top down, XRGB8888 */
static bool have_frame;

static void logf_(enum retro_log_level level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_cb)
        log_cb(level, "%s", buf);
    else
        fputs(buf, stderr);
}
#define LOG_I(...) logf_(RETRO_LOG_INFO, __VA_ARGS__)
#define LOG_E(...) logf_(RETRO_LOG_ERROR, __VA_ARGS__)

/* ---- libretro boilerplate ---- */

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "Atari VCS";
#ifndef GIT_VERSION
#define GIT_VERSION "dev"
#endif
    info->library_version = GIT_VERSION;
    info->valid_extensions = "ini";
    info->need_fullpath = true;
    info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = cur_w;
    info->geometry.base_height = cur_h;
    info->geometry.max_width = 1920;
    info->geometry.max_height = 1080;
    info->geometry.aspect_ratio = (float)cur_w / (float)cur_h;
    info->timing.fps = FPS;
    info->timing.sample_rate = SAMPLE_RATE;
}

void retro_init(void) {}
void retro_deinit(void) {}
void retro_reset(void) {}
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

/* ---- Finding the game, the shim and box64 ---- */

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Exec= of a bundle.ini, '\r' and all trimmed */
static bool read_exec(const char *ini, char *exec, size_t size)
{
    FILE *f = fopen(ini, "r");
    if (!f)
        return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Exec=", 5) != 0)
            continue;
        char *v = line + 5;
        v[strcspn(v, "\r\n")] = '\0';
        snprintf(exec, size, "%s", v);
        found = true;
        break;
    }
    fclose(f);
    return found;
}

/* The folder next to this core's library: the shim lives beside it */
static void core_dir(char *out, size_t size)
{
    Dl_info info;
    out[0] = '\0';
    if (dladdr((void *)core_dir, &info) && info.dli_fname) {
        snprintf(out, size, "%s", info.dli_fname);
        char *slash = strrchr(out, '/');
        if (slash)
            *slash = '\0';
    }
}

static bool find_shim(char *out, size_t size)
{
    char dir[PATH_MAX];
    core_dir(dir, sizeof(dir));
    snprintf(out, size, "%s/%s", dir, SHIM_NAME);
    if (is_file(out))
        return true;
    const char *system_dir = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir) {
        snprintf(out, size, "%s/atarivcs/%s", system_dir, SHIM_NAME);
        if (is_file(out))
            return true;
    }
    return false;
}

#if !defined(__x86_64__)
/* box64: ATARIVCS_BOX64, the one gameflix sets up, else PATH */
static bool find_box64(char *out, size_t size, char *libs, size_t libs_size)
{
    const char *home = getenv("HOME");
    libs[0] = '\0';
    const char *env = getenv("ATARIVCS_BOX64");
    if (env && is_file(env)) {
        snprintf(out, size, "%s", env);
        return true;
    }
    if (home) {
        snprintf(out, size, "%s/.cache/gameflix/box64/usr/bin/box64", home);
        if (is_file(out)) {
            snprintf(libs, libs_size, "%s/.cache/gameflix/box64/x86/usr/lib/x86_64-linux-gnu", home);
            return true;
        }
    }
    const char *path = getenv("PATH");
    if (path) {
        char copy[4096];
        snprintf(copy, sizeof(copy), "%s", path);
        for (char *dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
            snprintf(out, size, "%s/box64", dir);
            if (is_file(out))
                return true;
        }
    }
    return false;
}
#endif

/* ---- Shared memory ---- */

static bool create_shared(void)
{
    snprintf(shm_name, sizeof(shm_name), "/atarivcs-%d", (int)getpid());
    shm_unlink(shm_name);
    int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        LOG_E("atarivcs: shm_open %s: %s\n", shm_name, strerror(errno));
        return false;
    }
    if (ftruncate(fd, sizeof(atarivcs_shared)) != 0) {
        LOG_E("atarivcs: ftruncate: %s\n", strerror(errno));
        close(fd);
        shm_unlink(shm_name);
        return false;
    }
    void *p = mmap(NULL, sizeof(atarivcs_shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        LOG_E("atarivcs: mmap: %s\n", strerror(errno));
        shm_unlink(shm_name);
        return false;
    }
    shared = p;
    shared->magic = ATARIVCS_MAGIC;
    shared->version = ATARIVCS_VERSION;
    shared->axes[ATARIVCS_AXIS_LT] = -32768;
    shared->axes[ATARIVCS_AXIS_RT] = -32768;
    return true;
}

static void destroy_shared(void)
{
    if (shared) {
        munmap(shared, sizeof(atarivcs_shared));
        shared = NULL;
    }
    if (shm_name[0]) {
        shm_unlink(shm_name);
        shm_name[0] = '\0';
    }
}

/* ---- The game ---- */

static void stop_child(void)
{
    if (child <= 0)
        return;
    /* The game and what it started (box64 runs it in the same process) */
    kill(-child, SIGTERM);
    for (int i = 0; i < 30; i++) {
        if (waitpid(child, NULL, WNOHANG) == child) {
            child = -1;
            return;
        }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(-child, SIGKILL);
    waitpid(child, NULL, 0);
    child = -1;
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || !game->path)
        return false;

    /* The bundle.ini, its folder, or a program in that folder */
    char dir[PATH_MAX], ini[PATH_MAX], exec[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", game->path);
    if (!is_dir(dir)) {
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = '\0';
    }
    snprintf(ini, sizeof(ini), "%s/bundle.ini", dir);
    if (!read_exec(ini, exec, sizeof(exec))) {
        LOG_E("atarivcs: no Exec= in %s\n", ini);
        return false;
    }
    char program[PATH_MAX * 2];
    snprintf(program, sizeof(program), "%s/%s", dir, exec);
    if (!is_file(program)) {
        LOG_E("atarivcs: %s is not there\n", program);
        return false;
    }
    chmod(program, 0755);

    char shim[PATH_MAX];
    if (!find_shim(shim, sizeof(shim))) {
        LOG_E("atarivcs: %s is not beside the core nor in system/atarivcs\n", SHIM_NAME);
        return false;
    }

#if !defined(__x86_64__)
    char box64[PATH_MAX], libs[PATH_MAX];
    if (!find_box64(box64, sizeof(box64), libs, sizeof(libs))) {
        LOG_E("atarivcs: box64 not found (set ATARIVCS_BOX64, or let gameflix set it up)\n");
        return false;
    }
#endif

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
        return false;

    if (!create_shared())
        return false;
    frame = calloc((size_t)ATARIVCS_MAX_WIDTH * ATARIVCS_MAX_HEIGHT, 4);
    if (!frame) {
        destroy_shared();
        return false;
    }

    bool script = false;
    {
        FILE *f = fopen(program, "rb");
        char magic[2] = { 0 };
        if (f) {
            script = fread(magic, 1, 2, f) == 2 && magic[0] == '#' && magic[1] == '!';
            fclose(f);
        }
    }

    child = fork();
    if (child < 0) {
        LOG_E("atarivcs: fork: %s\n", strerror(errno));
        destroy_shared();
        return false;
    }
    if (child == 0) {
        setpgid(0, 0);
        if (chdir(dir) != 0)
            _exit(127);
        setenv("ATARIVCS_SHM", shm_name, 1);
#if defined(__x86_64__)
        setenv("LD_PRELOAD", shim, 1);
        if (script)
            execl("/bin/sh", "sh", program, (char *)NULL);
        else
            execl(program, program, (char *)NULL);
#else
        setenv("BOX64_LD_PRELOAD", shim, 1);
        setenv("BOX64_NOBANNER", "1", 1);
        if (libs[0])
            setenv("BOX64_LD_LIBRARY_PATH", libs, 1);
        unsetenv("LD_PRELOAD");
        if (script) {
            char bash[PATH_MAX];
            snprintf(bash, sizeof(bash), "%s/../../bin/bash", libs);
            setenv("BOX64_BASH", bash, 1);
            execl(box64, box64, bash, program, (char *)NULL);
        } else {
            execl(box64, box64, program, (char *)NULL);
        }
#endif
        _exit(127);
    }
    setpgid(child, child);
    LOG_I("atarivcs: started %s (pid %d)\n", program, (int)child);
    return true;
}

void retro_unload_game(void)
{
    stop_child();
    destroy_shared();
    free(frame);
    frame = NULL;
    have_frame = false;
    last_seq = 0;
}

/* ---- Each frame ---- */

static int16_t trigger(unsigned id)
{
    int16_t a = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, id);
    if (!a)
        a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) ? 0x7fff : 0;
    if (a < 0)
        a = 0;
    /* SDL's trigger axis: -32768 at rest, about 32767 pressed */
    return (int16_t)((int32_t)a * 2 - 32768);
}

static void write_pad(void)
{
    static const struct { unsigned id; unsigned btn; } map[] = {
        { RETRO_DEVICE_ID_JOYPAD_B, ATARIVCS_BTN_A },
        { RETRO_DEVICE_ID_JOYPAD_A, ATARIVCS_BTN_B },
        { RETRO_DEVICE_ID_JOYPAD_Y, ATARIVCS_BTN_X },
        { RETRO_DEVICE_ID_JOYPAD_X, ATARIVCS_BTN_Y },
        { RETRO_DEVICE_ID_JOYPAD_L, ATARIVCS_BTN_LB },
        { RETRO_DEVICE_ID_JOYPAD_R, ATARIVCS_BTN_RB },
        { RETRO_DEVICE_ID_JOYPAD_SELECT, ATARIVCS_BTN_BACK },
        { RETRO_DEVICE_ID_JOYPAD_START, ATARIVCS_BTN_START },
        { RETRO_DEVICE_ID_JOYPAD_L3, ATARIVCS_BTN_LS },
        { RETRO_DEVICE_ID_JOYPAD_R3, ATARIVCS_BTN_RS },
    };
    uint32_t buttons = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, map[i].id))
            buttons |= 1u << map[i].btn;
    shared->buttons = buttons;

    uint8_t hat = 0;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP)) hat |= ATARIVCS_HAT_UP;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN)) hat |= ATARIVCS_HAT_DOWN;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT)) hat |= ATARIVCS_HAT_LEFT;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) hat |= ATARIVCS_HAT_RIGHT;
    shared->hat = hat;

    shared->axes[ATARIVCS_AXIS_LX] = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    shared->axes[ATARIVCS_AXIS_LY] = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    shared->axes[ATARIVCS_AXIS_RX] = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    shared->axes[ATARIVCS_AXIS_RY] = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
    shared->axes[ATARIVCS_AXIS_LT] = trigger(RETRO_DEVICE_ID_JOYPAD_L2);
    shared->axes[ATARIVCS_AXIS_RT] = trigger(RETRO_DEVICE_ID_JOYPAD_R2);
}

/* The newest whole frame the shim wrote, flipped top down */
static bool take_frame(void)
{
    uint32_t seq = __atomic_load_n(&shared->frame_seq, __ATOMIC_ACQUIRE);
    if (seq == last_seq || (seq & 1))
        return false;
    unsigned w = shared->width, h = shared->height;
    if (!w || !h || w > ATARIVCS_MAX_WIDTH || h > ATARIVCS_MAX_HEIGHT)
        return false;
    const uint8_t *src = shared->pixels;
    for (unsigned y = 0; y < h; y++)
        memcpy(&frame[(size_t)y * w], src + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
    if (__atomic_load_n(&shared->frame_seq, __ATOMIC_ACQUIRE) != seq)
        return false; /* written meanwhile: the next run takes it */
    last_seq = seq;

    if (w != cur_w || h != cur_h) {
        cur_w = w;
        cur_h = h;
        struct retro_system_av_info av;
        retro_get_system_av_info(&av);
        if (w > av.geometry.max_width || h > av.geometry.max_height) {
            av.geometry.max_width = w > 1920 ? w : 1920;
            av.geometry.max_height = h > 1080 ? h : 1080;
            environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        } else {
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);
        }
    }
    have_frame = true;
    return true;
}

/* Debugging without a screenshot: while /tmp/atarivcs-dump exists, the
 * frame shown goes to /tmp/atarivcs-frame.ppm and the request is removed */
static void dump_frame_if_asked(void)
{
    if (!have_frame || access("/tmp/atarivcs-dump", F_OK) != 0)
        return;
    unlink("/tmp/atarivcs-dump");
    FILE *f = fopen("/tmp/atarivcs-frame.ppm", "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", cur_w, cur_h);
    for (size_t i = 0; i < (size_t)cur_w * cur_h; i++) {
        const uint32_t px = frame[i];
        const unsigned char rgb[3] = { (unsigned char)(px >> 16), (unsigned char)(px >> 8), (unsigned char)px };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    LOG_I("atarivcs: frame %ux%u dumped\n", cur_w, cur_h);
}

/* Debugging the pad without a keyboard: /tmp/atarivcs-press names a RetroPad
 * button (up, down, left, right, a, b, x, y, start, select), held for 10
 * frames, then the file is removed */
static void debug_press(void)
{
    static int held;
    static unsigned id;
    if (!held) {
        FILE *f = fopen("/tmp/atarivcs-press", "r");
        if (!f)
            return;
        char name[16] = { 0 };
        if (fscanf(f, "%15s", name) != 1)
            name[0] = '\0';
        fclose(f);
        unlink("/tmp/atarivcs-press");
        static const struct { const char *name; unsigned id; } names[] = {
            { "up", RETRO_DEVICE_ID_JOYPAD_UP }, { "down", RETRO_DEVICE_ID_JOYPAD_DOWN },
            { "left", RETRO_DEVICE_ID_JOYPAD_LEFT }, { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT },
            { "a", RETRO_DEVICE_ID_JOYPAD_A }, { "b", RETRO_DEVICE_ID_JOYPAD_B },
            { "x", RETRO_DEVICE_ID_JOYPAD_X }, { "y", RETRO_DEVICE_ID_JOYPAD_Y },
            { "start", RETRO_DEVICE_ID_JOYPAD_START }, { "select", RETRO_DEVICE_ID_JOYPAD_SELECT },
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(name, names[i].name)) {
                id = names[i].id;
                held = 10;
                LOG_I("atarivcs: debug press %s\n", name);
            }
        return;
    }
    held--;
    static const unsigned bits[] = {
        [RETRO_DEVICE_ID_JOYPAD_B] = 1u << ATARIVCS_BTN_A, [RETRO_DEVICE_ID_JOYPAD_A] = 1u << ATARIVCS_BTN_B,
        [RETRO_DEVICE_ID_JOYPAD_Y] = 1u << ATARIVCS_BTN_X, [RETRO_DEVICE_ID_JOYPAD_X] = 1u << ATARIVCS_BTN_Y,
        [RETRO_DEVICE_ID_JOYPAD_START] = 1u << ATARIVCS_BTN_START, [RETRO_DEVICE_ID_JOYPAD_SELECT] = 1u << ATARIVCS_BTN_BACK,
    };
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_UP: shared->hat |= ATARIVCS_HAT_UP; break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN: shared->hat |= ATARIVCS_HAT_DOWN; break;
    case RETRO_DEVICE_ID_JOYPAD_LEFT: shared->hat |= ATARIVCS_HAT_LEFT; break;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT: shared->hat |= ATARIVCS_HAT_RIGHT; break;
    default: shared->buttons |= bits[id]; break;
    }
}

void retro_run(void)
{
    input_poll_cb();
    if (shared) {
        write_pad();
        debug_press();
        __atomic_add_fetch(&shared->run_seq, 1, __ATOMIC_ACQ_REL);
    }

    if (child > 0) {
        int status;
        if (waitpid(child, &status, WNOHANG) == child) {
            LOG_I("atarivcs: the game ended (status %d)\n", status);
            child = -1;
            environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
        }
    }

    if (shared && take_frame())
        video_cb(frame, cur_w, cur_h, (size_t)cur_w * 4);
    else if (have_frame)
        video_cb(NULL, cur_w, cur_h, (size_t)cur_w * 4);
    else {
        static uint32_t black[64 * 64];
        video_cb(black, 64, 64, 64 * 4);
    }

    dump_frame_if_asked();

    /* The game plays its own sound; silence keeps RetroArch's timing */
    static int16_t silence[SAMPLE_RATE / FPS * 2];
    audio_batch_cb(silence, SAMPLE_RATE / FPS);
}
