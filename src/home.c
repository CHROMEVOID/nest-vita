/* The main screen: navigation, cinematic hero, shelves, browse grids and the
   Secret tab. */
#include "nest.h"
#include "music.h"

extern int g_completed_tid;

enum { TAB_HOME, TAB_MOVIES, TAB_SHOWS, TAB_BOOKS, TAB_MUSIC, TAB_SECRET, TAB_COUNT };
static const char *TAB_NAMES[TAB_COUNT] = {"Home", "Movies", "Shows", "Books", "Music", "Secret"};

enum { ROW_HERO, ROW_POSTER, ROW_WIDE, ROW_ACTION };
#define MAXROWS 14
#define MAXITEMS 120

typedef struct {
    int type;
    char label[96];
    int item[MAXITEMS];
    int ent[MAXITEMS];
    int n, col;
    float sx, sx_t, fsel;
    float y, h;
} Row;

static Row rows[MAXROWS];
static int nrows;
static int tab, prev_tab;
static float tab_anim = 1;
static int frow = 0;        /* focused row; -1 = navigation */
static int nav_i;           /* 0..4 tabs, 5 search, 6 settings */
static float sy, sy_t;      /* vertical scroll */
static float row_focus_anim[MAXROWS];
static int hero[8], nhero, hero_i, hero_prev = -1;
static float hero_timer, hero_fade = 1, idle;
static int hero_btn;
static int touch_axis;      /* 0 undecided 1 vertical 2 horizontal */
static int touch_row = -1;
static int built_secret = -1;

/* grid state (Movies / Shows / Books) */
static int gsort = SORT_RECENT, gfilter = 0;
static int gitems[4096], gn;
static int gfocus, gchip = -1; /* gchip >= 0 when focus is on the chips row */
static float gsy, gsy_t, gfsel;

static float pw(void) { return poster_w(); }
static float ph(void) { return poster_w() * 1.5f; }
static float gap(void) { return S.ambient ? 20 : 14; }
#define MARGIN 40.0f
#define HERO_H 322.0f
#define WIDE_W 232.0f
#define WIDE_H 130.0f

static int secret_view(void) { return tab == TAB_SECRET && g_secret_unlocked; }

static void add_row(int type, const char *label, int *items, int *ents, int n) {
    if (nrows >= MAXROWS || (type != ROW_HERO && type != ROW_ACTION && n <= 0)) return;
    Row *r = &rows[nrows++];
    memset(r, 0, sizeof(*r));
    r->type = type;
    snprintf(r->label, sizeof(r->label), "%s", label);
    if (n > MAXITEMS) n = MAXITEMS;
    r->n = n;
    for (int i = 0; i < n; i++) {
        r->item[i] = items ? items[i] : 0;
        r->ent[i] = ents ? ents[i] : -1;
    }
}

static void build_rows(void) {
    /* remember focus by title id so a rebuild keeps your place */
    int keep_row_type = frow >= 0 && frow < nrows ? rows[frow].type : -1;
    char keep_label[96] = "";
    int keep_col = 0;
    if (frow >= 0 && frow < nrows) {
        snprintf(keep_label, sizeof(keep_label), "%s", rows[frow].label);
        keep_col = rows[frow].col;
    }
    nrows = 0;
    int secret = secret_view() ? 1 : 0;
    static int tmp[4096], ents[4096];
    int n;

    nhero = lib_featured(secret, hero, 6);
    if (hero_i >= nhero) hero_i = 0;
    add_row(ROW_HERO, "", NULL, NULL, 0);

    n = lib_continue(secret ? -1 : -1, secret, tmp, MAXITEMS);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (L.t[tmp[i]].kind != KIND_BOOK) {
            tmp[m] = tmp[i];
            ents[m] = L.t[tmp[i]].resume;
            m++;
        }
    add_row(ROW_WIDE, secret ? "Continue" : "Continue watching", tmp, ents, m);

    n = lib_continue(-2, secret, tmp, MAXITEMS);
    add_row(ROW_POSTER, "Continue reading", tmp, NULL, n);

    if (!secret) {
        int seed;
        n = lib_because(0, &seed, tmp, 16);
        if (n >= 3 && seed >= 0) {
            char lab[96];
            snprintf(lab, sizeof(lab), "Because you watched %s", L.t[seed].title);
            add_row(ROW_POSTER, lab, tmp, NULL, n);
        }
    }
    n = lib_recent(-1, secret, tmp, 24);
    add_row(ROW_POSTER, "Recently added", tmp, NULL, n);
    n = lib_favorites(secret, tmp, MAXITEMS);
    add_row(ROW_POSTER, "Favorites", tmp, NULL, n);
    if (secret) {
        int a = lib_query(KIND_MOVIE, 1, SORT_AZ, 0, tmp, 2048);
        int b = lib_query(KIND_SERIES, 1, SORT_AZ, 0, tmp + a, 2048);
        add_row(ROW_POSTER, "Videos", tmp, NULL, a + b);
        n = lib_query(KIND_BOOK, 1, SORT_AZ, 0, tmp, MAXITEMS);
        add_row(ROW_POSTER, "Books", tmp, NULL, n);
    } else {
        n = lib_query(KIND_SERIES, 0, SORT_PROGRESS, 0, tmp, MAXITEMS);
        add_row(ROW_POSTER, "Shows", tmp, NULL, n);
        n = lib_query(KIND_MOVIE, 0, SORT_RECENT, 1, tmp, MAXITEMS);
        add_row(ROW_POSTER, "Movies to watch", tmp, NULL, n);
        n = lib_query(KIND_BOOK, 0, SORT_RECENT, 0, tmp, MAXITEMS);
        add_row(ROW_POSTER, "Books & manga", tmp, NULL, n);
    }
    /* Smart Views: saved filters as shelves */
    if (!secret) {
        int vid[8], vk[8], vs[8], vf[8];
        char vn[8][64];
        int nv = db_views(vid, vn, vk, vs, vf, 6);
        for (int v = 0; v < nv; v++) {
            n = lib_query(vk[v], 0, vs[v], vf[v], tmp, MAXITEMS);
            add_row(ROW_POSTER, vn[v], tmp, NULL, n);
        }
    }
    /* collections as shelves */
    int cids[8];
    char names[8][64];
    int nc = db_collections(secret, cids, names, 4);
    for (int c = 0; c < nc; c++) {
        int tids[MAXITEMS];
        int k = db_collection_items(cids[c], tids, MAXITEMS), q = 0;
        for (int i = 0; i < k; i++) {
            int ti = lib_title_index(tids[i]);
            if (ti >= 0 && L.t[ti].secret == secret) tmp[q++] = ti;
        }
        add_row(ROW_POSTER, names[c], tmp, NULL, q);
    }
    if (secret) add_row(ROW_ACTION, "Add a secret folder", NULL, NULL, 0);

    /* layout */
    float y = HERO_H + 6;
    for (int i = 1; i < nrows; i++) {
        Row *r = &rows[i];
        r->y = y;
        float ih = r->type == ROW_WIDE ? WIDE_H : r->type == ROW_ACTION ? 52 : ph();
        r->h = 34 + ih + (r->type == ROW_POSTER ? 22 : 0) + (S.ambient ? 30 : 20);
        y += r->h;
    }
    rows[0].y = 0;
    rows[0].h = HERO_H;
    /* restore focus */
    if (keep_row_type >= 0) {
        int found = -1;
        for (int i = 0; i < nrows; i++)
            if (rows[i].type == keep_row_type && !strcmp(rows[i].label, keep_label)) found = i;
        if (found >= 0) {
            frow = found;
            rows[found].col = clampi(keep_col, 0, rows[found].n > 0 ? rows[found].n - 1 : 0);
            rows[found].fsel = rows[found].col;
        } else if (frow >= nrows) {
            frow = nrows - 1;
        }
    }
    built_secret = secret;
}

