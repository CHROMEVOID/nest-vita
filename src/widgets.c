/* Shared visual building blocks. */
#include "nest.h"

int g_poster_caption;

float poster_w(void) {
    static const float sizes[3] = {104, 122, 146};
    float w = sizes[clampi(S.poster_size, 0, 2)];
    return S.ambient ? w * 1.14f : w;
}

const char *bg_scene_id(void) {
    if (S.bg_scene > 0 && S.bg_scene <= LOFI_SCENE_COUNT) return LOFI_SCENES[S.bg_scene - 1];
    return T->scene;
}

/* the animated lofi scene behind the app, dimmed so text and posters stay
   easy to read; strength lets busy screens ask for a little less */
void draw_scene_bg(float strength) {
    if (!S.show_lofi || S.bg_level <= 0) return;
    Lofi *l = lofi_get(bg_scene_id());
    if (!l) return;
    lofi_draw(l, 0, 0, SCR_W, SCR_H, 0, 0xFFFFFFFF, 0.5f, 0.5f);
    float veil = S.bg_level == 1 ? 0.80f : 0.58f;
    veil = 1.0f - (1.0f - veil) * clampf(strength, 0, 1.5f);
    gfx_rect(0, 0, SCR_W, SCR_H, col_a(T->bg, clampf(veil, 0, 1)));
    gfx_grad_v(0, SCR_H - 140, SCR_W, 140, col_a(T->bg, 0), col_a(T->bg, 0.5f));
}

void draw_backdrop_scene(float alpha) {
    if (!S.show_lofi || alpha <= 0) return;
    Lofi *l = lofi_get(T->scene);
    if (!l) return;
    lofi_draw(l, 0, 0, SCR_W, SCR_H, 0, col_a(0xFFFFFFFF, alpha), 0.5f, 0.5f);
    gfx_rect(0, 0, SCR_W, SCR_H, col_a(T->bg, 0.35f));
}

/* deterministic soft gradient placeholder with initials */
void draw_placeholder(Title *t, float x, float y, float w, float h, float r) {
    unsigned hsh = 2166136261u;
    for (const char *p = t->title; *p; p++) hsh = (hsh ^ (unsigned char)*p) * 16777619u;
    if (t->kind == KIND_BOOK) {
        /* a typeset cloth cover: muted colour from the title, spine, rules */
        static const unsigned cloth[8] = {0x3B4A63, 0x6B3A3A, 0x2F5446, 0x5A4A2E, 0x433A5E, 0x2E4F5C, 0x6A5236, 0x3E3E3E};
        col_t c = HEX(cloth[hsh % 8]);
        gfx_rrect_grad(x, y, w, h, r, col_mix(c, 0xFFFFFFFF, 0.06f), col_mix(c, 0xFF000000, 0.25f));
        gfx_clip(x, y, w, h);
        gfx_grad_h(x, y, w * 0.09f, h, col_a(0xFF000000, 0.35f), col_a(0xFF000000, 0.0f)); /* spine shadow */
        gfx_rect(x + w * 0.09f, y, 1.5f, h, col_a(0xFFFFFFFF, 0.08f));                     /* hinge highlight */
        gfx_noclip();
        col_t ink = HEX(0xF1E7D2);
        float m = w * 0.16f;
        gfx_rect(x + m, y + h * 0.17f, w - m * 1.6f, 1.5f, col_a(ink, 0.55f));
        int fs = w > 120 ? 19 : 15;
        int lines = text_wrap_count(F_DISPLAY, fs, w - m * 1.8f, t->title);
        if (lines > 4) lines = 4;
        float ty = y + h * 0.42f - (lines - 1) * (fs + 3) / 2.0f;
        text_wrap(F_DISPLAY, fs, x + m, ty, w - m * 1.8f, fs + 3, 4, col_a(ink, 0.95f), t->title);
        gfx_rect(x + m, ty + (lines - 1) * (fs + 3) + 12, w - m * 1.6f, 1.5f, col_a(ink, 0.55f));
        if (t->ne > 1) {
            char vols[24];
            snprintf(vols, sizeof(vols), "%d volumes", t->ne);
            text(F_META, 11, x + m, y + h - 16, col_a(ink, 0.65f), vols);
        }
        return;
    }
    col_t a = col_mix(T->surface2, T->accent, 0.18f + (hsh % 7) * 0.02f);
    col_t b = col_mix(T->surface, T->accent2, 0.08f + (hsh >> 8) % 5 * 0.02f);
    gfx_rrect_grad(x, y, w, h, r, a, b);
    /* soft top glow instead of fine lines (thin lines shimmer while scrolling) */
    gfx_rrect_grad(x, y, w, h * 0.5f, r, col_a(0xFFFFFFFF, T->light ? 0.10f : 0.05f), col_a(0xFFFFFFFF, 0.0f));
    /* initials */
    char ini[8] = {0};
    int n = 0;
    const char *p = t->title;
    if (!strncasecmp(p, "the ", 4)) p += 4;
    int start = 1;
    for (; *p && n < 2; p++) {
        if (start && isalnum((unsigned char)*p)) ini[n++] = (char)toupper((unsigned char)*p);
        start = (*p == ' ');
    }
    int fs = w > 120 ? 34 : 26;
    text_center(F_DISPLAY, fs, x + w / 2, y + h * 0.46f, col_a(T->text, 0.85f), ini);
    /* small title at the bottom (skipped where a caption sits underneath) */
    if (g_poster_caption) goto skip_title;
    int ts = 12;
    float ty = y + h - 30;
    int lines = text_wrap_count(F_SEMI, ts, w - 18, t->title);
    if (lines > 2) lines = 2;
    text_wrap(F_SEMI, ts, x + 9, ty - (lines - 1) * 14, w - 18, 14, 2, col_a(T->text, 0.7f), t->title);
skip_title:
    if (t->kind == KIND_BOOK) icon(IC_BOOK, x + w - 16, y + 16, 14, col_a(T->text, 0.45f));
}

