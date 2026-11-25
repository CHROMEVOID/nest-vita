/* Title page: artwork, story, actions, seasons/episodes or volumes, and
   "More like this". Also the full-screen Artwork Spotlight. */
#include "nest.h"

int g_pick_tid = -1;

static int tid = -1;
static int zone;            /* 0 buttons, 1 seasons, 2 entries, 3 more-like */
static int bi, si, ei, mi;
static float dsy, dsy_t, efsel, mfsel, enter_anim;
static int seasons[64], nseasons;
static int ents[2048], nents;
static int more[16], nmore;
static float more_sx, more_sx_t;
#define DM 40.0f

static Title *cur(void) { return lib_title_by_id(tid); }
static void build_entries(void);
void detail_refresh(void) { build_entries(); }

static void build_entries(void) {
    Title *t = cur();
    nents = 0;
    if (!t) return;
    int s = nseasons ? seasons[si] : -9999;
    for (int k = t->e0; k < t->e0 + t->ne && nents < 2048; k++)
        if (t->kind != KIND_SERIES || L.e[k].season == s) ents[nents++] = k;
    if (ei >= nents) ei = nents ? nents - 1 : 0;
}

static int similar(Title *s, int *out, int max) {
    static float sc[8192];
    int n = 0;
    static int cand[8192];
    for (int i = 0; i < L.nt && n < 8192; i++) {
        Title *t = &L.t[i];
        if (t == s || t->secret != s->secret) continue;
        if ((t->kind == KIND_BOOK) != (s->kind == KIND_BOOK)) continue;
        float v = 0;
        if (s->genres && t->genres) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", s->genres);
            char *save = NULL;
            for (char *g = strtok_r(buf, ",", &save); g; g = strtok_r(NULL, ",", &save)) {
                while (*g == ' ') g++;
                if (*g && strstr(t->genres, g)) v += 3;
            }
        }
        if (t->kind == s->kind) v += 1;
        if (s->year && t->year) v += fmaxf(0, 2 - fabsf((float)(s->year - t->year)) / 6);
        v += t->score * 0.1f;
        unsigned h = (unsigned)(t->id * 2654435761u) ^ (unsigned)s->id;
        v += (h % 100) / 200.0f;
        sc[n] = v;
        cand[n++] = i;
    }
    int m = 0;
    for (; m < max && m < n; m++) {
        int b = m;
        for (int k = m + 1; k < n; k++)
            if (sc[k] > sc[b]) b = k;
        float tv = sc[m];
        sc[m] = sc[b];
        sc[b] = tv;
        int ti = cand[m];
        cand[m] = cand[b];
        cand[b] = ti;
        out[m] = cand[m];
    }
    return m;
}

void detail_open(int id) {
    Title *t = lib_title_by_id(id);
    if (!t) return;
    tid = id;
    zone = 0;
    bi = 0;
    dsy = dsy_t = 0;
    enter_anim = 0;
    nseasons = 0;
    if (t->kind == KIND_SERIES) {
        int prev = -9999;
        for (int k = t->e0; k < t->e0 + t->ne && nseasons < 64; k++)
            if (L.e[k].season != prev) seasons[nseasons++] = prev = L.e[k].season;
    }
    si = 0;
    /* start on the season you are in */
    if (t->resume >= 0 && t->kind == KIND_SERIES)
        for (int i = 0; i < nseasons; i++)
            if (seasons[i] == L.e[t->resume].season) si = i;
    build_entries();
    ei = 0;
    if (t->resume >= 0)
        for (int i = 0; i < nents; i++)
            if (ents[i] == t->resume) ei = i;
    efsel = ei;
    nmore = similar(t, more, 12);
    mi = 0;
    more_sx = more_sx_t = 0;
    if (nav_top() == SCR_DETAIL) nav_replace(SCR_DETAIL);
    else nav_push(SCR_DETAIL);
}

static void play_entry(int e) {
    Entry *en = &L.e[e];
    if (en->media == MEDIA_BOOK) reader_open(e);
    else if (!en->playable) toast("This file needs converting to MP4 to play on Vita");
    else player_open(e, 1);
}

