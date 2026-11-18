/* Drawing primitives: anti-aliased (4x MSAA) rounded shapes, soft shadows,
   gradients, rounded textures, text and a small line-icon set. */
#include "nest.h"

static void *font_buf[6];
static unsigned font_len[6];
static const char *font_file[6];

typedef struct {
    int f, size;
    vita2d_font *font;
} FontSlot;
static FontSlot slots[48];
static int nslots;

col_t col_a(col_t c, float a) {
    a = clampf(a, 0, 1);
    unsigned al = (unsigned)(((c >> 24) & 255) * a + 0.5f);
    return (c & 0x00FFFFFF) | (al << 24);
}

col_t col_mix(col_t a, col_t b, float t) {
    t = clampf(t, 0, 1);
    unsigned r = (unsigned)lerpf(a & 255, b & 255, t);
    unsigned g = (unsigned)lerpf((a >> 8) & 255, (b >> 8) & 255, t);
    unsigned bl = (unsigned)lerpf((a >> 16) & 255, (b >> 16) & 255, t);
    unsigned al = (unsigned)lerpf((a >> 24) & 255, (b >> 24) & 255, t);
    return r | (g << 8) | (bl << 16) | (al << 24);
}

static void *load_file(const char *path, unsigned *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc(n);
    if (b && fread(b, 1, n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (unsigned)n;
    return b;
}

static void set_font(int role, const char *file) {
    if (font_file[role] && !strcmp(font_file[role], file)) return;
    /* free slots that use this role */
    for (int i = 0; i < nslots; i++)
        if (slots[i].f == role && slots[i].font) {
            vita2d_wait_rendering_done();
            vita2d_free_font(slots[i].font);
            slots[i] = slots[--nslots];
            i--;
        }
    /* share buffers between roles that use the same file */
    for (int r = 0; r < F_COUNT; r++)
        if (r != role && font_file[r] && !strcmp(font_file[r], file)) {
            int used_elsewhere = 0;
            for (int k = 0; k < F_COUNT; k++)
                if (k != role && font_buf[k] == font_buf[role]) used_elsewhere = 1;
            if (font_buf[role] && !used_elsewhere) free(font_buf[role]);
            font_buf[role] = font_buf[r];
            font_len[role] = font_len[r];
            font_file[role] = file;
            return;
        }
    int used_elsewhere = 0;
    for (int k = 0; k < F_COUNT; k++)
        if (k != role && font_buf[k] && font_buf[k] == font_buf[role]) used_elsewhere = 1;
    if (font_buf[role] && !used_elsewhere) free(font_buf[role]);
    font_buf[role] = load_file(file, &font_len[role]);
    font_file[role] = file;
}

static int want_display = -1, want_meta = -1;

/* font swaps are deferred to the start of a frame: freeing a font atlas
   that was already drawn this frame would crash the GPU */
void gfx_load_fonts(int display_font, int meta_mono) {
    want_display = display_font;
    want_meta = meta_mono;
}

void gfx_frame_begin(void) {
    if (want_display < 0) return;
    int display_font = want_display, meta_mono = want_meta;
    want_display = -1;
    set_font(F_REG, ASSET("fonts/Inter-Regular.ttf"));
    set_font(F_SEMI, ASSET("fonts/Inter-SemiBold.ttf"));
    set_font(F_BOLD, ASSET("fonts/Inter-Bold.ttf"));
    set_font(F_DISPLAY, display_font == 1   ? ASSET("fonts/DMSerifDisplay-Regular.ttf")
                        : display_font == 2 ? ASSET("fonts/SpaceMono-Bold.ttf")
                                            : ASSET("fonts/Inter-Bold.ttf"));
    set_font(F_META, meta_mono ? ASSET("fonts/SpaceMono-Regular.ttf") : ASSET("fonts/Inter-Regular.ttf"));
}

/* one atlas per (font, size) so large titles never crowd out small text */
static vita2d_font *font_for(int f, int size) {
    for (int i = 0; i < nslots; i++)
        if (slots[i].f == f && slots[i].size == size) return slots[i].font;
    if (!font_buf[f]) return NULL;
    if (nslots == (int)(sizeof(slots) / sizeof(slots[0]))) {
        vita2d_wait_rendering_done();
        vita2d_free_font(slots[0].font);
        slots[0] = slots[--nslots];
    }
    vita2d_font *vf = vita2d_load_font_mem(font_buf[f], font_len[f]);
    if (!vf) return NULL;
    slots[nslots].f = f;
    slots[nslots].size = size;
    slots[nslots].font = vf;
    nslots++;
    return vf;
}

void gfx_init(void) {
    vita2d_init_advanced_with_msaa(4 * 1024 * 1024, SCE_GXM_MULTISAMPLE_4X);
    vita2d_set_vblank_wait(1);
}

/* ---------- geometry helpers ---------- */

#define MAXP 96
static int rr_seg(float r) { return r < 1 ? 1 : clampi((int)(r / 1.6f) + 3, 4, 14); }
static int rr_points_n(float x, float y, float w, float h, float r, float *px, float *py, int seg) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (seg <= 0) seg = rr_seg(r);
    int n = 0;
    const float cx[4] = {x + w - r, x + r, x + r, x + w - r};
    const float cy[4] = {y + r, y + r, y + h - r, y + h - r};
    const float a0[4] = {-M_PI / 2, -M_PI, M_PI / 2, 0};
    /* order: top-right, top-left, bottom-left, bottom-right going counter clockwise */
    const float start[4] = {0, -M_PI / 2, -M_PI, M_PI / 2};
    (void)a0;
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= seg; i++) {
            float a = start[c] - (float)i / seg * (M_PI / 2);
            if (n >= MAXP) break;
            px[n] = cx[c] + cosf(a) * r;
            py[n] = cy[c] + sinf(a) * r;
            n++;
        }
    }
    return n;
}
static int rr_points(float x, float y, float w, float h, float r, float *px, float *py) {
    return rr_points_n(x, y, w, h, r, px, py, 0);
}

