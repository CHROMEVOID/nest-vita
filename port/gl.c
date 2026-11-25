/* Nest port: libvita2d rebuilt on OpenGL ES 2 (and desktop GL 2.1 for testing on a PC).
   Nest draws in the Vita's 960x544 space; here that space is scaled to the screen
   (exactly 2x on a 1920x1080 handheld) with letterboxing if the shape differs.
   Text is drawn with stb_truetype at the real screen size, so it stays sharp. */
#include "port.h"
#undef fopen
#undef main

#include <SDL.h>
#ifndef __ANDROID__
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#endif
#include <SDL_opengles2.h>
#include <math.h>

#include "vita2d.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

/* the image loader is compiled in img.c (STB_IMAGE_IMPLEMENTATION there) */
#include "stb_image.h"

#define LW 960.0f
#define LH 544.0f

void port_paths_init(void);
void port_assets_extract(void);
void port_pump_events(void);
void port_power_frame(void);
void port_ime_draw(void);

/* ---------------------------------------------------------------- GL functions */
/* loaded through SDL so the same code runs on Android, Windows and Linux */
#define GLF(ret, name, args) typedef ret(GL_APIENTRY *PFN_##name) args; static PFN_##name name##_;
GLF(void, glActiveTexture, (GLenum))
GLF(void, glAttachShader, (GLuint, GLuint))
GLF(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))
GLF(void, glBindFramebuffer, (GLenum, GLuint))
GLF(void, glBindTexture, (GLenum, GLuint))
GLF(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum))
GLF(void, glClear, (GLbitfield))
GLF(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))
GLF(void, glCompileShader, (GLuint))
GLF(GLuint, glCreateProgram, (void))
GLF(GLuint, glCreateShader, (GLenum))
GLF(void, glDeleteFramebuffers, (GLsizei, const GLuint *))
GLF(void, glDeleteTextures, (GLsizei, const GLuint *))
GLF(void, glDisable, (GLenum))
GLF(void, glDisableVertexAttribArray, (GLuint))
GLF(void, glDrawArrays, (GLenum, GLint, GLsizei))
GLF(void, glEnable, (GLenum))
GLF(void, glEnableVertexAttribArray, (GLuint))
GLF(void, glFinish, (void))
GLF(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLF(void, glGenFramebuffers, (GLsizei, GLuint *))
GLF(void, glGenTextures, (GLsizei, GLuint *))
GLF(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *))
GLF(void, glGetShaderiv, (GLuint, GLenum, GLint *))
GLF(GLint, glGetUniformLocation, (GLuint, const GLchar *))
GLF(void, glLinkProgram, (GLuint))
GLF(void, glPixelStorei, (GLenum, GLint))
GLF(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
GLF(void, glScissor, (GLint, GLint, GLsizei, GLsizei))
GLF(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *))
GLF(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
GLF(void, glTexParameteri, (GLenum, GLenum, GLint))
GLF(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))
GLF(void, glUniform1i, (GLint, GLint))
GLF(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLF(void, glUseProgram, (GLuint))
GLF(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))
GLF(void, glViewport, (GLint, GLint, GLsizei, GLsizei))