static void title_cb(const char *text, void *ud) {
    (void)ud;
    Title *t = cur();
    if (!text || !t) return;
    lib_set_title(t, text);
    toast("Title updated");
}

static void fmt_size(long long b, char *o, int n) {
    if (b >= 1024LL * 1024 * 1024) snprintf(o, n, "%.1f GB", b / 1073741824.0);
    else snprintf(o, n, "%d MB", (int)(b / 1048576));
}

static void delete_title_cb(int choice, void *ud) {
    (void)ud;
    Title *t = cur();
    if (choice != 1 || !t) return;
    int failed = 0;
    int n = remove_title_files(t, &failed);
    lib_load();
    thumb_reset();
    build_entries();
    char m[96];
    if (failed) snprintf(m, sizeof(m), "Deleted %d files; %d couldn't be removed", n, failed);
    else snprintf(m, sizeof(m), "Deleted from your Vita");
    toast(m);
}

static void ask_delete_title(Title *t) {
    if (t->ne > 0 && net_is_url(L.e[t->e0].path)) {
        toast("This is on your PC \xe2\x80\x94 delete it there");
        return;
    }
    long long bytes = 0;
    for (int k = t->e0; k < t->e0 + t->ne; k++) bytes += L.e[k].size;
    char sz[32], body[256];
    fmt_size(bytes, sz, sizeof(sz));
    snprintf(body, sizeof(body),
             "This permanently deletes %d %s (%s) and its artwork from the memory card. It can't be undone.",
             t->ne, t->kind == KIND_BOOK ? (t->ne == 1 ? "book" : "books") : (t->ne == 1 ? "file" : "files"), sz);
    modal_confirm("Delete from Vita?", body, "Delete", "Keep", delete_title_cb, NULL);
}

static void delete_entry_cb(int choice, void *ud) {
    int e = (int)(intptr_t)ud;
    Title *t = cur();
    if (choice != 1 || !t || e < 0 || e >= L.ne) return;
    char folder[512];
    db_title_folder(t->id, folder, sizeof(folder));
    int ok = remove_entry_files(e, folder);
    lib_load();
    build_entries();
    toast(ok ? "File deleted from your Vita" : "That file couldn't be deleted");
}

static void more_cb(int choice, void *ud) {
    Title *t = cur();
    (void)ud;
    if (!t || choice < 0) return;
    switch (choice) {
    case 0: ime_request("Title", t->title, 190, title_cb, NULL); break;
    case 1: g_pick_tid = t->id; picker_open(PICK_POSTER); break;
    case 2: g_pick_tid = t->id; picker_open(PICK_LOGO); break;
    case 3: g_pick_tid = t->id; picker_open(PICK_BACKDROP); break;
    case 4:
        lib_set_art(t, 0, NULL);
        lib_set_art(t, 1, NULL);
        lib_set_art(t, 2, NULL);
        lib_load();
        toast("Artwork reset to what was found in the folder");
        break;
    case 5: {
        int w = t->progress < 0.995f;
        lib_mark_watched(t, w);
        toast(w ? "Marked as finished" : "Progress cleared");
        break;
    }
    case 6: {
        extern int g_pending_secret_tid;
        if (!t->secret && !db_get("pin_hash", NULL)) {
            g_pending_secret_tid = t->id;
            toast("First choose a PIN for the Secret tab");
            pin_open(PIN_CREATE);
            break;
        }
        lib_set_secret(t, !t->secret);
        toast(t->secret ? "Added to the Secret tab" : "Moved back to your library");
        break;
    }
    case 7: spotlight_open(t->id); break;
    case 8: ask_delete_title(t); break;
    case 9: metafind_start(t->id); break;
    }
}

static void open_more(void) {
    Title *t = cur();
    static char w[40], s[40];
    snprintf(w, sizeof(w), "%s", t->progress >= 0.995f ? "Mark as unfinished" : "Mark as finished");
    snprintf(s, sizeof(s), "%s", t->secret ? "Remove from Secret tab" : "Add to Secret tab");
    const char *items[] = {"Edit title", "Choose poster image\xe2\x80\xa6", "Choose logo image\xe2\x80\xa6",
                           "Choose backdrop image\xe2\x80\xa6", "Reset artwork", w, s, "Artwork spotlight",
                           "Delete from Vita\xe2\x80\xa6", "Find info in the folder"};
    modal_menu("More", items, 10, more_cb, NULL);
}

