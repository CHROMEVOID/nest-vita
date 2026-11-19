/* The music library: finds songs in your folders, reads their tags, keeps
   them in the database and groups them into albums, artists and playlists. */
#include "nest.h"
#include "music.h"
#include "mtags.h"

#include <sqlite3.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>

sqlite3 *db_handle(void);

#define MUSIC_DIR DATA_DIR "/music"
#define ART_DIR MUSIC_DIR "/art"

MusicLib M;

/* ---------------- small helpers ---------------- */

static char *sdup(const char *s) { return strdup(s ? s : ""); }

/* case-insensitive "contains" */
static const char *ci_find(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l) return h;
    for (; *h; h++)
        if (!strncasecmp(h, n, l)) return h;
    return NULL;
}

static sqlite3_stmt *q(const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    return s;
}
static const char *col(sqlite3_stmt *s, int i) {
    const char *v = (const char *)sqlite3_column_text(s, i);
    return v ? v : "";
}

/* "The Pharcyde" sorts under P */
static const char *sort_key(const char *s) {
    if (!strncasecmp(s, "the ", 4)) return s + 4;
    return s;
}
static int cmp_name(const char *a, const char *b) { return strcasecmp(sort_key(a), sort_key(b)); }

/* first artist of "A;B", "A feat. B" */
static void primary_artist(const char *s, char *out, int n) {
    snprintf(out, n, "%s", s);
    char *cut = strchr(out, ';');
    if (cut) *cut = 0;
    const char *feats[] = {" feat. ", " feat ", " ft. ", " featuring ", " Feat. ", " Ft. ", " FEAT. ", " x "};
    for (int i = 0; i < 8; i++) {
        char *f = strstr(out, feats[i]);
        if (f && f != out) *f = 0;
    }
    size_t l = strlen(out);
    while (l && out[l - 1] == ' ') out[--l] = 0;
    if (!out[0]) snprintf(out, n, "Unknown artist");
}

/* "A;B" shown as "A, B" */
static char *nice_artists(const char *s) {
    char *r = sdup(s);
    for (char *p = r; *p; p++)
        if (*p == ';') *p = ',';
    /* ", " after the comma */
    size_t n = strlen(r);
    char *o = malloc(n * 2 + 1);
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        o[k++] = r[i];
        if (r[i] == ',' && r[i + 1] != ' ') o[k++] = ' ';
    }
    o[k] = 0;
    free(r);
    return o;
}

/* ---------------- schema ---------------- */

static void schema(void) {
    db_exec("CREATE TABLE IF NOT EXISTS mtracks(path TEXT PRIMARY KEY, title TEXT, artist TEXT, aartist TEXT, album TEXT,"
            " genre TEXT, art TEXT, track INT, disc INT, year INT, dur INT, size INT, added INT);"
            "CREATE TABLE IF NOT EXISTS mstats(path TEXT PRIMARY KEY, liked INT, plays INT, last INT);"
            "CREATE TABLE IF NOT EXISTS mplaylists(id INTEGER PRIMARY KEY, name TEXT, descr TEXT, cover TEXT, source TEXT UNIQUE,"
            " created INT, last INT);"
            "CREATE TABLE IF NOT EXISTS mpl_items(pid INT, pos INT, path TEXT, hint TEXT);"
            "CREATE INDEX IF NOT EXISTS mpl_items_pid ON mpl_items(pid, pos);"
            "CREATE TABLE IF NOT EXISTS mrecent(kind INT, key TEXT, t INT, PRIMARY KEY(kind, key));"
            "CREATE TABLE IF NOT EXISTS martists(name TEXT PRIMARY KEY, art TEXT);");
}

/* ---------------- path -> track index ---------------- */

static int *hidx;
static int hcap;

static void hash_build(void) {
    free(hidx);
    hcap = 64;
    while (hcap < M.nt * 2) hcap <<= 1;
    hidx = malloc(sizeof(int) * hcap);
    for (int i = 0; i < hcap; i++) hidx[i] = -1;
    for (int i = 0; i < M.nt; i++) {
        unsigned h = mtags_hash(M.t[i].path) & (hcap - 1);
        while (hidx[h] >= 0) h = (h + 1) & (hcap - 1);
        hidx[h] = i;
    }
}

static int track_by_path(const char *p) {
    if (!hidx) return -1;
    unsigned h = mtags_hash(p) & (hcap - 1);
    while (hidx[h] >= 0) {
        if (!strcasecmp(M.t[hidx[h]].path, p)) return hidx[h];
        h = (h + 1) & (hcap - 1);
    }
    return -1;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    const char *c = strrchr(p, ':');
    if (c && (!s || c > s)) s = c;
    return s ? s + 1 : p;
}

/* a playlist line that doesn't point at a real file: find the song another way */
static int track_fuzzy(const char *entry, const char *hint) {
    const char *b = base_name(entry);
    for (int i = 0; i < M.nt; i++)
        if (!strcasecmp(base_name(M.t[i].path), b)) return i;
    /* "#EXTINF:123,Artist - Title" */
    if (hint && hint[0]) {
        const char *sep = strstr(hint, " - ");
        char art[256] = "", tit[256];
        if (sep) {
            snprintf(art, sizeof(art), "%.*s", (int)(sep - hint), hint);
            snprintf(tit, sizeof(tit), "%s", sep + 3);
        } else snprintf(tit, sizeof(tit), "%s", hint);
        int best = -1;
        for (int i = 0; i < M.nt; i++) {
            if (strcasecmp(M.t[i].title, tit)) continue;
            if (!art[0] || ci_find(M.t[i].artist, art) || ci_find(art, M.t[i].artist)) return i;
            if (best < 0) best = i;
        }
        return best;
    }
    return -1;
}

/* ---------------- loading ---------------- */

static void lib_free(void) {
    for (int i = 0; i < M.nt; i++) {
        MTrack *t = &M.t[i];
        free(t->path); free(t->title); free(t->artist); free(t->album_artist); free(t->album); free(t->genre); free(t->art);
    }
    for (int i = 0; i < M.na; i++) { free(M.a[i].name); free(M.a[i].artist); free(M.a[i].art); free(M.a[i].tracks); }
    for (int i = 0; i < M.nr; i++) { free(M.r[i].name); free(M.r[i].art); free(M.r[i].albums); free(M.r[i].tracks); }
    for (int i = 0; i < M.np; i++) { free(M.p[i].name); free(M.p[i].desc); free(M.p[i].cover); free(M.p[i].source); free(M.p[i].tracks); }
    free(M.t); free(M.a); free(M.r); free(M.p);
    M.t = NULL; M.a = NULL; M.r = NULL; M.p = NULL;
    M.nt = M.na = M.nr = M.np = 0;
}