void draw_focus(float x, float y, float w, float h, float r, float f) {
    if (f <= 0.01f) return;
    switch (T->focus_style) {
    case 1: /* glow */
        gfx_shadow(x, y, w, h, r, 14 * f, col_a(T->focus, 0.45f * f));
        gfx_rrect_outline(x - 2, y - 2, w + 4, h + 4, r + 2, 2, col_a(T->focus, 0.9f * f));
        break;
    case 2: /* underline */
        gfx_rrect(x + w * 0.2f, y + h + 7, w * 0.6f * f, 3, 1.5f, col_a(T->focus, f));
        break;
    default: /* ring */
        gfx_rrect_outline(x - 4, y - 4, w + 8, h + 8, r + 4, 2.5f, col_a(T->focus, 0.95f * f));
        break;
    }
}

void draw_progress(float x, float y, float w, float h, float p) {
    gfx_rrect(x, y, w, h, h / 2, col_a(T->light ? 0xFF000000 : 0xFFFFFFFF, 0.22f));
    if (p > 0) gfx_rrect(x, y, fmaxf(h, w * clampf(p, 0, 1)), h, h / 2, T->accent);
}

void draw_poster(Title *t, float x, float y, float w, float h, float focus, float alpha) {
    if (alpha <= 0) return;
    float lift = (T->focus_style == 1 ? 0.03f : 0.05f) * focus;
    float cx = x + w / 2, cy = y + h / 2;
    float dw = w * (1 + lift), dh = h * (1 + lift);
    float dx = cx - dw / 2, dy = cy - dh / 2 - 4 * focus;
    float r = T->radius_sm;
    gfx_shadow(dx, dy + 6, dw, dh, r, 12 + 8 * focus, col_a(T->shadow, alpha * (0.6f + 0.4f * focus)));
    float fade = 0;
    const char *src = title_poster(t);
    int from_frame = 0;
    if (!src && t->backdrop) {
        src = t->backdrop; /* a still from the video, cropped to portrait */
        from_frame = 1;
    }
    vita2d_texture *tex = src ? img_get(src, from_frame ? 480 : (int)(w * 1.1f), from_frame ? 270 : (int)(h * 1.1f), &fade) : NULL;
    if (!tex || fade < 1) draw_placeholder(t, dx, dy, dw, dh, r);
    if (tex) gfx_tex(tex, dx, dy, dw, dh, r, col_a(0xFFFFFFFF, alpha * ease_out(fade)), 0.5f, from_frame ? 0.5f : 0.15f);
    if (tex && from_frame) {
        /* give it a poster feel: soft fade and the title */
        gfx_rrect_grad(dx, dy + dh * 0.45f, dw, dh * 0.55f, r, 0x00000000, col_a(0xFF000000, 0.85f * alpha * fade));
        int lines = text_wrap_count(F_DISPLAY, 17, dw - 16, t->title);
        if (lines > 3) lines = 3;
        text_wrap(F_DISPLAY, 17, dx + 9, dy + dh - 14 - (lines - 1) * 19, dw - 18, 19, 3, col_a(0xFFFFFFFF, alpha * fade), t->title);
    }
    if (alpha < 1) gfx_rrect(dx, dy, dw, dh, r, col_a(T->bg, 1 - alpha));
    /* subtle edge for definition: 2px and faint so it can't shimmer */
    gfx_rrect_outline(dx, dy, dw, dh, r, 2, col_a(T->light ? 0xFF000000 : 0xFFFFFFFF, 0.04f * alpha));
    /* progress or watched badge */
    if (t->progress > 0.01f && t->progress < 0.995f)
        draw_progress(dx + 8, dy + dh - 10, dw - 16, 4, t->progress);
    else if (t->progress >= 0.995f) {
        gfx_circle(dx + dw - 14, dy + 14, 9, col_a(T->accent, alpha));
        icon(IC_CHECK, dx + dw - 14, dy + 14, 12, col_a(T->on_accent, alpha));
    }
    if (t->favorite) {
        gfx_circle(dx + 14, dy + 14, 9, col_a(0xFF000000, 0.35f * alpha));
        icon(IC_HEART_FILL, dx + 14, dy + 14.5f, 11, col_a(T->accent2, alpha));
    }
    draw_focus(dx, dy, dw, dh, r, focus);
}

