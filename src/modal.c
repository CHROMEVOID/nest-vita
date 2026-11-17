/* Sheets that float over any screen: menus, confirmations, rating,
   collections and the quick-actions menu for a title. */
#include "nest.h"

#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>

enum { M_NONE, M_MENU, M_CONFIRM, M_RATE, M_COLL, M_QUICK };

static struct {
    int type;
    char title[96], body[256];
    char items[20][64];
    int n, focus;
    float anim, fsel;
    int closing, result;
    modal_cb cb;
    void *ud;
    Title *t;
    int stars;
    int cids[18];
    int qact[16];
    int guard; /* frames to ignore input after opening (stops tap-through) */
} M;

int g_pending_secret_tid = -1;

static void move_to_secret(Title *t) {
    if (!t->secret && !db_get("pin_hash", NULL)) {
        g_pending_secret_tid = t->id; /* moved once the PIN is set */
        toast("First choose a PIN for the Secret tab");
        pin_open(PIN_CREATE);
        return;
    }
    lib_set_secret(t, !t->secret);
    toast(t->secret ? "Added to the Secret tab" : "Moved back to your library");
}

int modal_active(void) { return M.type != M_NONE; }

static void open_common(int type, const char *title) {
    memset(M.items, 0, sizeof(M.items));
    M.type = type;
    snprintf(M.title, sizeof(M.title), "%s", title ? title : "");
    M.body[0] = 0;
    M.n = 0;
    M.focus = 0;
    M.anim = 0;
    M.fsel = 0;
    M.closing = 0;
    M.cb = NULL;
    M.ud = NULL;
    M.guard = 3;
    sfx(1);
}

void modal_menu(const char *title, const char *const *items, int n, modal_cb cb, void *ud) {
    open_common(M_MENU, title);
    M.n = n > 20 ? 20 : n;
    for (int i = 0; i < M.n; i++) snprintf(M.items[i], 64, "%s", items[i]);
    M.cb = cb;
    M.ud = ud;
}

void modal_confirm(const char *title, const char *body, const char *yes, const char *no, modal_cb cb, void *ud) {
    open_common(M_CONFIRM, title);
    snprintf(M.body, sizeof(M.body), "%s", body ? body : "");
    snprintf(M.items[0], 64, "%s", yes);
    snprintf(M.items[1], 64, "%s", no);
    M.n = 2;
    M.cb = cb;
    M.ud = ud;
}

void modal_rate(Title *t) {
    open_common(M_RATE, t->title);
    M.t = t;
    M.stars = t->rating ? t->rating : 3;
    snprintf(M.body, sizeof(M.body), "%s", "How did it feel?");
}

void modal_rate_ex(Title *t, const char *header) {
    modal_rate(t);
    snprintf(M.body, sizeof(M.body), "%s", header);
}

static void build_collections(void);
static void new_collection_cb(const char *name, void *ud) {
    Title *t = ud;
    if (!name || !name[0] || !t) return;
    int cid = db_collection_create(name, t->secret);
    if (cid > 0) db_collection_toggle(cid, t->id);
    if (M.type == M_COLL) build_collections();
    toast("Collection created");
}

static void build_collections(void) {
    char names[18][64];
    int n = db_collections(M.t->secret, M.cids, names, 16);
    M.n = 0;
    for (int i = 0; i < n; i++) snprintf(M.items[M.n++], 64, "%s", names[i]);
    snprintf(M.items[M.n++], 64, "%s", "New collection\xe2\x80\xa6");
}

void modal_collections(Title *t) {
    open_common(M_COLL, "Add to collection");
    M.t = t;
    build_collections();
}

enum { Q_PLAY, Q_DETAILS, Q_FAV, Q_RATE, Q_COLL, Q_WATCHED, Q_SECRET, Q_SPOT };