static const char *SORT_NAMES[5];
static const char *FILTER_NAMES[4];

static void view_named(const char *name, void *ud) {
    (void)ud;
    if (!name || !name[0]) return;
    int kind = tab == TAB_MOVIES ? KIND_MOVIE : tab == TAB_SHOWS ? KIND_SERIES : KIND_BOOK;
    db_view_create(name, kind, gsort, gfilter);
    build_rows();
    toast("Saved \xe2\x80\x94 it's a shelf on Home now");
}

/* suggest a name from the current sort and filter, then let you edit it */
static void save_view(void) {
    static const char *kinds[] = {"films", "shows", "books"};
    static const char *filt[] = {"All", "Unfinished", "Favourite", "In-progress"};
    static const char *sorts[] = {"newest", "A\xe2\x80\x93Z", "by year", "top rated", "last watched"};
    int k = tab == TAB_MOVIES ? 0 : tab == TAB_SHOWS ? 1 : 2;
    char sug[64];
    snprintf(sug, sizeof(sug), "%s %s, %s", filt[gfilter], kinds[k], sorts[gsort]);
    ime_request("Name this view", sug, 60, view_named, NULL);
}

static void build_grid(void) {
    int kind = tab == TAB_MOVIES ? KIND_MOVIE : tab == TAB_SHOWS ? KIND_SERIES : KIND_BOOK;
    gn = lib_query(kind, 0, gsort, gfilter, gitems, 4096);
    if (gfocus >= gn) gfocus = gn ? gn - 1 : 0;
}

void main_enter(void) {
    build_rows();
    if (tab >= TAB_MOVIES && tab <= TAB_BOOKS) build_grid();
    if (tab == TAB_MUSIC) music_tab_enter();
}

static void set_tab(int t) {
    if (t == tab) return;
    if (tab == TAB_SECRET && t != TAB_SECRET) g_secret_unlocked = 0; /* leaving Secret locks it */
    prev_tab = tab;
    tab = t;
    tab_anim = 0;
    sy = sy_t = 0;
    gsy = gsy_t = 0;
    gfocus = 0;
    gchip = -1;
    if (frow >= 0) frow = 0;
    hero_i = 0;
    if (tab == TAB_HOME || tab == TAB_SECRET) build_rows();
    else if (tab == TAB_MUSIC) music_tab_enter();
    else build_grid();
    sfx(0);
}

/* ---------------- navigation bar ---------------- */