static int gl_load(void) {
#define L(name)                                                     \
    name##_ = (PFN_##name)SDL_GL_GetProcAddress(#name);              \
    if (!name##_) { SDL_Log("Nest: GL function %s missing", #name); return 0; }
    L(glActiveTexture) L(glAttachShader) L(glBindAttribLocation) L(glBindFramebuffer) L(glBindTexture)
    L(glBlendFuncSeparate) L(glClear) L(glClearColor) L(glCompileShader) L(glCreateProgram) L(glCreateShader)
    L(glDeleteFramebuffers) L(glDeleteTextures) L(glDisable) L(glDisableVertexAttribArray) L(glDrawArrays)
    L(glEnable) L(glEnableVertexAttribArray) L(glFinish) L(glFramebufferTexture2D) L(glGenFramebuffers)
    L(glGenTextures) L(glGetShaderInfoLog) L(glGetShaderiv) L(glGetUniformLocation) L(glLinkProgram)
    L(glPixelStorei) L(glReadPixels) L(glScissor) L(glShaderSource) L(glTexImage2D) L(glTexParameteri)
    L(glTexSubImage2D) L(glUniform1i) L(glUniform4f) L(glUseProgram) L(glVertexAttribPointer) L(glViewport)
#undef L
    return 1;
}

/* ---------------------------------------------------------------- state */

static SDL_Window *win;
static SDL_GLContext ctx;
static int es; /* OpenGL ES context (Android), else desktop GL */
static int dw, dh; /* drawable size in pixels */
static float sc = 1, scx = 1, scy = 1, offx, offy; /* logical -> screen (sc: the smaller of the two) */
static unsigned int clear_color = 0xFF000000;
static int clip_on, clip_x0, clip_y0, clip_x1, clip_y1;
static vita2d_texture *target; /* render target being drawn into, or NULL for the screen */
static GLuint screen_fbo; /* 0 = the window; the PC test build draws off-screen so it works minimised */
static float tw_target, th_target;

enum { P_COLOR, P_TEX, P_ALPHA, P_YUV, P_N };
static GLuint prog[P_N];
static GLint u_xf[P_N], u_tint[P_N], u_tex[P_N], u_tex2[P_N], u_mat[P_N];
static GLuint yuv_y, yuv_vu; /* the video frame being shown */
static const void *yuv_data;
static unsigned int yuv_serial, yuv_w, yuv_h;

void port_screen_scale_xy(float *sx, float *sy, float *ox, float *oy) {
    *sx = scx;
    *sy = scy;
    *ox = offx;
    *oy = offy;
}
void port_screen_scale(float *s, float *ox, float *oy) {
    if (s) *s = sc;
    if (ox) *ox = offx;
    if (oy) *oy = offy;
}
SDL_Window *port_window(void) { return win; }

/* ---------------------------------------------------------------- shaders */

static const char *VS =
    "attribute vec2 a_pos;\n"
    "attribute vec4 a_col;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec4 u_xf;\n" /* x scale, y scale, x offset, y offset (to clip space) */
    "varying vec4 v_col;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos.x * u_xf.x + u_xf.z, a_pos.y * u_xf.y + u_xf.w, 0.0, 1.0);\n"
    "  v_col = a_col;\n"
    "  v_uv = a_uv;\n"
    "}\n";
static const char *FS_COLOR = "varying vec4 v_col;\nvoid main() { gl_FragColor = v_col; }\n";
static const char *FS_TEX =
    "uniform sampler2D u_tex;\nuniform vec4 u_tint;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_tint; }\n";
static const char *FS_ALPHA =
    "uniform sampler2D u_tex;\nuniform vec4 u_tint;\nvarying vec2 v_uv;\n"
    "void main() { gl_FragColor = vec4(u_tint.rgb, u_tint.a * texture2D(u_tex, v_uv).a); }\n";
/* the Vita's YVU420P2: Y plane + interleaved V,U at half size */
static const char *FS_YUV =
    "uniform sampler2D u_tex;\nuniform sampler2D u_tex2;\nuniform vec4 u_tint;\nuniform vec4 u_mat;\nvarying vec2 v_uv;\n"
    "void main() {\n"
    "  float y = (texture2D(u_tex, v_uv).r - 0.0625) * 1.1644;\n"
    "  vec4 vu = texture2D(u_tex2, v_uv);\n"
    "  float v = vu.r - 0.5; float u = vu.a - 0.5;\n"
    "  vec3 rgb = vec3(y + u_mat.x * v, y - u_mat.y * u - u_mat.z * v, y + u_mat.w * u);\n"
    "  gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0) * u_tint;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    const char *head = es ? "#version 100\nprecision mediump float;\n" : "#version 120\n";
    const char *srcs[2] = {head, src};
    GLuint s = glCreateShader_(type);
    glShaderSource_(s, 2, srcs, NULL);
    glCompileShader_(s);
    GLint ok = 0;
    glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog_(s, sizeof(log), NULL, log);
        SDL_Log("Nest: shader error: %s", log);
    }
    return s;
}

static GLuint link_prog(const char *fs, int i) {
    GLuint p = glCreateProgram_();
    glAttachShader_(p, compile(GL_VERTEX_SHADER, VS));
    glAttachShader_(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation_(p, 0, "a_pos");
    glBindAttribLocation_(p, 1, "a_col");
    glBindAttribLocation_(p, 2, "a_uv");
    glLinkProgram_(p);
    u_xf[i] = glGetUniformLocation_(p, "u_xf");
    u_tint[i] = glGetUniformLocation_(p, "u_tint");
    u_tex[i] = glGetUniformLocation_(p, "u_tex");
    u_tex2[i] = glGetUniformLocation_(p, "u_tex2");
    u_mat[i] = glGetUniformLocation_(p, "u_mat");
    return p;
}

/* ---------------------------------------------------------------- frame setup */

static void update_viewport(void) {
    if (screen_fbo) return; /* off-screen test drawing keeps its size */
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    sc = fminf(dw / LW, dh / LH);
    scx = scy = sc;
    /* a screen within a few percent of the Vita's shape (16:9 at 1920x1080 is 0.7% off)
       is filled edge to edge: thin black bars look like a mistake, a 0.7% stretch doesn't */
    float aspect = (dw / LW) / (dh / LH);
    if (aspect > 0.97f && aspect < 1.03f) {
        scx = dw / LW;
        scy = dh / LH;
    }
    offx = floorf((dw - LW * scx) / 2);
    offy = floorf((dh - LH * scy) / 2);
}

/* logical (960x544, y down) -> clip space for the current target */
static void xform(float *xf) {
    if (target) {
        xf[0] = 2.0f / tw_target;
        xf[1] = 2.0f / th_target; /* render targets: y up in memory, read back flipped */
        xf[2] = -1;
        xf[3] = -1;
    } else {
        xf[0] = 2.0f * scx / dw;
        xf[1] = -2.0f * scy / dh;
        xf[2] = -1 + 2.0f * offx / dw;
        xf[3] = 1 - 2.0f * offy / dh;
    }
}

static int cur_prog = -1;
static void use(int p) {
    if (cur_prog != p) {
        glUseProgram_(prog[p]);
        cur_prog = p;
    }
    float xf[4];
    xform(xf);
    glUniform4f_(u_xf[p], xf[0], xf[1], xf[2], xf[3]);
}

static void tint4(int p, unsigned int c) {
    glUniform4f_(u_tint[p], (c & 0xFF) / 255.0f, (c >> 8 & 0xFF) / 255.0f, (c >> 16 & 0xFF) / 255.0f, (c >> 24) / 255.0f);
}

static void apply_clip(void) {
    if (!clip_on) {
        glDisable_(GL_SCISSOR_TEST);
        return;
    }
    glEnable_(GL_SCISSOR_TEST);
    if (target) {
        glScissor_(clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0);
    } else {
        int x0 = (int)floorf(offx + clip_x0 * scx), x1 = (int)ceilf(offx + clip_x1 * scx);
        int y0 = (int)floorf(offy + clip_y0 * scy), y1 = (int)ceilf(offy + clip_y1 * scy);
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        glScissor_(x0, dh - y1, x1 - x0, y1 - y0);
    }
}

/* ---------------------------------------------------------------- init */

static void new_gl_tex_fwd(GLuint *id) {
    glGenTextures_(1, id);
    glBindTexture_(GL_TEXTURE_2D, *id);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}
int vita2d_init_advanced_with_msaa(unsigned int pool, SceGxmMultisampleMode msaa) {
    (void)pool;
    port_paths_init();
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "1");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO, "0"); /* music keeps playing in the background */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1"); /* on a PC the mouse acts as a finger */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        SDL_Log("Nest: SDL_Init failed: %s", SDL_GetError());
        return -1;
    }
    port_assets_extract();
    int ww = 1440, wh = 816;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef __ANDROID__
    flags |= SDL_WINDOW_FULLSCREEN;
    ww = 1920;
    wh = 1080;
    es = 1;