static int cmp_album_tracks(const void *x, const void *y) {
    const MTrack *a = &M.t[*(const int *)x], *b = &M.t[*(const int *)y];
    int da = a->disc ? a->disc : 1, dbb = b->disc ? b->disc : 1;
    if (da != dbb) return da - dbb;
    if (a->track != b->track) {
        if (!a->track) return 1;
        if (!b->track) return -1;
        return a->track - b->track;
    }
    return strcasecmp(base_name(a->path), base_name(b->path));
}
static int cmp_albums(const void *x, const void *y) {
    const MAlbum *a = x, *b = y;
    int c = cmp_name(a->artist, b->artist);
    if (c) return c;
    if (a->year != b->year) return a->year - b->year;
    return cmp_name(a->name, b->name);
}
static int cmp_artists(const void *x, const void *y) { return cmp_name(((const MArtist *)x)->name, ((const MArtist *)y)->name); }
static int cmp_artist_albums(const void *x, const void *y) {
    const MAlbum *a = &M.a[*(const int *)x], *b = &M.a[*(const int *)y];
    if (a->year != b->year) return b->year - a->year;
    return cmp_name(a->name, b->name);
}

static void group(void) {
    /* albums: same album name + same (album) artist */
    char (*keys)[300] = malloc(sizeof(*keys) * (M.nt + 1));
    int *akey = malloc(sizeof(int) * (M.nt + 1));
    M.a = calloc(M.nt + 1, sizeof(MAlbum));
    M.na = 0;
    /* hashed lookup of album keys */
    int cap = 64;
    while (cap < M.nt * 2) cap <<= 1;
    int *ah = malloc(sizeof(int) * cap);
    for (int i = 0; i < cap; i++) ah[i] = -1;
    for (int i = 0; i < M.nt; i++) {
        MTrack *t = &M.t[i];
        char pa[256];
        primary_artist(t->album_artist[0] ? t->album_artist : t->artist, pa, sizeof(pa));
        snprintf(keys[i], 300, "%s|%s", t->album[0] ? t->album : "\x01singles", pa);
        unsigned h = mtags_hash(keys[i]) & (cap - 1);
        int found = -1;
        while (ah[h] >= 0) {
            if (!strcasecmp(keys[akey[ah[h]]], keys[i])) { found = ah[h]; break; }
            h = (h + 1) & (cap - 1);
        }
        if (found < 0) {
            found = M.na++;
            MAlbum *a = &M.a[found];
            a->name = sdup(t->album[0] ? t->album : "Singles");
            a->artist = sdup(pa);
            a->tracks = malloc(sizeof(int) * 8);
            akey[found] = i;
            ah[h] = found;
        }
        MAlbum *a = &M.a[found];
        if (a->nt && (a->nt & (a->nt - 1)) == 0 && a->nt >= 8) a->tracks = realloc(a->tracks, sizeof(int) * a->nt * 2);
        a->tracks[a->nt++] = i;
        t->album_i = found;
    }
    free(ah);
    free(akey);
    free(keys);
    /* order albums, then remap */
    for (int i = 0; i < M.na; i++) {
        MAlbum *a = &M.a[i];
        qsort(a->tracks, a->nt, sizeof(int), cmp_album_tracks);
        for (int k = 0; k < a->nt; k++) {
            MTrack *t = &M.t[a->tracks[k]];
            if (t->year > a->year) a->year = t->year;
            if (!a->art && t->art && t->art[0]) a->art = sdup(t->art);
            a->dur += t->dur;
            if (t->last_played > a->last_played) a->last_played = t->last_played;
            if (t->added > a->added) a->added = t->added;
        }
    }
    qsort(M.a, M.na, sizeof(MAlbum), cmp_albums);
    for (int i = 0; i < M.na; i++)
        for (int k = 0; k < M.a[i].nt; k++) M.t[M.a[i].tracks[k]].album_i = i;

    /* artists */
    M.r = calloc(M.na + 1, sizeof(MArtist));
    M.nr = 0;
    for (int i = 0; i < M.na; i++) {
        MAlbum *a = &M.a[i];
        int r = -1;
        if (M.nr && !strcasecmp(M.r[M.nr - 1].name, a->artist)) r = M.nr - 1; /* albums are sorted by artist */
        else {
            for (int k = 0; k < M.nr; k++)
                if (!strcasecmp(M.r[k].name, a->artist)) { r = k; break; }
        }
        if (r < 0) {
            r = M.nr++;
            M.r[r].name = sdup(a->artist);
            M.r[r].albums = malloc(sizeof(int) * (M.na + 1));
        }
        M.r[r].albums[M.r[r].nal++] = i;
        a->artist_i = r;
        if (a->last_played > M.r[r].last_played) M.r[r].last_played = a->last_played;
    }
    qsort(M.r, M.nr, sizeof(MArtist), cmp_artists);
    sqlite3_stmt *s = q("SELECT art FROM martists WHERE name = ? COLLATE NOCASE");
    for (int r = 0; r < M.nr; r++) {
        MArtist *ar = &M.r[r];
        qsort(ar->albums, ar->nal, sizeof(int), cmp_artist_albums);
        int n = 0;
        for (int k = 0; k < ar->nal; k++) {
            M.a[ar->albums[k]].artist_i = r;
            n += M.a[ar->albums[k]].nt;
        }
        ar->tracks = malloc(sizeof(int) * (n + 1));
        for (int k = 0; k < ar->nal; k++) {
            MAlbum *a = &M.a[ar->albums[k]];
            for (int j = 0; j < a->nt; j++) {
                ar->tracks[ar->nt++] = a->tracks[j];
                M.t[a->tracks[j]].artist_i = r;
            }
        }
        if (s) {
            sqlite3_reset(s);
            sqlite3_bind_text(s, 1, ar->name, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW && col(s, 0)[0]) ar->art = sdup(col(s, 0));
        }
        for (int k = 0; !ar->art && k < ar->nal; k++)
            if (M.a[ar->albums[k]].art) ar->art = sdup(M.a[ar->albums[k]].art);
    }
    if (s) sqlite3_finalize(s);
}

static int cmp_liked(const void *x, const void *y) {
    const MTrack *a = &M.t[*(const int *)x], *b = &M.t[*(const int *)y];
    return a->liked < b->liked ? 1 : a->liked > b->liked ? -1 : 0;
}