void modal_quick(Title *t) {
    open_common(M_QUICK, t->title);
    M.t = t;
    int book = t->kind == KIND_BOOK;
    const char *labels[8];
    int acts[8], n = 0;
    labels[n] = t->resume >= 0 ? (book ? "Continue reading" : "Resume") : (book ? "Read" : "Play");
    acts[n++] = Q_PLAY;
    labels[n] = "Details";
    acts[n++] = Q_DETAILS;
    labels[n] = t->favorite ? "Remove from favorites" : "Add to favorites";
    acts[n++] = Q_FAV;
    labels[n] = "Rate\xe2\x80\xa6";
    acts[n++] = Q_RATE;
    labels[n] = "Add to collection\xe2\x80\xa6";
    acts[n++] = Q_COLL;
    labels[n] = t->progress >= 0.995f ? (book ? "Mark as unread" : "Mark as unwatched") : (book ? "Mark as read" : "Mark as watched");
    acts[n++] = Q_WATCHED;
    labels[n] = t->secret ? "Remove from Secret tab" : "Add to Secret tab";
    acts[n++] = Q_SECRET;
    labels[n] = "Artwork spotlight";
    acts[n++] = Q_SPOT;
    for (int i = 0; i < n; i++) {
        snprintf(M.items[i], 64, "%s", labels[i]);
        M.qact[i] = acts[i];
    }
    M.n = n;
}

static void play_title(Title *t) {
    int e = t->resume >= 0 ? t->resume : t->e0;
    if (t->ne <= 0) return;
    if (L.e[e].media == MEDIA_BOOK) {
        reader_open(e);
    } else if (!L.e[e].playable) {
        toast("This file needs converting to MP4 to play on Vita");
    } else {
        player_open(e, 1);
    }
}

static void close_modal(int result) {
    M.closing = 1;
    M.result = result;
}

static void finish(void) {
    int type = M.type, r = M.result;
    modal_cb cb = M.cb;
    void *ud = M.ud;
    Title *t = M.t;
    int act = (type == M_QUICK && r >= 0) ? M.qact[r] : -1;
    M.type = M_NONE;
    if (cb) cb(r, ud);
    if (type == M_QUICK && t && r >= 0) {
        switch (act) {
        case Q_PLAY: play_title(t); break;
        case Q_DETAILS: detail_open(t->id); break;
        case Q_FAV:
            lib_set_favorite(t, !t->favorite);
            toast(t->favorite ? "Added to favorites" : "Removed from favorites");
            break;
        case Q_RATE: modal_rate(t); break;
        case Q_COLL: modal_collections(t); break;
        case Q_WATCHED: {
            int w = t->progress < 0.995f;
            lib_mark_watched(t, w);
            toast(w ? "Marked as finished" : "Progress cleared");
            break;
        }
        case Q_SECRET: move_to_secret(t); break;
        case Q_SPOT: spotlight_open(t->id); break;
        }
    }
}

static int ime_run(const char *title, const char *initial, char *out, int max) {
    static uint16_t t16[128], i16[512], o16[512];
    int k = 0;
    /* utf8 -> utf16 (BMP) */
    for (const unsigned char *p = (const unsigned char *)title; *p && k < 127;) {
        unsigned c = *p++;
        if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        t16[k++] = (uint16_t)c;
    }
    t16[k] = 0;
    k = 0;
    for (const unsigned char *p = (const unsigned char *)(initial ? initial : ""); *p && k < 510;) {
        unsigned c = *p++;
        if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        i16[k++] = (uint16_t)c;
    }
    i16[k] = 0;
    memset(o16, 0, sizeof(o16));
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0;
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title = t16;
    p.maxTextLength = max - 1 < 500 ? max - 1 : 500;
    p.initialText = i16;
    p.inputTextBuffer = o16;
    if (sceImeDialogInit(&p) < 0) return 0;
    int ok = 0;
    for (;;) {
        vita2d_start_drawing();
        vita2d_clear_screen();
        {
            vita2d_texture *soft = lofi_soft(bg_scene_id());
            if (soft) gfx_tex(soft, 0, 0, SCR_W, SCR_H, 0, 0xFFFFFFFF, 0.5f, 0.5f);
        }
        gfx_rect(0, 0, SCR_W, SCR_H, col_a(T->bg, 0.6f));
        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        SceCommonDialogStatus st = sceImeDialogGetStatus();
        if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
            SceImeDialogResult res;
            memset(&res, 0, sizeof(res));
            sceImeDialogGetResult(&res);
            ok = res.button == SCE_IME_DIALOG_BUTTON_ENTER;
            sceImeDialogTerm();
            break;
        }
    }
    if (!ok) return 0;
    int o = 0;
    for (int i = 0; o16[i] && o < max - 4; i++) {
        unsigned c = o16[i];
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
    return 1;
}