static vita2d_color_vertex *cverts(int n) {
    return (vita2d_color_vertex *)vita2d_pool_memalign(n * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
}

void gfx_rect(float x, float y, float w, float h, col_t c) {
    if ((c >> 24) == 0 || w <= 0 || h <= 0) return;
    vita2d_draw_rectangle(x, y, w, h, c);
}

void gfx_rrect_grad(float x, float y, float w, float h, float r, col_t top, col_t bottom) {
    if (w <= 0 || h <= 0) return;
    if (((top >> 24) | (bottom >> 24)) == 0) return;
    float px[MAXP], py[MAXP];
    int n = rr_points(x, y, w, h, r, px, py);
    vita2d_color_vertex *v = cverts(n + 2);
    if (!v) return;
    v[0].x = x + w / 2;
    v[0].y = y + h / 2;
    v[0].z = 0.5f;
    v[0].color = col_mix(top, bottom, 0.5f);
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        v[i + 1].x = px[k];
        v[i + 1].y = py[k];
        v[i + 1].z = 0.5f;
        v[i + 1].color = top == bottom ? top : col_mix(top, bottom, (py[k] - y) / h);
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 2);
}

void gfx_rrect(float x, float y, float w, float h, float r, col_t c) { gfx_rrect_grad(x, y, w, h, r, c, c); }

static void ring_strip(float x, float y, float w, float h, float r, float grow, col_t cin, col_t cout) {
    float pi[MAXP], qi[MAXP], po[MAXP], qo[MAXP];
    float rc = r;
    if (rc > w / 2) rc = w / 2;
    if (rc > h / 2) rc = h / 2;
    if (rc < 0) rc = 0;
    int seg = rr_seg(rc + grow);
    int n = rr_points_n(x, y, w, h, rc, pi, qi, seg);
    rr_points_n(x - grow, y - grow, w + grow * 2, h + grow * 2, rc + grow, po, qo, seg);
    vita2d_color_vertex *v = cverts((n + 1) * 2);
    if (!v) return;
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        v[i * 2].x = pi[k];
        v[i * 2].y = qi[k];
        v[i * 2].z = 0.5f;
        v[i * 2].color = cin;
        v[i * 2 + 1].x = po[k];
        v[i * 2 + 1].y = qo[k];
        v[i * 2 + 1].z = 0.5f;
        v[i * 2 + 1].color = cout;
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, (n + 1) * 2);
}

void gfx_rrect_outline(float x, float y, float w, float h, float r, float t, col_t c) {
    if ((c >> 24) == 0) return;
    ring_strip(x + t / 2, y + t / 2, w - t, h - t, r - t / 2 > 0 ? r - t / 2 : 0, t, c, c);
}

void gfx_shadow(float x, float y, float w, float h, float r, float spread, col_t c) {
    if ((c >> 24) == 0) return;
    ring_strip(x, y, w, h, r, spread, c, c & 0x00FFFFFF);
}

