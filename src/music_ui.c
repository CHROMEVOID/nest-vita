/* Music screens: the Music tab (home, playlists, albums, artists, songs,
   search), album / artist / playlist pages, Now Playing with the queue, and
   the little player bar that follows you around the menus. */
#include "nest.h"
#include "music.h"

#define MG 40.0f

static void fmt_ms(int ms, char *b, int n) {
    int s = ms / 1000;
    if (s < 0) s = 0;
    if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, n, "%d:%02d", s / 60, s % 60);
}
static void fmt_long(int ms, char *b, int n) {
    int m = ms / 60000;
    if (m >= 60) snprintf(b, n, "%d hr %d min", m / 60, m % 60);
    else snprintf(b, n, "%d min", m < 1 ? 1 : m);
}

/* ---------------- artwork ---------------- */

static unsigned seed_hash(const char *s) {
    unsigned h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void art_placeholder(const char *seed, float x, float y, float w, float h, float r, float a) {
    static const unsigned pal[8] = {0x5B4B8A, 0x3D6B7A, 0x7A4B5B, 0x4B7A55, 0x7A6A3D, 0x3D4B7A, 0x6A3D7A, 0x3D7A72};
    unsigned hs = seed_hash(seed);
    col_t c1 = HEX(pal[hs % 8]), c2 = HEX(pal[(hs / 8) % 8]);
    gfx_rrect_grad(x, y, w, h, r, col_a(col_mix(c1, 0xFFFFFFFF, 0.08f), a), col_a(col_mix(c2, 0xFF000000, 0.35f), a));
    icon(IC_NOTE, x + w / 2, y + h / 2, fminf(w, h) * 0.34f, col_a(0xFFFFFFFF, 0.55f * a));
}

void art_draw(const char *art, const char *seed, float x, float y, float w, float h, float r, float a) {
    float fade = 0;
    int sz = w > 200 ? 360 : w > 90 ? 200 : 96;
    vita2d_texture *tex = art && art[0] ? img_get(art, sz, sz, &fade) : NULL;
    if (!tex || fade < 1) art_placeholder(seed, x, y, w, h, r, a * (tex ? 1 - fade : 1));
    if (tex) gfx_tex(tex, x, y, w, h, r, col_a(0xFFFFFFFF, a * fade), 0.5f, 0.5f);
}

/* a soft, blurred copy of the cover filling an area (a tiny image stretched up) */
static void art_blur(const char *art, float x, float y, float w, float h, float a) {
    float fade = 0;
    vita2d_texture *tex = art && art[0] ? img_get(art, 20, 20, &fade) : NULL;
    if (tex) gfx_tex(tex, x, y, w, h, 0, col_a(0xFFFFFFFF, a * fade), 0.5f, 0.5f);
}

static void liked_cover(float x, float y, float s, float r, float a) {
    gfx_rrect_grad(x, y, s, s, r, col_a(col_mix(T->accent, 0xFFFFFFFF, 0.25f), a), col_a(col_mix(T->accent, 0xFF000000, 0.45f), a));
    icon(IC_HEART_FILL, x + s / 2, y + s / 2, s * 0.36f, col_a(0xFFFFFFFF, a));
}

/* a playlist without its own picture shows the first four album covers */
static void playlist_cover(MPlaylist *p, float x, float y, float s, float r, float a) {
    if (p->liked_list) { liked_cover(x, y, s, r, a); return; }
    if (p->cover && p->cover[0]) { art_draw(p->cover, p->name, x, y, s, s, r, a); return; }
    const char *arts[4];
    int n = 0;
    for (int i = 0; i < p->nt && n < 4; i++) {
        const char *ar = M.t[p->tracks[i]].art;
        if (!ar || !ar[0]) continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (!strcmp(arts[k], ar)) dup = 1;
        if (!dup) arts[n++] = ar;
    }
    if (n < 4) { art_draw(n ? arts[0] : NULL, p->name, x, y, s, s, r, a); return; }
    float h = s / 2;
    art_draw(arts[0], p->name, x, y, h, h, 0, a);
    art_draw(arts[1], p->name, x + h, y, h, h, 0, a);
    art_draw(arts[2], p->name, x, y + h, h, h, 0, a);
    art_draw(arts[3], p->name, x + h, y + h, h, h, 0, a);
    gfx_rrect_outline(x, y, s, s, r, 2, col_a(T->bg, a)); /* soften the square corners */
}

/* kind: 0 album 1 artist 2 playlist */
static void card(int kind, int idx, float x, float y, float s, float f, float a) {
    float g = 4 * f;
    float r = kind == 1 ? (s + g * 2) / 2 : T->radius_sm;
    if (f > 0.02f) gfx_shadow(x - g, y - g + 6, s + g * 2, s + g * 2, r, 16, col_a(0xFF000000, 0.4f * f * a));
    const char *t1 = "", *t2 = "";
    char sub[96];
    if (kind == 0) {
        MAlbum *al = &M.a[idx];
        art_draw(al->art, al->name, x - g, y - g, s + g * 2, s + g * 2, r, a);
        t1 = al->name;
        if (al->year) snprintf(sub, sizeof(sub), "%d \xc2\xb7 %s", al->year, al->artist);
        else snprintf(sub, sizeof(sub), "%s", al->artist);
        t2 = sub;
    } else if (kind == 1) {
        MArtist *ar = &M.r[idx];
        art_draw(ar->art, ar->name, x - g, y - g, s + g * 2, s + g * 2, r, a);
        t1 = ar->name;
        snprintf(sub, sizeof(sub), "%d song%s", ar->nt, ar->nt == 1 ? "" : "s");
        t2 = sub;
    } else {
        MPlaylist *p = &M.p[idx];
        playlist_cover(p, x - g, y - g, s + g * 2, r, a);
        t1 = p->name;
        snprintf(sub, sizeof(sub), "Playlist \xc2\xb7 %d song%s", p->nt, p->nt == 1 ? "" : "s");
        t2 = sub;
    }
    if (f > 0.02f) draw_focus(x - g, y - g, s + g * 2, s + g * 2, r, f);
    float ty = y + s + 20;
    if (kind == 1) {
        text_center(F_SEMI, 13, x + s / 2, ty, col_a(f > 0.5f ? T->text : T->text2, a), t1);
    } else {
        text_ellipsis(F_SEMI, 13, x, ty, s, col_a(f > 0.5f ? T->text : T->text2, a), t1);
        text_ellipsis(F_REG, 11, x, ty + 16, s, col_a(T->text3, a), t2);
    }
}

/* a song row: number or art, title, artist, length; playing song in the accent colour */
static void song_row(int ti, int num, float x, float y, float w, float h, float f, int show_art, int show_album) {
    MTrack *t = &M.t[ti];
    int cur = mus_current() == ti;
    if (f > 0.02f) gfx_rrect(x - 8, y, w + 16, h, T->radius_sm, col_a(T->text, 0.08f * f));
    float tx = x;
    if (show_art) {
        art_draw(t->art, t->album, x, y + (h - 40) / 2, 40, 40, 6, 1);
        tx = x + 54;
    } else {
        char n[8];
        snprintf(n, sizeof(n), "%d", num);
        if (cur && mus_playing()) { /* little moving bars */
            for (int b = 0; b < 3; b++) {
                float k = 0.35f + 0.65f * fabsf(sinf(g_time * (5.5f + b * 1.7f) + b));
                float bh = 14 * k * (0.4f + mus_level());
                if (bh > 14) bh = 14;
                gfx_rect(x + 4 + b * 5, y + h / 2 + 7 - bh, 3, bh, T->accent);
            }
        } else text_center(F_META, 13, x + 12, y + h / 2 + 5, cur ? T->accent : T->text3, n);
        tx = x + 38;
    }
    float rw = 60;
    char line2[300];
    if (show_album && t->album[0]) snprintf(line2, sizeof(line2), "%s \xc2\xb7 %s", t->artist, t->album);
    else snprintf(line2, sizeof(line2), "%s", t->artist);
    text_ellipsis(F_SEMI, 15, tx, y + h / 2 - 2, w - (tx - x) - rw - 30, cur ? T->accent : T->text, t->title);
    text_ellipsis(F_REG, 12, tx, y + h / 2 + 15, w - (tx - x) - rw - 30, T->text3, line2);
    if (t->liked) icon(IC_HEART_FILL, x + w - rw - 10, y + h / 2, 13, T->accent);
    char d[16];
    fmt_ms(t->dur, d, sizeof(d));
    text_right(F_META, 12, x + w, y + h / 2 + 5, T->text3, t->dur ? d : "");
}

/* ---------------- track options ---------------- */

static int menu_track = -1, menu_pl = -1, menu_pos = -1;

static void new_playlist_named(const char *name, void *ud) {
    (void)ud;
    if (!name || !name[0]) return;
    int pi = music_playlist_create(name);
    if (pi >= 0 && menu_track >= 0) {
        music_playlist_add(pi, menu_track);
        toast("Added to your new playlist");
    } else if (pi >= 0) toast("Playlist made");
}

static int pick_map[24];
static void pick_playlist_cb(int c, void *ud) {
    (void)ud;
    if (c < 0) return;
    int pi = pick_map[c];
    if (pi == -2) {
        ime_request("Name your playlist", "", 60, new_playlist_named, NULL);
        return;
    }
    if (pi >= 0 && pi < M.np && menu_track >= 0) {
        MPlaylist *p = &M.p[pi];
        for (int i = 0; i < p->nt; i++)
            if (p->tracks[i] == menu_track) { toast("It's already in that playlist"); return; }
        music_playlist_add(pi, menu_track);
        char m[120];
        snprintf(m, sizeof(m), "Added to %s", p->name);
        toast(m);
    }
}

static void pick_playlist(void) {
    static char names[24][64];
    static const char *items[24];
    int n = 0;
    pick_map[n] = -2;
    snprintf(names[n], 64, "+ New playlist");
    items[n] = names[n];
    n++;
    for (int i = 1; i < M.np && n < 24; i++) {
        if (M.p[i].source && M.p[i].source[0]) continue; /* playlists from files are edited on the PC */
        pick_map[n] = i;
        snprintf(names[n], 64, "%s", M.p[i].name);
        items[n] = names[n];
        n++;
    }
    modal_menu("Add to playlist", items, n, pick_playlist_cb, NULL);
}

static int menu_act[10];
static void track_menu_cb(int c, void *ud) {
    (void)ud;
    if (c < 0 || menu_track < 0 || menu_track >= M.nt) return;
    MTrack *t = &M.t[menu_track];
    switch (menu_act[c]) {
    case 0: mus_queue_add(menu_track, 1); toast("Plays next"); break;
    case 1: mus_queue_add(menu_track, 0); toast("Added to the queue"); break;
    case 2:
        music_set_liked(menu_track, !t->liked);
        toast(t->liked ? "Added to Liked Songs" : "Removed from Liked Songs");
        break;
    case 3: pick_playlist(); break;
    case 4: mpage_open(FROM_ALBUM, t->album_i); break;
    case 5: mpage_open(FROM_ARTIST, t->artist_i); break;
    case 6: music_playlist_remove(menu_pl, menu_pos); toast("Removed from this playlist"); break;
    }
}

void music_track_menu(int ti, int pl_i, int pl_pos) {
    if (ti < 0 || ti >= M.nt) return;
    menu_track = ti;
    menu_pl = pl_i;
    menu_pos = pl_pos;
    static const char *items[10];
    int n = 0;
    items[n] = "Play next"; menu_act[n++] = 0;
    items[n] = "Add to queue"; menu_act[n++] = 1;
    items[n] = M.t[ti].liked ? "Remove from Liked Songs" : "Add to Liked Songs"; menu_act[n++] = 2;
    items[n] = "Add to playlist\xe2\x80\xa6"; menu_act[n++] = 3;
    items[n] = "Go to album"; menu_act[n++] = 4;
    items[n] = "Go to artist"; menu_act[n++] = 5;
    if (pl_i > 0 && pl_i < M.np && !(M.p[pl_i].source && M.p[pl_i].source[0])) { items[n] = "Remove from this playlist"; menu_act[n++] = 6; }
    modal_menu(M.t[ti].title, items, n, track_menu_cb, NULL);
}

/* ================================================================
   The Music tab
   ================================================================ */

enum { MV_HOME, MV_PLAYLISTS, MV_ALBUMS, MV_ARTISTS, MV_SONGS, MV_SEARCH, MV_COUNT };
static const char *MV_NAMES[MV_COUNT] = {"For you", "Playlists", "Albums", "Artists", "Songs", "Search"};
static int mv;
static int fr = -1;          /* -1 = the chips row */
static int fc;               /* focus column / index */
static float scy, scy_t;
static float fsel;

/* home rows */
typedef struct {
    char label[64];
    int kind[40], idx[40];
    int n, col, shortcuts;
    float sx, sx_t;
    float y, h;
} HRow;
static HRow hr[6];
static int nhr;

/* songs list / search results */
static int *slist;
static int sn, scap;
static int ssort; /* 0 recently added, 1 title, 2 artist */
static char sq[64];
static int s_art[8], s_alb[12], s_trk[120], n_art, n_alb, n_trk;

static void hr_add(const char *label, int shortcuts) {
    if (nhr >= 6) return;
    HRow *r = &hr[nhr++];
    memset(r, 0, sizeof(*r));
    snprintf(r->label, sizeof(r->label), "%s", label);
    r->shortcuts = shortcuts;
}
static void hr_push(int kind, int idx) {
    HRow *r = &hr[nhr - 1];
    for (int i = 0; i < r->n; i++)
        if (r->kind[i] == kind && r->idx[i] == idx) return;
    if (r->n < 40) { r->kind[r->n] = kind; r->idx[r->n] = idx; r->n++; }
}

static int *sort_tmp;
static int cmp_album_recent(const void *a, const void *b) {
    int64_t x = M.a[*(const int *)a].last_played, y = M.a[*(const int *)b].last_played;
    return x < y ? 1 : x > y ? -1 : 0;
}
static int cmp_album_added(const void *a, const void *b) {
    int64_t x = M.a[*(const int *)a].added, y = M.a[*(const int *)b].added;
    if (x != y) return x < y ? 1 : -1;
    return *(const int *)a - *(const int *)b;
}
static int cmp_artist_plays(const void *a, const void *b) {
    int pa = 0, pb = 0;
    const MArtist *x = &M.r[*(const int *)a], *y = &M.r[*(const int *)b];
    for (int i = 0; i < x->nt; i++) pa += M.t[x->tracks[i]].plays;
    for (int i = 0; i < y->nt; i++) pb += M.t[y->tracks[i]].plays;
    if (pa != pb) return pb - pa;
    return y->nt - x->nt;
}

static void build_home(void) {
    nhr = 0;
    if (!M.nt) return;
    int kinds[24], idx[24];
    int nr = music_recents(kinds, idx, 24);
    if (nr > 0) {
        hr_add("Jump back in", 1);
        for (int i = 0; i < nr && hr[nhr - 1].n < 6; i++) hr_push(kinds[i] == 0 ? 0 : kinds[i] == 1 ? 1 : 2, idx[i]);
    }
    hr_add("Your playlists", 0);
    for (int i = 0; i < M.np; i++)
        if (!M.p[i].liked_list || M.p[i].nt) hr_push(2, i);
    if (!hr[nhr - 1].n) nhr--;
    sort_tmp = realloc(sort_tmp, sizeof(int) * (M.na + M.nr + 1));
    for (int i = 0; i < M.na; i++) sort_tmp[i] = i;
    qsort(sort_tmp, M.na, sizeof(int), cmp_album_recent);
    if (M.na && M.a[sort_tmp[0]].last_played) {
        hr_add("Albums on repeat", 0);
        for (int i = 0; i < M.na && M.a[sort_tmp[i]].last_played; i++) hr_push(0, sort_tmp[i]);
    }
    for (int i = 0; i < M.nr; i++) sort_tmp[i] = i;
    qsort(sort_tmp, M.nr, sizeof(int), cmp_artist_plays);
    hr_add("Your artists", 0);
    for (int i = 0; i < M.nr; i++) hr_push(1, sort_tmp[i]);
    for (int i = 0; i < M.na; i++) sort_tmp[i] = i;
    qsort(sort_tmp, M.na, sizeof(int), cmp_album_added);
    hr_add("Recently added", 0);
    for (int i = 0; i < M.na; i++) hr_push(0, sort_tmp[i]);
}

static int cmp_song_title(const void *a, const void *b) { return strcasecmp(M.t[*(const int *)a].title, M.t[*(const int *)b].title); }
static int cmp_song_artist(const void *a, const void *b) {
    const MTrack *x = &M.t[*(const int *)a], *y = &M.t[*(const int *)b];
    if (x->artist_i != y->artist_i) return x->artist_i - y->artist_i;
    if (x->album_i != y->album_i) return x->album_i - y->album_i;
    return 0;
}
static int cmp_song_added(const void *a, const void *b) {
    int64_t x = M.t[*(const int *)a].added, y = M.t[*(const int *)b].added;
    if (x != y) return x < y ? 1 : -1;
    return cmp_song_artist(a, b);
}

static void build_songs(void) {
    if (scap < M.nt + 1) {
        scap = M.nt + 1;
        slist = realloc(slist, sizeof(int) * scap);
    }
    sn = M.nt;
    for (int i = 0; i < M.nt; i++) slist[i] = i;
    qsort(slist, sn, sizeof(int), ssort == 1 ? cmp_song_title : ssort == 2 ? cmp_song_artist : cmp_song_added);
}

static void do_search(void) {
    music_search(sq, s_trk, &n_trk, s_alb, &n_alb, s_art, &n_art, 8);
    /* more songs than the other kinds */
    int tmp_a[8], tmp_b[8], dummy;
    if (n_trk >= 8) music_search(sq, s_trk, &n_trk, tmp_a, &dummy, tmp_b, &dummy, 120);
    if (n_alb > 12) n_alb = 12;
}

static void set_mv(int v) {
    if (v == mv) return;
    mv = v;
    fc = 0;
    scy = scy_t = 0;
    if (mv == MV_HOME) build_home();
    if (mv == MV_SONGS) build_songs();
    sfx(0);
}

static int built_gen = -1;
void music_tab_enter(void) {
    built_gen = M.gen;
    if (mv == MV_HOME) build_home();
    if (mv == MV_SONGS) build_songs();
    if (mv == MV_SEARCH && sq[0]) do_search();
}

static void search_typed(const char *t, void *ud) {
    (void)ud;
    if (!t) return;
    snprintf(sq, sizeof(sq), "%s", t);
    do_search();
    mv = MV_SEARCH;
    fr = sq[0] ? 0 : -1;
    fc = 0;
    scy = scy_t = 0;
}

static void open_item(int kind, int idx) {
    sfx(1);
    if (kind == 0) mpage_open(FROM_ALBUM, idx);
    else if (kind == 1) mpage_open(FROM_ARTIST, idx);
    else mpage_open(M.p[idx].liked_list ? FROM_LIKED : FROM_PLAYLIST, idx);
}

static void item_menu(int kind, int idx);

/* --- home --- */
#define CARD 128.0f
#define CGAP 22.0f
static void draw_home_rows(float dt, int focused, float top) {
    float y = top - scy;
    for (int r = 0; r < nhr; r++) {
        HRow *row = &hr[r];
        row->y = y + scy;
        text(F_BOLD, 19, MG, y + 6, T->text, row->label);
        y += 22;
        if (row->shortcuts) {
            /* compact tiles, three across */
            int cols = 3;
            float tw = (SCR_W - MG * 2 - 16 * (cols - 1)) / cols, th = 56;
            for (int i = 0; i < row->n; i++) {
                float x = MG + (i % cols) * (tw + 16), ty = y + (i / cols) * (th + 12);
                float f = focused && fr == r && fc == i ? 1 : 0;
                gfx_rrect(x, ty, tw, th, T->radius_sm, col_a(T->text, 0.07f + 0.07f * f));
                const char *name = "";
                int k = row->kind[i], ix = row->idx[i];
                if (k == 0) { art_draw(M.a[ix].art, M.a[ix].name, x, ty, th, th, T->radius_sm, 1); name = M.a[ix].name; }
                else if (k == 1) { art_draw(M.r[ix].art, M.r[ix].name, x + 4, ty + 4, th - 8, th - 8, (th - 8) / 2, 1); name = M.r[ix].name; }
                else { playlist_cover(&M.p[ix], x, ty, th, T->radius_sm, 1); name = M.p[ix].name; }
                text_ellipsis(F_SEMI, 14, x + th + 12, ty + th / 2 + 5, tw - th - 24, T->text, name);
                if (f) draw_focus(x, ty, tw, th, T->radius_sm, 1);
                if (hit(x, ty, tw, th)) { fr = r; fc = i; open_item(k, ix); input_consume_touch(); }
            }
            row->h = 22 + ((row->n + cols - 1) / cols) * (th + 12) + 22;
            y += row->h - 22;
            continue;
        }
        /* a sideways shelf of cards */
        float stride = CARD + CGAP;
        if (focused && fr == r) {
            float want = fc * stride - (SCR_W - MG * 2 - CARD) / 2;
            float maxs = fmaxf(0, row->n * stride - CGAP - (SCR_W - MG * 2));
            row->sx_t = clampf(want, 0, maxs);
        }
        if (in.dragging && touch_in(0, y - 10, SCR_W, CARD + 50) && fabsf(in.tx - in.sx) > fabsf(in.ty - in.sy)) {
            row->sx -= in.ddx;
            row->sx_t = row->sx;
        }
        row->sx_t = clampf(row->sx_t, 0, fmaxf(0, row->n * stride - CGAP - (SCR_W - MG * 2)));
        if (!in.touching) row->sx = approach(row->sx, row->sx_t, 12, dt);
        for (int i = 0; i < row->n; i++) {
            float x = MG + i * stride - row->sx;
            if (x > SCR_W || x + CARD < 0) continue;
            float f = focused && fr == r && fc == i ? 1 : 0;
            card(row->kind[i], row->idx[i], x, y + 8, CARD, f, 1);
            if (hit(x, y + 8, CARD, CARD + 36) && !in.dragging) { fr = r; fc = i; open_item(row->kind[i], row->idx[i]); input_consume_touch(); }
        }
        row->h = 22 + 8 + CARD + 58;
        y += row->h - 22;
    }
}

/* --- grids --- */
static int grid_n(void) { return mv == MV_PLAYLISTS ? M.np : mv == MV_ALBUMS ? M.na : M.nr; }
static int grid_kind(void) { return mv == MV_PLAYLISTS ? 2 : mv == MV_ALBUMS ? 0 : 1; }
#define GCOLS 6
static void draw_grid(int focused, float top) {
    float s = (SCR_W - MG * 2 - CGAP * (GCOLS - 1)) / GCOLS;
    float ch = s + 58;
    int n = grid_n(), kind = grid_kind();
    for (int i = 0; i < n; i++) {
        float x = MG + (i % GCOLS) * (s + CGAP), y = top + (i / GCOLS) * ch - scy;
        if (y > SCR_H || y + ch < top - 40) continue;
        float f = focused && fr == 0 && fc == i ? 1 : 0;
        card(kind, i, x, y, s, f, 1);
        if (hit(x, y, s, s + 36) && !in.dragging) { fr = 0; fc = i; open_item(kind, i); input_consume_touch(); }
        if (hit_long(x, y, s, s + 36)) { fr = 0; fc = i; item_menu(kind, i); input_consume_touch(); }
    }
    if (mv == MV_PLAYLISTS) {
        int i = n;
        float x = MG + (i % GCOLS) * (s + CGAP), y = top + (i / GCOLS) * ch - scy;
        float f = focused && fr == 0 && fc == i ? 1 : 0;
        gfx_rrect_outline(x, y, s, s, T->radius_sm, 1.5f, col_a(T->text, 0.25f + 0.5f * f));
        icon(IC_PLUS, x + s / 2, y + s / 2, 30, col_a(T->text, 0.7f + 0.3f * f));
        if (f) draw_focus(x, y, s, s, T->radius_sm, 1);
        text_ellipsis(F_SEMI, 13, x, y + s + 20, s, T->text2, "New playlist");
        if (hit(x, y, s, s + 36)) { menu_track = -1; ime_request("Name your playlist", "", 60, new_playlist_named, NULL); }
    }
}

/* --- song lists --- */
#define ROWH 54.0f
static void draw_songs(int focused, float top) {
    float y = top - scy;
    for (int i = 0; i < sn; i++, y += ROWH) {
        if (y > SCR_H || y + ROWH < top - 20) continue;
        float f = focused && fr == 0 && fc == i ? 1 : 0;
        song_row(slist[i], i + 1, MG, y, SCR_W - MG * 2, ROWH, f, 1, 1);
        if (hit(MG, y, SCR_W - MG * 2, ROWH) && !in.dragging) {
            fr = 0;
            fc = i;
            mus_play(slist, sn, i, FROM_SONGS, -1, "Your songs");
            input_consume_touch();
        }
        if (hit_long(MG, y, SCR_W - MG * 2, ROWH)) { fc = i; music_track_menu(slist[i], -1, -1); input_consume_touch(); }
    }
}

/* search results: artists, albums, then songs, as one list */
static int sr_count(void) { return n_art + n_alb + n_trk; }
static void sr_item(int i, int *kind, int *idx) {
    if (i < n_art) { *kind = 1; *idx = s_art[i]; }
    else if (i < n_art + n_alb) { *kind = 0; *idx = s_alb[i - n_art]; }
    else { *kind = 3; *idx = s_trk[i - n_art - n_alb]; }
}
static void sr_open(int i) {
    int k, ix;
    sr_item(i, &k, &ix);
    if (k == 3) mus_play(s_trk, n_trk, ix == s_trk[i - n_art - n_alb] ? i - n_art - n_alb : 0, FROM_SEARCH, -1, sq);
    else open_item(k, ix);
}
static void draw_search(int focused, float top) {
    float y = top - scy;
    /* the search box */
    float bw = SCR_W - MG * 2, bh = 44;
    float f0 = focused && fr == -2 ? 1 : 0;
    gfx_rrect(MG, y, bw, bh, bh / 2, col_a(T->text, 0.08f + 0.05f * f0));
    icon(IC_SEARCH, MG + 24, y + bh / 2, 17, T->text2);
    text_ellipsis(F_SEMI, 15, MG + 46, y + 28, bw - 70, sq[0] ? T->text : T->text3, sq[0] ? sq : "Songs, albums or artists");
    if (f0) draw_focus(MG, y, bw, bh, bh / 2, 1);
    if (hit(MG, y, bw, bh)) ime_request("Search music", sq, 60, search_typed, NULL);
    y += bh + 18;
    if (sq[0] && !sr_count()) {
        text_center(F_SEMI, 16, SCR_W / 2, y + 60, T->text, "Nothing found");
        text_center(F_REG, 13, SCR_W / 2, y + 84, T->text3, "Try fewer letters, or check the spelling.");
        return;
    }
    for (int i = 0; i < sr_count(); i++, y += ROWH) {
        if (y > SCR_H || y + ROWH < top - 20) continue;
        int k, ix;
        sr_item(i, &k, &ix);
        float f = focused && fr == 0 && fc == i ? 1 : 0;
        if (k == 3) song_row(ix, i, MG, y, SCR_W - MG * 2, ROWH, f, 1, 1);
        else {
            if (f) gfx_rrect(MG - 8, y, SCR_W - MG * 2 + 16, ROWH, T->radius_sm, col_a(T->text, 0.08f));
            if (k == 1) art_draw(M.r[ix].art, M.r[ix].name, MG, y + 7, 40, 40, 20, 1);
            else art_draw(M.a[ix].art, M.a[ix].name, MG, y + 7, 40, 40, 6, 1);
            text_ellipsis(F_SEMI, 15, MG + 54, y + 25, 600, T->text, k == 1 ? M.r[ix].name : M.a[ix].name);
            char sub[160];
            if (k == 1) snprintf(sub, sizeof(sub), "Artist");
            else snprintf(sub, sizeof(sub), "Album \xc2\xb7 %s", M.a[ix].artist);
            text_ellipsis(F_REG, 12, MG + 54, y + 42, 600, T->text3, sub);
            icon(IC_CHEVRON_R, SCR_W - MG - 8, y + ROWH / 2, 14, T->text3);
        }
        if (hit(MG, y, SCR_W - MG * 2, ROWH) && !in.dragging) { fr = 0; fc = i; sr_open(i); input_consume_touch(); }
    }
}

/* --- options on a focused album / artist / playlist --- */
static int im_kind, im_idx;
static void item_menu_cb(int c, void *ud) {
    (void)ud;
    if (c < 0) return;
    int *tr = NULL, n = 0;
    const char *label = "";
    int fk = FROM_ALBUM;
    if (im_kind == 0) { tr = M.a[im_idx].tracks; n = M.a[im_idx].nt; label = M.a[im_idx].name; fk = FROM_ALBUM; }
    else if (im_kind == 1) { tr = M.r[im_idx].tracks; n = M.r[im_idx].nt; label = M.r[im_idx].name; fk = FROM_ARTIST; }
    else { tr = M.p[im_idx].tracks; n = M.p[im_idx].nt; label = M.p[im_idx].name; fk = M.p[im_idx].liked_list ? FROM_LIKED : FROM_PLAYLIST; }
    if (c == 0) mus_play(tr, n, 0, fk, im_idx, label);
    else if (c == 1) mus_play_shuffled(tr, n, fk, im_idx, label);
    else if (c == 2) {
        for (int i = 0; i < n; i++) mus_queue_add(tr[i], 0);
        toast("Added to the queue");
    } else if (c == 3) open_item(im_kind, im_idx);
}
static void item_menu(int kind, int idx) {
    im_kind = kind;
    im_idx = idx;
    static const char *items[] = {"Play", "Shuffle play", "Add to queue", "Open"};
    const char *title = kind == 0 ? M.a[idx].name : kind == 1 ? M.r[idx].name : M.p[idx].name;
    modal_menu(title, items, 4, item_menu_cb, NULL);
}

static void rescan(void) {
    music_scan_start();
    toast("Looking for music\xe2\x80\xa6");
}

static void draw_empty(int focused, float top) {
    float cx = SCR_W / 2;
    gfx_circle(cx, top + 70, 44, col_a(T->text, 0.07f));
    icon(IC_NOTE, cx, top + 70, 40, T->text2);
    text_center(F_DISPLAY, T->display_font == 2 ? 20 : 24, cx, top + 150, T->text, M.scanning ? "Looking for your music\xe2\x80\xa6" : "No music yet");
    if (M.scanning) {
        char m[200];
        snprintf(m, sizeof(m), "%d songs found so far", M.scan_found);
        text_center(F_REG, 14, cx, top + 178, T->text2, m);
        draw_spinner(cx, top + 214, 12, T->text2);
        return;
    }
    text_center(F_REG, 14, cx, top + 180, T->text2, "Put MP3, FLAC, OGG, M4A or WAV files in ux0:music (or any folder");
    text_center(F_REG, 14, cx, top + 200, T->text2, "you've added to Nest). Albums and artists sort themselves out.");
    text_center(F_REG, 13, cx, top + 226, T->text3, "Got Spotify playlists? The Nest Music PC app turns them into playlists here.");
    float bw = 190, bx = cx - bw / 2, by = top + 250;
    draw_button(bx, by, bw, 44, IC_REFRESH, "Look again", 1, focused && fr >= 0 ? 1 : 0);
    if (hit(bx, by, bw, 44) || (focused && fr >= 0 && (in.pressed & B_OK))) rescan();
}

void music_tab_frame(float dt, int focused, int *want_nav) {
    *want_nav = 0;
    if (built_gen != M.gen) { /* the library changed under us */
        music_tab_enter();
        if (fr >= 0) { fr = -1; fc = mv; }
    }
    draw_scene_bg(S.ambient ? 1.3f : 1.0f);
    float top = 172;
    int bottom_pad = miniplayer_height();

    /* header */
    float hy = 112 - fminf(scy, 60) * 0.5f;
    int ts = T->display_font == 2 ? 26 : 32;
    text(F_DISPLAY, ts, MG, hy, T->text, "Music");
    char cnt[64];
    if (M.nt) snprintf(cnt, sizeof(cnt), "%d songs \xc2\xb7 %d albums", M.nt, M.na);
    else cnt[0] = 0;
    text(F_META, 13, MG + text_w(F_DISPLAY, ts, "Music") + 14, hy - 1, T->text3, cnt);
    if (M.scanning && M.nt) {
        draw_spinner(SCR_W - MG - 8, hy - 6, 7, T->text3);
        text_right(F_META, 11, SCR_W - MG - 22, hy - 2, T->text3, "updating");
    }
    /* view chips */
    float cx = MG, chip_y = hy + 18;
    for (int i = 0; i < MV_COUNT; i++) {
        float w;
        float f = focused && fr == -1 && fc == i ? 1 : 0;
        draw_chip(cx, chip_y, MV_NAMES[i], mv == i, f, &w);
        if (hit(cx, chip_y, w, 30)) {
            set_mv(i);
            if (i == MV_SEARCH && !sq[0]) ime_request("Search music", sq, 60, search_typed, NULL);
        }
        cx += w + 8;
    }
    if (mv == MV_SONGS) {
        static const char *sorts[3] = {"Recently added", "Title", "Artist"};
        float w = text_w(F_SEMI, 13, sorts[ssort]) + 28, x = SCR_W - MG - w;
        float f = focused && fr == -1 && fc == MV_COUNT ? 1 : 0;
        draw_chip(x, chip_y, sorts[ssort], 0, f, NULL);
        icon(IC_CHEVRON_D, x - 12, chip_y + 15, 12, T->text3);
        if (hit(x, chip_y, w, 30)) { ssort = (ssort + 1) % 3; build_songs(); }
    }
    top = chip_y + 52;

    /* content height for scrolling */
    float content_h = 0;
    int n_items = 0, cols = 1;
    if (!M.nt) {
        draw_empty(focused, top);
        if (focused && (in.repeat & B_UP)) *want_nav = 1;
        return;
    }
    if (mv == MV_HOME) {
        for (int r = 0; r < nhr; r++) content_h += hr[r].h ? hr[r].h : 220;
    } else if (mv == MV_SONGS) { n_items = sn; content_h = sn * ROWH; }
    else if (mv == MV_SEARCH) { n_items = sr_count(); content_h = 62 + n_items * ROWH; }
    else {
        cols = GCOLS;
        n_items = grid_n() + (mv == MV_PLAYLISTS ? 1 : 0);
        float s = (SCR_W - MG * 2 - CGAP * (GCOLS - 1)) / GCOLS;
        content_h = ((n_items + cols - 1) / cols) * (s + 58);
    }

    /* ---------- input ---------- */
    if (focused) {
        int moved = 0;
        if (fr == -1) {
            int maxc = MV_COUNT - 1 + (mv == MV_SONGS ? 1 : 0);
            if (in.repeat & B_LEFT && fc > 0) fc--, moved = 1;
            if (in.repeat & B_RIGHT && fc < maxc) fc++, moved = 1;
            if (in.pressed & B_OK) {
                if (fc == MV_COUNT) { ssort = (ssort + 1) % 3; build_songs(); }
                else {
                    set_mv(fc);
                    if (fc == MV_SEARCH) ime_request("Search music", sq, 60, search_typed, NULL);
                }
            }
            if (in.repeat & B_UP) *want_nav = 1;
            if (in.repeat & B_DOWN) {
                if (mv == MV_SEARCH) fr = sr_count() ? 0 : -2;
                else fr = 0;
                fc = 0;
                moved = 1;
            }
        } else if (mv == MV_HOME) {
            HRow *row = &hr[clampi(fr, 0, nhr - 1)];
            int cols_r = row->shortcuts ? 3 : 1000;
            if (in.repeat & B_LEFT && fc > 0 && (row->shortcuts ? fc % 3 : 1)) fc--, moved = 1;
            if (in.repeat & B_RIGHT && fc < row->n - 1 && (row->shortcuts ? fc % 3 < 2 : 1)) fc++, moved = 1;
            if (in.repeat & B_UP) {
                if (row->shortcuts && fc >= 3) fc -= 3;
                else if (fr > 0) { fr--; fc = 0; if (hr[fr].shortcuts) fc = (hr[fr].n - 1) / 3 * 3; }
                else { fr = -1; fc = mv; }
                moved = 1;
            }
            if (in.repeat & B_DOWN) {
                if (row->shortcuts && fc + 3 < row->n) fc += 3;
                else if (fr < nhr - 1) { fr++; fc = 0; }
                moved = 1;
            }
            (void)cols_r;
            if (fr >= 0 && fr < nhr && fc < hr[fr].n) {
                if (in.pressed & B_OK) open_item(hr[fr].kind[fc], hr[fr].idx[fc]);
                if (in.pressed & B_TRI) item_menu(hr[fr].kind[fc], hr[fr].idx[fc]);
            }
        } else if (mv == MV_SEARCH && fr == -2) {
            if (in.pressed & B_OK) ime_request("Search music", sq, 60, search_typed, NULL);
            if (in.repeat & B_UP) { fr = -1; fc = mv; moved = 1; }
            if ((in.repeat & B_DOWN) && sr_count()) { fr = 0; fc = 0; moved = 1; }
        } else {
            if (cols > 1) {
                if (in.repeat & B_LEFT && fc % cols > 0) fc--, moved = 1;
                if (in.repeat & B_RIGHT && fc % cols < cols - 1 && fc < n_items - 1) fc++, moved = 1;
            }
            if (in.repeat & B_UP) {
                if (fc >= cols) fc -= cols;
                else if (mv == MV_SEARCH) fr = -2;
                else { fr = -1; fc = mv; }
                moved = 1;
            }
            if (in.repeat & B_DOWN && fc + cols < n_items) fc += cols, moved = 1;
            else if (in.repeat & B_DOWN && cols > 1 && fc / cols < (n_items - 1) / cols) fc = n_items - 1, moved = 1;
            if (n_items > 0 && fc < n_items) {
                if (in.pressed & B_OK) {
                    if (mv == MV_SONGS) mus_play(slist, sn, fc, FROM_SONGS, -1, "Your songs");
                    else if (mv == MV_SEARCH) sr_open(fc);
                    else if (mv == MV_PLAYLISTS && fc == M.np) { menu_track = -1; ime_request("Name your playlist", "", 60, new_playlist_named, NULL); }
                    else open_item(grid_kind(), fc);
                }
                if (in.pressed & B_TRI) {
                    if (mv == MV_SONGS) music_track_menu(slist[fc], -1, -1);
                    else if (mv == MV_SEARCH) {
                        int k, ix;
                        sr_item(fc, &k, &ix);
                        if (k == 3) music_track_menu(ix, -1, -1);
                        else item_menu(k, ix);
                    } else if (!(mv == MV_PLAYLISTS && fc == M.np)) item_menu(grid_kind(), fc);
                }
            }
        }
        if (in.pressed & B_SQR) mus_toggle();
        if (in.pressed & B_BACK) {
            if (fr >= 0 || fr == -2) { fr = -1; fc = mv; scy_t = 0; moved = 1; }
            else *want_nav = 1;
        }
        if (moved) sfx(0);
        /* keep the focus on screen */
        if (moved && fr >= 0) {
            float fy = 0, fh = ROWH;
            if (mv == MV_HOME) {
                for (int r = 0; r < fr && r < nhr; r++) fy += hr[r].h;
                fh = fr < nhr ? hr[fr].h : 200;
                if (fr < nhr && hr[fr].shortcuts) { fy += 22 + (fc / 3) * 68; fh = 68; }
            } else if (cols > 1) {
                float s = (SCR_W - MG * 2 - CGAP * (GCOLS - 1)) / GCOLS;
                fy = (fc / cols) * (s + 58);
                fh = s + 58;
            } else {
                fy = (mv == MV_SEARCH ? 62 : 0) + fc * ROWH;
            }
            float view = SCR_H - top - bottom_pad - 10;
            if (fy - scy_t < 0) scy_t = fy - 10;
            if (fy + fh - scy_t > view) scy_t = fy + fh - view;
        } else if (moved && fr < 0) scy_t = 0;
    }
    if (in.dragging && touch_in(0, top - 20, SCR_W, SCR_H)) {
        if (fabsf(in.ty - in.sy) >= fabsf(in.tx - in.sx) || mv != MV_HOME) {
            scy -= in.ddy;
            scy_t = scy;
        }
    }
    if (in.released) scy_t = scy - in.vy * 0.28f;
    float maxs = fmaxf(0, content_h - (SCR_H - top - bottom_pad - 20));
    scy_t = clampf(scy_t, 0, maxs);
    if (!in.touching) scy = approach(scy, scy_t, 11, dt);
    fsel = approach(fsel, (float)fc, 18, dt);

    /* ---------- content ---------- */
    gfx_clip(0, top - 16, SCR_W, SCR_H - top + 16);
    int cf = focused && fr != -1;
    if (mv == MV_HOME) draw_home_rows(dt, cf, top);
    else if (mv == MV_SONGS) draw_songs(cf, top);
    else if (mv == MV_SEARCH) draw_search(focused, top);
    else draw_grid(cf, top);
    gfx_noclip();
    gfx_grad_v(0, top - 16, SCR_W, 16, T->bg, col_a(T->bg, 0));
}

/* ================================================================
   Album / artist / playlist pages
   ================================================================ */

static int pk, pidx;
static int pf;        /* 0 header buttons, 1 albums strip (artist), 2+ songs */
static int pbtn;      /* focused header button */
static int pal;       /* focused album in the strip */
static float psy, psy_t, pstrip, pstrip_t;
static int *pt, pn;   /* the songs */
static char ptitle[256], psub[300], plabel[32];

static void page_tracks(void) {
    pt = NULL;
    pn = 0;
    if (pk == FROM_ALBUM && pidx < M.na) { pt = M.a[pidx].tracks; pn = M.a[pidx].nt; }
    else if (pk == FROM_ARTIST && pidx < M.nr) { pt = M.r[pidx].tracks; pn = M.r[pidx].nt; }
    else if (pidx < M.np) { pt = M.p[pidx].tracks; pn = M.p[pidx].nt; }
}

void mpage_open(int kind, int idx) {
    if (idx < 0) return;
    if (kind == FROM_ALBUM && idx >= M.na) return;
    if (kind == FROM_ARTIST && idx >= M.nr) return;
    if ((kind == FROM_PLAYLIST || kind == FROM_LIKED) && idx >= M.np) return;
    pk = kind;
    pidx = idx;
    pf = 0;
    pbtn = 0;
    pal = 0;
    psy = psy_t = 0;
    pstrip = pstrip_t = 0;
    page_tracks();
    if (nav_top() == SCR_MPAGE) nav_replace(SCR_MPAGE);
    else nav_push(SCR_MPAGE);
}

static void page_play(int start, int shuffled) {
    page_tracks();
    if (!pn) return;
    const char *label = pk == FROM_ALBUM ? M.a[pidx].name : pk == FROM_ARTIST ? M.r[pidx].name : M.p[pidx].name;
    if (shuffled) mus_play_shuffled(pt, pn, pk, pidx, label);
    else mus_play(pt, pn, start, pk, pidx, label);
}

static void pl_renamed(const char *name, void *ud) {
    (void)ud;
    if (name && name[0]) music_playlist_rename(pidx, name);
}
static void pl_delete_cb(int c, void *ud) {
    (void)ud;
    if (c != 1) return;
    music_playlist_delete(pidx);
    toast("Playlist deleted");
    nav_pop();
}
static void page_menu_cb(int c, void *ud) {
    (void)ud;
    if (c < 0) return;
    page_tracks();
    if (c == 0) {
        for (int i = 0; i < pn; i++) mus_queue_add(pt[i], 0);
        toast("Added to the queue");
    } else if (c == 1 && pk == FROM_PLAYLIST && !(M.p[pidx].source && M.p[pidx].source[0])) {
        ime_request("Rename playlist", M.p[pidx].name, 60, pl_renamed, NULL);
    } else if ((c == 2 || (c == 1 && M.p[pidx].source && M.p[pidx].source[0])) && pk == FROM_PLAYLIST) {
        modal_confirm("Delete this playlist?", M.p[pidx].source && M.p[pidx].source[0]
                          ? "It came from a playlist file. Nest will hide it; the file and songs stay on the card."
                          : "The songs stay in your library.",
                      "Delete", "Keep", pl_delete_cb, NULL);
    } else if (c == 1 && pk == FROM_ALBUM) {
        mpage_open(FROM_ARTIST, M.a[pidx].artist_i);
    }
}
static void page_menu(void) {
    static const char *pl_items[] = {"Add all to queue", "Rename", "Delete playlist"};
    static const char *pl_file_items[] = {"Add all to queue", "Delete playlist"};
    static const char *al_items[] = {"Add all to queue", "Go to artist"};
    static const char *other[] = {"Add all to queue"};
    if (pk == FROM_PLAYLIST) {
        if (M.p[pidx].source && M.p[pidx].source[0]) modal_menu(M.p[pidx].name, pl_file_items, 2, page_menu_cb, NULL);
        else modal_menu(M.p[pidx].name, pl_items, 3, page_menu_cb, NULL);
    } else if (pk == FROM_ALBUM) modal_menu(M.a[pidx].name, al_items, 2, page_menu_cb, NULL);
    else modal_menu(pk == FROM_ARTIST ? M.r[pidx].name : M.p[pidx].name, other, 1, page_menu_cb, NULL);
}

void mpage_frame(float dt) {
    if ((pk == FROM_ALBUM && pidx >= M.na) || (pk == FROM_ARTIST && pidx >= M.nr) ||
        ((pk == FROM_PLAYLIST || pk == FROM_LIKED) && pidx >= M.np)) {
        gfx_rect(0, 0, SCR_W, SCR_H, T->bg);
        nav_pop();
        return;
    }
    page_tracks();
    if (pf - 2 >= pn) pf = pn ? 2 + pn - 1 : 0;
    const char *art = NULL;
    const char *seed = "";
    int total_ms = 0;
    if (pk == FROM_ALBUM) {
        MAlbum *a = &M.a[pidx];
        art = a->art;
        seed = a->name;
        snprintf(plabel, sizeof(plabel), "ALBUM");
        snprintf(ptitle, sizeof(ptitle), "%s", a->name);
        total_ms = a->dur;
        char d[32];
        fmt_long(total_ms, d, sizeof(d));
        if (a->year) snprintf(psub, sizeof(psub), "%s \xc2\xb7 %d \xc2\xb7 %d song%s, %s", a->artist, a->year, a->nt, a->nt == 1 ? "" : "s", d);
        else snprintf(psub, sizeof(psub), "%s \xc2\xb7 %d song%s, %s", a->artist, a->nt, a->nt == 1 ? "" : "s", d);
    } else if (pk == FROM_ARTIST) {
        MArtist *r = &M.r[pidx];
        art = r->art;
        seed = r->name;
        snprintf(plabel, sizeof(plabel), "ARTIST");
        snprintf(ptitle, sizeof(ptitle), "%s", r->name);
        snprintf(psub, sizeof(psub), "%d album%s \xc2\xb7 %d song%s", r->nal, r->nal == 1 ? "" : "s", r->nt, r->nt == 1 ? "" : "s");
    } else {
        MPlaylist *p = &M.p[pidx];
        art = p->cover;
        seed = p->name;
        snprintf(plabel, sizeof(plabel), "PLAYLIST");
        snprintf(ptitle, sizeof(ptitle), "%s", p->name);
        char d[32];
        fmt_long(p->dur, d, sizeof(d));
        if (p->missing) snprintf(psub, sizeof(psub), "%d songs, %s \xc2\xb7 %d not on this card", p->nt, d, p->missing);
        else snprintf(psub, sizeof(psub), "%d song%s, %s", p->nt, p->nt == 1 ? "" : "s", d);
    }
    int is_artist = pk == FROM_ARTIST;
    int strip = is_artist && M.r[pidx].nal > 1;

    /* background: the cover, blurred, fading into the theme */
    gfx_rect(0, 0, SCR_W, SCR_H, T->bg);
    const char *bg_art = art;
    if (!bg_art && pk != FROM_ARTIST && pn) bg_art = M.t[pt[0]].art;
    if (bg_art) {
        art_blur(bg_art, 0, -psy * 0.4f, SCR_W, 340, 0.55f);
        gfx_grad_v(0, -psy * 0.4f, SCR_W, 342, col_a(T->bg, 0.35f), T->bg);
    } else {
        gfx_grad_v(0, 0, SCR_W, 300, col_a(T->accent, 0.18f), col_a(T->bg, 0));
    }

    /* ---------- input ---------- */
    int bottom_pad = miniplayer_height();
    float head_h = 296, strip_h = strip ? CARD + 96 : 0;
    float list_top = head_h + strip_h;
    int moved = 0;
    if (in.pressed & B_BACK) { sfx(2); nav_pop(); return; }
    if (pf == 0) {
        if (in.repeat & B_LEFT && pbtn > 0) pbtn--, moved = 1;
        if (in.repeat & B_RIGHT && pbtn < 2) pbtn++, moved = 1;
        if (in.repeat & B_DOWN) { pf = strip ? 1 : pn ? 2 : 0; moved = 1; }
        if (in.pressed & B_OK) {
            if (pbtn == 0) page_play(0, 0);
            else if (pbtn == 1) page_play(0, 1);
            else page_menu();
        }
    } else if (pf == 1) {
        int nal = M.r[pidx].nal;
        if (in.repeat & B_LEFT && pal > 0) pal--, moved = 1;
        if (in.repeat & B_RIGHT && pal < nal - 1) pal++, moved = 1;
        if (in.repeat & B_UP) pf = 0, moved = 1;
        if (in.repeat & B_DOWN && pn) pf = 2, moved = 1;
        if (in.pressed & B_OK) mpage_open(FROM_ALBUM, M.r[pidx].albums[pal]);
        if (in.pressed & B_TRI) item_menu(0, M.r[pidx].albums[pal]);
    } else {
        int i = pf - 2;
        if (in.repeat & B_UP) { pf = i > 0 ? pf - 1 : strip ? 1 : 0; moved = 1; }
        if (in.repeat & B_DOWN && i < pn - 1) pf++, moved = 1;
        if (in.repeat & B_L) { pf = 2 + clampi(i - 8, 0, pn - 1); moved = 1; }
        if (in.repeat & B_R) { pf = 2 + clampi(i + 8, 0, pn - 1); moved = 1; }
        if (in.pressed & B_OK && i < pn) page_play(i, 0);
        if (in.pressed & B_TRI && i < pn)
            music_track_menu(pt[i], pk == FROM_PLAYLIST || pk == FROM_LIKED ? pidx : -1, i);
    }
    if (in.pressed & B_SQR) mus_toggle();
    if (in.pressed & B_START) page_menu();
    if (in.pressed & B_SELECT && mus_active()) nowplaying_open();
    if (moved) {
        sfx(0);
        if (pf <= 1) psy_t = 0;
        else {
            float fy = list_top + (pf - 2) * ROWH;
            float view = SCR_H - bottom_pad - 20;
            if (fy - psy_t < 90) psy_t = fy - 90;
            if (fy + ROWH - psy_t > view) psy_t = fy + ROWH - view;
        }
    }
    if (in.dragging && !(strip && touch_in(0, head_h - psy, SCR_W, strip_h))) {
        psy -= in.ddy;
        psy_t = psy;
    }
    if (in.released) psy_t = psy - in.vy * 0.28f;
    psy_t = clampf(psy_t, 0, fmaxf(0, list_top + pn * ROWH - (SCR_H - bottom_pad - 30)));
    if (!in.touching) psy = approach(psy, psy_t, 11, dt);

    /* ---------- header ---------- */
    float y0 = 64 - psy;
    float as = 150;
    if (is_artist) art_draw(art, seed, MG, y0, as, as, as / 2, 1);
    else if (pk == FROM_PLAYLIST || pk == FROM_LIKED) playlist_cover(&M.p[pidx], MG, y0, as, T->radius_sm, 1);
    else {
        gfx_shadow(MG, y0 + 10, as, as, T->radius_sm, 24, col_a(0xFF000000, 0.5f));
        art_draw(art, seed, MG, y0, as, as, T->radius_sm, 1);
    }
    float tx = MG + as + 28;
    text(F_META, 11, tx, y0 + 24, T->text3, plabel);
    int fs = T->display_font == 2 ? 26 : 34;
    if (text_w(F_DISPLAY, fs, ptitle) > SCR_W - tx - MG) fs = fs * 3 / 4;
    text_ellipsis(F_DISPLAY, fs, tx, y0 + 24 + fs + 10, SCR_W - tx - MG, T->text, ptitle);
    text_ellipsis(F_REG, 14, tx, y0 + 24 + fs + 36, SCR_W - tx - MG, T->text2, psub);
    if (pk == FROM_PLAYLIST && M.p[pidx].desc && M.p[pidx].desc[0])
        text_ellipsis(F_REG, 13, tx, y0 + 24 + fs + 58, SCR_W - tx - MG, T->text3, M.p[pidx].desc);
    /* buttons */
    float by = y0 + as + 20; /* under the cover, never over the text */
    float bxs[3] = {MG, MG + 132, MG + 264};
    draw_button(bxs[0], by, 120, 40, IC_PLAY, "Play", 1, pf == 0 && pbtn == 0 ? 1 : 0);
    draw_button(bxs[1], by, 120, 40, IC_SHUFFLE, "Shuffle", 0, pf == 0 && pbtn == 1 ? 1 : 0);
    draw_button(bxs[2], by, 44, 40, IC_DOTS, "", 0, pf == 0 && pbtn == 2 ? 1 : 0);
    if (hit(bxs[0], by, 120, 40)) page_play(0, 0);
    if (hit(bxs[1], by, 120, 40)) page_play(0, 1);
    if (hit(bxs[2], by, 44, 40)) page_menu();
    if (pn == 0) {
        text_center(F_SEMI, 15, SCR_W / 2, head_h + 60 - psy, T->text2,
                    pk == FROM_LIKED ? "Heart a song (TRIANGLE \xe2\x80\xba Add to Liked Songs) and it shows up here." : "No songs here yet.");
    }

    /* ---------- artist: albums strip ---------- */
    if (strip) {
        MArtist *r = &M.r[pidx];
        float sy0 = head_h - psy;
        text(F_BOLD, 18, MG, sy0 + 6, T->text, "Albums");
        float stride = CARD + CGAP;
        if (pf == 1) pstrip_t = clampf(pal * stride - (SCR_W - MG * 2 - CARD) / 2, 0, fmaxf(0, r->nal * stride - CGAP - (SCR_W - MG * 2)));
        if (in.dragging && touch_in(0, sy0, SCR_W, strip_h)) { pstrip -= in.ddx; pstrip_t = pstrip; }
        pstrip_t = clampf(pstrip_t, 0, fmaxf(0, r->nal * stride - CGAP - (SCR_W - MG * 2)));
        if (!in.touching) pstrip = approach(pstrip, pstrip_t, 12, dt);
        for (int i = 0; i < r->nal; i++) {
            float x = MG + i * stride - pstrip;
            if (x > SCR_W || x + CARD < 0) continue;
            card(0, r->albums[i], x, sy0 + 22, CARD, pf == 1 && pal == i ? 1 : 0, 1);
            if (hit(x, sy0 + 22, CARD, CARD + 36) && !in.dragging) mpage_open(FROM_ALBUM, r->albums[i]);
        }
    }

    /* ---------- songs ---------- */
    float ly = list_top - psy;
    if (pn) {
        text(F_BOLD, 18, MG, ly + 4, T->text, is_artist ? "Songs" : "");
        if (is_artist) ly += 18;
    }
    gfx_clip(0, 0, SCR_W, SCR_H - bottom_pad);
    for (int i = 0; i < pn; i++) {
        float y = ly + i * ROWH;
        if (y > SCR_H || y + ROWH < 0) continue;
        int ti = pt[i];
        int disc_head = pk == FROM_ALBUM && M.t[ti].disc > 1 && (i == 0 || M.t[pt[i - 1]].disc != M.t[ti].disc);
        (void)disc_head;
        song_row(ti, pk == FROM_ALBUM ? i + 1 : i + 1, MG, y, SCR_W - MG * 2, ROWH, pf == 2 + i ? 1 : 0,
                 pk != FROM_ALBUM, pk != FROM_ALBUM);
        if (hit(MG, y, SCR_W - MG * 2, ROWH) && !in.dragging) { pf = 2 + i; page_play(i, 0); }
        if (hit_long(MG, y, SCR_W - MG * 2, ROWH)) {
            pf = 2 + i;
            music_track_menu(ti, pk == FROM_PLAYLIST || pk == FROM_LIKED ? pidx : -1, i);
            input_consume_touch();
        }
    }
    gfx_noclip();

    /* back */
    gfx_grad_v(0, 0, SCR_W, 60, col_a(T->bg, clampf(psy / 120.0f, 0, 0.9f)), col_a(T->bg, 0));
    icon(IC_CHEVRON_L, 24, 30, 20, T->text);
    if (psy > 140) text_ellipsis(F_SEMI, 15, 46, 36, 600, col_a(T->text, clampf((psy - 140) / 60.0f, 0, 1)), ptitle);
    if (hit(0, 0, 70, 60)) { nav_pop(); return; }
    if (!bottom_pad && !modal_active()) {
        static const char *h[] = {"OK", "Play", "TRI", "Options", "SQR", "Pause / play"};
        draw_hint_bar(h, 6);
    }
}

/* ================================================================
   Now Playing
   ================================================================ */

static int nf = 1;      /* 0 seek bar, 1 main controls, 2 extras */
static int nb = 2;      /* focused button in the row */
static int queue_open;
static int qf;          /* queue focus */
static float qsy, qsy_t, qa;
static int seek_drag, vol_drag;
static float seek_drag_p;
static int qbuf[8192];
static int eq_open, eq_f, eq_band_i, eq_drag = -1;
static float eq_a, eq_psx, eq_psx_t;

/* ---------------- equaliser ---------------- */

static void eq_changed(void) { eq_save(); }

static void draw_eq(float dt, float a) {
    float w = 860, h = 430, x = (SCR_W - w) / 2, y = (SCR_H - h) / 2 + (1 - a) * 20;
    gfx_shadow(x, y + 10, w, h, T->radius, 40, col_a(0xFF000000, 0.6f * a));
    gfx_rrect(x, y, w, h, T->radius, col_a(col_mix(T->bg, 0xFF000000, 0.3f), 0.98f * a));
    gfx_rrect_outline(x, y, w, h, T->radius, 1, col_a(0xFFFFFFFF, 0.08f * a));
    text(F_BOLD, 21, x + 28, y + 42, col_a(T->text, a), "Sound");
    int on = eq_enabled(), loud = eq_loud();
    /* switches */
    {
        float sx2 = x + w - 28;
        const char *l2 = loud ? "Loud mode: on" : "Loud mode: off";
        const char *l1 = on ? "Equaliser: on" : "Equaliser: off";
        float w2 = text_w(F_SEMI, 13, l2) + 28, w1 = text_w(F_SEMI, 13, l1) + 28;
        float x2 = sx2 - w2, x1 = x2 - 10 - w1, cy0 = y + 20;
        draw_chip(x1, cy0, l1, on, eq_f == 0 ? 1 : 0, NULL);
        draw_chip(x2, cy0, l2, loud, eq_f == 0 ? 0.5f : 0, NULL);
        if (hit(x1, cy0, w1, 30)) { eq_enable(!on); eq_changed(); }
        if (hit(x2, cy0, w2, 30)) { eq_set_loud(!loud); eq_changed(); toast(!loud ? "Loud mode: quiet songs come up, peaks stay clean" : "Loud mode off"); }
    }
    /* presets */
    int cur = on ? eq_matching_preset() : -1;
    float py = y + 66;
    text(F_META, 11, x + 28, py + 4, col_a(T->text3, a), "PRESETS");
    float px = x + 28 - eq_psx;
    gfx_clip(x + 16, py + 10, w - 32, 44);
    float cxs[EQ_PRESET_COUNT], cws[EQ_PRESET_COUNT];
    for (int i = 0; i < EQ_PRESET_COUNT; i++) {
        float cw = text_w(F_SEMI, 13, EQ_PRESETS[i].name) + 28;
        cxs[i] = px;
        cws[i] = cw;
        draw_chip(px, py + 16, EQ_PRESETS[i].name, cur == i, eq_f == 1 && eq_preset() == i && cur != i ? 0.6f : 0, NULL);
        if (hit(px, py + 16, cw, 30) && !in.dragging) { eq_apply_preset(i); eq_changed(); }
        px += cw + 8;
    }
    gfx_noclip();
    {
        int fp = eq_preset();
        float right = cxs[fp] + eq_psx + cws[fp];
        if (right - eq_psx_t > w - 56) eq_psx_t = right - (w - 56);
        if (cxs[fp] + eq_psx - eq_psx_t < x + 28) eq_psx_t = cxs[fp] + eq_psx - x - 28;
        if (eq_psx_t < 0) eq_psx_t = 0;
        if (in.dragging && touch_in(x, py + 10, w, 44)) { eq_psx -= in.ddx; eq_psx_t = eq_psx; }
        eq_psx = in.touching ? eq_psx : approach(eq_psx, eq_psx_t, 14, dt);
    }
    if (cur < 0 && on) text_right(F_META, 11, x + w - 28, py + 4, col_a(T->accent, a), "CUSTOM");
    /* sliders */
    float top = y + 140, sh = 200, colw = (w - 56) / EQ_BANDS;
    gfx_rect(x + 28, top + sh / 2, w - 56, 1, col_a(0xFFFFFFFF, 0.12f * a)); /* 0 dB */
    for (int b = 0; b < EQ_BANDS; b++) {
        float cx = x + 28 + colw * (b + 0.5f);
        float g = eq_band(b);
        float k = (g + 12) / 24;
        float f = eq_f == 2 && eq_band_i == b ? 1 : 0;
        gfx_rrect(cx - 3, top, 6, sh, 3, col_a(0xFFFFFFFF, 0.14f * a));
        float ky = top + sh * (1 - k), mid = top + sh / 2;
        col_t fill = on ? T->accent : col_a(0xFFFFFFFF, 0.35f);
        if (ky < mid) gfx_rrect(cx - 3, ky, 6, mid - ky, 3, col_a(fill, a));
        else gfx_rrect(cx - 3, mid, 6, ky - mid, 3, col_a(fill, a));
        gfx_circle(cx, ky, f || eq_drag == b ? 11 : 9, col_a(0xFFFFFFFF, a));
        if (f) gfx_ring(cx, ky, 15, 2, col_a(T->accent, a));
        char dbs[16];
        snprintf(dbs, sizeof(dbs), "%+.0f dB", g);
        text_center(F_META, 11, cx, top - 12, col_a(f ? T->text : T->text3, a), dbs);
        text_center(F_SEMI, 13, cx, top + sh + 24, col_a(T->text2, a), EQ_LABEL[b]);
        if (in.touching && (eq_drag == b || (eq_drag < 0 && fabsf(in.sx - cx) < colw / 2 && in.sy > top - 20 && in.sy < top + sh + 20))) {
            eq_drag = b;
            eq_band_i = b;
            eq_f = 2;
            float ng = roundf((1 - clampf((in.ty - top) / sh, 0, 1)) * 24 - 12);
            if (ng != g) { eq_set_band(b, ng); if (!on) eq_enable(1); }
        }
    }
    if (eq_drag >= 0 && !in.touching) { eq_drag = -1; eq_changed(); }
    text(F_META, 10, x + 28, top + sh + 24, col_a(T->text3, a), "Hz");
    text_center(F_META, 11, x + w / 2, y + h - 18, col_a(T->text3, a),
                "Left / Right pick  -  Up / Down change  -  L / R presets  -  SQUARE equaliser on/off  -  TRIANGLE loud mode");
    /* keys */
    if (in.repeat & B_LEFT) { if (eq_f == 2 && eq_band_i > 0) eq_band_i--; else if (eq_f == 1) { eq_apply_preset((eq_preset() + EQ_PRESET_COUNT - 1) % EQ_PRESET_COUNT); eq_changed(); } sfx(0); }
    if (in.repeat & B_RIGHT) { if (eq_f == 2 && eq_band_i < EQ_BANDS - 1) eq_band_i++; else if (eq_f == 1) { eq_apply_preset((eq_preset() + 1) % EQ_PRESET_COUNT); eq_changed(); } sfx(0); }
    if (in.repeat & B_UP) {
        if (eq_f == 2) { eq_set_band(eq_band_i, eq_band(eq_band_i) + 1); if (!on) eq_enable(1); eq_changed(); }
        else if (eq_f > 0) eq_f--;
        sfx(0);
    }
    if (in.repeat & B_DOWN) {
        if (eq_f == 2) { eq_set_band(eq_band_i, eq_band(eq_band_i) - 1); if (!on) eq_enable(1); eq_changed(); }
        else eq_f++;
        sfx(0);
    }
    if (in.pressed & B_OK) {
        if (eq_f == 0) { eq_enable(!on); eq_changed(); }
        else if (eq_f == 1) { eq_apply_preset(eq_preset()); eq_changed(); eq_f = 2; }
        else eq_f = 1;
    }
    if (in.pressed & B_L) { eq_apply_preset((eq_preset() + EQ_PRESET_COUNT - 1) % EQ_PRESET_COUNT); eq_changed(); }
    if (in.pressed & B_R) { eq_apply_preset((eq_preset() + 1) % EQ_PRESET_COUNT); eq_changed(); }
    if (in.pressed & B_SQR) { eq_enable(!on); eq_changed(); toast(!on ? "Equaliser on" : "Equaliser off"); }
    if (in.pressed & B_TRI) { eq_set_loud(!loud); eq_changed(); toast(!loud ? "Loud mode on" : "Loud mode off"); }
    if (in.tap && !touch_in(x, y, w, h)) eq_open = 0;
}

void nowplaying_open(void) {
    nf = 1;
    nb = 2;
    queue_open = 0;
    if (nav_top() != SCR_NOWPLAYING) nav_push(SCR_NOWPLAYING);
}

static void sleep_cb(int c, void *ud) {
    (void)ud;
    static const int mins[] = {0, 15, 30, 45, 60, 90, -1};
    if (c < 0 || c > 6) return;
    mus_sleep_timer(mins[c]);
    toast(c == 0 ? "Sleep timer off" : c == 6 ? "Music stops after this song" : "Sleep timer set");
}
static void sleep_menu(void) {
    static const char *items[] = {"Off", "15 minutes", "30 minutes", "45 minutes", "1 hour", "1 hour 30", "End of this song"};
    modal_menu("Sleep timer", items, 7, sleep_cb, NULL);
}

static void np_action(int row, int b) {
    int ti = mus_current();
    if (row == 1) {
        switch (b) {
        case 0: mus_set_shuffle(!mus_shuffle()); toast(mus_shuffle() ? "Shuffle on" : "Shuffle off"); break;
        case 1: mus_prev(); break;
        case 2: mus_toggle(); break;
        case 3: mus_next(); break;
        case 4: {
            mus_cycle_repeat();
            static const char *m[] = {"Repeat off", "Repeat all", "Repeat this song"};
            toast(m[mus_repeat()]);
            break;
        }
        }
    } else if (row == 2) {
        switch (b) {
        case 0:
            if (ti >= 0) {
                music_set_liked(ti, !M.t[ti].liked);
                toast(M.t[ti].liked ? "Added to Liked Songs" : "Removed from Liked Songs");
            }
            break;
        case 1: if (ti >= 0) { menu_track = ti; pick_playlist(); } break;
        case 2: queue_open = !queue_open; qf = 0; break;
        case 3: eq_open = 1; eq_f = 1; break;
        case 4: sleep_menu(); break;
        case 5: mus_screen_off(); break;
        }
    }
}

static void draw_queue(float dt, float a) {
    int cur = 0;
    int n = mus_queue(qbuf, 8192, &cur);
    float x = SCR_W - 440, w = 420, y0 = 20, h = SCR_H - 40;
    gfx_shadow(x, y0 + 8, w, h, T->radius, 30, col_a(0xFF000000, 0.55f * a));
    gfx_rrect(x, y0, w, h, T->radius, col_a(col_mix(T->bg, 0xFF000000, 0.35f), 0.97f * a));
    text(F_BOLD, 19, x + 20, y0 + 34, col_a(T->text, a), "Queue");
    char sub[64];
    snprintf(sub, sizeof(sub), "%d song%s up next", n - cur - 1 > 0 ? n - cur - 1 : 0, n - cur - 1 == 1 ? "" : "s");
    text_right(F_META, 12, x + w - 20, y0 + 32, col_a(T->text3, a), sub);
    float ly = y0 + 52;
    float rh = 50;
    int first = cur;
    int count = n - first;
    if (qf >= count) qf = count - 1;
    if (qf < 0) qf = 0;
    float view = h - 60;
    float fy = qf * rh;
    if (fy - qsy_t < 0) qsy_t = fy;
    if (fy + rh - qsy_t > view) qsy_t = fy + rh - view;
    if (in.dragging && touch_in(x, y0, w, h)) { qsy -= in.ddy; qsy_t = qsy; }
    qsy_t = clampf(qsy_t, 0, fmaxf(0, count * rh - view));
    if (!in.touching) qsy = approach(qsy, qsy_t, 12, dt);
    gfx_clip(x, ly, w, view);
    for (int i = 0; i < count; i++) {
        float y = ly + i * rh - qsy;
        if (y > y0 + h || y + rh < ly) continue;
        int ti = qbuf[first + i];
        if (ti < 0 || ti >= M.nt) continue;
        MTrack *t = &M.t[ti];
        if (i == qf) gfx_rrect(x + 10, y, w - 20, rh - 4, T->radius_sm, col_a(T->text, 0.1f * a));
        if (i == 1) text(F_META, 10, x + 20, y - 2, col_a(T->text3, a), "NEXT UP");
        art_draw(t->art, t->album, x + 20, y + 6, 36, 36, 5, a);
        text_ellipsis(F_SEMI, 14, x + 66, y + 22, w - 100, col_a(i == 0 ? T->accent : T->text, a), t->title);
        text_ellipsis(F_REG, 12, x + 66, y + 39, w - 100, col_a(T->text3, a), t->artist);
        if (hit(x + 10, y, w - 20, rh - 4) && i > 0) { mus_queue_jump(first + i); qf = 0; }
    }
    gfx_noclip();
    /* keys while the queue is open */
    if (in.repeat & B_UP && qf > 0) qf--, sfx(0);
    if (in.repeat & B_DOWN && qf < count - 1) qf++, sfx(0);
    if (in.pressed & B_OK && qf > 0) { mus_queue_jump(first + qf); qf = 0; }
    if (in.pressed & B_SQR && qf > 0) { mus_queue_remove(first + qf); toast("Removed from the queue"); }
    if (in.pressed & B_L && qf > 1) { mus_queue_move(first + qf, first + qf - 1); qf--; }
    if (in.pressed & B_R && qf > 0 && qf < count - 1) { mus_queue_move(first + qf, first + qf + 1); qf++; }
    static const char *hh[] = {"OK", "Play", "SQR", "Remove", "L", "Up", "R", "Down"};
    (void)hh;
    text(F_META, 11, x + 20, y0 + h - 12, col_a(T->text3, a), "OK play \xc2\xb7 SQUARE remove \xc2\xb7 L / R move");
}

void nowplaying_frame(float dt) {
    int ti = mus_current();
    if (ti < 0 || ti >= M.nt) {
        gfx_rect(0, 0, SCR_W, SCR_H, T->bg);
        text_center(F_SEMI, 16, SCR_W / 2, SCR_H / 2, T->text2, "Nothing playing");
        if (in.pressed & (B_BACK | B_OK) || in.tap) nav_pop();
        return;
    }
    MTrack *t = &M.t[ti];
    /* background */
    gfx_rect(0, 0, SCR_W, SCR_H, col_mix(T->bg, 0xFF000000, 0.4f));
    art_blur(t->art, -40, -40, SCR_W + 80, SCR_H + 80, 0.7f);
    gfx_grad_v(0, 0, SCR_W, SCR_H, col_a(0xFF000000, 0.35f), col_a(0xFF000000, 0.72f));

    qa = approach(qa, queue_open ? 1 : 0, 12, dt);

    /* ---------- input ---------- */
    eq_a = approach(eq_a, eq_open ? 1 : 0, 14, dt);
    Input keep = in;
    if (eq_open) { /* the sound panel has the buttons */
        if (in.pressed & B_BACK) { eq_open = 0; sfx(2); }
        in.pressed = in.repeat = 0;
        in.tap = in.long_press = in.double_tap = 0;
        in.touching = 0;
        in.dragging = 0;
    }
    if (in.pressed & B_BACK) {
        if (queue_open) queue_open = 0;
        else { sfx(2); nav_pop(); return; }
        in.pressed &= ~B_BACK;
    }
    if (in.pressed & B_TRI) { queue_open = !queue_open; qf = 0; }
    if (in.pressed & B_SELECT) mus_screen_off();
    if (in.pressed & B_START) music_track_menu(ti, -1, -1);
    if (!queue_open) {
        int moved = 0;
        int maxb = nf == 1 ? 4 : 5;
        if (in.repeat & B_UP && nf > 0) { nf--; moved = 1; if (nf == 1) nb = 2; else if (nf == 2) nb = 0; }
        if (in.repeat & B_DOWN && nf < 3) { nf++; moved = 1; nb = nf == 1 ? 2 : 0; }
        if (nf == 0) {
            if (in.repeat & B_LEFT) mus_seek(mus_pos_ms() - 5000);
            if (in.repeat & B_RIGHT) mus_seek(mus_pos_ms() + 5000);
            if (in.pressed & B_OK) mus_toggle();
        } else if (nf == 3) {
            if (in.repeat & B_LEFT) { mus_set_volume(mus_volume() - 5); db_seti("music_vol", mus_volume()); }
            if (in.repeat & B_RIGHT) { mus_set_volume(mus_volume() + 5); db_seti("music_vol", mus_volume()); }
            if (in.pressed & B_OK) { eq_open = 1; eq_f = 1; }
        } else {
            if (in.repeat & B_LEFT && nb > 0) nb--, moved = 1;
            if (in.repeat & B_RIGHT && nb < maxb) nb++, moved = 1;
            if (in.pressed & B_OK) np_action(nf, nb);
        }
        if (in.pressed & B_SQR) mus_toggle();
        if (in.pressed & B_L) mus_prev();
        if (in.pressed & B_R) mus_next();
        if (moved) sfx(0);
    }

    /* ---------- cover ---------- */
    float as = 300, ax = 64, ay = 104;
    float bump = 1 + 0.012f * mus_level() * (S.reduce_motion ? 0 : 1);
    float aw = as * bump;
    gfx_shadow(ax, ay + 14, as, as, T->radius, 40, col_a(0xFF000000, 0.6f));
    art_draw(t->art, t->album, ax + (as - aw) / 2, ay + (as - aw) / 2, aw, aw, T->radius, 1);
    if (hit(ax, ay, as, as)) mus_toggle();

    /* top bar */
    icon(IC_CHEVRON_D, 34, 34, 20, 0xFFFFFFFF);
    if (hit(0, 0, 80, 64)) { nav_pop(); return; }
    {
        static const char *fk[] = {"", "ALBUM", "ARTIST", "PLAYLIST", "YOUR SONGS", "SEARCH", "PLAYLIST"};
        int k = mus_from_kind();
        char lab[64];
        snprintf(lab, sizeof(lab), "PLAYING FROM %s", k > 0 && k < 7 ? fk[k] : "YOUR LIBRARY");
        text_center(F_META, 11, SCR_W / 2, 30, col_a(0xFFFFFFFF, 0.6f), lab);
        text_center(F_SEMI, 14, SCR_W / 2, 50, 0xFFFFFFFF, mus_from_label()[0] ? mus_from_label() : t->album);
        float lw = text_w(F_SEMI, 14, mus_from_label()[0] ? mus_from_label() : t->album);
        if (hit(SCR_W / 2 - lw / 2 - 10, 14, lw + 20, 48)) {
            int fki = mus_from_kind(), fi = mus_from_idx();
            if (fki == FROM_ALBUM || fki == FROM_ARTIST || fki == FROM_PLAYLIST || fki == FROM_LIKED) mpage_open(fki, fi);
        }
    }
    icon(IC_DOTS, SCR_W - 40, 34, 18, 0xFFFFFFFF);
    if (hit(SCR_W - 80, 0, 80, 64)) music_track_menu(ti, -1, -1);

    /* ---------- title block ---------- */
    float rx = 420, rw = SCR_W - rx - 56;
    float ty = 190;
    int tfs = T->display_font == 2 ? 24 : 30;
    if (text_w(F_DISPLAY, tfs, t->title) > rw) tfs = tfs * 4 / 5;
    text_ellipsis(F_DISPLAY, tfs, rx, ty, rw, 0xFFFFFFFF, t->title);
    text_ellipsis(F_SEMI, 17, rx, ty + 30, rw - 40, col_a(0xFFFFFFFF, 0.8f), t->artist);
    char al[300];
    if (t->year) snprintf(al, sizeof(al), "%s \xc2\xb7 %d", t->album[0] ? t->album : "Single", t->year);
    else snprintf(al, sizeof(al), "%s", t->album[0] ? t->album : "Single");
    text_ellipsis(F_REG, 14, rx, ty + 52, rw - 40, col_a(0xFFFFFFFF, 0.55f), al);
    if (hit(rx, ty + 14, rw, 22)) mpage_open(FROM_ARTIST, t->artist_i);
    if (hit(rx, ty + 38, rw, 22)) mpage_open(FROM_ALBUM, t->album_i);

    /* ---------- seek bar ---------- */
    float sx = rx, sw = rw, sy = 290;
    int dur = mus_dur_ms(), pos = mus_pos_ms();
    if (in.touching && !seek_drag && fabsf(in.sy - sy) < 18 && in.sx > sx - 10 && in.sx < sx + sw + 10 && dur > 0) seek_drag = 1;
    if (seek_drag) {
        seek_drag_p = clampf((in.tx - sx) / sw, 0, 1);
        pos = (int)(seek_drag_p * dur);
        if (!in.touching) {
            mus_seek(pos);
            seek_drag = 0;
        }
    }
    float p = dur > 0 ? clampf((float)pos / dur, 0, 1) : 0;
    gfx_rrect(sx, sy - 2, sw, 4, 2, col_a(0xFFFFFFFF, 0.22f));
    gfx_rrect(sx, sy - 2, fmaxf(4, sw * p), 4, 2, 0xFFFFFFFF);
    gfx_circle(sx + sw * p, sy, nf == 0 || seek_drag ? 8 : 6, 0xFFFFFFFF);
    if (nf == 0 && !queue_open) gfx_ring(sx + sw * p, sy, 12, 2, T->accent);
    char a1[16], a2[16];
    fmt_ms(pos, a1, sizeof(a1));
    fmt_ms(dur > pos ? dur - pos : 0, a2 + 1, sizeof(a2) - 1);
    a2[0] = '-';
    text(F_META, 12, sx, sy + 24, col_a(0xFFFFFFFF, 0.65f), a1);
    text_right(F_META, 12, sx + sw, sy + 24, col_a(0xFFFFFFFF, 0.65f), dur ? a2 : "");

    /* ---------- main controls ---------- */
    float cy = 362, cx0 = rx + sw / 2;
    float bx[5] = {cx0 - 190, cx0 - 95, cx0, cx0 + 95, cx0 + 190};
    int playing = mus_playing();
    for (int i = 0; i < 5; i++) {
        float f = nf == 1 && nb == i && !queue_open ? 1 : 0;
        float r = i == 2 ? 34 : 24;
        if (i == 2) {
            gfx_circle(bx[i], cy, r + 2 * f, 0xFFFFFFFF);
            icon(playing ? IC_PAUSE : IC_PLAY, bx[i] + (playing ? 0 : 2), cy, 28, 0xFF101014);
            if (f) gfx_ring(bx[i], cy, r + 7, 2.5f, T->accent);
        } else {
            if (f) gfx_circle(bx[i], cy, r, col_a(0xFFFFFFFF, 0.16f));
            int ic = i == 0 ? IC_SHUFFLE : i == 1 ? IC_PREV : i == 3 ? IC_NEXT : IC_REPEAT;
            col_t c = 0xFFFFFFFF;
            if (i == 0) c = mus_shuffle() ? T->accent : col_a(0xFFFFFFFF, 0.75f);
            if (i == 4) c = mus_repeat() ? T->accent : col_a(0xFFFFFFFF, 0.75f);
            icon(ic, bx[i], cy, i == 1 || i == 3 ? 26 : 22, c);
            if (i == 0 && mus_shuffle()) gfx_circle(bx[i], cy + 18, 2, T->accent);
            if (i == 4 && mus_repeat()) {
                gfx_circle(bx[i], cy + 18, 2, T->accent);
                if (mus_repeat() == REPEAT_ONE) {
                    gfx_circle(bx[i] + 11, cy - 10, 7, T->accent);
                    text_center(F_SEMI, 9, bx[i] + 11, cy - 6.5f, 0xFF101014, "1");
                }
            }
            if (f) gfx_ring(bx[i], cy, r, 1.5f, col_a(0xFFFFFFFF, 0.6f));
        }
        if (hit(bx[i] - r - 6, cy - r - 6, r * 2 + 12, r * 2 + 12)) { nf = 1; nb = i; np_action(1, i); }
    }

    /* ---------- extras row ---------- */
    float ey = 432;
    int ics[6] = {t->liked ? IC_HEART_FILL : IC_HEART, IC_PLUS, IC_QUEUE, IC_EQ, IC_TIMER, IC_SCREEN_OFF};
    static const char *elab[6] = {"Like", "Playlist", "Queue", "Sound", "Sleep", "Screen off"};
    float estep = rw / 6;
    for (int i = 0; i < 6; i++) {
        float x = rx + estep * (i + 0.5f);
        float f = nf == 2 && nb == i && !queue_open && !eq_open ? 1 : 0;
        if (f) gfx_circle(x, ey, 21, col_a(0xFFFFFFFF, 0.16f));
        col_t c = col_a(0xFFFFFFFF, 0.8f);
        if (i == 0 && t->liked) c = T->accent;
        if (i == 2 && queue_open) c = T->accent;
        if (i == 3 && (eq_enabled() || eq_loud())) c = T->accent;
        if (i == 4 && mus_sleep_left()) c = T->accent;
        icon(ics[i], x, ey, 20, c);
        if (f) gfx_ring(x, ey, 21, 1.5f, col_a(0xFFFFFFFF, 0.6f));
        const char *lab = elab[i];
        char sl[32];
        if (i == 4 && mus_sleep_left()) {
            int s2 = mus_sleep_left();
            if (s2 < 0) snprintf(sl, sizeof(sl), "end of song");
            else snprintf(sl, sizeof(sl), "%d:%02d", s2 / 60, s2 % 60);
            lab = sl;
        }
        if (i == 3 && eq_enabled()) {
            int pr = eq_matching_preset();
            lab = pr >= 0 ? EQ_PRESETS[pr].name : "Custom";
        }
        text_center(F_META, 10, x, ey + 32, f || c != col_a(0xFFFFFFFF, 0.8f) ? c : col_a(0xFFFFFFFF, 0.5f), lab);
        if (hit(x - 26, ey - 26, 52, 60)) { nf = 2; nb = i; np_action(2, i); }
    }
    /* volume: its own row, 0-100% normal and up to 200% boost */
    {
        float vy = 492;
        float f = nf == 3 && !queue_open && !eq_open ? 1 : 0;
        icon(IC_VOLUME, rx + 10, vy, 18, col_a(0xFFFFFFFF, 0.8f));
        float bx0 = rx + 34, bw = rw - 34 - 56;
        int v = mus_volume();
        if (!eq_open && !queue_open && in.touching && (vol_drag || (fabsf(in.sy - vy) < 20 && in.sx > bx0 - 12 && in.sx < bx0 + bw + 12))) {
            vol_drag = 1;
            int nv = (int)roundf(clampf((in.tx - bx0) / bw, 0, 1) * 200 / 5) * 5;
            if (nv != v) mus_set_volume(nv);
            nf = 3;
        }
        if (vol_drag && !in.touching) { vol_drag = 0; db_seti("music_vol", mus_volume()); }
        v = mus_volume();
        float k = v / 200.0f;
        gfx_rrect(bx0, vy - 2, bw, 4, 2, col_a(0xFFFFFFFF, 0.22f));
        gfx_rrect(bx0 + bw * 0.5f, vy - 2, bw * 0.5f, 4, 2, col_a(T->accent, 0.18f)); /* the boost zone */
        if (v > 100) {
            gfx_rrect(bx0, vy - 2, bw * 0.5f, 4, 2, 0xFFFFFFFF);
            gfx_rrect(bx0 + bw * 0.5f, vy - 2, bw * (k - 0.5f), 4, 2, T->accent);
        } else gfx_rrect(bx0, vy - 2, fmaxf(4, bw * k), 4, 2, 0xFFFFFFFF);
        gfx_rect(bx0 + bw * 0.5f - 1, vy - 6, 2, 12, col_a(0xFFFFFFFF, 0.5f));
        gfx_circle(bx0 + bw * k, vy, f || vol_drag ? 8 : 6, 0xFFFFFFFF);
        if (f) gfx_ring(bx0 + bw * k, vy, 12, 2, T->accent);
        char vl[16];
        snprintf(vl, sizeof(vl), "%d%%", v);
        text_right(F_SEMI, 13, rx + rw, vy + 5, v > 100 ? T->accent : col_a(0xFFFFFFFF, 0.75f), vl);
        if (v > 100) text_center(F_META, 9, bx0 + bw * 0.75f, vy - 10, col_a(T->accent, 0.8f), "BOOST");
    }

    /* little level bars under the cover */
    if (playing && !S.reduce_motion) {
        for (int b = 0; b < 24; b++) {
            float k2 = 0.25f + 0.75f * fabsf(sinf(g_time * (3.1f + (b % 5) * 0.9f) + b * 0.7f));
            float h = 3 + 22 * k2 * mus_level();
            gfx_rrect(ax + b * (as / 24) + 2, ay + as + 34 - h, as / 24 - 4, h, 1.5f, col_a(0xFFFFFFFF, 0.25f));
        }
    }

    if (qa > 0.01f) {
        gfx_rect(0, 0, SCR_W, SCR_H, col_a(0xFF000000, 0.35f * qa));
        draw_queue(dt, qa);
        if (in.tap && in.tap_x < SCR_W - 440) queue_open = 0;
    }
    in = keep;
    if (eq_a > 0.01f) {
        gfx_rect(0, 0, SCR_W, SCR_H, col_a(0xFF000000, 0.45f * eq_a));
        if (eq_open) draw_eq(dt, eq_a);
        else {
            Input none = in;
            in.pressed = in.repeat = 0;
            in.tap = in.touching = in.dragging = 0;
            draw_eq(dt, eq_a);
            in = none;
        }
    }
    if (!modal_active() && !queue_open && !eq_open) {
        text_center(F_META, 11, SCR_W / 2, SCR_H - 14, col_a(0xFFFFFFFF, 0.4f),
                    "L / R skip \xc2\xb7 SQUARE play / pause \xc2\xb7 TRIANGLE queue \xc2\xb7 SELECT screen off \xc2\xb7 START options");
    }
}

/* ================================================================
   Mini player
   ================================================================ */

static float mini_a;
static int mini_shows_on(int top);

int miniplayer_height(void) { return mus_active() ? 70 : 0; }

static int mini_shows_on(int top) {
    return top == SCR_MAIN || top == SCR_DETAIL || top == SCR_MPAGE || top == SCR_SEARCH || top == SCR_HISTORY ||
           top == SCR_STATS || top == SCR_STORAGE;
}
int miniplayer_owns_tap(int top) {
    if (!mus_active() || !mini_shows_on(top) || mini_a < 0.5f || modal_active()) return 0;
    float y = SCR_H - 66;
    return (in.tap && in.tap_y >= y) || (in.long_press && in.ty >= y) || (in.double_tap && in.tap_y >= y);
}

void miniplayer_draw(float dt, int top) {
    int show = mus_active() && mini_shows_on(top);
    mini_a = approach(mini_a, show ? 1 : 0, 12, dt);
    if (mini_a < 0.01f) return;
    int ti = mus_current();
    if (ti < 0 || ti >= M.nt) return;
    MTrack *t = &M.t[ti];
    float a = mini_a;
    float h = 56, x = 14, w = SCR_W - 28, y = SCR_H - h - 10 + (1 - a) * 30;
    gfx_shadow(x, y + 6, w, h, 14, 18, col_a(0xFF000000, 0.5f * a));
    gfx_rrect(x, y, w, h, 14, col_a(col_mix(T->bg, 0xFF000000, 0.25f), 0.96f * a));
    gfx_clip(x, y, w, h);
    art_blur(t->art, x, y, w, h, 0.35f * a);
    gfx_noclip();
    gfx_rrect_outline(x, y, w, h, 14, 1, col_a(0xFFFFFFFF, 0.08f * a));
    art_draw(t->art, t->album, x + 8, y + 8, 40, 40, 8, a);
    text_ellipsis(F_SEMI, 14, x + 60, y + 25, w - 260, col_a(0xFFFFFFFF, a), t->title);
    text_ellipsis(F_REG, 12, x + 60, y + 43, w - 260, col_a(0xFFFFFFFF, 0.65f * a), t->artist);
    /* buttons */
    float bxh = x + w - 150;
    icon(t->liked ? IC_HEART_FILL : IC_HEART, bxh, y + h / 2, 18, t->liked ? col_a(T->accent, a) : col_a(0xFFFFFFFF, 0.75f * a));
    icon(mus_playing() ? IC_PAUSE : IC_PLAY, bxh + 52, y + h / 2, 22, col_a(0xFFFFFFFF, a));
    icon(IC_NEXT, bxh + 100, y + h / 2, 20, col_a(0xFFFFFFFF, a));
    /* progress */
    int dur = mus_dur_ms();
    float p = dur > 0 ? clampf((float)mus_pos_ms() / dur, 0, 1) : 0;
    gfx_rect(x + 14, y + h - 3, (w - 28), 2, col_a(0xFFFFFFFF, 0.15f * a));
    gfx_rect(x + 14, y + h - 3, (w - 28) * p, 2, col_a(0xFFFFFFFF, 0.9f * a));
    if (!show || modal_active()) return;
    if (hit(bxh - 22, y, 44, h)) { music_set_liked(ti, !t->liked); input_consume_touch(); }
    else if (hit(bxh + 30, y, 44, h)) { mus_toggle(); input_consume_touch(); }
    else if (hit(bxh + 78, y, 44, h)) { mus_next(); input_consume_touch(); }
    else if (hit(x, y, w, h)) { nowplaying_open(); input_consume_touch(); }
}