static void draw_nav(float dt, float alpha) {
    float scrolled = clampf((tab == TAB_HOME || tab == TAB_SECRET ? sy : gsy) / 60.0f, 0, 1);
    gfx_grad_v(0, 0, SCR_W, 92, col_a(T->bg, 0.55f + 0.4f * scrolled), col_a(T->bg, 0));
    if (scrolled > 0) gfx_rect(0, 0, SCR_W, 58, col_a(T->bg, 0.5f * scrolled));
    /* wordmark */
    text(F_DISPLAY, T->display_font == 2 ? 22 : 25, MARGIN, 40, col_a(T->text, alpha), "nest");
    float wm = text_w(F_DISPLAY, T->display_font == 2 ? 22 : 25, "nest");
    gfx_circle(MARGIN + wm + 5, 35, 2.6f, col_a(T->accent, alpha));

    /* tabs */
    static float pill_x, pill_w;
    float x = 168;
    float tx[TAB_COUNT], tw[TAB_COUNT];
    for (int i = 0; i < TAB_COUNT; i++) {
        tw[i] = (i == TAB_SECRET ? 18 : 0) + text_w(F_SEMI, 15, TAB_NAMES[i]) + 30;
        tx[i] = x;
        x += tw[i] + 4;
    }
    if (T->nav_style == 0) gfx_glass(160, 15, x - 160 + 4, 40, 20, alpha);
    pill_x = approach(pill_x, tx[tab], S.reduce_motion ? 40 : 16, dt);
    pill_w = approach(pill_w, tw[tab], 16, dt);
    if (T->nav_style == 0)
        gfx_rrect(pill_x, 19, pill_w, 32, 16, col_a(T->text, 0.94f * alpha));
    else
        gfx_rrect(pill_x + 14, 50, pill_w - 28, 3, 1.5f, col_a(T->accent, alpha));
    for (int i = 0; i < TAB_COUNT; i++) {
        int active = i == tab;
        int focused = frow == -1 && nav_i == i;
        col_t c = T->nav_style == 0 && active ? T->bg : (active || focused ? T->text : T->text2);
        float lx = tx[i] + 15;
        if (i == TAB_SECRET) {
            icon(g_secret_unlocked ? IC_UNLOCK : IC_LOCK, lx + 6, 34.5f, 14, col_a(c, alpha));
            lx += 18;
        }
        text(F_SEMI, 15, lx, 40.5f, col_a(c, alpha), TAB_NAMES[i]);
        if (focused && !active) gfx_rrect_outline(tx[i], 19, tw[i], 32, T->nav_style == 0 ? 16 : 6, 2, col_a(T->focus, alpha));
        if (hit(tx[i], 10, tw[i], 48)) {
            set_tab(i);
            input_consume_touch();
        }
    }
    /* right icons */
    float ix[2] = {SCR_W - 92, SCR_W - 48};
    int ics[2] = {IC_SEARCH, IC_GEAR};
    for (int i = 0; i < 2; i++) {
        int focused = frow == -1 && nav_i == TAB_COUNT + i;
        if (focused) gfx_circle(ix[i], 35, 19, col_a(T->text, 0.92f));
        else gfx_glass(ix[i] - 19, 16, 38, 38, 19, 0.9f);
        icon(ics[i], ix[i], 35, 19, focused ? T->bg : col_a(T->text, alpha));
        if (hit(ix[i] - 22, 12, 44, 46)) {
            input_consume_touch();
            if (i == 0) search_enter();
            else settings_enter();
        }
    }
    if (S.private_session) {
        icon(IC_EYE_OFF, SCR_W - 136, 35, 16, col_a(T->accent2, 0.9f));
    }
}

/* ---------------- hero ---------------- */

static void hero_action(int which) {
    if (nhero <= 0) return;
    Title *t = &L.t[hero[hero_i]];
    if (which == 0) {
        int e = t->resume >= 0 ? t->resume : t->e0;
        if (L.e[e].media == MEDIA_BOOK) reader_open(e);
        else if (!L.e[e].playable) toast("This file needs converting to MP4 to play on Vita");
        else player_open(e, 1);
    } else if (which == 1) {
        detail_open(t->id);
    } else {
        lib_set_favorite(t, !t->favorite);
        toast(t->favorite ? "Added to favorites" : "Removed from favorites");
    }
}

static void draw_hero_art(Title *t, float y0, float alpha) {
    float fade = 0;
    vita2d_texture *tex = NULL;
    float ay = 0.3f;
    if (t->backdrop) tex = img_get(t->backdrop, SCR_W, (int)HERO_H, &fade);
    if (!tex && t->poster) {
        tex = img_get(t->poster, SCR_W, (int)HERO_H, &fade);
        ay = 0.18f;
    }
    if (!tex || fade < 1) {
        Lofi *l = S.show_lofi ? lofi_get(T->scene) : NULL;
        if (l) lofi_draw(l, 0, y0, SCR_W, HERO_H + 40, 0, col_a(0xFFFFFFFF, alpha * T->scene_alpha * 1.4f), 0.5f, 0.6f);
        else gfx_grad_v(0, y0, SCR_W, HERO_H, col_a(T->surface2, alpha), col_a(T->bg, alpha));
    }
    if (tex) {
        /* very slow drift keeps the hero alive without distracting */
        float drift = 0; /* the old slow pan moved the picture by fractions of a pixel: it shimmered */
        gfx_tex(tex, 0, y0, SCR_W, HERO_H + 40, 0, col_a(0xFFFFFFFF, alpha * ease_out(fade)), 0.5f + drift, ay);
    }
}