void draw_wide_card(Title *t, int eidx, float x, float y, float w, float h, float focus, float alpha) {
    float lift = 0.04f * focus;
    float dw = w * (1 + lift), dh = h * (1 + lift);
    float dx = x + (w - dw) / 2, dy = y + (h - dh) / 2 - 3 * focus;
    float r = T->radius_sm;
    gfx_shadow(dx, dy + 6, dw, dh, r, 12 + 8 * focus, col_a(T->shadow, alpha * (0.6f + 0.4f * focus)));
    float fade = 0;
    const char *src = t->backdrop ? t->backdrop : title_poster(t);
    vita2d_texture *tex = src ? img_get(src, (int)(w * 1.1f), (int)(h * 1.1f), &fade) : NULL;
    if (!tex || fade < 1) {
        if (S.show_lofi) {
            /* the theme's scene as a soft stand-in for missing artwork */
            vita2d_texture *soft = lofi_soft(T->scene);
            if (soft) gfx_tex(soft, dx, dy, dw, dh, r, col_a(0xFFFFFFFF, alpha), 0.5f, 0.5f);
            else draw_placeholder(t, dx, dy, dw, dh, r);
        } else {
            draw_placeholder(t, dx, dy, dw, dh, r);
        }
    }
    if (tex) gfx_tex(tex, dx, dy, dw, dh, r, col_a(0xFFFFFFFF, alpha * ease_out(fade)), 0.5f, t->backdrop ? 0.4f : 0.2f);
    gfx_rrect_grad(dx, dy + dh * 0.35f, dw, dh * 0.65f, r, 0x00000000, col_a(0xFF000000, 0.78f * alpha));
    /* text */
    char lab[64] = "";
    Entry *e = eidx >= 0 ? &L.e[eidx] : NULL;
    if (e && t->kind != KIND_MOVIE) lib_entry_label(e, lab, sizeof(lab));
    char left[48] = "";
    if (e && e->dur > 0 && e->media == MEDIA_VIDEO) {
        int rem = (e->dur - e->pos) / 60000;
        if (rem > 0) snprintf(left, sizeof(left), "%dm left", rem);
    } else if (e && e->media == MEDIA_BOOK && e->dur > 0) {
        snprintf(left, sizeof(left), "p. %d / %d", e->pos + 1, e->dur);
    }
    text_ellipsis(F_SEMI, 14, dx + 12, dy + dh - 30, dw - 24, col_a(0xFFFFFFFF, alpha), t->title);
    char line[96];
    if (lab[0] && left[0]) snprintf(line, sizeof(line), "%s  \xc2\xb7  %s", lab, left);
    else snprintf(line, sizeof(line), "%s%s", lab, left);
    text_ellipsis(F_META, 12, dx + 12, dy + dh - 13, dw - 24, col_a(0xFFFFFFFF, 0.72f * alpha), line);
    float p = e && e->dur > 0 ? (e->finished ? 1 : (float)e->pos / e->dur) : 0;
    if (p > 0) {
        gfx_rect(dx + 2, dy + dh - 3, dw - 4, 3, col_a(0xFFFFFFFF, 0.2f * alpha));
        gfx_rect(dx + 2, dy + dh - 3, (dw - 4) * p, 3, col_a(T->accent, alpha));
    }
    if (focus > 0.05f) {
        gfx_circle(dx + dw / 2, dy + dh / 2 - 8, 20 * focus, col_a(0xFFFFFFFF, 0.9f * focus));
        icon(e && e->media == MEDIA_BOOK ? IC_BOOK : IC_PLAY, dx + dw / 2 + (e && e->media == MEDIA_BOOK ? 0 : 1.5f),
             dy + dh / 2 - 8, 18 * focus, col_a(0xFF101010, focus));
    }
    draw_focus(dx, dy, dw, dh, r, focus);
}