static void entry_menu_cb(int choice, void *ud) {
    int e = (int)(intptr_t)ud;
    if (choice < 0 || e < 0 || e >= L.ne) return;
    Entry *en = &L.e[e];
    if (choice == 0) {
        en->pos = 0;
        if (en->media == MEDIA_BOOK) reader_open(e);
        else if (en->playable) player_open(e, 0);
        else toast("This file needs converting to MP4 to play on Vita");
    } else if (choice == 1) {
        int f = !en->finished;
        en->finished = f;
        if (!f) en->pos = 0;
        db_save_progress(en->path, en->pos, en->dur, f);
        en->updated = now_s();
        lib_derive();
        toast(f ? "Marked as finished" : "Marked as unfinished");
    } else if (choice == 2) {
        if (net_is_url(en->path)) {
            toast("This is on your PC \xe2\x80\x94 delete it there");
            return;
        }
        char body[400];
        snprintf(body, sizeof(body), "This permanently deletes \"%s\" from the memory card. It can't be undone.",
                 path_basename(en->path));
        modal_confirm("Delete this file?", body, "Delete", "Keep", delete_entry_cb, (void *)(intptr_t)e);
    }
}

static void button_action(Title *t, int i) {
    switch (i) {
    case 0: play_entry(t->resume >= 0 ? t->resume : t->e0); break;
    case 1:
        lib_set_favorite(t, !t->favorite);
        toast(t->favorite ? "Added to favorites" : "Removed from favorites");
        break;
    case 2: modal_rate(t); break;
    case 3: modal_collections(t); break;
    case 4: open_more(); break;
    }
}

static void entry_menu(int k) {
    Entry *en = &L.e[k];
    static const char *items[3];
    items[0] = en->media == MEDIA_BOOK ? "Read from the start" : "Play from the beginning";
    items[1] = en->finished ? "Mark as unfinished" : "Mark as finished";
    items[2] = "Delete from Vita\xe2\x80\xa6";
    modal_menu(path_basename(en->path), items, 3, entry_menu_cb, (void *)(intptr_t)k);
}

static void fmt_dur(int ms, char *b, int n) {
    int m = ms / 60000;
    if (m >= 60) snprintf(b, n, "%dh %02dm", m / 60, m % 60);
    else snprintf(b, n, "%dm", m);
}

