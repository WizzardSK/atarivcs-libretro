/* Zero-copy frames, the core's side (gpu.h) */
#define _GNU_SOURCE
#include "gpu.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG(level, ...) do { if (log_cb) log_cb(level, __VA_ARGS__); } while (0)

static retro_log_printf_t log_cb;

/* ---- The buffers: gbm, on the GPU's render node ---- */

#define GBM_FORMAT_ABGR8888 0x34324241u /* R, G, B, A in memory: GL_RGBA8 */
#define GBM_BO_USE_RENDERING (1u << 2)
#define GBM_BO_USE_LINEAR (1u << 4)

static void *gbm_lib, *gbm_dev;
static void *bos[3];
static int fds[3] = { -1, -1, -1 };
static unsigned buf_w, buf_h;
static size_t buf_size;
static char env[128];
static int render_fd = -1;

static void (*gbm_bo_destroy_)(void *);
static void (*gbm_device_destroy_)(void *);

bool gpu_alloc(unsigned w, unsigned h, retro_log_printf_t log)
{
    log_cb = log;
    gpu_free();
    const char *off = getenv("ATARIVCS_NO_ZEROCOPY");
    if (off && *off == '1')
        return false;
    gbm_lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!gbm_lib) {
        LOG(RETRO_LOG_INFO, "atarivcs: no libgbm, frames are read back\n");
        return false;
    }
    void *(*create_device)(int) = (void *(*)(int))dlsym(gbm_lib, "gbm_create_device");
    void *(*bo_create)(void *, uint32_t, uint32_t, uint32_t, uint32_t) =
        (void *(*)(void *, uint32_t, uint32_t, uint32_t, uint32_t))dlsym(gbm_lib, "gbm_bo_create");
    int (*bo_get_fd)(void *) = (int (*)(void *))dlsym(gbm_lib, "gbm_bo_get_fd");
    uint32_t (*bo_get_stride)(void *) = (uint32_t (*)(void *))dlsym(gbm_lib, "gbm_bo_get_stride");
    gbm_bo_destroy_ = (void (*)(void *))dlsym(gbm_lib, "gbm_bo_destroy");
    gbm_device_destroy_ = (void (*)(void *))dlsym(gbm_lib, "gbm_device_destroy");
    if (!create_device || !bo_create || !bo_get_fd || !bo_get_stride || !gbm_bo_destroy_ || !gbm_device_destroy_)
        goto fail;

    for (int n = 128; n < 136 && render_fd < 0; n++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", n);
        render_fd = open(path, O_RDWR | O_CLOEXEC);
    }
    if (render_fd < 0 || !(gbm_dev = create_device(render_fd)))
        goto fail;

    /* Linear, so both sides lay it out alike without a modifier to agree
     * on; the width a multiple of 64 pixels, which is the pitch linear
     * buffers get anyway, checked below */
    buf_w = (w + 63) & ~63u;
    buf_h = h;
    for (int i = 0; i < 3; i++) {
        bos[i] = bo_create(gbm_dev, buf_w, buf_h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!bos[i])
            goto fail;
        if (bo_get_stride(bos[i]) != buf_w * 4) {
            LOG(RETRO_LOG_INFO, "atarivcs: GPU buffer pitch %u, not %u: frames are read back\n",
                bo_get_stride(bos[i]), buf_w * 4);
            goto fail;
        }
        fds[i] = bo_get_fd(bos[i]);
        if (fds[i] < 0)
            goto fail;
    }
    off_t end = lseek(fds[0], 0, SEEK_END);
    buf_size = end > 0 ? (size_t)end : (size_t)buf_w * buf_h * 4;
    snprintf(env, sizeof(env), "%u:%u:%zu:%d,%d,%d", buf_w, buf_h, buf_size, fds[0], fds[1], fds[2]);
    LOG(RETRO_LOG_INFO, "atarivcs: three %ux%u GPU buffers for the frames (%s)\n", buf_w, buf_h, env);
    return true;
fail:
    LOG(RETRO_LOG_INFO, "atarivcs: no GPU buffers for the frames, they are read back\n");
    gpu_free();
    return false;
}