void draw_button(float x, float y, float w, float h, int ic, const char *label, int primary, float focus) {
    float r = T->nav_style == 0 ? h / 2 : T->radius_sm;
    col_t bg = primary ? T->accent : col_a(T->light ? 0xFF000000 : 0xFFFFFFFF, 0.1f + 0.06f * focus);
    col_t fg = primary ? T->on_accent : T->text;
    if (!primary && focus > 0.5f && T->focus_style != 2) {
        bg = col_mix(bg, T->text, 0.92f * focus);
        fg = col_mix(T->text, T->bg, focus);
    }
    float grow = 2 * focus;
    if (primary) gfx_shadow(x, y + 4, w, h, r, 10 + 6 * focus, col_a(T->accent, 0.25f + 0.2f * focus));
    gfx_rrect(x - grow, y - grow, w + grow * 2, h + grow * 2, r + grow, bg);
    if (!primary) gfx_rrect_outline(x, y, w, h, r, 1, col_a(T->text, 0.12f));
    float tw = label && *label ? text_w(F_SEMI, 15, label) : 0;
    float iw = ic >= 0 ? 20 : 0;
    float gap = (iw && tw) ? 8 : 0;
    float sx = x + (w - (iw + gap + tw)) / 2;
    if (ic >= 0) icon(ic, sx + 10, y + h / 2, 18, fg);
    if (tw) text(F_SEMI, 15, sx + iw + gap, y + h / 2 + 5.5f, fg, label);
    if (primary) draw_focus(x - grow, y - grow, w + grow * 2, h + grow * 2, r + grow, focus);
    else if (T->focus_style == 2) draw_focus(x, y, w, h, r, focus);
}

void draw_chip(float x, float y, const char *label, int active, float focus, float *out_w) {
    float w = text_w(F_SEMI, 13, label) + 28, h = 30;
    float r = T->nav_style == 0 ? h / 2 : T->radius_sm * 0.8f;
    col_t bg = active ? col_a(T->text, 0.92f) : col_a(T->text, 0.07f + 0.08f * focus);
    col_t fg = active ? T->bg : col_mix(T->text2, T->text, focus);
    gfx_rrect(x, y, w, h, r, bg);
    if (!active) gfx_rrect_outline(x, y, w, h, r, 1, col_a(T->text, 0.1f + 0.2f * focus));
    text(F_SEMI, 13, x + 14, y + 19.5f, fg, label);
    if (focus > 0.05f) {
        if (T->focus_style == 2) gfx_rrect(x + w * 0.25f, y + h + 5, w * 0.5f * focus, 2.5f, 1.2f, col_a(T->focus, focus));
        else gfx_rrect_outline(x - 3, y - 3, w + 6, h + 6, r + 3, 2, col_a(T->focus, 0.9f * focus));
    }
    if (out_w) *out_w = w;
}