static void load_playlists(void) {
    int cap = 8;
    M.p = calloc(cap, sizeof(MPlaylist));
    M.np = 0;
    /* Liked Songs first */
    MPlaylist *lk = &M.p[M.np++];
    lk->id = -1;
    lk->liked_list = 1;
    lk->name = sdup("Liked Songs");
    lk->desc = sdup("Songs you hearted");
    lk->tracks = malloc(sizeof(int) * (M.nt + 1));
    for (int i = 0; i < M.nt; i++)
        if (M.t[i].liked) lk->tracks[lk->nt++] = i;
    qsort(lk->tracks, lk->nt, sizeof(int), cmp_liked);
    for (int k = 0; k < lk->nt; k++) lk->dur += M.t[lk->tracks[k]].dur;

    sqlite3_stmt *s = q("SELECT id, name, descr, cover, source, created, last FROM mplaylists ORDER BY COALESCE(last, created) DESC");
    sqlite3_stmt *it = q("SELECT path, hint FROM mpl_items WHERE pid = ? ORDER BY pos");
    while (s && sqlite3_step(s) == SQLITE_ROW) {
        if (M.np == cap) {
            cap *= 2;
            M.p = realloc(M.p, sizeof(MPlaylist) * cap);
        }
        MPlaylist *p = &M.p[M.np++];
        memset(p, 0, sizeof(*p));
        p->id = sqlite3_column_int(s, 0);
        p->name = sdup(col(s, 1));
        p->desc = sdup(col(s, 2));
        p->cover = col(s, 3)[0] ? sdup(col(s, 3)) : NULL;
        p->source = col(s, 4)[0] ? sdup(col(s, 4)) : NULL;
        p->created = sqlite3_column_int64(s, 5);
        p->last_played = sqlite3_column_int64(s, 6);
        int tc = 16;
        p->tracks = malloc(sizeof(int) * tc);
        if (it) {
            sqlite3_reset(it);
            sqlite3_bind_int(it, 1, p->id);
            while (sqlite3_step(it) == SQLITE_ROW) {
                int ti = track_by_path(col(it, 0));
                if (ti < 0) ti = track_fuzzy(col(it, 0), col(it, 1));
                if (ti < 0) { p->missing++; continue; }
                if (p->nt == tc) {
                    tc *= 2;
                    p->tracks = realloc(p->tracks, sizeof(int) * tc);
                }
                p->tracks[p->nt++] = ti;
                p->dur += M.t[ti].dur;
            }
        }
    }
    if (s) sqlite3_finalize(s);
    if (it) sqlite3_finalize(it);
}