const char *gpu_env(void)
{
    return fds[0] >= 0 ? env : NULL;
}

void gpu_child_keep_fds(void)
{
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0)
            fcntl(fds[i], F_SETFD, 0);
}

/* ---- RetroArch's OpenGL context ---- */

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef intptr_t GLsizeiptr;
typedef uint64_t GLuint64;

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_TRIANGLE_STRIP 0x0005
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_SCISSOR_TEST 0x0C11
#define GL_CULL_FACE 0x0B44
#define GL_STENCIL_TEST 0x0B90
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_TEXTURE_TILING_EXT 0x9580
#define GL_LINEAR_TILING_EXT 0x9585
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT 0x9586

static struct retro_hw_render_callback hw;
static bool hw_on, ctx_ready, gpu_ok;

static struct {
    void (*GenTextures)(GLsizei, GLuint *);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*ActiveTexture)(GLenum);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (*PixelStorei)(GLenum, GLint);
    void (*CreateMemoryObjectsEXT)(GLsizei, GLuint *);
    void (*DeleteMemoryObjectsEXT)(GLsizei, const GLuint *);
    void (*ImportMemoryFdEXT)(GLuint, GLuint64, GLenum, GLint);
    void (*TexStorageMem2DEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint *);
    void (*DeleteShader)(GLuint);
    GLuint (*CreateProgram)(void);
    void (*AttachShader)(GLuint, GLuint);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint *);
    void (*UseProgram)(GLuint);
    void (*DeleteProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const GLchar *);
    void (*Uniform1i)(GLint, GLint);
    void (*Uniform4f)(GLint, float, float, float, float);
    void (*GenVertexArrays)(GLsizei, GLuint *);
    void (*BindVertexArray)(GLuint);
    void (*DeleteVertexArrays)(GLsizei, const GLuint *);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*GenFramebuffers)(GLsizei, GLuint *);
    void (*DeleteFramebuffers)(GLsizei, const GLuint *);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Disable)(GLenum);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    GLenum (*GetError)(void);
} gl;

static GLuint mems[3], gpu_tex[3], cpu_tex, prog, vao, read_fbo;
static GLint u_tex, u_map;
static unsigned cpu_tex_w, cpu_tex_h;

/* What is shown: a GPU buffer (0-2) or the read-back texture (-1), and its
 * picture's size; the shim's middle buffer starts as 1, the core's own as 2 */
static int front = 2, shown = -2;
static unsigned shown_w, shown_h;

#define LOAD(name) (*(void **)&gl.name = (void *)hw.get_proc_address("gl" #name))

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

/* A quad over the viewport; map turns its 0..1 corners into texture
 * coordinates (scale x, y, offset x, y): the GPU buffers are bottom up like
 * the frame, the read-back texture top down */
static const char *vs_src =
    "#version 330 core\n"
    "uniform vec4 map;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 q = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "    uv = q * map.xy + map.zw;\n"
    "    gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *fs_src =
    "#version 330 core\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "void main() { color = vec4(texture(tex, uv).rgb, 1.0); }\n";