static void draw_hero(float y0, float dt, int focused) {
    if (nhero <= 0) {
        /* empty library: a quiet invitation */
        Lofi *l = lofi_get(T->scene);
        float wx = 520, wy = y0 + 70, ww = 380, wh = 214;
        if (l) {
            gfx_shadow(wx, wy + 8, ww, wh, T->radius + 6, 30, col_a(T->shadow, 1));
            lofi_draw(l, wx, wy, ww, wh, T->radius + 6, 0xFFFFFFFF, 0.5f, 0.5f);
        }
        text(F_DISPLAY, T->display_font == 2 ? 28 : 36, MARGIN, y0 + 150, T->text,
             secret_view() ? "A quiet corner" : "Your nest is empty");
        text_wrap(F_REG, 15, MARGIN, y0 + 184, 420, 22, 3, T->text2,
                  secret_view() ? "Add a folder here and its videos and books stay behind your PIN."
                                : "Pick the folders where your films, shows and books live. Nest will tidy the names and sort everything for you.");
        int f = focused;
        draw_button(MARGIN, y0 + 246, 190, 46, IC_FOLDER, "Add a folder", 1, f ? 1 : 0);
        if (hit(MARGIN, y0 + 246, 190, 46) || (f && (in.pressed & B_OK))) picker_open(secret_view() ? PICK_SECRET : PICK_LIBRARY);
        return;
    }
    Title *t = &L.t[hero[hero_i]];
    /* rotation */
    if (S.hero_rotate && nhero > 1 && !S.reduce_motion) {
        idle = in.any ? 0 : idle + dt;
        hero_timer += dt;
        if (hero_timer > 10 && (idle > 4 || !focused)) {
            hero_prev = hero_i;
            hero_i = (hero_i + 1) % nhero;
            hero_timer = 0;
            hero_fade = 0;
        }
    }
    hero_fade = approach(hero_fade, 1.02f, 3.2f, dt);
    if (hero_fade > 1) hero_fade = 1;
    if (hero_prev >= 0 && hero_fade < 1) draw_hero_art(&L.t[hero[hero_prev]], y0, 1);
    draw_hero_art(t, y0, hero_prev >= 0 ? hero_fade : 1);
    /* legibility */
    gfx_grad_h(0, y0, 680, HERO_H + 40, col_a(T->bg, 0.94f), col_a(T->bg, 0));
    gfx_grad_v(0, y0 + HERO_H - 150, SCR_W, 190, col_a(T->bg, 0), T->bg);

    float ta = hero_prev >= 0 ? ease_out(hero_fade) : 1;
    float slide = 0; /* no sliding text: it read as a twitch when the hero changed */
    if (S.glass) gfx_glass(MARGIN - 18, y0 + 78, 520, HERO_H - 78 - 8, T->radius + 2, 0.85f);
    float x = MARGIN, y = y0 + 98 + slide;
    /* label */
    const char *label = t->resume >= 0 ? (t->kind == KIND_BOOK ? "CONTINUE READING" : "CONTINUE WATCHING")
                        : t->last_used == 0 && (now_s() - t->added) < 7 * 86400 ? "NEW IN YOUR NEST"
                        : t->favorite ? "ONE OF YOUR FAVORITES"
                                      : "FROM YOUR SHELVES";
    gfx_rrect(x, y - 11, 3, 13, 1.5f, col_a(T->accent, ta));
    text(F_META, 12, x + 10, y, col_a(T->accent, ta), label);
    y += 14;
    /* clearlogo or title */
    float fade = 0;
    vita2d_texture *logo = t->logo ? img_get(t->logo, 380, 120, &fade) : NULL;
    if (t->logo) {
        /* the logo always gets the same box, loaded or not, so nothing below it jumps */
        if (logo && fade > 0) {
            float lw = vita2d_texture_get_width(logo), lh = vita2d_texture_get_height(logo);
            float s = fminf(300 / lw, 72 / lh);
            vita2d_draw_texture_tint_scale(logo, x, y + 6 + (72 - lh * s), s, s, col_a(0xFFFFFFFF, ta * ease_out(fade)));
        }
        y += 88;
    } else {
        int size = T->display_font == 2 ? 30 : 40;
        int lines = text_wrap_count(F_DISPLAY, size, 520, t->title);
        if (lines > 1) size = T->display_font == 2 ? 24 : 32;
        lines = text_wrap_count(F_DISPLAY, size, 520, t->title);
        if (lines > 2) lines = 2;
        text_wrap(F_DISPLAY, size, x, y + size + 6, 520, size + 4, 2, col_a(T->text, ta), t->title);
        y += 18 + lines * (size + 4);
    }
    /* meta line */
    char meta[160] = "", part[64];
    if (t->year) { snprintf(part, sizeof(part), "%d", t->year); strcat(meta, part); }
    const char *k = t->kind == KIND_MOVIE ? "Film" : t->kind == KIND_SERIES ? "Series" : (t->ne > 1 ? "Manga" : "Book");
    if (meta[0]) strcat(meta, "  \xc2\xb7  ");
    strcat(meta, k);
    if (t->kind == KIND_SERIES) {
        snprintf(part, sizeof(part), "  \xc2\xb7  %d %s", t->seasons, t->seasons == 1 ? "Season" : "Seasons");
        strcat(meta, part);
    } else if (t->kind == KIND_BOOK && t->ne > 1) {
        snprintf(part, sizeof(part), "  \xc2\xb7  %d volumes", t->ne);
        strcat(meta, part);
    }
    if (t->genres) {
        char g[64];
        snprintf(g, sizeof(g), "%s", t->genres);
        char *c = strchr(g, ',');
        if (c) *c = 0;
        snprintf(part, sizeof(part), "  \xc2\xb7  %s", g);
        strcat(meta, part);
    }
    text(F_META, 14, x, y + 8, col_a(T->text2, ta), meta);
    float mw = text_w(F_META, 14, meta);
    if (t->score > 0) {
        icon(IC_STAR_FILL, x + mw + 20, y + 3, 12, col_a(T->accent2, ta));
        char sc[16];
        snprintf(sc, sizeof(sc), "%.1f", t->score);
        text(F_META, 14, x + mw + 30, y + 8, col_a(T->text2, ta), sc);
    }
    y += 16;
    /* buttons */
    float by = y0 + HERO_H - 66;
    /* description: only as many lines as fit above the buttons */
    int ov_lines = (int)((by - 14 - (y + 2)) / 20);
    if (ov_lines > 2) ov_lines = 2;
    if (t->overview && ov_lines > 0) {
        /* never under the buttons, whatever the font does */
        gfx_clip(0, y, SCR_W, by - 8 - y);
        text_wrap(F_REG, 14, x, y + 16, 460, 20, ov_lines, col_a(T->text2, ta * 0.9f), t->overview);
        gfx_noclip();
    }

    char lab[64];
    if (t->resume >= 0 && t->kind != KIND_MOVIE) {
        char el[32];
        snprintf(lab, sizeof(lab), "%s %s", t->kind == KIND_BOOK ? "Read" : "Resume", lib_entry_label(&L.e[t->resume], el, sizeof(el)));
    } else {
        snprintf(lab, sizeof(lab), "%s", t->kind == KIND_BOOK ? (t->resume >= 0 ? "Keep reading" : "Read")
                                         : t->resume >= 0 ? "Resume" : "Play");
    }
    float b0w = fmaxf(140, text_w(F_SEMI, 15, lab) + 64);
    float bx[3] = {x, x + b0w + 12, x + b0w + 12 + 126};
    float bw[3] = {b0w, 114, 46};
    draw_button(bx[0], by, bw[0], 44, t->kind == KIND_BOOK ? IC_BOOK : IC_PLAY, lab, 1, focused && hero_btn == 0);
    draw_button(bx[1], by, bw[1], 44, IC_INFO, "Details", 0, focused && hero_btn == 1);
    draw_button(bx[2], by, bw[2], 44, t->favorite ? IC_HEART_FILL : IC_HEART, "", 0, focused && hero_btn == 2);
    for (int i = 0; i < 3; i++)
        if (hit(bx[i], by, bw[i], 44)) {
            hero_action(i);
            input_consume_touch();
        }
    /* tap the art area for details */
    if (hit(560, y0 + 70, 400, HERO_H - 140)) detail_open(t->id);
    /* indicators */
    if (nhero > 1) {
        float dw = 26, dg = 6, total = nhero * dw + (nhero - 1) * dg;
        float dx = SCR_W - MARGIN - total, dy = y0 + HERO_H - 44;
        for (int i = 0; i < nhero; i++) {
            float px = dx + i * (dw + dg);
            gfx_rrect(px, dy, dw, 3, 1.5f, col_a(T->text, 0.22f));
            if (i == hero_i) gfx_rrect(px, dy, dw * clampf(S.hero_rotate ? hero_timer / 10 : 1, 0.12f, 1), 3, 1.5f, T->accent);
            if (hit(px - 3, dy - 14, dw + 6, 30)) {
                hero_prev = hero_i;
                hero_i = i;
                hero_timer = 0;
                hero_fade = 0;
            }
        }
    }
}