void mus_remap_begin(void);
void mus_remap_end(void);
static void music_reload_inner(void);
void music_reload(void) {
    mus_remap_begin();
    music_reload_inner();
    mus_remap_end();
    M.gen++;
}
static void music_reload_inner(void) {
    lib_free();
    sqlite3_stmt *s = q("SELECT COUNT(*) FROM mtracks");
    int n = 0;
    if (s && sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    if (s) sqlite3_finalize(s);
    M.t = calloc(n + 1, sizeof(MTrack));
    s = q("SELECT m.path, title, artist, aartist, album, genre, art, track, disc, year, dur, added,"
          " COALESCE(st.liked, 0), COALESCE(st.plays, 0), COALESCE(st.last, 0)"
          " FROM mtracks m LEFT JOIN mstats st ON st.path = m.path");
    while (s && sqlite3_step(s) == SQLITE_ROW && M.nt < n) {
        MTrack *t = &M.t[M.nt++];
        t->path = sdup(col(s, 0));
        t->title = sdup(col(s, 1));
        t->artist = nice_artists(col(s, 2));
        t->album_artist = sdup(col(s, 3));
        t->album = sdup(col(s, 4));
        t->genre = sdup(col(s, 5));
        t->art = col(s, 6)[0] ? sdup(col(s, 6)) : NULL;
        t->track = sqlite3_column_int(s, 7);
        t->disc = sqlite3_column_int(s, 8);
        t->year = sqlite3_column_int(s, 9);
        t->dur = sqlite3_column_int(s, 10);
        t->added = sqlite3_column_int64(s, 11);
        t->liked = (int)sqlite3_column_int64(s, 12) != 0;
        t->plays = sqlite3_column_int(s, 13);
        t->last_played = sqlite3_column_int64(s, 14);
        if (sqlite3_column_int64(s, 12)) t->liked = 1;
    }
    if (s) sqlite3_finalize(s);
    /* liked holds the time it was liked: keep it for ordering */
    s = q("SELECT path, liked FROM mstats WHERE liked > 0");
    hash_build();
    while (s && sqlite3_step(s) == SQLITE_ROW) {
        int ti = track_by_path(col(s, 0));
        if (ti >= 0) M.t[ti].liked = (int)(sqlite3_column_int64(s, 1) & 0x7FFFFFFF);
    }
    if (s) sqlite3_finalize(s);
    group();
    load_playlists();
}

int music_has_library(void) { return M.nt > 0; }

/* ---------------- scanning (background thread) ---------------- */

typedef struct {
    char *path;
    long long size;
    MTags tags;
    int had_album;
} Found;

static Found *found;
static int nfound, capfound;
static char (*plists)[512];
static int nplists, capplists;
static char (*artist_imgs)[2][512];
static int nartimg, capartimg;
static char roots[24][512];
static int roots_ok[24], roots_songs[24], roots_picked[24];
static int rescan_after; /* a scan was asked for while one was running */
static int last_nroots;
/* the report shown on the Music screen: copied only from a search that finished */
static char rep_roots[24][512];
static int rep_ok[24], rep_songs[24], rep_picked[24], rep_n;
static int nroots;
static volatile int scan_done, scan_cancel;
static SceUID scan_th = -1;
/* what's already known: path + size, so unchanged files aren't opened again */
static char **known_path;
static long long *known_size;
static int nknown;
static unsigned *known_hash;
static int known_cap;

static int known_find(const char *p) {
    if (!known_hash) return -1;
    unsigned h = mtags_hash(p) & (known_cap - 1);
    while (known_hash[h] != 0xFFFFFFFFu) {
        if (!strcmp(known_path[known_hash[h]], p)) return (int)known_hash[h];
        h = (h + 1) & (known_cap - 1);
    }
    return -1;
}

typedef struct {
    char dir[512];
    int depth;
} WalkCtx;

/* a folder's files: no limit (some people keep thousands of songs in one folder) */
typedef struct {
    char (*names)[256];
    int *dir;
    long long *size;
    int n, cap;
} DirList;

static void list_cb(void *ud, const char *name, int is_dir, long long size) {
    DirList *d = ud;
    if (d->n == d->cap) {
        int nc = d->cap ? d->cap * 2 : 256;
        void *a = realloc(d->names, sizeof(*d->names) * nc);
        if (!a) return;
        d->names = a;
        a = realloc(d->dir, sizeof(int) * nc);
        if (!a) return;
        d->dir = a;
        a = realloc(d->size, sizeof(long long) * nc);
        if (!a) return;
        d->size = a;
        d->cap = nc;
    }
    snprintf(d->names[d->n], 256, "%s", name);
    d->dir[d->n] = is_dir;
    d->size[d->n] = size;
    d->n++;
}
static void dl_free(DirList *d) {
    free(d->names);
    free(d->dir);
    free(d->size);
    free(d);
}

static const char *COVER_NAMES[] = {"cover", "folder", "front", "album", "albumart", "albumartlarge", "thumb", NULL};

static void found_add(const char *path, long long size, const char *folder_art, int known) {
    if (nfound == capfound) {
        capfound = capfound ? capfound * 2 : 256;
        found = realloc(found, sizeof(Found) * capfound);
    }
    Found *f = &found[nfound];
    memset(f, 0, sizeof(*f));
    f->path = strdup(path);
    f->size = size;
    if (known) {
        f->tags.dur = -1; /* unchanged: keep what the database has */
    } else {
        mtags_read(path, size, &f->tags, ART_DIR);
        f->had_album = f->tags.album[0] != 0;
        mtags_guess(path, &f->tags);
        if (!f->tags.art[0] && folder_art) snprintf(f->tags.art, sizeof(f->tags.art), "%s", folder_art);
    }
    nfound++;
    M.scan_found = nfound;
}

static void walk(const char *dir, int depth) {
    if (depth > 8 || scan_cancel) return;
    DirList *d = calloc(1, sizeof(DirList));
    if (!d) return;
    if (fs_list(dir, list_cb, d) < 0) { dl_free(d); return; }
    snprintf(M.scan_now, sizeof(M.scan_now), "%s", dir);
    /* cover image in this folder */
    char cover[512] = "";
    int best = 99;
    for (int i = 0; i < d->n; i++) {
        if (d->dir[i]) continue;
        const char *e = strrchr(d->names[i], '.');
        if (!e || (strcasecmp(e, ".jpg") && strcasecmp(e, ".jpeg") && strcasecmp(e, ".png"))) continue;
        char stem[256];
        snprintf(stem, sizeof(stem), "%.*s", (int)(e - d->names[i]), d->names[i]);
        for (char *p = stem; *p; p++) *p = (char)tolower((unsigned char)*p);
        int rank = 50;
        for (int k = 0; COVER_NAMES[k]; k++)
            if (!strncmp(stem, COVER_NAMES[k], strlen(COVER_NAMES[k]))) { rank = k; break; }
        if (!strcmp(stem, "artist")) {
            if (nartimg == capartimg) {
                capartimg = capartimg ? capartimg * 2 : 16;
                artist_imgs = realloc(artist_imgs, sizeof(*artist_imgs) * capartimg);
            }
            snprintf(artist_imgs[nartimg][0], 512, "%s", base_name(dir));
            snprintf(artist_imgs[nartimg][1], 512, "%s/%s", dir, d->names[i]);
            nartimg++;
            continue;
        }
        if (rank < best) {
            best = rank;
            snprintf(cover, sizeof(cover), "%s/%s", dir, d->names[i]);
        }
    }
    for (int i = 0; i < d->n; i++) {
        if (d->dir[i]) continue;
        char path[768];
        snprintf(path, sizeof(path), "%s/%s", dir, d->names[i]);
        if (mtags_is_audio(d->names[i])) {
            int k = known_find(path);
            found_add(path, d->size[i], cover[0] ? cover : NULL, k >= 0 && known_size[k] == d->size[i]);
        } else if (mtags_is_playlist(d->names[i])) {
            if (nplists == capplists) {
                capplists = capplists ? capplists * 2 : 16;
                plists = realloc(plists, sizeof(*plists) * capplists);
            }
            snprintf(plists[nplists++], 512, "%s", path);
        }
    }
    for (int i = 0; i < d->n; i++) {
        if (!d->dir[i] || d->names[i][0] == '.') continue;
        if (!strcasecmp(d->names[i], "sce_sys") || !strcasecmp(d->names[i], "sce_pfs")) continue;
        if (depth == 0 && strchr(dir, ':') && strchr(dir, ':')[1] == 0) { /* a whole card: skip the system's own folders */
            static const char *sysd[] = {"app", "patch", "addcont", "appmeta", "license", "cache", "savedata", "theme", "user", "pspemu", "vitasdk", "tai", NULL};
            int skip = 0;
            for (int k = 0; sysd[k]; k++)
                if (!strcasecmp(d->names[i], sysd[k])) skip = 1;
            if (skip) continue;
        }
        char sub[512];
        snprintf(sub, sizeof(sub), "%s/%s", dir, d->names[i]);
        if (!strncasecmp(sub, DATA_DIR, strlen(DATA_DIR)) && strncasecmp(sub, MUSIC_DIR, strlen(MUSIC_DIR))) continue;
        walk(sub, depth + 1);
    }
    dl_free(d);
}

static int scan_thread(SceSize a, void *p) {
    (void)a;
    (void)p;
    for (int i = 0; i < nroots; i++) {
        SceIoStat st;
        size_t rl = strlen(roots[i]);
        int is_dev = rl && roots[i][rl - 1] == ':';
        roots_ok[i] = 0;
        SceUID dd = sceIoDopen(roots[i]); /* exactly how the film scanner opens folders */
        if (dd >= 0) { roots_ok[i] = 1; sceIoDclose(dd); }
        else if (!is_dev && sceIoGetstat(roots[i], &st) >= 0) roots_ok[i] = 1;
        int before = nfound;
        if (roots_ok[i] && !scan_cancel) walk(roots[i], 0);
        roots_songs[i] = nfound - before;
    }
    scan_done = 1;
    return sceKernelExitDeleteThread(0);
}

static void add_root_ex(const char *r, int picked) {
    for (int i = 0; i < nroots; i++)
        if (!strcasecmp(roots[i], r)) { if (picked) roots_picked[i] = 1; return; }
    /* a folder inside one already listed is walked anyway */
    for (int i = 0; i < nroots; i++) {
        size_t l = strlen(roots[i]);
        if (!strncasecmp(roots[i], r, l) && r[l] == '/') return;
    }
    if (nroots < 24) {
        roots_picked[nroots] = picked;
        roots_songs[nroots] = 0;
        snprintf(roots[nroots++], 512, "%s", r);
    }
}
static void add_root(const char *r) { add_root_ex(r, 0); }

void music_scan_start(void) {
    if (M.scanning) { /* a new folder was picked: stop the old search and start on it now */
        scan_cancel = 1;
        rescan_after = 1;
        return;
    }
    sceIoMkdir(MUSIC_DIR, 0777);
    sceIoMkdir(ART_DIR, 0777);
    sceIoMkdir("ux0:music", 0777); /* so there's always an obvious place to put songs */
    nroots = 0;
    { /* folders you picked come first */
        sqlite3_stmt *s = q("SELECT v FROM settings WHERE k LIKE 'music_root:%'");
        while (s && sqlite3_step(s) == SQLITE_ROW) add_root_ex(col(s, 0), 1);
        if (s) sqlite3_finalize(s);
    }
    /* the usual music folders. Film and show folders are never searched for music:
       big video libraries made the music search crawl (and look like it found nothing) */
    const char *usual[] = {"ux0:music", "ux0:Music", "ux0:mp3", "ux0:data/Nest/music", "uma0:music", "uma0:Music",
                           "imc0:music", "xmc0:music", "grw0:music", "ur0:music"};
    for (int i = 0; i < 10; i++) add_root(usual[i]);
    /* snapshot of what's known */
    for (int i = 0; i < nknown; i++) free(known_path[i]);
    free(known_path);
    free(known_size);
    free(known_hash);
    nknown = 0;
    sqlite3_stmt *s = q("SELECT path, size FROM mtracks");
    int cap = 256;
    known_path = malloc(sizeof(char *) * cap);
    known_size = malloc(sizeof(long long) * cap);
    while (s && sqlite3_step(s) == SQLITE_ROW) {
        if (nknown == cap) {
            cap *= 2;
            known_path = realloc(known_path, sizeof(char *) * cap);
            known_size = realloc(known_size, sizeof(long long) * cap);
        }
        known_path[nknown] = strdup(col(s, 0));
        known_size[nknown] = sqlite3_column_int64(s, 1);
        nknown++;
    }
    if (s) sqlite3_finalize(s);
    known_cap = 64;
    while (known_cap < nknown * 2) known_cap <<= 1;
    known_hash = malloc(sizeof(unsigned) * known_cap);
    memset(known_hash, 0xFF, sizeof(unsigned) * known_cap);
    for (int i = 0; i < nknown; i++) {
        unsigned h = mtags_hash(known_path[i]) & (known_cap - 1);
        while (known_hash[h] != 0xFFFFFFFFu) h = (h + 1) & (known_cap - 1);
        known_hash[h] = i;
    }
    nfound = 0;
    nplists = 0;
    nartimg = 0;
    scan_done = 0;
    scan_cancel = 0;
    for (int i = 0; i < 24; i++) roots_ok[i] = roots_songs[i] = 0;
    M.scanning = 1;
    M.scan_found = 0;
    scan_th = sceKernelCreateThread("nest_mscan", scan_thread, 0x10000100 + 30, 0x80000, 0, 0x40000, NULL); /* tag reading uses big buffers */
    if (scan_th < 0 || sceKernelStartThread(scan_th, 0, NULL) < 0) {
        if (scan_th >= 0) sceKernelDeleteThread(scan_th);
        M.scanning = 0;
        scan_th = -1;
        rescan_after = 1; /* try again in a moment (the last search may still be closing) */
    }
}

/* ---------------- saving a finished scan ---------------- */

static void import_playlist(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    char name[256], desc[512] = "", cover[512] = "";
    const char *b = base_name(path);
    snprintf(name, sizeof(name), "%s", b);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    /* a cover next to it: "<name>.jpg", "<name>.png", or cover.jpg when the folder is just this playlist */
    const char *exts[] = {".jpg", ".png", ".jpeg"};
    for (int i = 0; i < 3 && !cover[0]; i++) {
        char c[600];
        snprintf(c, sizeof(c), "%s/%s%s", dir, name, exts[i]);
        SceIoStat st;
        if (sceIoGetstat(c, &st) >= 0) snprintf(cover, sizeof(cover), "%s", c);
    }
    {
        char hk[600];
        snprintf(hk, sizeof(hk), "mplhide:%s", path);
        if (db_get(hk, NULL)) { fclose(f); return; } /* you deleted it in Nest */
    }
    char line[1024], hint[512] = "";
    char (*items)[512] = malloc(sizeof(*items) * 4000);
    char (*hints)[256] = malloc(sizeof(*hints) * 4000);
    int ni = 0;
    int first = 1;
    while (items && hints && fgets(line, sizeof(line), f) && ni < 4000) {
        char *l = line;
        if (first && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF) l += 3;
        first = 0;
        size_t n = strlen(l);
        while (n && (l[n - 1] == '\n' || l[n - 1] == '\r' || l[n - 1] == ' ')) l[--n] = 0;
        if (!n) continue;
        if (l[0] == '#') {
            if (!strncasecmp(l, "#PLAYLIST:", 10)) snprintf(name, sizeof(name), "%s", l + 10);
            else if (!strncasecmp(l, "#NEST-DESC:", 11) || !strncasecmp(l, "#DESCRIPTION:", 13))
                snprintf(desc, sizeof(desc), "%s", strchr(l, ':') + 1);
            else if (!strncasecmp(l, "#NEST-COVER:", 12) || !strncasecmp(l, "#EXTIMG:", 8)) {
                const char *c = strchr(l, ':') + 1;
                while (*c == ' ') c++;
                if (strchr(c, ':')) snprintf(cover, sizeof(cover), "%s", c);
                else snprintf(cover, sizeof(cover), "%s/%s", dir, c);
                for (char *p = cover; *p; p++)
                    if (*p == '\\') *p = '/';
            } else if (!strncasecmp(l, "#EXTINF:", 8)) {
                const char *c = strchr(l, ',');
                snprintf(hint, sizeof(hint), "%s", c ? c + 1 : "");
            }
            continue;
        }
        char p[512];
        for (char *c = l; *c; c++)
            if (*c == '\\') *c = '/';
        if (!strncmp(l, "file://", 7)) l += 7;
        if (strchr(l, ':')) snprintf(p, sizeof(p), "%s", l);          /* ux0:music/... */
        else if (l[0] == '/') snprintf(p, sizeof(p), "ux0:%s", l + 1);
        else {
            /* relative, with ../ allowed */
            char base[512];
            snprintf(base, sizeof(base), "%s", dir);
            const char *r = l;
            while (!strncmp(r, "../", 3)) {
                char *s2 = strrchr(base, '/');
                if (s2) *s2 = 0;
                r += 3;
            }
            while (!strncmp(r, "./", 2)) r += 2;
            snprintf(p, sizeof(p), "%s/%s", base, r);
        }
        snprintf(items[ni], 512, "%s", p);
        snprintf(hints[ni], 256, "%s", hint);
        hint[0] = 0;
        ni++;
    }
    fclose(f);
    sqlite3_stmt *s = q("INSERT INTO mplaylists(name, descr, cover, source, created) VALUES(?,?,?,?,?)"
                        " ON CONFLICT(source) DO UPDATE SET name = excluded.name, descr = excluded.descr, cover = excluded.cover");
    if (s) {
        sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, desc, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, cover, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 5, now_s());
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    int pid = -1;
    s = q("SELECT id FROM mplaylists WHERE source = ?");
    if (s) {
        sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) pid = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (pid >= 0) {
        s = q("DELETE FROM mpl_items WHERE pid = ?");
        if (s) { sqlite3_bind_int(s, 1, pid); sqlite3_step(s); sqlite3_finalize(s); }
        s = q("INSERT INTO mpl_items(pid, pos, path, hint) VALUES(?,?,?,?)");
        for (int i = 0; s && i < ni; i++) {
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, pid);
            sqlite3_bind_int(s, 2, i);
            sqlite3_bind_text(s, 3, items[i], -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 4, hints[i], -1, SQLITE_STATIC);
            sqlite3_step(s);
        }
        if (s) sqlite3_finalize(s);
    }
    free(items);
    free(hints);
}