#else
    flags |= SDL_WINDOW_RESIZABLE;
    const char *ws = getenv("NEST_WINDOW"); /* e.g. 1920x1080 */
    if (ws) sscanf(ws, "%dx%d", &ww, &wh);
#endif
    for (int attempt = 0; attempt < 3 && !ctx; attempt++) {
        int want_ms = attempt == 0 && msaa != SCE_GXM_MULTISAMPLE_NONE;
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, want_ms ? 1 : 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, want_ms ? 4 : 0);
        if (es || attempt < 2) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            es = 1;
        } else {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
            es = 0;
        }
        if (win) SDL_DestroyWindow(win);
        win = SDL_CreateWindow("Nest", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, flags);
        if (win) ctx = SDL_GL_CreateContext(win);
    }
    if (!ctx || !gl_load()) {
        SDL_Log("Nest: no OpenGL: %s", SDL_GetError());
        return -1;
    }
    SDL_GL_SetSwapInterval(1);
    prog[P_COLOR] = link_prog(FS_COLOR, P_COLOR);
    prog[P_TEX] = link_prog(FS_TEX, P_TEX);
    prog[P_ALPHA] = link_prog(FS_ALPHA, P_ALPHA);
    prog[P_YUV] = link_prog(FS_YUV, P_YUV);
    glEnable_(GL_BLEND);
    /* colour blends normally; the screen itself stays fully opaque */
    glBlendFuncSeparate_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glPixelStorei_(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei_(GL_PACK_ALIGNMENT, 1);
    update_viewport();
#ifndef __ANDROID__
    if (getenv("NEST_TEST_DIR")) {
        GLuint tex;
        new_gl_tex_fwd(&tex);
        glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenFramebuffers_(1, &screen_fbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, screen_fbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
#endif
    SDL_ShowCursor(SDL_DISABLE);
#ifndef __ANDROID__
    SDL_ShowCursor(SDL_ENABLE);
#endif
    return 0;
}
int vita2d_init_advanced(unsigned int pool) { return vita2d_init_advanced_with_msaa(pool, SCE_GXM_MULTISAMPLE_4X); }
int vita2d_init(void) { return vita2d_init_advanced(0); }
int vita2d_fini(void) {
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    return 0;
}
void vita2d_set_vblank_wait(int enable) { SDL_GL_SetSwapInterval(enable ? 1 : 0); }
void vita2d_wait_rendering_done(void) {}
void vita2d_set_clear_color(unsigned int c) { clear_color = c; }
int vita2d_common_dialog_update(void) {
    port_ime_draw(); /* our own keyboard box, drawn over the frame */
    return 0;
}

/* ---------------------------------------------------------------- per-frame scratch memory */

typedef struct Pool { struct Pool *next; size_t used, cap; unsigned char data[]; } Pool;
static Pool *pool_head;
void *vita2d_pool_memalign(unsigned int size, unsigned int align) {
    if (align < 16) align = 16;
    Pool *p = pool_head;
    size_t at = p ? (p->used + align - 1) & ~(size_t)(align - 1) : 0;
    if (!p || at + size > p->cap) {
        size_t cap = size + align > 4u * 1024 * 1024 ? size + align : 4u * 1024 * 1024;
        Pool *np = malloc(sizeof(Pool) + cap);
        if (!np) return NULL;
        np->next = pool_head;
        np->used = 0;
        np->cap = cap;
        pool_head = p = np;
        at = 0;
    }
    p->used = at + size;
    return p->data + at;
}
void *vita2d_pool_malloc(unsigned int size) { return vita2d_pool_memalign(size, 16); }
static void pool_reset(void) {
    /* keep the newest block, free the rest */
    if (!pool_head) return;
    Pool *p = pool_head->next;
    while (p) {
        Pool *n = p->next;
        free(p);
        p = n;
    }
    pool_head->next = NULL;
    pool_head->used = 0;
}

/* ---------------------------------------------------------------- drawing */

void vita2d_start_drawing(void) { vita2d_start_drawing_advanced(NULL, 0); }

void vita2d_start_drawing_advanced(vita2d_texture *t, unsigned int flags) {
    (void)flags;
    target = t && t->is_rt ? t : NULL;
    if (target) {
        glBindFramebuffer_(GL_FRAMEBUFFER, target->fbo);
        tw_target = target->gxm_tex.width;
        th_target = target->gxm_tex.height;
        glViewport_(0, 0, (int)tw_target, (int)th_target);
    } else {
        glBindFramebuffer_(GL_FRAMEBUFFER, screen_fbo);
        update_viewport();
        glViewport_(0, 0, dw, dh);
    }
    clip_on = 0;
    apply_clip();
    cur_prog = -1;
}

void vita2d_clear_screen(void) {
    glDisable_(GL_SCISSOR_TEST);
    unsigned int c = target ? 0 : clear_color;
    if (!target) glClearColor_(0, 0, 0, 1); /* letterbox */
    else glClearColor_((c & 0xFF) / 255.0f, (c >> 8 & 0xFF) / 255.0f, (c >> 16 & 0xFF) / 255.0f, (c >> 24) / 255.0f);
    glClear_(GL_COLOR_BUFFER_BIT);
    if (!target) vita2d_draw_rectangle(0, 0, LW, LH, clear_color | 0xFF000000u);
    apply_clip();
}

void vita2d_end_drawing(void) {
    if (target) {
        glBindFramebuffer_(GL_FRAMEBUFFER, screen_fbo);
        target->dirty = 2; /* the CPU copy is stale: read back on request */
        target = NULL;
    }
}

extern char port_test_shot[];
#ifndef __ANDROID__
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"
#endif
void vita2d_swap_buffers(void) {
#ifndef __ANDROID__
    if (port_test_shot[0]) { /* test hook: save exactly what's on screen */
        unsigned char *px = malloc((size_t)dw * dh * 4);
        if (px) {
            glBindFramebuffer_(GL_FRAMEBUFFER, screen_fbo);
            glReadPixels_(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
            for (size_t i = 3; i < (size_t)dw * dh * 4; i += 4) px[i] = 255;
            stbi_flip_vertically_on_write(1);
            stbi_write_png(port_test_shot, dw, dh, 4, px, dw * 4);
            free(px);
        }
        port_test_shot[0] = 0;
    }
#endif
    SDL_GL_SwapWindow(win);
    pool_reset();
    port_pump_events();
    port_power_frame();
}

void vita2d_enable_clipping(void) { clip_on = 1; apply_clip(); }
void vita2d_disable_clipping(void) { clip_on = 0; apply_clip(); }
void vita2d_set_clip_rectangle(int x0, int y0, int x1, int y1) {
    clip_x0 = x0;
    clip_y0 = y0;
    clip_x1 = x1;
    clip_y1 = y1;
    apply_clip();
}

static GLenum prim(SceGxmPrimitiveType m) {
    switch (m) {
    case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case SCE_GXM_PRIMITIVE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    case SCE_GXM_PRIMITIVE_LINES: return GL_LINES;
    case SCE_GXM_PRIMITIVE_POINTS: return GL_POINTS;
    default: return GL_TRIANGLES;
    }
}

void vita2d_draw_array(SceGxmPrimitiveType mode, const vita2d_color_vertex *v, size_t count) {
    if (!count) return;
    use(P_COLOR);
    glEnableVertexAttribArray_(0);
    glEnableVertexAttribArray_(1);
    glDisableVertexAttribArray_(2);
    glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, sizeof(vita2d_color_vertex), &v->x);
    glVertexAttribPointer_(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vita2d_color_vertex), &v->color);
    glDrawArrays_(prim(mode), 0, (GLsizei)count);
}

void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color) {
    vita2d_color_vertex v[4] = {{x, y, 0.5f, color}, {x + w, y, 0.5f, color}, {x, y + h, 0.5f, color}, {x + w, y + h, 0.5f, color}};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

/* ---------------------------------------------------------------- textures */

void vita2d_texture_set_alloc_memblock_type(SceKernelMemBlockType type) { (void)type; }

static vita2d_texture *tex_new(unsigned int w, unsigned int h, unsigned int fmt) {
    vita2d_texture *t = calloc(1, sizeof(vita2d_texture));
    if (!t) return NULL;
    t->gxm_tex.width = w;
    t->gxm_tex.height = h;
    t->gxm_tex.format = fmt;
    t->linear = 1;
    return t;
}

vita2d_texture *vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format) {
    if (!w || !h) return NULL;
    vita2d_texture *t = tex_new(w, h, format);
    if (!t) return NULL;
    if (format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR) {
        t->pixels = calloc(1, (size_t)w * h);
        t->palette = calloc(256, 4);
    } else {
        t->pixels = calloc(1, (size_t)w * h * 4);
    }
    if (!t->pixels) {
        free(t->palette);
        free(t);
        return NULL;
    }
    t->dirty = 1;
    return t;
}
vita2d_texture *vita2d_create_empty_texture(unsigned int w, unsigned int h) {
    return vita2d_create_empty_texture_format(w, h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
}
vita2d_texture *vita2d_create_empty_texture_rendertarget(unsigned int w, unsigned int h, SceGxmTextureFormat format) {
    vita2d_texture *t = tex_new(w, h, format);
    if (!t) return NULL;
    t->is_rt = 1;
    glGenTextures_(1, &t->gl);
    glBindTexture_(GL_TEXTURE_2D, t->gl);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers_(1, &t->fbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->gl, 0);
    glBindFramebuffer_(GL_FRAMEBUFFER, screen_fbo);
    return t;
}

void vita2d_free_texture(vita2d_texture *t) {
    if (!t) return;
    if (t->gl) glDeleteTextures_(1, &t->gl);
    if (t->fbo) glDeleteFramebuffers_(1, &t->fbo);
    free(t->pixels);
    free(t->palette);
    free(t);
}

unsigned int vita2d_texture_get_width(const vita2d_texture *t) { return t ? t->gxm_tex.width : 0; }
unsigned int vita2d_texture_get_height(const vita2d_texture *t) { return t ? t->gxm_tex.height : 0; }
unsigned int vita2d_texture_get_stride(const vita2d_texture *t) {
    if (!t) return 0;
    return t->gxm_tex.format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR ? t->gxm_tex.width : t->gxm_tex.width * 4;
}

void *vita2d_texture_get_datap(const vita2d_texture *ct) {
    vita2d_texture *t = (vita2d_texture *)ct;
    if (!t) return NULL;
    if (t->is_rt) { /* read what was drawn into it (flipped: GL keeps rows bottom-up) */
        size_t row = (size_t)t->gxm_tex.width * 4;
        if (!t->pixels) t->pixels = malloc(row * t->gxm_tex.height);
        if (t->pixels && t->dirty == 2) {
            glBindFramebuffer_(GL_FRAMEBUFFER, t->fbo);
            glReadPixels_(0, 0, t->gxm_tex.width, t->gxm_tex.height, GL_RGBA, GL_UNSIGNED_BYTE, t->pixels);
            glBindFramebuffer_(GL_FRAMEBUFFER, screen_fbo);
            /* we drew with y up into the target, so rows already come out top-down */
            t->dirty = 0;
        }
        return t->pixels;
    }
    if (!t->pixels) { /* the CPU copy was let go after upload */
        size_t bpp = t->gxm_tex.format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR ? 1 : 4;
        t->pixels = calloc(1, (size_t)t->gxm_tex.width * t->gxm_tex.height * bpp);
    }
    t->dirty = 1; /* whoever asks for the pixels is about to change them */
    return t->pixels;
}
void *vita2d_texture_get_palette(const vita2d_texture *ct) {
    vita2d_texture *t = (vita2d_texture *)ct;
    if (!t) return NULL;
    t->dirty = 1;
    return t->palette;
}
void vita2d_texture_set_filters(vita2d_texture *t, SceGxmTextureFilter min_f, SceGxmTextureFilter mag_f) {
    (void)min_f;
    if (!t) return;
    int lin = mag_f == SCE_GXM_TEXTURE_FILTER_LINEAR;
    if (lin != t->linear && t->gl) {
        glBindTexture_(GL_TEXTURE_2D, t->gl);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, lin ? GL_LINEAR : GL_NEAREST);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, lin ? GL_LINEAR : GL_NEAREST);
    }
    t->linear = lin;
}

