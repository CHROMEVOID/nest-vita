/* Search: titles, original filenames, years and genres; recent searches and
   gentle suggestions when the box is empty. */
#include "nest.h"

static char q[128];
static int res[2048], nres;
static char recent[12][64];
static int nrecent;
static char genres[8][32];
static int ngenres;
static int zone, chip_i, rfocus;
static float rsy, rsy_t, rfsel;

static void run(void) {
    int scope = g_secret_unlocked ? 2 : 0;
    if (q[0]) nres = lib_search(q, scope, res, 2048);
    else nres = lib_recent(-1, 0, res, 30);
    rfocus = 0;
    rsy = rsy_t = 0;
}

static void collect_genres(void) {
    ngenres = 0;
    int counts[64] = {0};
    static char names[64][32];
    int nn = 0;
    for (int i = 0; i < L.nt; i++) {
        if (!L.t[i].genres || L.t[i].secret) continue;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", L.t[i].genres);
        char *save = NULL;
        for (char *g = strtok_r(buf, ",", &save); g; g = strtok_r(NULL, ",", &save)) {
            while (*g == ' ') g++;
            int k = 0;
            for (; k < nn; k++)
                if (!strcmp(names[k], g)) break;
            if (k == nn && nn < 64) snprintf(names[nn++], 32, "%s", g);
            if (k < 64) counts[k]++;
        }
    }
    for (int r = 0; r < 6 && r < nn; r++) {
        int best = -1;
        for (int k = 0; k < nn; k++)
            if (counts[k] > 0 && (best < 0 || counts[k] > counts[best])) best = k;
        if (best < 0) break;
        snprintf(genres[ngenres++], 32, "%s", names[best]);
        counts[best] = 0;
    }
}

void search_enter(void) {
    nrecent = db_search_recent(recent, 8);
    collect_genres();
    zone = 0;
    chip_i = 0;
    run();
    nav_push(SCR_SEARCH);
}

static void query_cb(const char *text, void *ud) {
    (void)ud;
    if (!text) return;
    snprintf(q, sizeof(q), "%s", text);
    db_search_add(q);
    nrecent = db_search_recent(recent, 8);
    run();
    zone = nres ? 2 : 0;
}

static void type_query(void) { ime_request("Search your library", q, 120, query_cb, NULL); }