static void scan_commit(void) {
    db_exec("BEGIN");
    /* a folder of loose songs from lots of artists isn't an album: forget the folder-name album */
    for (int i = 0; i < nfound; i++) {
        Found *f = &found[i];
        if (f->tags.dur < 0 || f->had_album || !f->tags.album[0]) continue;
        const char *sl = strrchr(f->path, '/');
        int dl = sl ? (int)(sl - f->path) : 0;
        int same = 0, diff = 0;
        for (int k = 0; k < nfound && same + diff < 40; k++) {
            Found *g = &found[k];
            if (g->tags.dur < 0 || strncmp(g->path, f->path, dl) || strchr(g->path + dl + 1, '/')) continue;
            if (!strcasecmp(g->tags.artist, f->tags.artist)) same++;
            else diff++;
        }
        if (diff > 2 && diff > same) {
            f->tags.album[0] = 0;
            f->tags.aartist[0] = 0;
        }
    }
    sqlite3_stmt *s = q("INSERT OR REPLACE INTO mtracks(path, title, artist, aartist, album, genre, art, track, disc, year, dur, size, added)"
                        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?, COALESCE((SELECT added FROM mtracks WHERE path = ?1), ?13))");
    int64_t now = now_s();
    for (int i = 0; s && i < nfound; i++) {
        Found *f = &found[i];
        if (f->tags.dur < 0) continue; /* unchanged */
        MTags *t = &f->tags;
        sqlite3_reset(s);
        sqlite3_bind_text(s, 1, f->path, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, t->title, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, t->artist[0] ? t->artist : "Unknown artist", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, t->aartist, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 5, t->album, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 6, t->genre, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 7, t->art, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 8, t->track);
        sqlite3_bind_int(s, 9, t->disc);
        sqlite3_bind_int(s, 10, t->year);
        sqlite3_bind_int(s, 11, t->dur);
        sqlite3_bind_int64(s, 12, f->size);
        sqlite3_bind_int64(s, 13, now);
        sqlite3_step(s);
    }
    if (s) sqlite3_finalize(s);
    /* forget songs that are gone, but only from folders that were actually there */
    db_exec("CREATE TEMP TABLE IF NOT EXISTS mseen(path TEXT PRIMARY KEY); DELETE FROM mseen;");
    s = q("INSERT OR IGNORE INTO mseen(path) VALUES(?)");
    for (int i = 0; s && i < nfound; i++) {
        sqlite3_reset(s);
        sqlite3_bind_text(s, 1, found[i].path, -1, SQLITE_STATIC);
        sqlite3_step(s);
    }
    if (s) sqlite3_finalize(s);
    for (int r = 0; r < nroots; r++) {
        if (!roots_ok[r]) continue;
        char like[600];
        snprintf(like, sizeof(like), "%s/%%", roots[r]);
        s = q("DELETE FROM mtracks WHERE path LIKE ? AND path NOT IN (SELECT path FROM mseen)");
        if (s) {
            sqlite3_bind_text(s, 1, like, -1, SQLITE_STATIC);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }
    /* artist pictures */
    s = q("INSERT OR REPLACE INTO martists(name, art) VALUES(?, ?)");
    for (int i = 0; s && i < nartimg; i++) {
        sqlite3_reset(s);
        sqlite3_bind_text(s, 1, artist_imgs[i][0], -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, artist_imgs[i][1], -1, SQLITE_STATIC);
        sqlite3_step(s);
    }
    if (s) sqlite3_finalize(s);
    db_exec("COMMIT");
    /* playlists need the songs loaded to match up */
    music_reload();
    db_exec("BEGIN");
    for (int i = 0; i < nplists; i++) import_playlist(plists[i]);
    /* playlists whose file is gone */
    s = q("SELECT id, source FROM mplaylists WHERE source IS NOT NULL AND source != ''");
    int gone[256], ng = 0;
    while (s && sqlite3_step(s) == SQLITE_ROW && ng < 256) {
        const char *src = col(s, 1);
        int under = 0, seen = 0;
        for (int r = 0; r < nroots; r++) {
            size_t l = strlen(roots[r]);
            if (roots_ok[r] && !strncasecmp(src, roots[r], l)) under = 1;
        }
        for (int k = 0; k < nplists; k++)
            if (!strcmp(plists[k], src)) seen = 1;
        if (under && !seen) gone[ng++] = sqlite3_column_int(s, 0);
    }
    if (s) sqlite3_finalize(s);
    for (int i = 0; i < ng; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "DELETE FROM mpl_items WHERE pid = %d; DELETE FROM mplaylists WHERE id = %d;", gone[i], gone[i]);
        db_exec(sql);
    }
    db_exec("COMMIT");
    music_reload();
    for (int i = 0; i < nfound; i++) free(found[i].path);
    nfound = 0;
}