int sceGxmTextureInitLinear(SceGxmTexture *tx, const void *data, SceGxmTextureFormat fmt, unsigned int w, unsigned int h, unsigned int mips) {
    (void)mips;
    static unsigned int serial;
    tx->data = (void *)data;
    tx->format = fmt;
    tx->width = w;
    tx->height = h;
    tx->serial = ++serial;
    return 0;
}

static void new_gl_tex(GLuint *id, int linear) {
    glGenTextures_(1, id);
    glBindTexture_(GL_TEXTURE_2D, *id);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* make sure the GL copy is current; returns the program to draw it with */
static int bind_tex(const vita2d_texture *ct) {
    vita2d_texture *t = (vita2d_texture *)ct;
    if (t->gxm_tex.format == SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1) {
        unsigned int w = t->gxm_tex.width, h = t->gxm_tex.height;
        if (!yuv_y) {
            new_gl_tex(&yuv_y, 1);
            new_gl_tex(&yuv_vu, 1);
        }
        if (t->gxm_tex.data && (t->gxm_tex.data != yuv_data || t->gxm_tex.serial != yuv_serial)) {
            const unsigned char *d = t->gxm_tex.data;
            glBindTexture_(GL_TEXTURE_2D, yuv_y);
            if (w != yuv_w || h != yuv_h) glTexImage2D_(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, d);
            else glTexSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, d);
            glBindTexture_(GL_TEXTURE_2D, yuv_vu);
            if (w != yuv_w || h != yuv_h)
                glTexImage2D_(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, w / 2, h / 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, d + (size_t)w * h);
            else glTexSubImage2D_(GL_TEXTURE_2D, 0, 0, 0, w / 2, h / 2, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, d + (size_t)w * h);
            yuv_w = w;
            yuv_h = h;
            yuv_data = t->gxm_tex.data;
            yuv_serial = t->gxm_tex.serial;
        }
        glActiveTexture_(GL_TEXTURE1);
        glBindTexture_(GL_TEXTURE_2D, yuv_vu);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture_(GL_TEXTURE_2D, yuv_y);
        return P_YUV;
    }
    if (!t->gl) {
        new_gl_tex(&t->gl, t->linear);
        t->dirty = 1;
    } else glBindTexture_(GL_TEXTURE_2D, t->gl);
    if (t->dirty == 1 && t->pixels) {
        unsigned int w = t->gxm_tex.width, h = t->gxm_tex.height;
        if (t->gxm_tex.format == SCE_GXM_TEXTURE_FORMAT_P8_ABGR) {
            /* palette pixel art: expand to RGBA, keep the indices for the next frame */
            uint32_t *rgba = malloc((size_t)w * h * 4);
            const uint32_t *pal = (const uint32_t *)t->palette;
            if (rgba) {
                for (size_t i = 0; i < (size_t)w * h; i++) rgba[i] = pal[t->pixels[i]];
                glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                free(rgba);
            }
        } else {
            glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t->pixels);
            /* pictures are written once: let the CPU copy go */
            free(t->pixels);
            t->pixels = NULL;
        }
        t->dirty = 0;
    }
    return P_TEX;
}