void detail_frame(float dt) {
    Title *t = cur();
    if (!t) {
        nav_pop();
        return;
    }
    enter_anim = approach(enter_anim, 1, 7, dt);
    int has_list = t->ne > 1 || t->kind == KIND_SERIES; /* a single film or book needs no list */
    int nb = 5;

    /* ---------- input ---------- */
    if (in.pressed & B_BACK) { sfx(2); nav_pop(); }
    if (in.repeat & B_UP) {
        if (zone == 3) zone = has_list ? 2 : 0;
        else if (zone == 2) {
            if (ei > 0) ei--;
            else zone = nseasons > 1 ? 1 : 0;
        } else if (zone == 1) zone = 0;
        sfx(0);
    }
    if (in.repeat & B_DOWN) {
        if (zone == 0) zone = nseasons > 1 ? 1 : has_list && nents ? 2 : nmore ? 3 : 0;
        else if (zone == 1) zone = nents ? 2 : 1;
        else if (zone == 2) {
            if (ei < nents - 1) ei++;
            else if (nmore) zone = 3;
        }
        sfx(0);
    }
    if (zone == 0) {
        if (in.repeat & B_LEFT && bi > 0) bi--, sfx(0);
        if (in.repeat & B_RIGHT && bi < nb - 1) bi++, sfx(0);
    } else if (zone == 1) {
        int old = si;
        if (in.repeat & B_LEFT && si > 0) si--;
        if (in.repeat & B_RIGHT && si < nseasons - 1) si++;
        if (old != si) { build_entries(); ei = 0; efsel = 0; sfx(0); }
    } else if (zone == 3) {
        if (in.repeat & B_LEFT && mi > 0) mi--, sfx(0);
        if (in.repeat & B_RIGHT && mi < nmore - 1) mi++, sfx(0);
    }
    if (in.pressed & B_OK) {
        sfx(1);
        if (zone == 0) {
            button_action(t, bi);
        } else if (zone == 2 && nents) play_entry(ents[ei]);
        else if (zone == 3 && nmore) detail_open(L.t[more[mi]].id);
    }
    if (in.pressed & B_TRI) {
        if (zone == 2 && nents) entry_menu(ents[ei]);
        else open_more();
    }
    if (in.pressed & B_SQR) {
        lib_set_favorite(t, !t->favorite);
        toast(t->favorite ? "Added to favorites" : "Removed from favorites");
    }

    /* ---------- layout + scrolling ---------- */
    /* the list starts below the buttons, wherever a long description pushed them */
    static float btn_rel = 346;
    float list_top = fmaxf(400, btn_rel + 46 + 40);
    float row_h = 84;
    float list_y = list_top + (nseasons > 1 ? 52 : 8);
    float more_y = (has_list ? list_y + nents * row_h + 24 : 380);
    /* the d-pad moves the page to what's focused; a finger moves it freely and it stays put */
    if ((in.repeat & (B_UP | B_DOWN | B_LEFT | B_RIGHT)) || (in.pressed & (B_OK | B_BACK))) {
        if (zone == 0) dsy_t = 0;
        else if (zone == 1) dsy_t = list_top - 200;
        else if (zone == 2) dsy_t = list_y + ei * row_h - 230;
        else dsy_t = more_y - 220;
    }
    static KScroll dks;
    kscroll(&dsy, &dsy_t, &dks, fmaxf(0, more_y + (nmore ? 300 : 40) - SCR_H), in.dragging, in.ddy, in.vy, dt);
    efsel = approach(efsel, (float)ei, 18, dt);
    mfsel = approach(mfsel, (float)mi, 18, dt);

    /* ---------- backdrop ---------- */
    float bgh = 470;
    float fade = 0;
    vita2d_texture *bg = NULL;
    float ay = 0.3f;
    if (t->backdrop) bg = img_get(t->backdrop, SCR_W, (int)bgh, &fade);
    if (!bg && t->poster) { bg = img_get(t->poster, SCR_W, (int)bgh, &fade); ay = 0.2f; }
    float by = -dsy * 0.5f;
    if (!t->backdrop && !t->poster) { /* nothing to show: the scene, never a flash of it while a picture loads */
        Lofi *l = S.show_lofi ? lofi_get(T->scene) : NULL;
        if (l) lofi_draw(l, 0, by, SCR_W, bgh, 0, col_a(0xFFFFFFFF, T->scene_alpha), 0.5f, 0.6f);
    }
    if (bg) gfx_tex(bg, 0, by, SCR_W, bgh, 0, col_a(0xFFFFFFFF, ease_out(fade) * 0.95f), 0.5f, ay);
    gfx_grad_h(0, by, 700, bgh, col_a(T->bg, 0.95f), col_a(T->bg, 0.15f));
    gfx_grad_v(0, by + bgh - 200, SCR_W, 200, col_a(T->bg, 0), T->bg);
    gfx_rect(0, by + bgh, SCR_W, SCR_H, T->bg);
    float y0 = -dsy;
    float ea = ease_out(enter_anim);
    float slide = (1 - ea) * 16;

    /* poster on the right */
    {
        float pw2 = 168, ph2 = 252, px = SCR_W - DM - pw2, py = y0 + 92 + slide;
        draw_poster(t, px, py, pw2, ph2, 0, ea);
        if (hit(px, py, pw2, ph2)) spotlight_open(t->id);
    }
    /* back */
    gfx_circle(DM + 18, y0 + 38, 20, col_a(T->bg, 0.5f));
    icon(IC_CHEVRON_L, DM + 17, y0 + 38, 20, T->text);
    if (hit(DM - 4, y0 + 14, 46, 48)) nav_pop();

    /* title block */
    float x = DM, y = y0 + 106 + slide;
    float logo_fade = 0;
    vita2d_texture *logo = t->logo ? img_get(t->logo, 380, 120, &logo_fade) : NULL;
    if (logo) {
        float lw = vita2d_texture_get_width(logo), lh = vita2d_texture_get_height(logo);
        float s = fminf(360 / lw, 90 / lh);
        vita2d_draw_texture_tint_scale(logo, x, y + (90 - lh * s), s, s, col_a(0xFFFFFFFF, ea * logo_fade));
        y += 104;
    } else {
        int size = T->display_font == 2 ? 30 : 38;
        if (text_wrap_count(F_DISPLAY, size, 560, t->title) > 1) size = T->display_font == 2 ? 24 : 30;
        int lines = text_wrap(F_DISPLAY, size, x, y + size, 560, size + 4, 2, col_a(T->text, ea), t->title);
        y += lines * (size + 4) + 14;
    }
    char meta[200] = "", part[80];
    if (t->year) { snprintf(part, sizeof(part), "%d  \xc2\xb7  ", t->year); strcat(meta, part); }
    if (t->kind == KIND_MOVIE) {
        Entry *e = &L.e[t->e0];
        if (e->dur > 0) {
            char d[24];
            fmt_dur(e->dur, d, sizeof(d));
            snprintf(part, sizeof(part), "%s  \xc2\xb7  ", d);
            strcat(meta, part);
        }
        strcat(meta, "Film");
    } else if (t->kind == KIND_SERIES) {
        snprintf(part, sizeof(part), "%d %s  \xc2\xb7  %d episodes", nseasons, nseasons == 1 ? "season" : "seasons", t->ne);
        strcat(meta, part);
    } else {
        snprintf(part, sizeof(part), t->ne > 1 ? "%d volumes" : "Book", t->ne);
        strcat(meta, part);
    }
    if (t->genres) { snprintf(part, sizeof(part), "  \xc2\xb7  %s", t->genres); strncat(meta, part, sizeof(meta) - strlen(meta) - 1); }
    text_ellipsis(F_META, 14, x, y, 560, col_a(T->text2, ea), meta);
    y += 12;
    if (t->rating || t->score > 0) {
        if (t->rating) draw_stars(x, y + 2, 13, t->rating, T->accent2, col_a(T->text3, 0.6f));
        if (t->score > 0) {
            char sc[32];
            snprintf(sc, sizeof(sc), "%.1f / 10", t->score);
            text(F_META, 13, x + (t->rating ? 96 : 0), y + 14, T->text3, sc);
        }
        y += 22;
    }
    if (t->overview) {
        int lines = text_wrap(F_REG, 15, x, y + 20, 540, 21, 4, col_a(T->text2, ea), t->overview);
        y += lines * 21 + 10;
    } else {
        Entry *e0 = &L.e[t->e0];
        text_ellipsis(F_REG, 13, x, y + 18, 540, col_a(T->text3, ea), path_basename(e0->path));
        y += 26;
    }

    /* buttons */
    float bty = fmaxf(y + 8, y0 + 300);
    btn_rel = bty - y0;
    char lab[64];
    if (t->resume >= 0 && t->kind != KIND_MOVIE) {
        char el[32];
        snprintf(lab, sizeof(lab), "%s %s", t->kind == KIND_BOOK ? "Read" : "Resume", lib_entry_label(&L.e[t->resume], el, sizeof(el)));
    } else snprintf(lab, sizeof(lab), "%s", t->kind == KIND_BOOK ? "Read" : t->resume >= 0 ? "Resume" : "Play");
    float b0 = fmaxf(150, text_w(F_SEMI, 15, lab) + 64);
    float bx = x;
    int ics[5] = {t->kind == KIND_BOOK ? IC_BOOK : IC_PLAY, t->favorite ? IC_HEART_FILL : IC_HEART,
                  t->rating ? IC_STAR_FILL : IC_STAR, IC_PLUS, IC_DOTS};
    for (int i = 0; i < 5; i++) {
        float w = i == 0 ? b0 : 46;
        draw_button(bx, bty, w, 46, ics[i], i == 0 ? lab : "", i == 0, zone == 0 && bi == i ? 1 : 0);
        if (hit(bx, bty, w, 46)) {
            bi = i;
            zone = 0;
            input_consume_touch();
            button_action(t, i);
        }
        bx += w + 10;
    }
    /* unplayable note */
    Entry *first = &L.e[t->resume >= 0 ? t->resume : t->e0];
    if (first->media == MEDIA_VIDEO && !first->playable) {
        icon(IC_INFO, x + 8, bty + 70, 15, T->accent2);
        text(F_REG, 13, x + 22, bty + 75, T->text2, "Vita's video chip plays MP4 (H.264). Convert this file to watch it here.");
    }

    /* the seasons and episodes sit on a sheet that slides up over the backdrop */
    if (has_list || nmore) {
        float sheet = y0 + list_top - 26;
        if (sheet < SCR_H) {
            gfx_grad_v(0, sheet - 60, SCR_W, 60, col_a(T->bg, 0), col_a(T->bg, 0.85f));
            gfx_rect(0, sheet, SCR_W, SCR_H - sheet + 2, col_a(T->bg, 0.96f));
        }
    }
    /* seasons */
    if (nseasons > 1) {
        float cx = DM, cy = y0 + list_top;
        for (int i = 0; i < nseasons; i++) {
            char sl[32];
            if (seasons[i] == 0) snprintf(sl, sizeof(sl), "Specials");
            else snprintf(sl, sizeof(sl), "Season %d", seasons[i]);
            float cw;
            draw_chip(cx, cy, sl, i == si, zone == 1 && i == si ? 1 : 0, &cw);
            if (hit(cx, cy, cw, 30)) { si = i; build_entries(); ei = 0; zone = 1; }
            cx += cw + 8;
        }
    }
    /* entries */
    if (has_list) {
        for (int i = 0; i < nents; i++) {
            float ry = y0 + list_y + i * row_h;
            if (ry > SCR_H || ry + row_h < 0) continue;
            Entry *e = &L.e[ents[i]];
            float f = zone == 2 ? clampf(1 - fabsf(efsel - i), 0, 1) : 0;
            if (f > 0) {
                gfx_rrect(DM - 12, ry + 2, SCR_W - DM * 2 + 24, row_h - 4, T->radius_sm, col_a(T->text, (T->light ? 0.06f : 0.07f) * f));
                if (T->focus_style == 1) gfx_rrect_outline(DM - 12, ry + 2, SCR_W - DM * 2 + 24, row_h - 4, T->radius_sm, 1.5f, col_a(T->focus, 0.7f * f));
            }
            /* a picture tile: a different part of the show's artwork for each episode */
            char num[16];
            if (t->kind == KIND_SERIES && e->number_end > e->number) snprintf(num, sizeof(num), "%d-%d", e->number, e->number_end);
            else if (t->kind == KIND_SERIES) snprintf(num, sizeof(num), "%d", e->number);
            else if (t->kind == KIND_BOOK) snprintf(num, sizeof(num), "%d", e->number);
            else snprintf(num, sizeof(num), "%d", i + 1);
            int is_resume = ents[i] == t->resume;
            float tw2 = 120, th2 = 68, tx2 = DM, ty2 = ry + (row_h - th2) / 2;
            float tr = T->radius_sm;
            if (bg && fade >= 1) {
                /* zoom in 2.6x and pan to a different spot per episode, so no two tiles look alike */
                /* golden-ratio steps: neighbouring episodes land far apart on the picture */
                float zx = fmodf(0.21f + (e->number + e->season * 7) * 0.618034f, 1.0f);
                float zy = fmodf(0.37f + (e->number + e->season * 3) * 0.414214f, 1.0f);
                /* a window 1/2.6 of the picture's size, in the picture's own shape */
                float bw2 = vita2d_texture_get_width(bg), bh2 = vita2d_texture_get_height(bg);
                float uw = 1 / 2.6f, vh2 = fminf(1.0f, uw * (bw2 / bh2) * (th2 / tw2));
                float u0 = (1 - uw) * zx, v0 = (1 - vh2) * (0.15f + zy * 0.7f);
                gfx_tex_uv(bg, tx2, ty2, tw2, th2, tr, u0, v0, u0 + uw, v0 + vh2, col_a(0xFFFFFFFF, e->finished && !is_resume ? 0.55f : 0.95f));
            } else {
                gfx_rrect_grad(tx2, ty2, tw2, th2, tr, col_mix(T->surface2, T->accent, 0.18f), col_mix(T->surface, 0xFF000000, 0.2f));
            }
            gfx_rrect_grad(tx2, ty2, tw2, th2, tr, col_a(0xFF000000, 0.0f), col_a(0xFF000000, 0.55f));
            text(F_BOLD, 18, tx2 + 9, ty2 + th2 - 9, 0xFFFFFFFF, num);
            if (e->finished && !is_resume) {
                gfx_circle(tx2 + tw2 - 13, ty2 + 13, 9, T->accent);
                icon(IC_CHECK, tx2 + tw2 - 13, ty2 + 13, 12, T->on_accent);
            }
            if (e->dur > 0 && !e->finished && e->pos > 0) {
                gfx_rect(tx2, ty2 + th2 - 4, tw2, 4, col_a(0xFFFFFFFF, 0.25f));
                gfx_rect(tx2, ty2 + th2 - 4, tw2 * clampf((float)e->pos / e->dur, 0, 1), 4, T->accent);
            }
            if (is_resume) gfx_rrect_outline(tx2 - 2, ty2 - 2, tw2 + 4, th2 + 4, tr + 2, 2, T->accent);
            if (f > 0.3f) {
                gfx_circle(tx2 + tw2 / 2, ty2 + th2 / 2, 17, col_a(0xFF000000, 0.45f * f));
                icon(e->media == MEDIA_BOOK ? IC_BOOK : IC_PLAY, tx2 + tw2 / 2 + 1.5f, ty2 + th2 / 2, 16, col_a(0xFFFFFFFF, f));
            }
            char name[256];
            if (e->name && e->name[0] && t->kind == KIND_SERIES) snprintf(name, sizeof(name), "%s", e->name);
            else if (t->kind == KIND_SERIES) snprintf(name, sizeof(name), e->season == 0 ? "Special %d" : "Episode %d", e->number);
            else if (t->kind == KIND_BOOK && t->ne > 1) snprintf(name, sizeof(name), "Volume %d", e->number);
            else snprintf(name, sizeof(name), "%s", e->name && e->name[0] ? e->name : path_basename(e->path));
            text_ellipsis(F_SEMI, 16, DM + 138, ry + 36, 560, i == ei && zone == 2 ? T->text : col_a(T->text, 0.88f), name);
            char sub[96] = "";
            if (e->media == MEDIA_BOOK) {
                if (e->dur > 0) snprintf(sub, sizeof(sub), e->finished ? "Finished" : "Page %d of %d", e->pos + 1, e->dur);
                else snprintf(sub, sizeof(sub), "Not started");
            } else {
                char d[24] = "";
                if (e->dur > 0) fmt_dur(e->dur, d, sizeof(d));
                if (!e->playable) snprintf(sub, sizeof(sub), "Needs MP4 to play on Vita");
                else if (e->finished) snprintf(sub, sizeof(sub), "Watched%s%s", d[0] ? "  \xc2\xb7  " : "", d);
                else if (e->pos > 0 && e->dur > 0) snprintf(sub, sizeof(sub), "%dm left", (e->dur - e->pos) / 60000);
                else snprintf(sub, sizeof(sub), "%s", d[0] ? d : "Not watched yet");
            }
            text(F_META, 12, DM + 138, ry + 56, is_resume ? T->accent : T->text3, is_resume && !e->finished ? (e->pos > 0 ? sub : "Up next") : sub);
            if (hit(DM - 12, ry, SCR_W - DM * 2 + 24, row_h) && !in.dragging) {
                ei = i;
                zone = 2;
                play_entry(ents[i]);
            }
            if (hit_long(DM - 12, ry, SCR_W - DM * 2 + 24, row_h)) {
                ei = i;
                zone = 2;
                entry_menu(ents[i]);
            }
        }
    }
    /* more like this */
    if (nmore) {
        float my = y0 + more_y;
        text(T->display_font == 1 ? F_DISPLAY : F_SEMI, 17, DM, my + 20, zone == 3 ? T->text : T->text2, "More like this");
        float w = 104, h = 156, g = 14;
        float x2 = mi * (w + g);
        if (x2 - more_sx_t > SCR_W - DM * 2 - w * 1.6f) more_sx_t = x2 - (SCR_W - DM * 2 - w * 1.6f);
        if (x2 - more_sx_t < 0) more_sx_t = x2;
        more_sx = approach(more_sx, more_sx_t, 11, dt);
        for (int i = 0; i < nmore; i++) {
            float px = DM + i * (w + g) - more_sx;
            if (px > SCR_W || px + w < 0) continue;
            float f = zone == 3 ? clampf(1 - fabsf(mfsel - i), 0, 1) : 0;
            draw_poster(&L.t[more[i]], px, my + 34, w, h, f, 1);
            if (hit(px, my + 34, w, h)) detail_open(L.t[more[i]].id);
        }
    }
    {
        float k = clampf((dsy - 220) / 80.0f, 0, 1);
        if (k > 0.01f) {
            gfx_rect(0, 0, SCR_W, 58, col_a(T->bg, 0.97f * k));
            gfx_grad_v(0, 58, SCR_W, 18, col_a(T->bg, 0.97f * k), col_a(T->bg, 0));
            icon(IC_CHEVRON_L, DM + 17, 30, 20, col_a(T->text, k));
            text_ellipsis(F_SEMI, 16, DM + 44, 36, SCR_W - DM * 2 - 60, col_a(T->text, k), t->title);
            if (hit(0, 0, 160, 58)) nav_pop();
        }
    }
}