/* ---------------- shelves ---------------- */

static float item_w(Row *r) { return r->type == ROW_WIDE ? WIDE_W : pw(); }

static void open_item(Row *r, int i) {
    Title *t = &L.t[r->item[i]];
    if (r->type == ROW_WIDE && r->ent[i] >= 0) {
        Entry *e = &L.e[r->ent[i]];
        if (e->media == MEDIA_BOOK) reader_open(r->ent[i]);
        else if (!e->playable) toast("This file needs converting to MP4 to play on Vita");
        else player_open(r->ent[i], 1);
    } else {
        detail_open(t->id);
    }
}

static void draw_rows(float dt) {
    for (int ri = 1; ri < nrows; ri++) {
        Row *r = &rows[ri];
        float y = roundf(r->y - sy); /* whole pixels while scrolling */
        if (y > SCR_H || y + r->h < 0) continue;
        int focused = frow == ri;
        row_focus_anim[ri] = approach(row_focus_anim[ri], focused ? 1 : 0, 14, dt);
        r->fsel = approach(r->fsel, (float)r->col, S.reduce_motion ? 40 : 18, dt);
        if (!in.dragging || touch_row != ri) r->sx = approach(r->sx, r->sx_t, S.ambient ? 7 : 11, dt);
        /* label */
        int ls = T->display_font == 1 ? 19 : 17;
        int lf = T->display_font == 1 ? F_DISPLAY : F_SEMI;
        if (S.glass) { /* a clear section header: glass pill with an accent mark */
            float lw = text_w(lf, ls, r->label);
            gfx_glass(MARGIN - 12, y + 2, lw + 40, 30, 15, focused ? 1.0f : 0.7f);
            gfx_rrect(MARGIN - 1, y + 10, 3, 14, 1.5f, col_a(T->accent, focused ? 1 : 0.6f));
            text(lf, ls, MARGIN + 10, y + 23, col_a(focused ? T->text : T->text2, 1), r->label);
            gfx_rect(MARGIN + lw + 40, y + 17, SCR_W - MARGIN * 2 - lw - 40, 1, col_a(T->text, 0.07f)); /* divider */
        } else
            text(lf, ls, MARGIN, y + 22, col_a(focused ? T->text : T->text2, 1), r->label);
        if (r->type == ROW_ACTION) {
            draw_button(MARGIN, y + 36, 220, 46, IC_FOLDER, r->label, 0, focused ? 1 : 0);
            if (hit(MARGIN, y + 36, 220, 46) || (focused && (in.pressed & B_OK))) picker_open(PICK_SECRET);
            continue;
        }
        float w = item_w(r), h = r->type == ROW_WIDE ? WIDE_H : ph(), g = gap();
        float iy = y + 36;
        gfx_clip(0, y, SCR_W, r->h + 10);
        for (int i = 0; i < r->n; i++) {
            float ix = roundf(MARGIN + i * (w + g) - r->sx); /* whole pixels: no shimmer */
            if (ix > SCR_W + 10 || ix + w < -10) continue;
            float f = focused ? clampf(1 - fabsf(r->fsel - i), 0, 1) * row_focus_anim[ri] : 0;
            Title *t = &L.t[r->item[i]];
            if (r->type == ROW_WIDE) draw_wide_card(t, r->ent[i], ix, iy, w, h, f, 1);
            else {
                draw_poster(t, ix, iy, w, h, f, 1);
                if (f > 0.05f) text_ellipsis(F_REG, 12, ix + 2, iy + h + 18, w + 10, col_a(T->text2, f), t->title);
            }
            if (hit(ix, iy, w, h) && !in.dragging) {
                frow = ri;
                r->col = i;
                open_item(r, i);
                input_consume_touch();
            }
            if (hit_long(ix, iy, w, h)) {
                frow = ri;
                r->col = i;
                modal_quick(t);
                input_consume_touch();
            }
        }
        gfx_noclip();
    }
}

static void ensure_visible(Row *r) {
    float w = item_w(r), g = gap();
    float x = r->col * (w + g);
    float view = SCR_W - MARGIN * 2;
    if (x - r->sx_t > view - w - (w + g) * 0.6f) r->sx_t = x - view + w + (w + g) * 0.6f;
    if (x - r->sx_t < 0) r->sx_t = x - (r->col > 0 ? (w + g) * 0.4f : 0);
    float maxs = fmaxf(0, r->n * (w + g) - g - view);
    r->sx_t = clampf(r->sx_t, 0, maxs);
}

static float content_h(void) {
    if (nrows <= 1) return HERO_H;
    Row *r = &rows[nrows - 1];
    return r->y + r->h + 30;
}