/* ---------------- main-loop side ---------------- */

extern volatile int g_mus_started_track; /* set by the engine when a song begins */

void music_frame(float dt) {
    mus_state_tick(dt);
    {
        static float since;
        static int started;
        since += dt;
        if (!started && since > 4.0f) {
            started = 1;
            if (!M.scanning) music_scan_start();
        }
    }
    if (M.scanning && scan_done) {
        if (scan_th >= 0) {
            SceUInt to = 1000000;
            sceKernelWaitThreadEnd(scan_th, NULL, &to);
            scan_th = -1;
        }
        int before = M.nt;
        last_nroots = nroots;
        if (scan_cancel) { /* stopped half way: keep everything as it was */
            for (int i = 0; i < nfound; i++) free(found[i].path);
            nfound = 0;
        } else {
            rep_n = nroots;
            for (int i = 0; i < nroots; i++) {
                snprintf(rep_roots[i], 512, "%s", roots[i]);
                rep_ok[i] = roots_ok[i];
                rep_songs[i] = roots_songs[i];
                rep_picked[i] = roots_picked[i];
            }
            scan_commit();
        }
        M.scanning = 0;
        if (rescan_after) {
            rescan_after = 0;
            music_scan_start();
        }
        if (M.nt > before && before > 0) {
            char m[64];
            snprintf(m, sizeof(m), "%d new songs in Music", M.nt - before);
            toast(m);
        }
    }
    int st = g_mus_started_track;
    if (st >= 0 && st < M.nt) {
        g_mus_started_track = -1;
        MTrack *t = &M.t[st];
        t->plays++;
        t->last_played = now_s();
        sqlite3_stmt *s = q("INSERT INTO mstats(path, liked, plays, last) VALUES(?, 0, 1, ?)"
                            " ON CONFLICT(path) DO UPDATE SET plays = plays + 1, last = excluded.last");
        if (s) {
            sqlite3_bind_text(s, 1, t->path, -1, SQLITE_STATIC);
            sqlite3_bind_int64(s, 2, t->last_played);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        if (t->album_i >= 0 && t->album_i < M.na) M.a[t->album_i].last_played = t->last_played;
    }
}

void music_init(void) {
    schema();
    eq_load();
    sceIoMkdir(MUSIC_DIR, 0777);
    sceIoMkdir(ART_DIR, 0777);
    music_reload();
    mus_state_restore();
    /* the search for new songs starts a few seconds after launch, so opening Nest stays quick */
}

/* ---------------- search ---------------- */

int music_search(const char *qs, int *tracks, int *nt, int *albums, int *na, int *artists, int *nr, int max) {
    *nt = *na = *nr = 0;
    if (!qs || !qs[0]) return 0;
    /* starts-with first, then contains */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < M.nr && *nr < max; i++) {
            const char *n = M.r[i].name;
            int pre = !strncasecmp(n, qs, strlen(qs)) || !strncasecmp(sort_key(n), qs, strlen(qs));
            if (pass == 0 ? pre : (!pre && ci_find(n, qs))) artists[(*nr)++] = i;
        }
        for (int i = 0; i < M.na && *na < max; i++) {
            const char *n = M.a[i].name;
            int pre = !strncasecmp(n, qs, strlen(qs));
            if (pass == 0 ? pre : (!pre && (ci_find(n, qs) || ci_find(M.a[i].artist, qs)))) albums[(*na)++] = i;
        }
        for (int i = 0; i < M.nt && *nt < max; i++) {
            const char *n = M.t[i].title;
            int pre = !strncasecmp(n, qs, strlen(qs));
            if (pass == 0 ? pre : (!pre && (ci_find(n, qs) || ci_find(M.t[i].artist, qs) || ci_find(M.t[i].album, qs))))
                tracks[(*nt)++] = i;
        }
    }
    return *nt + *na + *nr;
}