/* ---------------- spotlight ---------------- */

static int spot_id;
static float spot_t;

void spotlight_open(int id) {
    spot_id = id;
    spot_t = 0;
    nav_push(SCR_SPOTLIGHT);
}

void spotlight_frame(float dt) {
    Title *t = lib_title_by_id(spot_id);
    if (!t || (in.pressed & (B_BACK | B_OK)) || in.tap) {
        nav_pop();
        return;
    }
    spot_t += dt;
    gfx_rect(0, 0, SCR_W, SCR_H, 0xFF000000);
    float fade = 0;
    const char *src = t->backdrop ? t->backdrop : title_poster(t);
    vita2d_texture *tex = src ? img_get(src, SCR_W * 2 > 1600 ? 1600 : SCR_W * 2, 0, &fade) : NULL;
    if (tex) {
        float k = S.reduce_motion ? 0 : fmodf(spot_t, 40) / 40;
        float zoom = 1.0f + 0.12f * ease_inout(k < 0.5f ? k * 2 : 2 - k * 2);
        float w = SCR_W * zoom, h = SCR_H * zoom;
        float ax = 0.5f + 0.25f * sinf(spot_t * 0.05f);
        if (t->backdrop) gfx_tex(tex, (SCR_W - w) / 2, (SCR_H - h) / 2, w, h, 0, col_a(0xFFFFFFFF, fade), ax, 0.4f);
        else {
            /* posters: blurred fill behind, sharp poster in the middle */
            gfx_tex(tex, 0, 0, SCR_W, SCR_H, 0, col_a(0xFFFFFFFF, 0.25f * fade), 0.5f, 0.5f);
            float ph2 = SCR_H - 60, pw2 = ph2 * vita2d_texture_get_width(tex) / vita2d_texture_get_height(tex);
            gfx_shadow((SCR_W - pw2 * zoom) / 2, 30, pw2 * zoom, ph2, T->radius, 40, col_a(0xFF000000, 0.6f * fade));
            gfx_tex(tex, (SCR_W - pw2 * zoom) / 2, 30 - (zoom - 1) * ph2 / 2, pw2 * zoom, ph2 * zoom, T->radius,
                    col_a(0xFFFFFFFF, fade), 0.5f, 0.5f);
        }
    } else {
        Lofi *l = lofi_get(T->scene);
        if (l) lofi_draw(l, 0, 0, SCR_W, SCR_H, 0, 0xFFFFFFFF, 0.5f, 0.5f);
    }
    float a = clampf(1 - (spot_t - 3) / 1.5f, 0, 1);
    if (a > 0) {
        gfx_grad_v(0, SCR_H - 120, SCR_W, 120, 0, col_a(0xFF000000, 0.7f * a));
        text(F_DISPLAY, 26, 40, SCR_H - 40, col_a(0xFFFFFFFF, a), t->title);
    }
}