void search_frame(float dt) {
    float pwv = 104, phv = 156, g = 16;
    int cols = (int)((SCR_W - 80 + g) / (pwv + g));
    float total = cols * pwv + (cols - 1) * g, x0 = (SCR_W - total) / 2;
    int nchips = q[0] ? 0 : nrecent + ngenres;

    if (in.pressed & B_BACK) {
        if (q[0]) { q[0] = 0; run(); zone = 0; }
        else { sfx(2); nav_pop(); return; }
    }
    if (in.pressed & B_SELECT) type_query();
    if (zone == 0) {
        if (in.pressed & B_OK) type_query();
        if (in.repeat & B_DOWN) zone = nchips ? 1 : (nres ? 2 : 0), sfx(0);
    } else if (zone == 1) {
        if (in.repeat & B_LEFT && chip_i > 0) chip_i--, sfx(0);
        if (in.repeat & B_RIGHT && chip_i < nchips - 1) chip_i++, sfx(0);
        if (in.repeat & B_UP) zone = 0, sfx(0);
        if (in.repeat & B_DOWN && nres) zone = 2, sfx(0);
        if (in.pressed & B_OK && nchips) {
            snprintf(q, sizeof(q), "%s", chip_i < nrecent ? recent[chip_i] : genres[chip_i - nrecent]);
            run();
            zone = nres ? 2 : 0;
        }
    } else {
        if (in.repeat & B_LEFT && rfocus % cols > 0) rfocus--, sfx(0);
        if (in.repeat & B_RIGHT && rfocus % cols < cols - 1 && rfocus < nres - 1) rfocus++, sfx(0);
        if (in.repeat & B_UP) {
            if (rfocus >= cols) rfocus -= cols;
            else zone = nchips ? 1 : 0;
            sfx(0);
        }
        if (in.repeat & B_DOWN && rfocus + cols < nres) rfocus += cols, sfx(0);
        if (nres) {
            Title *t = &L.t[res[rfocus]];
            if (in.pressed & B_OK) detail_open(t->id);
            if (in.pressed & B_TRI) modal_quick(t);
        }
    }

    draw_scene_bg(1.0f);
    /* header */
    float fy = 30;
    float fw = SCR_W - 80;
    int ff = zone == 0;
    gfx_rrect(40, fy, fw, 48, T->nav_style == 0 ? 24 : T->radius_sm, col_a(T->text, ff ? 0.13f : 0.07f));
    if (ff) draw_focus(40, fy, fw, 48, T->nav_style == 0 ? 24 : T->radius_sm, 1);
    icon(IC_SEARCH, 70, fy + 24, 20, T->text2);
    if (q[0]) text_ellipsis(F_SEMI, 17, 94, fy + 30, fw - 120, T->text, q);
    else text(F_REG, 16, 94, fy + 30, T->text3, "Titles, years, genres or file names");
    if (hit(40, fy, fw, 48)) type_query();
    if (q[0]) {
        icon(IC_CLOSE, 40 + fw - 26, fy + 24, 16, T->text2);
        if (hit(40 + fw - 50, fy, 50, 48)) { q[0] = 0; run(); }
    }
    float y = 104;
    if (!q[0] && nchips) {
        float cx = 40;
        if (nrecent) text(F_META, 12, 40, y, T->text3, "RECENT");
        if (!nrecent && ngenres) text(F_META, 12, 40, y, T->text3, "GENRES");
        y += 10;
        for (int i = 0; i < nchips; i++) {
            const char *lab = i < nrecent ? recent[i] : genres[i - nrecent];
            float w = text_w(F_SEMI, 13, lab) + 28;
            if (cx + w > SCR_W - 40) break;
            float cw;
            draw_chip(cx, y, lab, 0, zone == 1 && chip_i == i ? 1 : 0, &cw);
            if (hit(cx, y, cw, 30)) {
                snprintf(q, sizeof(q), "%s", lab);
                run();
            }
            cx += cw + 8;
        }
        y += 52;
    }
    text(F_SEMI, 16, 40, y + 4, T->text2, q[0] ? (nres ? "Results" : "") : "Recently added");
    if (q[0] && nres) {
        char c[32];
        snprintf(c, sizeof(c), "%d", nres);
        text(F_META, 13, 40 + text_w(F_SEMI, 16, "Results") + 10, y + 4, T->text3, c);
    }
    float top = y + 24;
    if (q[0] && !nres) {
        Lofi *l = lofi_get("rooftop");
        if (l) lofi_draw(l, SCR_W / 2 - 110, top + 10, 220, 172, T->radius, 0xFFFFFFFF, 0.5f, 0.5f);
        text_center(F_SEMI, 16, SCR_W / 2, top + 214, T->text, "Nothing by that name");
        text_center(F_REG, 13, SCR_W / 2, top + 236, T->text3, "Try part of a title, a year, or a word from the file name.");
        return;
    }
    float cell = phv + 36;
    int row = rfocus / cols;
    if ((in.repeat & (B_UP | B_DOWN | B_LEFT | B_RIGHT))) rsy_t = zone == 2 ? fmaxf(0, row * cell - cell * 0.5f) : 0;
    static KScroll sks;
    kscroll(&rsy, &rsy_t, &sks, ((nres + cols - 1) / cols) * cell - (SCR_H - top) + 20, in.dragging, in.ddy, in.vy, dt);
    rfsel = approach(rfsel, (float)rfocus, 20, dt);
    gfx_clip(0, top - 10, SCR_W, SCR_H - top + 10);
    for (int i = 0; i < nres; i++) {
        float x = x0 + (i % cols) * (pwv + g), yy = top + (i / cols) * cell - rsy;
        if (yy > SCR_H || yy + cell < top - 20) continue;
        float f = zone == 2 && i == rfocus ? clampf(1 - fabsf(rfsel - i), 0, 1) : 0;
        Title *t = &L.t[res[i]];
        g_poster_caption = 1;
        draw_poster(t, x, yy, pwv, phv, f, 1);
        g_poster_caption = 0;
        text_ellipsis(F_REG, 12, x, yy + phv + 17, pwv, f > 0.5f ? T->text : T->text3, t->title);
        if (hit(x, yy, pwv, phv) && !in.dragging) detail_open(t->id);
        if (hit_long(x, yy, pwv, phv)) modal_quick(t);
    }
    gfx_noclip();
}