static void home_input(void) {
    if (frow == -1) return;
    Row *r = frow >= 0 && frow < nrows ? &rows[frow] : NULL;
    if (!r) return;
    if (in.repeat & B_UP) {
        if (frow == 0) {
            frow = -1;
            nav_i = tab;
        } else {
            frow--;
            if (frow > 0) ensure_visible(&rows[frow]);
        }
        sfx(0);
    }
    if (in.repeat & B_DOWN) {
        if (frow < nrows - 1) {
            frow++;
            if (frow > 0) {
                Row *nr = &rows[frow];
                if (nr->col >= nr->n) nr->col = nr->n ? nr->n - 1 : 0;
                ensure_visible(nr);
            }
            sfx(0);
        }
    }
    r = &rows[frow];
    if (r->type == ROW_HERO) {
        if (nhero > 0) {
            if (in.repeat & B_LEFT) {
                if (hero_btn > 0) hero_btn--, sfx(0);
                else if (nhero > 1) {
                    hero_prev = hero_i;
                    hero_i = (hero_i + nhero - 1) % nhero;
                    hero_timer = 0;
                    hero_fade = 0;
                    sfx(0);
                }
            }
            if (in.repeat & B_RIGHT) {
                if (hero_btn < 2) hero_btn++, sfx(0);
                else if (nhero > 1) {
                    hero_prev = hero_i;
                    hero_i = (hero_i + 1) % nhero;
                    hero_timer = 0;
                    hero_fade = 0;
                    sfx(0);
                }
            }
            if (in.pressed & B_OK) { sfx(1); hero_action(hero_btn); }
            if (in.pressed & B_TRI) modal_quick(&L.t[hero[hero_i]]);
            if (in.pressed & B_SQR) hero_action(2);
        }
        return;
    }
    if (r->type == ROW_ACTION) return;
    if (in.repeat & B_LEFT && r->col > 0) { r->col--; ensure_visible(r); sfx(0); }
    if (in.repeat & B_RIGHT && r->col < r->n - 1) { r->col++; ensure_visible(r); sfx(0); }
    if (r->n > 0) {
        Title *t = &L.t[r->item[r->col]];
        if (in.pressed & B_OK) { sfx(1); open_item(r, r->col); }
        if (in.pressed & B_TRI) modal_quick(t);
        if (in.pressed & B_SQR) {
            lib_set_favorite(t, !t->favorite);
            toast(t->favorite ? "Added to favorites" : "Removed from favorites");
        }
    }
    if (in.pressed & B_BACK) {
        frow = 0;
        sfx(2);
    }
}

static void home_touch(float dt) {
    (void)dt;
    if (in.touching && !in.dragging) {
        touch_axis = 0;
        touch_row = -1;
    }
    if (in.dragging) {
        if (touch_axis == 0) {
            touch_axis = fabsf(in.tx - in.sx) > fabsf(in.ty - in.sy) ? 2 : 1;
            if (touch_axis == 2) {
                for (int ri = 1; ri < nrows; ri++) {
                    float y = rows[ri].y - sy;
                    if (in.sy >= y && in.sy < y + rows[ri].h) touch_row = ri;
                }
                if (touch_row < 0) touch_axis = 1;
            }
        }
        if (touch_axis == 1) {
            sy -= in.ddy;
            sy_t = sy;
        } else if (touch_row >= 0) {
            Row *r = &rows[touch_row];
            r->sx -= in.ddx;
            r->sx_t = r->sx;
        }
    }
    if (in.released) {
        if (touch_axis == 1) sy_t = sy - in.vy * 0.28f;
        else if (touch_axis == 2 && touch_row >= 0) {
            Row *r = &rows[touch_row];
            float w = item_w(r), g = gap();
            r->sx_t = r->sx - in.vx * 0.3f;
            float maxs = fmaxf(0, r->n * (w + g) - g - (SCR_W - MARGIN * 2));
            r->sx_t = clampf(r->sx_t, 0, maxs);
            /* keep focus on something visible */
            r->col = clampi((int)((r->sx_t + (w + g) * 0.5f) / (w + g)), 0, r->n - 1);
        }
        touch_axis = 0;
    }
}

static void draw_home(float dt) {
    int secret = secret_view();
    if (built_secret != secret) build_rows();
    draw_scene_bg(S.ambient ? 1.3f : 1.0f);
    home_input();
    home_touch(dt);
    /* vertical scroll follows focus */
    if (!in.dragging) {
        if (in.repeat & (B_UP | B_DOWN)) {
            if (frow <= 0) sy_t = 0;
            else sy_t = rows[frow].y - 84;
        }
        float maxs = fmaxf(0, content_h() - SCR_H + 10);
        sy_t = clampf(sy_t, 0, maxs);
        if (!in.touching) sy = approach(sy, sy_t, S.ambient ? 6 : 10, dt);
    }
    draw_hero(roundf(-sy), dt, frow == 0); /* moves with the shelves: nothing slides over its buttons */
    /* the hero gently dims away as the shelves scroll up */
    float hero_bottom = HERO_H - sy;
    if (sy > 1 && hero_bottom > 0) gfx_rect(0, 0, SCR_W, hero_bottom, col_a(T->bg, clampf(sy / 200.0f, 0, 1)));
    draw_rows(dt);
}

/* ---------------- grids ---------------- */

static const char *SORT_NAMES[5] = {"Recently added", "A\xe2\x80\x93Z", "Year", "Rating", "Last watched"};
static const char *FILTER_NAMES[4] = {"All", "Unfinished", "Favorites", "In progress"};