static void set_yuv_matrix(unsigned int h) {
    if (h >= 700) glUniform4f_(u_mat[P_YUV], 1.7927f, 0.2132f, 0.5329f, 2.1124f); /* BT.709 (HD) */
    else glUniform4f_(u_mat[P_YUV], 1.5960f, 0.3918f, 0.8130f, 2.0172f);           /* BT.601 (SD) */
}

void vita2d_draw_array_textured(const vita2d_texture *t, SceGxmPrimitiveType mode, const vita2d_texture_vertex *v, size_t count,
                                unsigned int color) {
    if (!t || !count) return;
    glActiveTexture_(GL_TEXTURE0);
    int p = bind_tex(t);
    use(p);
    glUniform1i_(u_tex[p], 0);
    if (p == P_YUV) {
        glUniform1i_(u_tex2[p], 1);
        set_yuv_matrix(t->gxm_tex.height);
    }
    tint4(p, color);
    glEnableVertexAttribArray_(0);
    glDisableVertexAttribArray_(1);
    glEnableVertexAttribArray_(2);
    glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, sizeof(vita2d_texture_vertex), &v->x);
    glVertexAttribPointer_(2, 2, GL_FLOAT, GL_FALSE, sizeof(vita2d_texture_vertex), &v->u);
    glDrawArrays_(prim(mode), 0, (GLsizei)count);
}