void gfx_grad_v(float x, float y, float w, float h, col_t top, col_t bottom) {
    vita2d_color_vertex *v = cverts(4);
    if (!v) return;
    float xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    col_t cs[4] = {top, top, bottom, bottom};
    for (int i = 0; i < 4; i++) v[i] = (vita2d_color_vertex){xs[i], ys[i], 0.5f, cs[i]};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void gfx_grad_h(float x, float y, float w, float h, col_t left, col_t right) {
    vita2d_color_vertex *v = cverts(4);
    if (!v) return;
    float xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    col_t cs[4] = {left, right, left, right};
    for (int i = 0; i < 4; i++) v[i] = (vita2d_color_vertex){xs[i], ys[i], 0.5f, cs[i]};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
}

void gfx_circle(float x, float y, float r, col_t c) { gfx_rrect(x - r, y - r, r * 2, r * 2, r, c); }

void gfx_ring(float x, float y, float r, float t, col_t c) {
    gfx_rrect_outline(x - r, y - r, r * 2, r * 2, r, t, c);
}

void gfx_line(float x0, float y0, float x1, float y1, float t, col_t c) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    vita2d_color_vertex *v = cverts(4);
    if (!v) return;
    v[0] = (vita2d_color_vertex){x0 + nx, y0 + ny, 0.5f, c};
    v[1] = (vita2d_color_vertex){x0 - nx, y0 - ny, 0.5f, c};
    v[2] = (vita2d_color_vertex){x1 + nx, y1 + ny, 0.5f, c};
    v[3] = (vita2d_color_vertex){x1 - nx, y1 - ny, 0.5f, c};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4);
    /* round caps */
    if (t >= 2.5f) {
        gfx_rrect(x0 - t / 2, y0 - t / 2, t, t, t / 2, c);
        gfx_rrect(x1 - t / 2, y1 - t / 2, t, t, t / 2, c);
    }
}

void gfx_tex(vita2d_texture *tex, float x, float y, float w, float h, float r, col_t tint, float ax, float ay) {
    if (!tex || w <= 0 || h <= 0) return;
    float tw = vita2d_texture_get_width(tex), th = vita2d_texture_get_height(tex);
    float s = fmaxf(w / tw, h / th);
    float vw = w / s, vh = h / s;          /* visible region in texels */
    float u0 = (tw - vw) * clampf(ax, 0, 1), v0 = (th - vh) * clampf(ay, 0, 1);
    float px[MAXP], py[MAXP];
    int n = rr_points(x, y, w, h, r, px, py);
    vita2d_texture_vertex *v =
        vita2d_pool_memalign((n + 2) * sizeof(vita2d_texture_vertex), sizeof(vita2d_texture_vertex));
    if (!v) return;
    v[0].x = x + w / 2;
    v[0].y = y + h / 2;
    v[0].z = 0.5f;
    v[0].u = (u0 + vw / 2) / tw;
    v[0].v = (v0 + vh / 2) / th;
    for (int i = 0; i <= n; i++) {
        int k = i % n;
        v[i + 1].x = px[k];
        v[i + 1].y = py[k];
        v[i + 1].z = 0.5f;
        v[i + 1].u = (u0 + (px[k] - x) / w * vw) / tw;
        v[i + 1].v = (v0 + (py[k] - y) / h * vh) / th;
    }
    vita2d_draw_array_textured(tex, SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, n + 2, tint);
}

void gfx_tex_fit(vita2d_texture *tex, float x, float y, float w, float h, col_t tint) {
    if (!tex) return;
    float tw = vita2d_texture_get_width(tex), th = vita2d_texture_get_height(tex);
    float s = fminf(w / tw, h / th);
    float dw = tw * s, dh = th * s;
    vita2d_draw_texture_tint_scale(tex, x + (w - dw) / 2, y + (h - dh) / 2, s, s, tint);
}

void gfx_clip(float x, float y, float w, float h) {
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle((int)x, (int)y, (int)(x + w), (int)(y + h));
}
void gfx_noclip(void) { vita2d_disable_clipping(); }

/* ---------- text ---------- */

void text(int f, int size, float x, float y, col_t c, const char *s) {
    if (!s || !*s || (c >> 24) == 0) return;
    vita2d_font *vf = font_for(f, size);
    if (vf) vita2d_font_draw_text(vf, (int)roundf(x), (int)roundf(y), c, size, s);
}

int text_w(int f, int size, const char *s) {
    if (!s || !*s) return 0;
    vita2d_font *vf = font_for(f, size);
    return vf ? vita2d_font_text_width(vf, size, s) : 0;
}

void text_center(int f, int size, float cx, float y, col_t c, const char *s) {
    text(f, size, cx - text_w(f, size, s) / 2.0f, y, c, s);
}

void text_right(int f, int size, float rx, float y, col_t c, const char *s) {
    text(f, size, rx - text_w(f, size, s), y, c, s);
}

static int utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