static void glyph(const char *k, float x, float cy) {
    col_t c = col_a(T->text, 0.85f);
    float r = 8.5f;
    if (!strcmp(k, "OK") || !strcmp(k, "BACK")) {
        int cross = (!strcmp(k, "OK")) == g_enter_cross;
        gfx_ring(x + r, cy, r, 1.4f, col_a(T->text, 0.5f));
        if (cross) {
            gfx_line(x + r - 3.5f, cy - 3.5f, x + r + 3.5f, cy + 3.5f, 1.6f, c);
            gfx_line(x + r + 3.5f, cy - 3.5f, x + r - 3.5f, cy + 3.5f, 1.6f, c);
        } else {
            gfx_ring(x + r, cy, 4.2f, 1.5f, c);
        }
    } else if (!strcmp(k, "TRI")) {
        gfx_ring(x + r, cy, r, 1.4f, col_a(T->text, 0.5f));
        gfx_line(x + r, cy - 4, x + r - 4, cy + 3, 1.5f, c);
        gfx_line(x + r - 4, cy + 3, x + r + 4, cy + 3, 1.5f, c);
        gfx_line(x + r + 4, cy + 3, x + r, cy - 4, 1.5f, c);
    } else if (!strcmp(k, "SQR")) {
        gfx_ring(x + r, cy, r, 1.4f, col_a(T->text, 0.5f));
        gfx_rrect_outline(x + r - 3.8f, cy - 3.8f, 7.6f, 7.6f, 0.5f, 1.5f, c);
    } else {
        float w = text_w(F_SEMI, 11, k) + 12;
        gfx_rrect_outline(x, cy - 8.5f, w, 17, 5, 1.3f, col_a(T->text, 0.5f));
        text(F_SEMI, 11, x + 6, cy + 4, c, k);
    }
}

static float glyph_w(const char *k) {
    if (!strcmp(k, "OK") || !strcmp(k, "BACK") || !strcmp(k, "TRI") || !strcmp(k, "SQR")) return 17;
    return text_w(F_SEMI, 11, k) + 12;
}

void draw_hint_bar(const char *const *items, int n) {
    float total = 0;
    for (int i = 0; i < n; i += 2) total += glyph_w(items[i]) + 6 + text_w(F_REG, 12, items[i + 1]) + 18;
    float x = SCR_W - 28 - total, cy = SCR_H - 20;
    for (int i = 0; i < n; i += 2) {
        glyph(items[i], x, cy);
        x += glyph_w(items[i]) + 6;
        text(F_REG, 12, x, cy + 4.5f, col_a(T->text2, 0.9f), items[i + 1]);
        x += text_w(F_REG, 12, items[i + 1]) + 18;
    }
}

void draw_stars(float x, float y, float s, int v, col_t on, col_t off) {
    for (int i = 0; i < 5; i++) icon(i < v ? IC_STAR_FILL : IC_STAR, x + i * (s + 4) + s / 2, y + s / 2, s, i < v ? on : off);
}

void draw_spinner(float cx, float cy, float r, col_t c) {
    for (int i = 0; i < 10; i++) {
        float a = g_time * 4 + i * 0.32f;
        gfx_circle(cx + cosf(a) * r, cy + sinf(a) * r, 2.6f * (0.35f + i / 15.0f), col_a(c, 0.15f + i * 0.085f));
    }
}

/* ---------------- toasts ---------------- */
static char toast_msg[128];
static float toast_t = -1;

void toast(const char *msg) {
    snprintf(toast_msg, sizeof(toast_msg), "%s", msg);
    toast_t = 0;
}

void draw_toasts(float dt) {
    if (toast_t < 0) return;
    toast_t += dt;
    float life = 2.4f;
    if (toast_t > life) {
        toast_t = -1;
        return;
    }
    float a = clampf(fminf(toast_t / 0.2f, (life - toast_t) / 0.35f), 0, 1);
    float w = text_w(F_SEMI, 14, toast_msg) + 40, h = 38;
    float x = (SCR_W - w) / 2, y = SCR_H - 70 + (1 - ease_out(a)) * 14;
    gfx_shadow(x, y + 4, w, h, h / 2, 14, col_a(T->shadow, a));
    gfx_rrect(x, y, w, h, h / 2, col_a(col_mix(T->surface2, T->text, T->light ? 0.0f : 0.06f), 0.97f * a));
    text_center(F_SEMI, 14, SCR_W / 2.0f, y + 24, col_a(T->text, a), toast_msg);
}