/* ---------------- likes and playlists ---------------- */

void music_set_liked(int ti, int v) {
    if (ti < 0 || ti >= M.nt) return;
    MTrack *t = &M.t[ti];
    t->liked = v ? (int)(now_s() & 0x7FFFFFFF) : 0;
    sqlite3_stmt *s = q("INSERT INTO mstats(path, liked, plays, last) VALUES(?, ?, 0, 0)"
                        " ON CONFLICT(path) DO UPDATE SET liked = excluded.liked");
    if (s) {
        sqlite3_bind_text(s, 1, t->path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 2, t->liked);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    /* rebuild Liked Songs in place */
    MPlaylist *lk = &M.p[0];
    lk->nt = 0;
    lk->dur = 0;
    for (int i = 0; i < M.nt; i++)
        if (M.t[i].liked) lk->tracks[lk->nt++] = i;
    qsort(lk->tracks, lk->nt, sizeof(int), cmp_liked);
    for (int k = 0; k < lk->nt; k++) lk->dur += M.t[lk->tracks[k]].dur;
}

int music_playlist_create(const char *name) {
    sqlite3_stmt *s = q("INSERT INTO mplaylists(name, descr, cover, source, created) VALUES(?, '', '', NULL, ?)");
    if (!s) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, now_s());
    sqlite3_step(s);
    sqlite3_finalize(s);
    int id = (int)sqlite3_last_insert_rowid(db_handle());
    music_reload();
    for (int i = 0; i < M.np; i++)
        if (M.p[i].id == id) return i;
    return -1;
}

static void playlist_save_items(MPlaylist *p) {
    char sql[64];
    snprintf(sql, sizeof(sql), "DELETE FROM mpl_items WHERE pid = %d", p->id);
    db_exec("BEGIN");
    db_exec(sql);
    sqlite3_stmt *s = q("INSERT INTO mpl_items(pid, pos, path, hint) VALUES(?,?,?, '')");
    for (int i = 0; s && i < p->nt; i++) {
        sqlite3_reset(s);
        sqlite3_bind_int(s, 1, p->id);
        sqlite3_bind_int(s, 2, i);
        sqlite3_bind_text(s, 3, M.t[p->tracks[i]].path, -1, SQLITE_STATIC);
        sqlite3_step(s);
    }
    if (s) sqlite3_finalize(s);
    db_exec("COMMIT");
}