void text_ellipsis(int f, int size, float x, float y, float maxw, col_t c, const char *s) {
    if (!s) return;
    if (text_w(f, size, s) <= maxw) {
        text(f, size, x, y, c, s);
        return;
    }
    char buf[512];
    int n = (int)strlen(s);
    if (n > 500) n = 500;
    memcpy(buf, s, n);
    buf[n] = 0;
    int ew = text_w(f, size, "\xe2\x80\xa6");
    /* binary-ish shrink */
    while (n > 0) {
        n = utf8_prev(buf, n);
        buf[n] = 0;
        if (text_w(f, size, buf) + ew <= maxw) break;
    }
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
    strcat(buf, "\xe2\x80\xa6");
    text(f, size, x, y, c, buf);
}

static int wrap_impl(int draw, int f, int size, float x, float y, float maxw, float lh, int max_lines, col_t c,
                     const char *s) {
    if (!s || !*s) return 0;
    char line[512];
    int lines = 0;
    const char *p = s;
    while (*p && (max_lines <= 0 || lines < max_lines)) {
        while (*p == ' ' || *p == '\n') {
            if (*p == '\n' && 0) break;
            p++;
        }
        if (!*p) break;
        int ll = 0;
        const char *best = NULL;
        int best_len = 0;
        const char *q = p;
        while (*q && *q != '\n') {
            const char *w_end = q;
            while (*w_end && *w_end != ' ' && *w_end != '\n') w_end++;
            int add = (int)(w_end - q);
            if (ll + add + 1 >= (int)sizeof(line)) break;
            memcpy(line + ll, q, add);
            line[ll + add] = 0;
            if (text_w(f, size, line) > maxw && best) break;
            best = w_end;
            best_len = ll + add;
            ll += add;
            if (*w_end == ' ') line[ll++] = ' ';
            line[ll] = 0;
            q = w_end;
            while (*q == ' ') q++;
        }
        if (!best) break;
        line[best_len] = 0;
        int last = (max_lines > 0 && lines == max_lines - 1);
        const char *rest = best;
        while (*rest == ' ') rest++;
        if (draw) {
            if (last && *rest && *rest != '\n') {
                char tmp[600];
                snprintf(tmp, sizeof(tmp), "%s %s", line, rest);
                text_ellipsis(f, size, x, y + lines * lh, maxw, c, tmp);
            } else {
                text(f, size, x, y + lines * lh, c, line);
            }
        }
        lines++;
        p = best;
        if (*p == '\n') p++;
    }
    return lines;
}

int text_wrap(int f, int size, float x, float y, float maxw, float line_h, int max_lines, col_t c, const char *s) {
    return wrap_impl(1, f, size, x, y, maxw, line_h, max_lines, c, s);
}
int text_wrap_count(int f, int size, float maxw, const char *s) { return wrap_impl(0, f, size, 0, 0, maxw, 0, 0, 0, s); }

/* ---------- icons (24-unit grid, 2px strokes) ---------- */