static void context_reset(void)
{
    LOAD(GenTextures); LOAD(DeleteTextures); LOAD(BindTexture); LOAD(ActiveTexture);
    LOAD(TexParameteri); LOAD(TexImage2D); LOAD(TexSubImage2D); LOAD(PixelStorei);
    LOAD(CreateMemoryObjectsEXT); LOAD(DeleteMemoryObjectsEXT); LOAD(ImportMemoryFdEXT);
    LOAD(TexStorageMem2DEXT); LOAD(CreateShader); LOAD(ShaderSource); LOAD(CompileShader);
    LOAD(GetShaderiv); LOAD(DeleteShader); LOAD(CreateProgram); LOAD(AttachShader);
    LOAD(LinkProgram); LOAD(GetProgramiv); LOAD(UseProgram); LOAD(DeleteProgram);
    LOAD(GetUniformLocation); LOAD(Uniform1i); LOAD(Uniform4f); LOAD(GenVertexArrays);
    LOAD(BindVertexArray); LOAD(DeleteVertexArrays); LOAD(BindFramebuffer);
    LOAD(GenFramebuffers); LOAD(DeleteFramebuffers); LOAD(FramebufferTexture2D);
    LOAD(ReadPixels); LOAD(Viewport); LOAD(Disable); LOAD(DrawArrays); LOAD(GetError);

    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        LOG(RETRO_LOG_ERROR, "atarivcs: the frame shaders do not compile\n");
        return;
    }
    prog = gl.CreateProgram();
    gl.AttachShader(prog, vs);
    gl.AttachShader(prog, fs);
    gl.LinkProgram(prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint linked = 0;
    gl.GetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        LOG(RETRO_LOG_ERROR, "atarivcs: the frame shaders do not link\n");
        return;
    }
    u_tex = gl.GetUniformLocation(prog, "tex");
    u_map = gl.GetUniformLocation(prog, "map");
    gl.GenVertexArrays(1, &vao);
    gl.GenFramebuffers(1, &read_fbo);

    gl.GenTextures(1, &cpu_tex);
    cpu_tex_w = cpu_tex_h = 0;

    /* The GPU buffers, imported as textures */
    gpu_ok = false;
    if (fds[0] >= 0 && gl.CreateMemoryObjectsEXT && gl.ImportMemoryFdEXT && gl.TexStorageMem2DEXT) {
        while (gl.GetError())
            ;
        gl.CreateMemoryObjectsEXT(3, mems);
        gl.GenTextures(3, gpu_tex);
        for (int i = 0; i < 3; i++) {
            int fd = dup(fds[i]); /* the import takes it */
            gl.ImportMemoryFdEXT(mems[i], buf_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
            gl.BindTexture(GL_TEXTURE_2D, gpu_tex[i]);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_LINEAR_TILING_EXT);
            gl.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)buf_w, (GLsizei)buf_h, mems[i], 0);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        gl.BindTexture(GL_TEXTURE_2D, 0);
        GLenum err = gl.GetError();
        gpu_ok = err == 0;
        LOG(RETRO_LOG_INFO, "atarivcs: GPU buffers imported into RetroArch's OpenGL: %s (error 0x%x)\n",
            gpu_ok ? "yes" : "no", err);
    }
    shown = -2;
    ctx_ready = true;
}

static void context_destroy(void)
{
    ctx_ready = false;
    if (!gl.DeleteTextures)
        return;
    gl.DeleteTextures(3, gpu_tex);
    gl.DeleteTextures(1, &cpu_tex);
    if (gl.DeleteMemoryObjectsEXT)
        gl.DeleteMemoryObjectsEXT(3, mems);
    gl.DeleteProgram(prog);
    gl.DeleteVertexArrays(1, &vao);
    gl.DeleteFramebuffers(1, &read_fbo);
    memset(gpu_tex, 0, sizeof(gpu_tex));
    memset(mems, 0, sizeof(mems));
    cpu_tex = prog = vao = read_fbo = 0;
    gpu_ok = false;
}

bool gpu_request_hw(retro_environment_t environ_cb)
{
    hw_on = false;
    if (fds[0] < 0)
        return false;
    memset(&hw, 0, sizeof(hw));
    hw.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    hw.version_major = 3;
    hw.version_minor = 3;
    hw.context_reset = context_reset;
    hw.context_destroy = context_destroy;
    hw.bottom_left_origin = true;
    hw_on = environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw);
    LOG(RETRO_LOG_INFO, "atarivcs: RetroArch's OpenGL for the frames: %s\n", hw_on ? "yes" : "no, frames are read back");
    return hw_on;
}

bool gpu_active(void)
{
    return hw_on;
}

static void draw(GLuint tex, float sx, float sy, float ox, float oy, unsigned w, unsigned h)
{
    gl.BindFramebuffer(GL_FRAMEBUFFER, (GLuint)hw.get_current_framebuffer());
    gl.Viewport(0, 0, (GLsizei)w, (GLsizei)h);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_FRAMEBUFFER_SRGB);
    gl.UseProgram(prog);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.Uniform1i(u_tex, 0);
    gl.Uniform4f(u_map, sx, sy, ox, oy);
    gl.BindVertexArray(vao);
    gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl.BindVertexArray(0);
    gl.UseProgram(0);
    gl.BindTexture(GL_TEXTURE_2D, 0);
}