void music_playlist_add(int pi, int ti) {
    if (pi < 0 || pi >= M.np || ti < 0) return;
    MPlaylist *p = &M.p[pi];
    if (p->liked_list) { music_set_liked(ti, 1); return; }
    p->tracks = realloc(p->tracks, sizeof(int) * (p->nt + 1));
    p->tracks[p->nt++] = ti;
    p->dur += M.t[ti].dur;
    sqlite3_stmt *s = q("INSERT INTO mpl_items(pid, pos, path, hint) VALUES(?, ?, ?, '')");
    if (s) {
        sqlite3_bind_int(s, 1, p->id);
        sqlite3_bind_int(s, 2, 100000 + p->nt);
        sqlite3_bind_text(s, 3, M.t[ti].path, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

void music_playlist_remove(int pi, int pos) {
    if (pi < 0 || pi >= M.np) return;
    MPlaylist *p = &M.p[pi];
    if (pos < 0 || pos >= p->nt) return;
    if (p->liked_list) { music_set_liked(p->tracks[pos], 0); return; }
    p->dur -= M.t[p->tracks[pos]].dur;
    memmove(p->tracks + pos, p->tracks + pos + 1, sizeof(int) * (p->nt - pos - 1));
    p->nt--;
    playlist_save_items(p);
}

void music_playlist_move(int pi, int from, int to) {
    if (pi < 0 || pi >= M.np) return;
    MPlaylist *p = &M.p[pi];
    if (p->liked_list || from < 0 || to < 0 || from >= p->nt || to >= p->nt || from == to) return;
    int v = p->tracks[from];
    if (from < to) memmove(p->tracks + from, p->tracks + from + 1, sizeof(int) * (to - from));
    else memmove(p->tracks + to + 1, p->tracks + to, sizeof(int) * (from - to));
    p->tracks[to] = v;
    playlist_save_items(p);
}

void music_playlist_rename(int pi, const char *name) {
    if (pi <= 0 || pi >= M.np || !name || !name[0]) return;
    sqlite3_stmt *s = q("UPDATE mplaylists SET name = ? WHERE id = ?");
    if (s) {
        sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 2, M.p[pi].id);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    free(M.p[pi].name);
    M.p[pi].name = sdup(name);
}

void music_playlist_delete(int pi) {
    if (pi <= 0 || pi >= M.np) return;
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM mpl_items WHERE pid = %d; DELETE FROM mplaylists WHERE id = %d;", M.p[pi].id, M.p[pi].id);
    db_exec(sql);
    /* a playlist that came from a file would come back on the next scan: remember it's hidden */
    if (M.p[pi].source && M.p[pi].source[0]) {
        char hk[600];
        snprintf(hk, sizeof(hk), "mplhide:%s", M.p[pi].source);
        db_set(hk, "1");
    }
    music_reload();
}

/* ---------------- recents ---------------- */

static void recent_key(int kind, int idx, char *k, int n) {
    k[0] = 0;
    if (kind == 0 && idx < M.na) snprintf(k, n, "%s|%s", M.a[idx].name, M.a[idx].artist);
    else if (kind == 1 && idx < M.nr) snprintf(k, n, "%s", M.r[idx].name);
    else if (kind == 2 && idx < M.np) snprintf(k, n, "%d", M.p[idx].id);
    else if (kind == 3 && idx < M.nt) snprintf(k, n, "%s", M.t[idx].path);
}

void music_note_recent(int kind, int idx) {
    if (idx < 0) return;
    char k[600];
    recent_key(kind, idx, k, sizeof(k));
    if (!k[0]) return;
    sqlite3_stmt *s = q("INSERT OR REPLACE INTO mrecent(kind, key, t) VALUES(?,?,?)");
    if (s) {
        sqlite3_bind_int(s, 1, kind);
        sqlite3_bind_text(s, 2, k, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 3, now_s());
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    if (kind == 2 && M.p[idx].id >= 0) {
        char sql[96];
        snprintf(sql, sizeof(sql), "UPDATE mplaylists SET last = %lld WHERE id = %d", (long long)now_s(), M.p[idx].id);
        db_exec(sql);
    }
}

int music_recents(int *kinds, int *idx, int max) {
    sqlite3_stmt *s = q("SELECT kind, key FROM mrecent WHERE kind < 3 ORDER BY t DESC LIMIT 60");
    int n = 0;
    while (s && sqlite3_step(s) == SQLITE_ROW && n < max) {
        int kind = sqlite3_column_int(s, 0);
        const char *k = col(s, 1);
        int found = -1;
        if (kind == 0) {
            for (int i = 0; i < M.na && found < 0; i++) {
                char kk[600];
                snprintf(kk, sizeof(kk), "%s|%s", M.a[i].name, M.a[i].artist);
                if (!strcmp(kk, k)) found = i;
            }
        } else if (kind == 1) {
            for (int i = 0; i < M.nr && found < 0; i++)
                if (!strcmp(M.r[i].name, k)) found = i;
        } else {
            int id = atoi(k);
            for (int i = 0; i < M.np && found < 0; i++)
                if (M.p[i].id == id) found = i;
        }
        if (found >= 0) {
            kinds[n] = kind;
            idx[n] = found;
            n++;
        }
    }
    if (s) sqlite3_finalize(s);
    return n;
}

/* ---------------- music folders you pick ---------------- */

void music_add_root(const char *path_in) {
    if (!path_in || !path_in[0]) return;
    char path[512];
    snprintf(path, sizeof(path), "%s", path_in);
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/' && path[pl - 2] != ':') path[--pl] = 0;
    char k[600];
    snprintf(k, sizeof(k), "music_root:%s", path);
    db_set(k, path);
    char m[160];
    const char *b = strrchr(path, '/');
    snprintf(m, sizeof(m), "Looking for music in %s" "\xe2\x80\xa6", b ? b + 1 : path);
    toast(m);
    music_scan_start();
}

void music_remove_root(const char *path) {
    char sql[700];
    sqlite3_stmt *s = q("DELETE FROM settings WHERE k = ?");
    snprintf(sql, sizeof(sql), "music_root:%s", path);
    if (s) {
        sqlite3_bind_text(s, 1, sql, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    /* its songs go too (unless another folder still covers them) */
    s = q("DELETE FROM mtracks WHERE path LIKE ?");
    snprintf(sql, sizeof(sql), "%s/%%", path);
    if (s) {
        sqlite3_bind_text(s, 1, sql, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    music_reload();
    music_scan_start();
}

int music_roots(char out[][512], int max) {
    int n = 0;
    sqlite3_stmt *s = q("SELECT v FROM settings WHERE k LIKE 'music_root:%' ORDER BY v");
    while (s && sqlite3_step(s) == SQLITE_ROW && n < max) snprintf(out[n++], 512, "%s", col(s, 0));
    if (s) sqlite3_finalize(s);
    return n;
}

int music_report(char lines[][160], int max) {
    int n = 0;
    for (int i = 0; i < rep_n && n < max; i++) {
        if (!rep_ok[i] && !rep_picked[i]) continue; /* the usual places that just aren't there: not interesting */
        if (!rep_ok[i]) snprintf(lines[n++], 160, "%s  \xc2\xb7  can't open this folder", rep_roots[i]);
        else snprintf(lines[n++], 160, "%s  \xc2\xb7  %d song%s", rep_roots[i], rep_songs[i], rep_songs[i] == 1 ? "" : "s");
    }
    return n;
}

int music_track_by_path(const char *p) { return track_by_path(p); }