void vita2d_draw_texture_tint_part_scale(const vita2d_texture *t, float x, float y, float tx, float ty, float tw, float th, float xs,
                                         float ys, unsigned int color) {
    if (!t) return;
    float W = t->gxm_tex.width, H = t->gxm_tex.height;
    float u0 = tx / W, v0 = ty / H, u1 = (tx + tw) / W, v1 = (ty + th) / H;
    float w = tw * xs, h = th * ys;
    vita2d_texture_vertex v[4] = {{x, y, 0.5f, u0, v0}, {x + w, y, 0.5f, u1, v0}, {x, y + h, 0.5f, u0, v1}, {x + w, y + h, 0.5f, u1, v1}};
    vita2d_draw_array_textured(t, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4, color);
}
void vita2d_draw_texture_tint_scale(const vita2d_texture *t, float x, float y, float xs, float ys, unsigned int color) {
    if (!t) return;
    vita2d_draw_texture_tint_part_scale(t, x, y, 0, 0, t->gxm_tex.width, t->gxm_tex.height, xs, ys, color);
}
void vita2d_draw_texture_scale(const vita2d_texture *t, float x, float y, float xs, float ys) {
    vita2d_draw_texture_tint_scale(t, x, y, xs, ys, 0xFFFFFFFF);
}
void vita2d_draw_texture(const vita2d_texture *t, float x, float y) { vita2d_draw_texture_scale(t, x, y, 1, 1); }

/* ---------------------------------------------------------------- image files */