void gpu_run(atarivcs_shared *shared, const uint32_t *cpu_frame, unsigned cpu_w, unsigned cpu_h,
             retro_video_refresh_t video_cb, unsigned *w, unsigned *h)
{
    if (shared)
        __atomic_store_n(&shared->gpu_ready, (uint32_t)(ctx_ready && gpu_ok), __ATOMIC_RELEASE);
    if (!ctx_ready || !prog) {
        video_cb(NULL, shown_w ? shown_w : 64, shown_h ? shown_h : 64, 0);
        return;
    }
    /* A new GPU frame: the middle buffer becomes the core's, its old one
     * the middle */
    if (gpu_ok && shared &&
        (__atomic_load_n(&shared->gpu_middle, __ATOMIC_ACQUIRE) & ATARIVCS_GPU_NEW)) {
        uint32_t old = __atomic_exchange_n(&shared->gpu_middle, (uint32_t)front, __ATOMIC_ACQ_REL);
        front = (int)(old & 3u);
        if (front <= 2) {
            shown = front;
            shown_w = shared->gpu_w[front];
            shown_h = shared->gpu_h[front];
        }
    } else if (cpu_frame) {
        if (cpu_tex_w != cpu_w || cpu_tex_h != cpu_h) {
            gl.BindTexture(GL_TEXTURE_2D, cpu_tex);
            gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)cpu_w, (GLsizei)cpu_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            cpu_tex_w = cpu_w;
            cpu_tex_h = cpu_h;
        }
        gl.BindTexture(GL_TEXTURE_2D, cpu_tex);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)cpu_w, (GLsizei)cpu_h, GL_BGRA, GL_UNSIGNED_BYTE, cpu_frame);
        gl.BindTexture(GL_TEXTURE_2D, 0);
        shown = -1;
        shown_w = cpu_w;
        shown_h = cpu_h;
    }
    if (shown == -2 || !shown_w || !shown_h) {
        video_cb(NULL, 64, 64, 0); /* nothing yet */
        return;
    }
    if (shown >= 0)
        draw(gpu_tex[shown], (float)shown_w / buf_w, (float)shown_h / buf_h, 0.0f, 0.0f, shown_w, shown_h);
    else
        draw(cpu_tex, 1.0f, -1.0f, 0.0f, 1.0f, shown_w, shown_h);
    *w = shown_w;
    *h = shown_h;
    video_cb(RETRO_HW_FRAME_BUFFER_VALID, shown_w, shown_h, 0);
}

bool gpu_read_shown(uint32_t *out, unsigned max_pixels, unsigned *w, unsigned *h)
{
    if (!ctx_ready || shown < -1 || !shown_w || !shown_h || shown_w * shown_h > max_pixels)
        return false;
    GLuint tex = shown >= 0 ? gpu_tex[shown] : cpu_tex;
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl.ReadPixels(0, 0, (GLsizei)shown_w, (GLsizei)shown_h, GL_BGRA, GL_UNSIGNED_BYTE, out);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (shown >= 0) {
        /* bottom up: turned top down */
        for (unsigned y = 0; y < shown_h / 2; y++) {
            uint32_t *a = out + (size_t)y * shown_w, *b = out + (size_t)(shown_h - 1 - y) * shown_w;
            for (unsigned x = 0; x < shown_w; x++) {
                uint32_t t = a[x];
                a[x] = b[x];
                b[x] = t;
            }
        }
    }
    *w = shown_w;
    *h = shown_h;
    return true;
}

void gpu_free(void)
{
    for (int i = 0; i < 3; i++) {
        if (fds[i] >= 0)
            close(fds[i]);
        fds[i] = -1;
        if (bos[i] && gbm_bo_destroy_)
            gbm_bo_destroy_(bos[i]);
        bos[i] = NULL;
    }
    if (gbm_dev && gbm_device_destroy_)
        gbm_device_destroy_(gbm_dev);
    gbm_dev = NULL;
    if (render_fd >= 0)
        close(render_fd);
    render_fd = -1;
    front = 2;
    shown = -2;
    shown_w = shown_h = 0;
    hw_on = false;
}