void icon(int id, float cx, float cy, float s, col_t c) {
    float k = s / 24.0f;
    float t = fmaxf(1.6f, 2.0f * k);
#define X(v) (cx + ((v) - 12) * k)
#define Y(v) (cy + ((v) - 12) * k)
#define L(a, b, c2, d) gfx_line(X(a), Y(b), X(c2), Y(d), t, c)
    switch (id) {
    case IC_PLAY: {
        vita2d_color_vertex *v = cverts(3);
        if (!v) break;
        v[0] = (vita2d_color_vertex){X(7), Y(4.5f), 0.5f, c};
        v[1] = (vita2d_color_vertex){X(7), Y(19.5f), 0.5f, c};
        v[2] = (vita2d_color_vertex){X(19.5f), Y(12), 0.5f, c};
        vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
        break;
    }
    case IC_PAUSE:
        gfx_rrect(X(6), Y(4.5f), 4 * k, 15 * k, 1.2f * k, c);
        gfx_rrect(X(14), Y(4.5f), 4 * k, 15 * k, 1.2f * k, c);
        break;
    case IC_SEARCH:
        gfx_ring(X(10.5f), Y(10.5f), 6.5f * k, t, c);
        L(15.5f, 15.5f, 20, 20);
        break;
    case IC_GEAR:
        gfx_ring(cx, cy, 6.5f * k, t, c);
        gfx_ring(cx, cy, 2.4f * k, t, c);
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4;
            gfx_line(cx + cosf(a) * 7.5f * k, cy + sinf(a) * 7.5f * k, cx + cosf(a) * 10 * k, cy + sinf(a) * 10 * k,
                     t * 1.3f, c);
        }
        break;
    case IC_LOCK:
    case IC_UNLOCK:
        gfx_rrect(X(5), Y(10.5f), 14 * k, 10 * k, 2.5f * k, c);
        if (id == IC_LOCK) {
            L(8, 10.5f, 8, 7.5f);
            L(16, 10.5f, 16, 7.5f);
            gfx_rrect_outline(X(8) - t / 2, Y(3.5f) - t / 2, 8 * k + t, 8 * k + t, 4 * k + t / 2, t, c);
            gfx_rect(X(7), Y(7.5f), 10 * k + 1, 3 * k, c);
        } else {
            L(8, 10.5f, 8, 7);
            gfx_rrect_outline(X(8) - t / 2, Y(3) - t / 2, 8 * k + t, 8 * k + t, 4 * k + t / 2, t, c);
        }
        break;
    case IC_HEART:
    case IC_HEART_FILL: {
        /* two circles and a triangle */
        float r = 4.6f * k;
        if (id == IC_HEART_FILL) {
            gfx_circle(X(8), Y(9.5f), r, c);
            gfx_circle(X(16), Y(9.5f), r, c);
            vita2d_color_vertex *v = cverts(3);
            if (v) {
                v[0] = (vita2d_color_vertex){X(3.6f), Y(11), 0.5f, c};
                v[1] = (vita2d_color_vertex){X(20.4f), Y(11), 0.5f, c};
                v[2] = (vita2d_color_vertex){X(12), Y(20.5f), 0.5f, c};
                vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
            }
        } else {
            for (int i = 0; i <= 10; i++) {
                float a0 = (float)M_PI * (0.85f + i * 0.115f), a1 = (float)M_PI * (0.85f + (i + 1) * 0.115f);
                if (i < 10) {
                    gfx_line(X(8) + cosf(a0) * r, Y(9.5f) + sinf(a0) * r, X(8) + cosf(a1) * r, Y(9.5f) + sinf(a1) * r, t, c);
                    float b0 = (float)M_PI * (1.0f + i * 0.115f) + 0.05f, b1 = b0 + 0.115f * (float)M_PI;
                    gfx_line(X(16) + cosf(b0) * r, Y(9.5f) + sinf(b0) * r, X(16) + cosf(b1) * r, Y(9.5f) + sinf(b1) * r, t, c);
                }
            }
            L(3.8f, 11.2f, 12, 20);
            L(20.2f, 11.2f, 12, 20);
        }
        break;
    }
    case IC_STAR:
    case IC_STAR_FILL: {
        float px[10], py[10];
        for (int i = 0; i < 10; i++) {
            float a = -M_PI / 2 + i * M_PI / 5;
            float rr = (i % 2 ? 4.2f : 9.5f) * k;
            px[i] = cx + cosf(a) * rr;
            py[i] = cy + 0.6f * k + sinf(a) * rr;
        }
        if (id == IC_STAR_FILL) {
            vita2d_color_vertex *v = cverts(12);
            if (!v) break;
            v[0] = (vita2d_color_vertex){cx, cy + 0.6f * k, 0.5f, c};
            for (int i = 0; i <= 10; i++) v[i + 1] = (vita2d_color_vertex){px[i % 10], py[i % 10], 0.5f, c};
            vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLE_FAN, v, 12);
        } else {
            for (int i = 0; i < 10; i++) gfx_line(px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], t * 0.85f, c);
        }
        break;
    }
    case IC_BACK:
    case IC_CHEVRON_L:
        L(14.5f, 5.5f, 8, 12);
        L(8, 12, 14.5f, 18.5f);
        if (id == IC_BACK) L(8.5f, 12, 19, 12);
        break;
    case IC_CHEVRON_R:
        L(9.5f, 5.5f, 16, 12);
        L(16, 12, 9.5f, 18.5f);
        break;
    case IC_CHEVRON_D:
        L(6, 9.5f, 12, 15.5f);
        L(12, 15.5f, 18, 9.5f);
        break;
    case IC_PLUS:
        L(12, 5, 12, 19);
        L(5, 12, 19, 12);
        break;
    case IC_CHECK:
        L(5, 12.5f, 10, 17.5f);
        L(10, 17.5f, 19.5f, 7);
        break;
    case IC_CLOSE:
        L(6.5f, 6.5f, 17.5f, 17.5f);
        L(17.5f, 6.5f, 6.5f, 17.5f);
        break;
    case IC_HOME:
        L(4, 11.5f, 12, 4.5f);
        L(12, 4.5f, 20, 11.5f);
        L(6.5f, 10, 6.5f, 19.5f);
        L(17.5f, 10, 17.5f, 19.5f);
        L(6.5f, 19.5f, 17.5f, 19.5f);
        break;
    case IC_FILM:
        gfx_rrect_outline(X(4), Y(5), 16 * k, 14 * k, 2.5f * k, t, c);
        L(8.5f, 5, 8.5f, 19);
        L(15.5f, 5, 15.5f, 19);
        L(4, 12, 8.5f, 12);
        L(15.5f, 12, 20, 12);
        break;
    case IC_TV:
        gfx_rrect_outline(X(3.5f), Y(6), 17 * k, 12 * k, 2.5f * k, t, c);
        L(8, 21, 16, 21);
        L(9, 3, 12, 6);
        L(15, 3, 12, 6);
        break;
    case IC_BOOK:
        L(12, 6.5f, 12, 19.5f);
        L(12, 6.5f, 4, 5);
        L(4, 5, 4, 18);
        L(4, 18, 12, 19.5f);
        L(12, 6.5f, 20, 5);
        L(20, 5, 20, 18);
        L(20, 18, 12, 19.5f);
        break;
    case IC_NEXT:
    case IC_PREV: {
        int d = id == IC_NEXT ? 1 : -1;
        vita2d_color_vertex *v = cverts(3);
        if (!v) break;
        v[0] = (vita2d_color_vertex){cx - d * 6 * k, cy - 7 * k, 0.5f, c};
        v[1] = (vita2d_color_vertex){cx - d * 6 * k, cy + 7 * k, 0.5f, c};
        v[2] = (vita2d_color_vertex){cx + d * 4 * k, cy, 0.5f, c};
        vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
        gfx_rrect(cx + d * 5.5f * k - 1.5f * k, cy - 7 * k, 3 * k, 14 * k, 1 * k, c);
        break;
    }
    case IC_REW:
    case IC_FWD: {
        int d = id == IC_FWD ? 1 : -1;
        /* circular arrow */
        for (int i = 0; i < 14; i++) {
            float a0 = (float)M_PI * (-0.5f + d * (0.15f + i * 0.1f));
            float a1 = (float)M_PI * (-0.5f + d * (0.15f + (i + 1) * 0.1f));
            if (i < 13) gfx_line(cx + cosf(a0) * 8 * k, cy + sinf(a0) * 8 * k, cx + cosf(a1) * 8 * k, cy + sinf(a1) * 8 * k, t, c);
        }
        float ax = cx - d * 1.5f * k, ay = cy - 8 * k;
        gfx_line(ax, ay, ax - d * 3.5f * k, ay - 3 * k, t, c);
        gfx_line(ax, ay, ax - d * 3.5f * k, ay + 3 * k, t, c);
        break;
    }
    case IC_VOLUME:
        gfx_rrect(X(4), Y(9), 4 * k, 6 * k, 1 * k, c);
        L(8, 9, 13, 5);
        L(13, 5, 13, 19);
        L(13, 19, 8, 15);
        for (int i = 0; i < 6; i++) {
            float a0 = -0.9f + i * 0.3f, a1 = a0 + 0.3f;
            gfx_line(X(13) + cosf(a0) * 5 * k, cy + sinf(a0) * 5 * k, X(13) + cosf(a1) * 5 * k, cy + sinf(a1) * 5 * k, t, c);
        }
        break;
    case IC_SUBS:
        gfx_rrect_outline(X(3.5f), Y(5.5f), 17 * k, 13 * k, 3 * k, t, c);
        L(7, 14.5f, 11, 14.5f);
        L(13, 14.5f, 17, 14.5f);
        L(7, 11, 15, 11);
        break;
    case IC_SPEED:
        for (int i = 0; i < 12; i++) {
            float a0 = (float)M_PI * (0.8f + i * 0.12f), a1 = a0 + 0.12f * (float)M_PI;
            gfx_line(cx + cosf(a0) * 8.5f * k, Y(15) + sinf(a0) * 8.5f * k, cx + cosf(a1) * 8.5f * k, Y(15) + sinf(a1) * 8.5f * k, t, c);
        }
        L(12, 15, 16.5f, 9.5f);
        break;
    case IC_FOLDER:
        L(3.5f, 7, 3.5f, 18.5f);
        L(3.5f, 18.5f, 20.5f, 18.5f);
        L(20.5f, 18.5f, 20.5f, 9);
        L(20.5f, 9, 11, 9);
        L(11, 9, 9, 6);
        L(9, 6, 3.5f, 6);
        break;
    case IC_INFO:
        gfx_ring(cx, cy, 8.5f * k, t, c);
        L(12, 11, 12, 16.5f);
        gfx_circle(cx, Y(7.8f), 1.3f * k, c);
        break;
    case IC_GRID:
        for (int i = 0; i < 4; i++) gfx_rrect_outline(X(4 + (i % 2) * 9), Y(4 + (i / 2) * 9), 7 * k, 7 * k, 1.8f * k, t, c);
        break;
    case IC_CHART:
        L(5, 19, 5, 13);
        L(10, 19, 10, 8);
        L(15, 19, 15, 11);
        L(20, 19, 20, 5);
        break;
    case IC_CALENDAR:
        gfx_rrect_outline(X(4), Y(5.5f), 16 * k, 14.5f * k, 2.5f * k, t, c);
        L(4, 10, 20, 10);
        L(8.5f, 3.5f, 8.5f, 7);
        L(15.5f, 3.5f, 15.5f, 7);
        break;
    case IC_SPARK:
        L(12, 3.5f, 12, 20.5f);
        L(3.5f, 12, 20.5f, 12);
        L(7, 7, 17, 17);
        L(17, 7, 7, 17);
        break;
    case IC_DOTS:
        gfx_circle(X(6), cy, 1.8f * k, c);
        gfx_circle(cx, cy, 1.8f * k, c);
        gfx_circle(X(18), cy, 1.8f * k, c);
        break;
    case IC_EYE_OFF:
        for (int i = 0; i < 10; i++) {
            float a0 = (float)M_PI * (1.1f + i * 0.08f), a1 = a0 + 0.08f * (float)M_PI;
            gfx_line(cx + cosf(a0) * 11 * k, Y(17) + sinf(a0) * 11 * k, cx + cosf(a1) * 11 * k, Y(17) + sinf(a1) * 11 * k, t, c);
        }
        gfx_circle(cx, Y(12.5f), 2.4f * k, c);
        L(4.5f, 4.5f, 19.5f, 19.5f);
        break;
    case IC_REFRESH:
        for (int i = 0; i < 15; i++) {
            float a0 = (float)M_PI * (-0.3f + i * 0.11f), a1 = a0 + 0.11f * (float)M_PI;
            gfx_line(cx + cosf(a0) * 7.5f * k, cy + sinf(a0) * 7.5f * k, cx + cosf(a1) * 7.5f * k, cy + sinf(a1) * 7.5f * k, t, c);
        }
        L(17.5f, 4.5f, 18.5f, 8.5f);
        L(18.5f, 8.5f, 14.5f, 9);
        break;
    case IC_TRASH:
        L(4.5f, 6.5f, 19.5f, 6.5f);
        L(9.5f, 6.5f, 9.5f, 4);
        L(9.5f, 4, 14.5f, 4);
        L(14.5f, 4, 14.5f, 6.5f);
        L(6.5f, 6.5f, 7.5f, 20);
        L(7.5f, 20, 16.5f, 20);
        L(16.5f, 20, 17.5f, 6.5f);
        break;
    case IC_PENCIL:
        L(5, 19, 6, 15);
        L(6, 15, 16, 5);
        L(16, 5, 19, 8);
        L(19, 8, 9, 18);
        L(9, 18, 5, 19);
        break;
    case IC_IMAGE:
        gfx_rrect_outline(X(3.5f), Y(5), 17 * k, 14 * k, 2.5f * k, t, c);
        L(5, 17, 10, 11.5f);
        L(10, 11.5f, 13.5f, 15);
        L(13, 14.5f, 15.5f, 12);
        L(15.5f, 12, 19, 16);
        gfx_circle(X(15.5f), Y(8.8f), 1.5f * k, c);
        break;
    case IC_SHUFFLE: /* two crossing arrows */
        L(4, 7, 8, 7);
        L(8, 7, 15, 17);
        L(15, 17, 20, 17);
        L(4, 17, 8, 17);
        L(8, 17, 10.5f, 13.4f);
        L(12.5f, 10.6f, 15, 7);
        L(15, 7, 20, 7);
        L(17.5f, 4.5f, 20, 7);
        L(17.5f, 9.5f, 20, 7);
        L(17.5f, 14.5f, 20, 17);
        L(17.5f, 19.5f, 20, 17);
        break;
    case IC_REPEAT: /* a loop with an arrow */
        L(5, 11, 5, 8.5f);
        L(5, 8.5f, 7, 6.5f);
        L(7, 6.5f, 19, 6.5f);
        L(16.5f, 4, 19, 6.5f);
        L(16.5f, 9, 19, 6.5f);
        L(19, 13, 19, 15.5f);
        L(19, 15.5f, 17, 17.5f);
        L(17, 17.5f, 5, 17.5f);
        L(7.5f, 15, 5, 17.5f);
        L(7.5f, 20, 5, 17.5f);
        break;
    case IC_NOTE: /* a quaver */
        gfx_circle(X(8.5f), Y(17), 3.4f * k, c);
        gfx_circle(X(17), Y(15), 3.4f * k, c);
        L(11.6f, 17, 11.6f, 5.5f);
        L(20.1f, 15, 20.1f, 3.5f);
        L(11.6f, 5.5f, 20.1f, 3.5f);
        L(11.6f, 8.5f, 20.1f, 6.5f);
        break;
    case IC_QUEUE: /* lines with a little play mark */
        L(4, 6, 15, 6);
        L(4, 11, 15, 11);
        L(4, 16, 11, 16);
        {
            vita2d_color_vertex *v = cverts(3);
            if (v) {
                v[0] = (vita2d_color_vertex){X(15), Y(13), 0.5f, c};
                v[1] = (vita2d_color_vertex){X(15), Y(20), 0.5f, c};
                v[2] = (vita2d_color_vertex){X(20.5f), Y(16.5f), 0.5f, c};
                vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
            }
        }
        break;
    case IC_TIMER: /* a little clock */
        gfx_ring(cx, Y(13), 8 * k, t, c);
        L(12, 13, 12, 8.5f);
        L(12, 13, 15, 15);
        L(10, 2.8f, 14, 2.8f);
        break;
    case IC_SCREEN_OFF: /* a screen with a moon */
        gfx_rrect_outline(X(3), Y(5), 18 * k, 12 * k, 2 * k, t, c);
        L(9, 20, 15, 20);
        gfx_circle(X(12), Y(11), 3.2f * k, c);
        break;
    case IC_EQ: /* three sliders */
        L(6, 4, 6, 20);
        L(12, 4, 12, 20);
        L(18, 4, 18, 20);
        gfx_circle(X(6), Y(14), 2.8f * k, c);
        gfx_circle(X(12), Y(8), 2.8f * k, c);
        gfx_circle(X(18), Y(16), 2.8f * k, c);
        break;
    case IC_MIC:
        gfx_rrect_outline(X(9), Y(3), 6 * k, 11 * k, 3 * k, t, c);
        L(6, 11, 6, 12);
        L(18, 11, 18, 12);
        L(12, 17.5f, 12, 21);
        break;
    case IC_MOON:
        gfx_circle(cx, cy, 8 * k, c);
        break;
    case IC_DROP:
        gfx_circle(cx, Y(14), 5.5f * k, c);
        {
            vita2d_color_vertex *v = cverts(3);
            if (v) {
                v[0] = (vita2d_color_vertex){cx, Y(3.5f), 0.5f, c};
                v[1] = (vita2d_color_vertex){X(6.9f), Y(12.3f), 0.5f, c};
                v[2] = (vita2d_color_vertex){X(17.1f), Y(12.3f), 0.5f, c};
                vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
            }
        }
        break;
    }