static vita2d_texture *load_image(const char *filename) {
    char p[1024];
    port_path(filename, p, sizeof(p));
    int w, h, n;
    unsigned char *px = stbi_load(p, &w, &h, &n, 4);
    if (!px) return NULL;
    vita2d_texture *t = vita2d_create_empty_texture(w, h);
    if (t) memcpy(t->pixels, px, (size_t)w * h * 4);
    stbi_image_free(px);
    return t;
}
vita2d_texture *vita2d_load_JPEG_file(const char *filename) { return load_image(filename); }
vita2d_texture *vita2d_load_PNG_file(const char *filename) { return load_image(filename); }

/* ---------------------------------------------------------------- fonts */

typedef struct {
    int font_id;
    unsigned int cp;
    int px;          /* rasterised at this pixel size */
    int page;
    float u0, v0, u1, v1;
    int w, h, xoff, yoff;
    int used;
} Glyph;

#define GPAGES 8
#define GPAGE_SIZE 1024
static GLuint gpage[GPAGES];
static int gpages_n, gp_x, gp_y, gp_row;
#define GCACHE 8192
static Glyph gcache[GCACHE];

struct vita2d_font {
    stbtt_fontinfo info;
    unsigned char *data;
    int id;
};
static int font_ids;

vita2d_font *vita2d_load_font_mem(const void *buffer, unsigned int size) {
    vita2d_font *f = calloc(1, sizeof(vita2d_font));
    if (!f) return NULL;
    f->data = malloc(size);
    if (!f->data) { free(f); return NULL; }
    memcpy(f->data, buffer, size);
    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        free(f->data);
        free(f);
        return NULL;
    }
    f->id = ++font_ids;
    return f;
}
void vita2d_free_font(vita2d_font *f) {
    if (!f) return;
    /* forget its glyphs */
    for (int i = 0; i < GCACHE; i++)
        if (gcache[i].used && gcache[i].font_id == f->id) gcache[i].used = 0;
    free(f->data);
    free(f);
}

static void gpages_clear(void) {
    for (int i = 0; i < GCACHE; i++) gcache[i].used = 0;
    gp_x = gp_y = gp_row = 0;
    gpages_n = gpages_n ? 1 : 0;
}

static Glyph *glyph_get(vita2d_font *f, unsigned int cp, int px) {
    unsigned int hsh = (cp * 2654435761u) ^ (unsigned)(px * 40503) ^ (unsigned)(f->id * 97);
    for (int probe = 0; probe < 32; probe++) {
        Glyph *g = &gcache[(hsh + probe) % GCACHE];
        if (g->used && g->font_id == f->id && g->cp == cp && g->px == px) return g;
        if (!g->used) {
            /* rasterise */
            float scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)px);
            int gi = stbtt_FindGlyphIndex(&f->info, cp);
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(&f->info, gi, scale, scale, &x0, &y0, &x1, &y1);
            int w = x1 - x0, h = y1 - y0;
            if (w > 0 && h > 0) {
                if (gp_x + w + 1 > GPAGE_SIZE) { gp_x = 0; gp_y += gp_row + 1; gp_row = 0; }
                if (!gpages_n || gp_y + h + 1 > GPAGE_SIZE) {
                    if (gpages_n >= GPAGES) { gpages_clear(); return glyph_get(f, cp, px); }
                    if (!gpage[gpages_n]) {
                        new_gl_tex(&gpage[gpages_n], 1);
                        unsigned char *z = calloc(1, GPAGE_SIZE * GPAGE_SIZE);
                        glTexImage2D_(GL_TEXTURE_2D, 0, GL_ALPHA, GPAGE_SIZE, GPAGE_SIZE, 0, GL_ALPHA, GL_UNSIGNED_BYTE, z);
                        free(z);
                    }
                    gpages_n++;
                    gp_x = gp_y = gp_row = 0;
                }
                unsigned char *bm = calloc(1, (size_t)w * h);
                if (bm) {
                    stbtt_MakeGlyphBitmap(&f->info, bm, w, h, w, scale, scale, gi);
                    glBindTexture_(GL_TEXTURE_2D, gpage[gpages_n - 1]);
                    glTexSubImage2D_(GL_TEXTURE_2D, 0, gp_x, gp_y, w, h, GL_ALPHA, GL_UNSIGNED_BYTE, bm);
                    free(bm);
                }
                g->page = gpages_n - 1;
                g->u0 = gp_x / (float)GPAGE_SIZE;
                g->v0 = gp_y / (float)GPAGE_SIZE;
                g->u1 = (gp_x + w) / (float)GPAGE_SIZE;
                g->v1 = (gp_y + h) / (float)GPAGE_SIZE;
                gp_x += w + 1;
                if (h > gp_row) gp_row = h;
            } else g->page = -1;
            g->w = w;
            g->h = h;
            g->xoff = x0;
            g->yoff = y0;
            g->font_id = f->id;
            g->cp = cp;
            g->px = px;
            g->used = 1;
            return g;
        }
    }
    gpages_clear(); /* the cache is full: start again */
    return glyph_get(f, cp, px);
}

static int utf8_next(const unsigned char **s) {
    const unsigned char *p = *s;
    unsigned int c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = p;
    return (int)c;
}