static void draw_grid(float dt) {
    draw_scene_bg(S.ambient ? 1.3f : 1.0f);
    float w = pw(), h = ph(), g = gap() + 6;
    int cols = (int)((SCR_W - MARGIN * 2 + g) / (w + g));
    if (cols < 1) cols = 1;
    float total = cols * w + (cols - 1) * g;
    float x0 = (SCR_W - total) / 2;
    float cell_h = h + 40;
    float top = 172;

    /* input */
    if (frow != -1) {
        if (gchip >= 0) {
            if (in.repeat & B_LEFT && gchip > 0) gchip--, sfx(0);
            if (in.repeat & B_RIGHT && gchip < 2) gchip++, sfx(0);
            if (in.repeat & B_UP) { frow = -1; nav_i = tab; sfx(0); }
            if (in.repeat & B_DOWN && gn > 0) { gchip = -1; sfx(0); }
            if (in.pressed & B_OK) {
                if (gchip == 0) gsort = (gsort + 1) % 5;
                else if (gchip == 1) gfilter = (gfilter + 1) % 4;
                else save_view();
                build_grid();
                sfx(1);
            }
        } else {
            if (in.repeat & B_LEFT && gfocus % cols > 0) gfocus--, sfx(0);
            if (in.repeat & B_RIGHT && gfocus % cols < cols - 1 && gfocus < gn - 1) gfocus++, sfx(0);
            if (in.repeat & B_UP) {
                if (gfocus >= cols) gfocus -= cols;
                else gchip = 0;
                sfx(0);
            }
            if (in.repeat & B_DOWN && gfocus + cols < gn + (cols - gn % cols) % cols && gfocus < gn - 1) {
                gfocus = gfocus + cols < gn ? gfocus + cols : gn - 1;
                sfx(0);
            }
            if (gn > 0) {
                Title *t = &L.t[gitems[gfocus]];
                if (in.pressed & B_OK) { sfx(1); detail_open(t->id); }
                if (in.pressed & B_TRI) modal_quick(t);
                if (in.pressed & B_SQR) {
                    lib_set_favorite(t, !t->favorite);
                    toast(t->favorite ? "Added to favorites" : "Removed from favorites");
                }
            }
            if (in.pressed & B_BACK) { gfocus = 0; gsy_t = 0; sfx(2); }
            if (in.pressed & B_L && 0) {}
        }
        if (gn == 0 && gchip < 0) gchip = 0;
    }
    int row = gfocus / cols;
    if (!in.dragging && (in.repeat & (B_UP | B_DOWN))) gsy_t = row * cell_h - (row > 0 ? cell_h * 0.5f : 0);
    if (in.dragging) {
        gsy -= in.ddy;
        gsy_t = gsy;
    }
    if (in.released) gsy_t = gsy - in.vy * 0.28f;
    int nrows_g = (gn + cols - 1) / cols;
    float maxs = fmaxf(0, nrows_g * cell_h - (SCR_H - top) + 30);
    gsy_t = clampf(gsy_t, 0, maxs);
    if (!in.touching) gsy = approach(gsy, gsy_t, 11, dt);
    gfsel = approach(gfsel, (float)gfocus, 20, dt);

    /* header */
    float hy = 112 - fminf(gsy, 60) * 0.5f;
    const char *title = TAB_NAMES[tab];
    int ts = T->display_font == 2 ? 26 : 32;
    text(F_DISPLAY, ts, MARGIN, hy, T->text, title);
    char cnt[48];
    int kind = tab == TAB_MOVIES ? KIND_MOVIE : tab == TAB_SHOWS ? KIND_SERIES : KIND_BOOK;
    int all = kind == KIND_MOVIE ? L.nmovies : kind == KIND_SERIES ? L.nshows : L.nbooks;
    snprintf(cnt, sizeof(cnt), kind == KIND_BOOK ? "%d files" : "%d titles", all);
    text(F_META, 13, MARGIN + text_w(F_DISPLAY, ts, title) + 14, hy - 1, T->text3, cnt);
    float cw;
    char c1[48], c2[48];
    const char *c3 = "+ Save view";
    snprintf(c2, sizeof(c2), "%s", FILTER_NAMES[gfilter]);
    snprintf(c1, sizeof(c1), "%s", SORT_NAMES[gsort]);
    float w3 = text_w(F_SEMI, 13, c3) + 28, w2 = text_w(F_SEMI, 13, c2) + 28, w1 = text_w(F_SEMI, 13, c1) + 28;
    float x3 = SCR_W - MARGIN - w3, x2 = x3 - 10 - w2, x1 = x2 - 10 - w1;
    float chip_y = hy - 22;
    draw_chip(x3, chip_y, c3, 0, frow != -1 && gchip == 2 ? 1 : 0, &cw);
    if (hit(x3, chip_y, w3, 30)) save_view();
    draw_chip(x2, chip_y, c2, 0, frow != -1 && gchip == 1 ? 1 : 0, &cw);
    if (hit(x2, chip_y, w2, 30)) { gfilter = (gfilter + 1) % 4; build_grid(); }
    draw_chip(x1, chip_y, c1, 0, frow != -1 && gchip == 0 ? 1 : 0, &cw);
    if (hit(x1, chip_y, w1, 30)) { gsort = (gsort + 1) % 5; build_grid(); }
    icon(IC_CHEVRON_D, x1 - 14, chip_y + 15, 12, T->text3);

    if (gn == 0) {
        Lofi *l = lofi_get(T->scene);
        float ww = 300, wh = 170, wx = (SCR_W - ww) / 2, wy = 190;
        if (l) lofi_draw(l, wx, wy, ww, wh, T->radius, 0xFFFFFFFF, 0.5f, 0.5f);
        const char *msg = gfilter ? "Nothing matches this filter" : tab == TAB_BOOKS ? "No books or manga yet" : "Nothing here yet";
        text_center(F_SEMI, 17, SCR_W / 2, wy + wh + 36, T->text, msg);
        text_center(F_REG, 14, SCR_W / 2, wy + wh + 60, T->text3,
                    gfilter ? "Change the filter above to see everything." : "Add a folder from Settings \xe2\x80\xba Library.");
        return;
    }
    gfx_clip(0, top - 30, SCR_W, SCR_H - top + 30);
    for (int i = 0; i < gn; i++) {
        int rr = i / cols, cc = i % cols;
        float x = roundf(x0 + cc * (w + g)), y = roundf(top + rr * cell_h - gsy);
        if (y > SCR_H || y + cell_h < top - 40) continue;
        float f = (frow != -1 && gchip < 0) ? clampf(1 - fabsf(gfsel - i) * 1.5f, 0, 1) * (i == gfocus ? 1 : 0.0f) : 0;
        if (i == gfocus && frow != -1 && gchip < 0) f = clampf(1 - fabsf(gfsel - i), 0, 1);
        Title *t = &L.t[gitems[i]];
        g_poster_caption = 1;
        draw_poster(t, x, y, w, h, f, 1);
        g_poster_caption = 0;
        text_ellipsis(F_REG, 12, x + 1, y + h + 18, w, col_a(f > 0.5f ? T->text : T->text3, 1), t->title);
        if (hit(x, y, w, h) && !in.dragging) {
            gfocus = i;
            gchip = -1;
            detail_open(t->id);
            input_consume_touch();
        }
        if (hit_long(x, y, w, h)) {
            gfocus = i;
            modal_quick(t);
            input_consume_touch();
        }
    }
    gfx_noclip();
    /* soft fade under the header */
    gfx_grad_v(0, top - 30, SCR_W, 26, T->bg, col_a(T->bg, 0));
}