void modal_frame(float dt) {
    if (M.type == M_NONE) return;
    M.anim = approach(M.anim, M.closing ? 0 : 1, 16, dt);
    if (M.closing && M.anim < 0.03f) {
        finish();
        return;
    }
    float a = M.anim;
    /* ignore the press that opened the sheet so it can't also pick a row */
    Input saved_in = in;
    if (M.guard > 0) {
        M.guard--;
        in.pressed = in.repeat = 0;
        in.tap = in.long_press = in.double_tap = 0;
    }
    /* ---- input ---- */
    if (!M.closing) {
        if (in.pressed & B_BACK) { sfx(2); close_modal(-1); }
        int horiz = M.type == M_CONFIRM || M.type == M_RATE;
        if (M.type == M_RATE) {
            if (in.repeat & B_LEFT) M.stars = clampi(M.stars - 1, 1, 5), sfx(0);
            if (in.repeat & B_RIGHT) M.stars = clampi(M.stars + 1, 1, 5), sfx(0);
            if (in.repeat & B_DOWN) M.stars = 0, sfx(0);
            if (in.pressed & B_OK) {
                lib_set_rating(M.t, M.stars);
                toast(M.stars ? "Rating saved" : "Rating cleared");
                close_modal(0);
            }
        } else if (horiz) {
            if (in.repeat & (B_LEFT | B_UP)) M.focus = 0, sfx(0);
            if (in.repeat & (B_RIGHT | B_DOWN)) M.focus = 1, sfx(0);
            if (in.pressed & B_OK) close_modal(M.focus == 0 ? 1 : 0);
        } else {
            if (in.repeat & B_UP) M.focus = (M.focus + M.n - 1) % M.n, sfx(0);
            if (in.repeat & B_DOWN) M.focus = (M.focus + 1) % M.n, sfx(0);
            if (in.pressed & B_OK) {
                sfx(1);
                if (M.type == M_COLL) {
                    if (M.focus == M.n - 1) {
                        ime_request("Collection name", "", 60, new_collection_cb, M.t);
                    } else {
                        db_collection_toggle(M.cids[M.focus], M.t->id);
                    }
                } else {
                    close_modal(M.focus);
                }
            }
        }
    }
    M.fsel = approach(M.fsel, (float)M.focus, 20, dt);

    /* ---- draw ---- */
    gfx_rect(0, 0, SCR_W, SCR_H, col_a(0xFF000000, (T->light ? 0.35f : 0.55f) * a));
    float w = M.type == M_CONFIRM ? 440 : 400;
    float rowh = 44;
    float head = 70;
    float bodyh = 0;
    if (M.type == M_CONFIRM) bodyh = text_wrap_count(F_REG, 15, w - 56, M.body) * 21 + 18;
    float listh = (M.type == M_MENU || M.type == M_QUICK || M.type == M_COLL) ? M.n * rowh + 8 : 0;
    if (M.type == M_COLL) listh += 0;
    float h = head + bodyh + listh + (M.type == M_CONFIRM ? 70 : 0) + (M.type == M_RATE ? 120 : 0) + 22;
    if (h > SCR_H - 40) h = SCR_H - 40;
    float x = (SCR_W - w) / 2, y = (SCR_H - h) / 2 + (1 - ease_out(a)) * 30;
    gfx_shadow(x, y + 10, w, h, T->radius + 4, 40, col_a(0xFF000000, 0.45f * a));
    gfx_rrect(x, y, w, h, T->radius + 4, col_a(T->light ? T->surface : col_mix(T->surface, T->surface2, 0.5f), a));
    gfx_rrect_outline(x, y, w, h, T->radius + 4, 1, col_a(T->text, 0.08f * a));

    float tx = x + 28;
    if (M.type == M_QUICK && M.t) {
        /* tiny poster beside the title */
        float fade = 0;
        const char *src = title_poster(M.t);
        vita2d_texture *tex = src ? img_get(src, 44, 66, &fade) : NULL;
        if (tex) gfx_tex(tex, x + 24, y + 20, 30, 45, 5, col_a(0xFFFFFFFF, a * fade), 0.5f, 0.2f);
        else draw_placeholder(M.t, x + 24, y + 20, 30, 45, 5);
        tx = x + 66;
    }
    text_ellipsis(F_DISPLAY, T->display_font == 2 ? 18 : 21, tx, y + 44, x + w - tx - 24, col_a(T->text, a), M.title);
    float cy = y + head;
    if (M.type == M_CONFIRM) {
        text_wrap(F_REG, 15, x + 28, cy + 6, w - 56, 21, 6, col_a(T->text2, a), M.body);
        cy += bodyh;
        float bw = (w - 56 - 12) / 2;
        for (int i = 0; i < 2; i++) {
            float bx = x + 28 + i * (bw + 12), by = cy + 6;
            float f = (M.focus == i) ? 1 : 0;
            draw_button(bx, by, bw, 44, -1, M.items[i], i == 0, f);
            if (hit(bx, by, bw, 44) && !M.closing) close_modal(i == 0 ? 1 : 0);
        }
    } else if (M.type == M_RATE) {
        text(F_REG, 15, x + 28, cy + 4, col_a(T->text2, a), M.body);
        float s = 34, sx = x + (w - (5 * s + 4 * 14)) / 2, sy = cy + 26;
        for (int i = 0; i < 5; i++) {
            float bx = sx + i * (s + 14);
            int on = i < M.stars;
            float pop = on && i == M.stars - 1 ? 1.1f : 1.0f;
            icon(on ? IC_STAR_FILL : IC_STAR, bx + s / 2, sy + s / 2, s * pop, on ? col_a(T->accent2, a) : col_a(T->text3, a));
            if (hit(bx - 6, sy - 6, s + 12, s + 12) && !M.closing) {
                M.stars = i + 1;
                lib_set_rating(M.t, M.stars);
                toast("Rating saved");
                close_modal(0);
            }
        }
        static const char *words[6] = {"No rating", "Not for me", "It was okay", "Liked it", "Really good", "Loved it"};
        text_center(F_SEMI, 14, x + w / 2, sy + s + 30, col_a(T->text, 0.8f * a), words[clampi(M.stars, 0, 5)]);
        static const char *hb[] = {"OK", "Save", "BACK", "Cancel"};
        (void)hb;
    } else {
        for (int i = 0; i < M.n; i++) {
            float ry = cy + i * rowh;
            if (ry + rowh > y + h - 10) break;
            float f = clampf(1 - fabsf(M.fsel - i), 0, 1);
            if (f > 0) gfx_rrect(x + 12, ry + 2, w - 24, rowh - 4, T->radius_sm, col_a(T->text, (T->light ? 0.07f : 0.09f) * f * a));
            int check = M.type == M_COLL && i < M.n - 1 && db_collection_has(M.cids[i], M.t->id);
            col_t c = col_a(i == M.focus ? T->text : T->text2, a);
            if (M.type == M_COLL && i == M.n - 1) {
                icon(IC_PLUS, x + 38, ry + rowh / 2, 16, c);
                text(F_SEMI, 15, x + 56, ry + 27, c, M.items[i]);
            } else {
                text(F_SEMI, 15, x + 28, ry + 27, c, M.items[i]);
            }
            if (check) icon(IC_CHECK, x + w - 40, ry + rowh / 2, 18, col_a(T->accent, a));
            if (hit(x + 12, ry, w - 24, rowh) && !M.closing) {
                M.focus = i;
                if (M.type == M_COLL) {
                    if (i == M.n - 1) {
                        ime_request("Collection name", "", 60, new_collection_cb, M.t);
                    } else {
                        db_collection_toggle(M.cids[i], M.t->id);
                    }
                } else {
                    close_modal(i);
                }
            }
        }
    }
    /* tap outside closes */
    if (in.tap && !M.closing && !(in.tap_x >= x && in.tap_x < x + w && in.tap_y >= y && in.tap_y < y + h)) close_modal(-1);
    in = saved_in;
}

/* The system keyboard has its own draw loop, so it must never start in the
   middle of a frame. Requests are queued and run between frames. */
static struct {
    int pending;
    char title[96], initial[256];
    int max;
    ime_cb cb;
    void *ud;
} IME;

void ime_request(const char *title, const char *initial, int max, ime_cb cb, void *ud) {
    snprintf(IME.title, sizeof(IME.title), "%s", title ? title : "");
    snprintf(IME.initial, sizeof(IME.initial), "%s", initial ? initial : "");
    IME.max = max > 255 ? 255 : max;
    IME.cb = cb;
    IME.ud = ud;
    IME.pending = 1;
}

void ime_pump(void) {
    if (!IME.pending) return;
    IME.pending = 0;
    char out[260];
    out[0] = 0;
    int ok = ime_run(IME.title, IME.initial, out, IME.max);
    if (IME.cb) IME.cb(ok ? out : NULL, IME.ud);
}