/* the same layout as the Vita: y is the baseline, whole-pixel advances, kerning */
static int font_run(vita2d_font *f, int draw, int x, int y, unsigned int color, unsigned int size, const char *text) {
    if (!f || !text) return 0;
    float lscale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)size);
    float s = target ? 1.0f : sc;
    int px = (int)lroundf(size * s);
    if (px < 1) px = 1;
    int pen_x = x, pen_y = y, start = x, max_x = x, prev = 0;
    int cur_page = -1, nq = 0;
    vita2d_texture_vertex *q = NULL;
    if (draw) q = vita2d_pool_memalign(sizeof(vita2d_texture_vertex) * 6 * (strlen(text) + 1), 16);
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        int cp = utf8_next(&p);
        if (cp == '\n') {
            if (pen_x > max_x) max_x = pen_x;
            pen_x = start;
            pen_y += size;
            prev = 0;
            continue;
        }
        if (prev) pen_x += (int)lroundf(stbtt_GetCodepointKernAdvance(&f->info, prev, cp) * lscale);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
        if (draw && q) {
            Glyph *g = glyph_get(f, (unsigned)cp, px);
            if (g->page >= 0) {
                if (cur_page != g->page && nq) {
                    /* flush the run drawn from the previous page */
                    use(P_ALPHA);
                    glActiveTexture_(GL_TEXTURE0);
                    glBindTexture_(GL_TEXTURE_2D, gpage[cur_page]);
                    glUniform1i_(u_tex[P_ALPHA], 0);
                    tint4(P_ALPHA, color);
                    glEnableVertexAttribArray_(0);
                    glDisableVertexAttribArray_(1);
                    glEnableVertexAttribArray_(2);
                    glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, sizeof(vita2d_texture_vertex), &q->x);
                    glVertexAttribPointer_(2, 2, GL_FLOAT, GL_FALSE, sizeof(vita2d_texture_vertex), &q->u);
                    glDrawArrays_(GL_TRIANGLES, 0, nq);
                    q += nq;
                    nq = 0;
                }
                cur_page = g->page;
                /* place it on whole screen pixels so it stays crisp */
                float kx = target ? 1.0f : scx, ky = target ? 1.0f : scy;
                float sx0 = target ? pen_x + g->xoff : roundf(offx + pen_x * kx) + g->xoff;
                float sy0 = target ? pen_y + g->yoff : roundf(offy + pen_y * ky) + g->yoff;
                float lx0 = target ? sx0 : (sx0 - offx) / kx, ly0 = target ? sy0 : (sy0 - offy) / ky;
                float lx1 = lx0 + g->w / kx, ly1 = ly0 + g->h / ky;
                vita2d_texture_vertex *v = q + nq;
                v[0] = (vita2d_texture_vertex){lx0, ly0, 0.5f, g->u0, g->v0};
                v[1] = (vita2d_texture_vertex){lx1, ly0, 0.5f, g->u1, g->v0};
                v[2] = (vita2d_texture_vertex){lx0, ly1, 0.5f, g->u0, g->v1};
                v[3] = (vita2d_texture_vertex){lx1, ly0, 0.5f, g->u1, g->v0};
                v[4] = (vita2d_texture_vertex){lx1, ly1, 0.5f, g->u1, g->v1};
                v[5] = (vita2d_texture_vertex){lx0, ly1, 0.5f, g->u0, g->v1};
                nq += 6;
            }
        }
        pen_x += (int)lroundf(adv * lscale);
        prev = cp;
    }
    if (draw && q && nq && cur_page >= 0) {
        use(P_ALPHA);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture_(GL_TEXTURE_2D, gpage[cur_page]);
        glUniform1i_(u_tex[P_ALPHA], 0);
        tint4(P_ALPHA, color);
        glEnableVertexAttribArray_(0);
        glDisableVertexAttribArray_(1);
        glEnableVertexAttribArray_(2);
        glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, sizeof(vita2d_texture_vertex), &q->x);
        glVertexAttribPointer_(2, 2, GL_FLOAT, GL_FALSE, sizeof(vita2d_texture_vertex), &q->u);
        glDrawArrays_(GL_TRIANGLES, 0, nq);
    }
    if (pen_x > max_x) max_x = pen_x;
    return max_x - x;
}

int vita2d_font_draw_text(vita2d_font *f, int x, int y, unsigned int color, unsigned int size, const char *text) {
    return font_run(f, 1, x, y, color, size, text);
}
int vita2d_font_text_width(vita2d_font *f, unsigned int size, const char *text) { return font_run(f, 0, 0, 0, 0, size, text); }
int vita2d_font_text_height(vita2d_font *f, unsigned int size, const char *text) {
    (void)f;
    int lines = 1;
    for (const char *c = text; c && *c; c++) lines += *c == '\n';
    return lines * (int)size;
}

/* ---------------------------------------------------------------- for the keyboard box */
void port_draw_text_raw(vita2d_font *f, int x, int y, unsigned int color, unsigned int size, const char *text) {
    font_run(f, 1, x, y, color, size, text);
}