/* ---------------- secret (locked) ---------------- */

static void draw_secret_locked(float dt) {
    (void)dt;
    vita2d_texture *soft = lofi_soft("alley");
    if (soft) gfx_tex(soft, 0, 0, SCR_W, SCR_H, 0, 0xFFFFFFFF, 0.5f, 0.5f);
    gfx_rect(0, 0, SCR_W, SCR_H, col_a(T->bg, 0.55f));
    float cx = SCR_W / 2.0f;
    gfx_circle(cx, 210, 38, col_a(T->text, 0.08f));
    gfx_ring(cx, 210, 38, 1.5f, col_a(T->text, 0.18f));
    icon(IC_LOCK, cx, 210, 30, T->text);
    text_center(F_DISPLAY, T->display_font == 2 ? 24 : 30, cx, 292, T->text, "Secret");
    int has_pin = db_get("pin_hash", NULL) != NULL;
    text_center(F_REG, 15, cx, 322, T->text2,
                has_pin ? "Private videos and books, kept behind your PIN." : "Set a PIN to keep private videos and books here.");
    float bw = 180, bx = cx - bw / 2, by = 352;
    int f = frow != -1;
    draw_button(bx, by, bw, 46, IC_UNLOCK, has_pin ? "Unlock" : "Set up PIN", 1, f ? 1 : 0);
    if (hit(bx, by, bw, 46) || (f && (in.pressed & B_OK))) pin_open(has_pin ? PIN_UNLOCK : PIN_CREATE);
    if (f && (in.repeat & B_UP)) { frow = -1; nav_i = tab; }
}

/* ---------------- frame ---------------- */

static void music_intro_cb(int c, void *ud) {
    (void)ud;
    db_seti("seen_music_intro", 1);
    if (c == 1) {
        set_tab(TAB_MUSIC);
        frow = 0;
    }
}

void main_frame(float dt) {
    tab_anim = approach(tab_anim, 1, 10, dt);
    {
        static int asked;
        if (!asked && !modal_active() && g_time > 1.5f) {
            asked = 1;
            if (!db_geti("seen_music_intro", 0))
                modal_confirm("New: Music",
                              "Nest plays your music now. Put songs in ux0:music and open the Music tab: albums, artists, "
                              "playlists, an equaliser, and it keeps playing with the screen off.",
                              "Open Music", "Later", music_intro_cb, NULL);
        }
    }
    if (g_completed_tid >= 0 && !modal_active()) {
        Title *t = lib_title_by_id(g_completed_tid);
        g_completed_tid = -1;
        if (t) modal_rate_ex(t, "You finished it. How was it?");
    }
    /* global keys */
    if (in.pressed & B_L) set_tab((tab + TAB_COUNT - 1) % TAB_COUNT);
    if (in.pressed & B_R) set_tab((tab + 1) % TAB_COUNT);
    if (in.pressed & B_START) { settings_enter(); return; }
    if (in.pressed & B_SELECT) {
        if (tab == TAB_MUSIC && mus_active()) nowplaying_open();
        else search_enter();
        return;
    }

    /* navigation bar focus */
    if (frow == -1) {
        if (in.repeat & B_LEFT && nav_i > 0) nav_i--, sfx(0);
        if (in.repeat & B_RIGHT && nav_i < TAB_COUNT + 1) nav_i++, sfx(0);
        if (in.pressed & B_OK) {
            sfx(1);
            if (nav_i < TAB_COUNT) set_tab(nav_i);
            else if (nav_i == TAB_COUNT) search_enter();
            else settings_enter();
            if (nav_i < TAB_COUNT) frow = 0;
        }
        if (in.repeat & B_DOWN) {
            if (nav_i < TAB_COUNT) set_tab(nav_i);
            frow = 0;
            gchip = (tab >= TAB_MOVIES && tab <= TAB_BOOKS) ? 0 : -1;
            sfx(0);
            in.repeat &= ~B_DOWN;
        }
        if (in.pressed & B_BACK) { frow = 0; nav_i = tab; }
        in.pressed &= ~(B_OK | B_BACK);
    }

    float slide = (1 - ease_out(tab_anim)) * 18;
    (void)slide;
    if (tab == TAB_HOME) draw_home(dt);
    else if (tab == TAB_MUSIC) {
        int want_nav = 0;
        music_tab_frame(dt, frow != -1 && !modal_active(), &want_nav);
        if (want_nav) { frow = -1; nav_i = tab; sfx(0); }
    } else if (tab == TAB_SECRET) {
        if (g_secret_unlocked) draw_home(dt);
        else draw_secret_locked(dt);
    } else draw_grid(dt);
    if (tab_anim < 1) gfx_rect(0, 0, SCR_W, SCR_H, col_a(T->bg, 1 - ease_out(tab_anim)));

    draw_nav(dt, 1);
    if (frow != -1 && !modal_active()) {
        static const char *hints[] = {"OK", "Open", "TRI", "Options", "L", "R", "SELECT", "Search"};
        (void)hints;
        static const char *h2[] = {"OK", "Open", "TRI", "Options", "SQR", "Favorite"};
        if ((tab != TAB_SECRET || g_secret_unlocked) && tab != TAB_MUSIC && !miniplayer_height()) {
            float a = (tab == TAB_HOME || tab == TAB_SECRET) ? clampf(sy / 80.0f, 0, 1) : 1;
            if (a > 0.05f) {
                gfx_grad_v(0, SCR_H - 46, SCR_W, 46, col_a(T->bg, 0), col_a(T->bg, 0.9f * a));
                draw_hint_bar(h2, 6);
            }
        }
    }
}