#undef X
#undef Y
#undef L
}

/* ---------- glass ---------- */

void gfx_glass_tint(float x, float y, float w, float h, float r, float a, col_t tint) {
    if (a <= 0.01f) return;
    if (!S.glass) { /* plain, solid panels */
        gfx_shadow(x, y + 6, w, h, r, 16, col_a(T->shadow, 0.8f * a));
        gfx_rrect(x, y, w, h, r, col_a(col_mix(T->surface, T->surface2, 0.5f), 0.97f * a));
        return;
    }
    int light = T->light;
    gfx_shadow(x, y + 8, w, h, r, 22, col_a(0xFF000000, (light ? 0.14f : 0.32f) * a));
    /* the frosted body: lets the scene behind show through, tinted by the theme */
    col_t body = light ? col_a(0xFFFFFFFF, 0.62f * a) : col_a(col_mix(tint, 0xFF000000, 0.35f), 0.58f * a);
    gfx_rrect(x, y, w, h, r, body);
    /* gloss: bright at the top, fading down */
    gfx_rrect_grad(x, y, w, h, r, col_a(0xFFFFFFFF, (light ? 0.30f : 0.13f) * a), col_a(0xFFFFFFFF, 0.0f));
    /* a second, tighter shine along the top edge */
    float sh = h < 60 ? h * 0.45f : 26;
    gfx_rrect_grad(x + 1, y + 1, w - 2, sh, r, col_a(0xFFFFFFFF, (light ? 0.35f : 0.10f) * a), col_a(0xFFFFFFFF, 0));
    /* rim light and a crisp top highlight */
    gfx_rrect_outline(x, y, w, h, r, 1, col_a(0xFFFFFFFF, (light ? 0.55f : 0.14f) * a));
    if (w > r * 2 + 4) gfx_rect(x + r, y + 0.5f, w - r * 2, 1, col_a(0xFFFFFFFF, (light ? 0.8f : 0.30f) * a));
}

void gfx_glass(float x, float y, float w, float h, float r, float a) { gfx_glass_tint(x, y, w, h, r, a, T->surface2); }
