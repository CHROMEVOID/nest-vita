/* In-memory library: loaded from SQLite, with derived state such as what to
   resume, what is up next, search and on-device recommendations. */
#include "nest.h"

#include "sqlite3.h"

sqlite3 *db_handle(void);
Library L;

static char *dupn(const unsigned char *s) { return strdup(s ? (const char *)s : ""); }
static char *dupnull(const unsigned char *s) { return s && *s ? strdup((const char *)s) : NULL; }

static void lib_free(void) {
    for (int i = 0; i < L.nt; i++) {
        Title *t = &L.t[i];
        free(t->title); free(t->orig_title); free(t->overview); free(t->genres);
        free(t->poster); free(t->backdrop); free(t->logo); free(t->key);
    }
    for (int i = 0; i < L.ne; i++) {
        free(L.e[i].name);
        free(L.e[i].path);
    }
    free(L.t);
    free(L.e);
    memset(&L, 0, sizeof(L));
}

int lib_load(void) {
    sqlite3 *db = db_handle();
    if (!db) return -1;
    lib_free();
    sqlite3_stmt *s;
    int cap = 256;
    L.t = calloc(cap, sizeof(Title));
    if (sqlite3_prepare_v2(db,
                           "SELECT id, kind, CASE WHEN u_secret>=0 THEN u_secret ELSE secret END, COALESCE(u_title, title),"
                           " title, year, overview, genres, COALESCE(u_poster, poster), COALESCE(u_backdrop, backdrop),"
                           " COALESCE(u_logo, logo), favorite, rating, score, added, key FROM titles"
                           " WHERE id IN (SELECT DISTINCT tid FROM entries) ORDER BY id",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (L.nt == cap) {
            cap *= 2;
            Title *nt = realloc(L.t, sizeof(Title) * cap);
            if (!nt) break;
            L.t = nt;
        }
        Title *t = &L.t[L.nt++];
        memset(t, 0, sizeof(*t));
        t->id = sqlite3_column_int(s, 0);
        t->kind = sqlite3_column_int(s, 1);
        t->secret = sqlite3_column_int(s, 2);
        t->title = dupn(sqlite3_column_text(s, 3));
        t->orig_title = dupn(sqlite3_column_text(s, 4));
        t->year = sqlite3_column_int(s, 5);
        t->overview = dupnull(sqlite3_column_text(s, 6));
        t->genres = dupnull(sqlite3_column_text(s, 7));
        t->poster = dupnull(sqlite3_column_text(s, 8));
        t->backdrop = dupnull(sqlite3_column_text(s, 9));
        t->logo = dupnull(sqlite3_column_text(s, 10));
        t->favorite = sqlite3_column_int(s, 11);
        t->rating = sqlite3_column_int(s, 12);
        t->score = (float)sqlite3_column_double(s, 13);
        t->added = sqlite3_column_int64(s, 14);
        t->key = dupn(sqlite3_column_text(s, 15));
        t->resume = -1;
        t->present = 1;
    }
    sqlite3_finalize(s);

    int ecap = 1024;
    L.e = calloc(ecap, sizeof(Entry));
    if (sqlite3_prepare_v2(db,
                           "SELECT e.id, e.tid, e.season, e.number, e.number_end, e.name, e.path, e.size, e.playable,"
                           " e.media, COALESCE(p.pos,0), COALESCE(p.dur,0), COALESCE(p.finished,0), COALESCE(p.updated,0),"
                           " COALESCE(e.aspect,0)"
                           " FROM entries e LEFT JOIN progress p ON p.path = e.path"
                           " ORDER BY e.tid, e.season = 0, e.season, e.number, e.path",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    int ti = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int tid = sqlite3_column_int(s, 1);
        while (ti < L.nt && L.t[ti].id < tid) ti++;
        if (ti >= L.nt || L.t[ti].id != tid) continue;
        if (L.ne == ecap) {
            ecap *= 2;
            Entry *ne = realloc(L.e, sizeof(Entry) * ecap);
            if (!ne) break;
            L.e = ne;
        }
        Entry *e = &L.e[L.ne];
        e->id = sqlite3_column_int(s, 0);
        e->tidx = ti;
        e->season = sqlite3_column_int(s, 2);
        e->number = sqlite3_column_int(s, 3);
        e->number_end = sqlite3_column_int(s, 4);
        e->name = dupn(sqlite3_column_text(s, 5));
        e->path = dupn(sqlite3_column_text(s, 6));
        e->size = sqlite3_column_int64(s, 7);
        e->playable = sqlite3_column_int(s, 8);
        e->media = sqlite3_column_int(s, 9);
        e->pos = sqlite3_column_int(s, 10);
        e->dur = sqlite3_column_int(s, 11);
        e->finished = sqlite3_column_int(s, 12);
        e->updated = sqlite3_column_int64(s, 13);
        e->aspect = (float)sqlite3_column_double(s, 14);
        if (L.t[ti].ne == 0) L.t[ti].e0 = L.ne;
        L.t[ti].ne++;
        L.ne++;
    }
    sqlite3_finalize(s);
    lib_derive();
    thumb_reset();
    return 0;
}

void lib_derive(void) {
    L.nmovies = L.nshows = L.nbooks = L.nepisodes = 0;
    for (int i = 0; i < L.nt; i++) {
        Title *t = &L.t[i];
        t->last_used = 0;
        t->resume = -1;
        t->watched = 0;
        int last = -1;
        int seasons = 0, prev_season = -999;
        double pos_sum = 0;
        for (int k = t->e0; k < t->e0 + t->ne; k++) {
            Entry *e = &L.e[k];
            if (e->finished) t->watched++;
            if (e->updated > t->last_used) {
                t->last_used = e->updated;
                last = k;
            }
            if (e->season != prev_season) {
                seasons++;
                prev_season = e->season;
            }
            if (e->dur > 0) pos_sum += e->finished ? 1.0 : (double)e->pos / e->dur;
        }
        t->seasons = seasons;
        if (last >= 0) {
            Entry *e = &L.e[last];
            int is_book = e->media == MEDIA_BOOK;
            int started = is_book ? e->pos > 0 : e->pos > 20000;
            if (!e->finished && started) {
                t->resume = last;
            } else if (e->finished) {
                int nx = lib_next_entry(last);
                if (nx >= 0 && !L.e[nx].finished) t->resume = nx;
            } else if (!started && t->ne > 1) {
                t->resume = last;
            }
        }
        if (t->kind == KIND_MOVIE) {
            Entry *e = &L.e[t->e0];
            t->progress = e->finished ? 1 : (e->dur > 0 ? (float)e->pos / e->dur : 0);
        } else {
            t->progress = t->ne ? (float)(pos_sum / t->ne) : 0;
        }
        if (t->secret) continue;
        if (t->kind == KIND_MOVIE) L.nmovies++;
        else if (t->kind == KIND_SERIES) L.nshows++, L.nepisodes += t->ne;
        else L.nbooks += t->ne;
    }
}

Title *lib_title_by_id(int id) {
    int i = lib_title_index(id);
    return i >= 0 ? &L.t[i] : NULL;
}

int lib_title_index(int id) {
    int a = 0, b = L.nt - 1;
    while (a <= b) {
        int m = (a + b) / 2;
        if (L.t[m].id == id) return m;
        if (L.t[m].id < id) a = m + 1;
        else b = m - 1;
    }
    return -1;
}

int lib_next_entry(int eidx) {
    if (eidx < 0 || eidx >= L.ne) return -1;
    Title *t = &L.t[L.e[eidx].tidx];
    return eidx + 1 < t->e0 + t->ne ? eidx + 1 : -1;
}

int lib_prev_entry(int eidx) {
    if (eidx < 0 || eidx >= L.ne) return -1;
    Title *t = &L.t[L.e[eidx].tidx];
    return eidx - 1 >= t->e0 ? eidx - 1 : -1;
}

void lib_progress(Entry *e, int pos, int dur, int finished, int seconds) {
    Title *t = &L.t[e->tidx];
    if (S.private_session) return;
    e->pos = pos;
    e->dur = dur;
    if (finished) e->finished = 1;
    e->updated = now_s();
    db_save_progress(e->path, pos, dur, e->finished);
    if (seconds > 0) db_add_activity(t->id, t->kind, t->secret, seconds);
    lib_derive();
}

const char *lib_entry_label(Entry *e, char *buf, int n) {
    Title *t = &L.t[e->tidx];
    if (t->kind == KIND_MOVIE) snprintf(buf, n, "%s", "Movie");
    else if (t->kind == KIND_BOOK) {
        if (t->ne > 1) snprintf(buf, n, "Vol. %d", e->number);
        else snprintf(buf, n, "Book");
    } else if (e->season == 0) snprintf(buf, n, "Special %d", e->number);
    else if (e->number_end > e->number) snprintf(buf, n, "S%d \xc2\xb7 E%d\xe2\x80\x93%d", e->season, e->number, e->number_end);
    else snprintf(buf, n, "S%d \xc2\xb7 E%d", e->season, e->number);
    return buf;
}

const char *title_poster(Title *t) {
    static char buf[4][600];
    static int bi;
    if (t->poster) return t->poster;
    if (t->kind == KIND_BOOK && t->ne > 0) {
        Entry *e = &L.e[t->e0];
        const char *d = strrchr(e->path, '.');
        if (d && (!strcasecmp(d, ".cbz") || !strcasecmp(d, ".cbr") || !strcasecmp(d, ".epub"))) {
            char *b = buf[bi++ & 3];
            snprintf(b, 600, "cover:%s", e->path);
            return b;
        }
    }
    return NULL;
}

/* ---------------- queries ---------------- */

static int g_sort;
static int cmp_title(const void *a, const void *b) {
    const Title *x = &L.t[*(const int *)a], *y = &L.t[*(const int *)b];
    switch (g_sort) {
    case SORT_RECENT:
        if (x->added != y->added) return x->added > y->added ? -1 : 1;
        if (x->id != y->id) return y->id - x->id;
        break;
    case SORT_YEAR:
        if (x->year != y->year) return y->year - x->year;
        break;
    case SORT_RATING: {
        float rx = x->rating ? x->rating * 2.0f : x->score, ry = y->rating ? y->rating * 2.0f : y->score;
        if (rx != ry) return rx > ry ? -1 : 1;
        break;
    }
    case SORT_PROGRESS:
        if (x->last_used != y->last_used) return x->last_used > y->last_used ? -1 : 1;
        break;
    }
    const char *p = x->title, *q = y->title;
    if (!strncasecmp(p, "the ", 4)) p += 4;
    if (!strncasecmp(q, "the ", 4)) q += 4;
    return strcasecmp(p, q);
}

static void sort_idx(int *v, int n, int sort) {
    g_sort = sort;
    qsort(v, n, sizeof(int), cmp_title);
}

static int visible(Title *t, int secret) {
    if (secret == 2) return 1;
    return t->secret == secret;
}

int lib_query(int kind, int secret, int sort, int filter, int *out, int max) {
    int n = 0;
    for (int i = 0; i < L.nt && n < max; i++) {
        Title *t = &L.t[i];
        if (!visible(t, secret)) continue;
        if (kind >= 0 && t->kind != kind) continue;
        if (filter == 1 && t->progress >= 0.999f) continue;
        if (filter == 2 && !t->favorite) continue;
        if (filter == 3 && t->last_used == 0) continue;
        out[n++] = i;
    }
    sort_idx(out, n, sort);
    return n;
}

int lib_continue(int kind, int secret, int *out, int max) {
    int n = 0;
    for (int i = 0; i < L.nt && n < max; i++) {
        Title *t = &L.t[i];
        if (!visible(t, secret) || t->resume < 0) continue;
        if (kind == -2 ? t->kind == KIND_BOOK : kind >= 0 && t->kind != kind) continue;
        out[n++] = i;
    }
    sort_idx(out, n, SORT_PROGRESS);
    return n;
}

int lib_recent(int kind, int secret, int *out, int max) {
    int n = 0;
    for (int i = 0; i < L.nt && n < max * 4 && n < 400; i++) {
        Title *t = &L.t[i];
        if (!visible(t, secret)) continue;
        if (kind == -2 ? t->kind == KIND_BOOK : kind >= 0 && t->kind != kind) continue;
        out[n++] = i;
    }
    sort_idx(out, n, SORT_RECENT);
    return n < max ? n : max;
}

int lib_favorites(int secret, int *out, int max) {
    int n = 0;
    for (int i = 0; i < L.nt && n < max; i++)
        if (visible(&L.t[i], secret) && L.t[i].favorite) out[n++] = i;
    sort_idx(out, n, SORT_AZ);
    return n;
}

static int genre_overlap(const char *a, const char *b) {
    if (!a || !b) return 0;
    int score = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", a);
    char *save = NULL;
    for (char *g = strtok_r(buf, ",", &save); g; g = strtok_r(NULL, ",", &save)) {
        while (*g == ' ') g++;
        if (*g && strstr(b, g)) score++;
    }
    return score;
}

/* "Because you watched X": seed = most recently enjoyed title */
int lib_because(int secret, int *seed, int *out, int max) {
    int best = -1;
    for (int i = 0; i < L.nt; i++) {
        Title *t = &L.t[i];
        if (!visible(t, secret) || t->kind == KIND_BOOK || !t->last_used) continue;
        if (best < 0 || t->last_used + t->rating * 86400 > L.t[best].last_used + L.t[best].rating * 86400) best = i;
    }
    *seed = best;
    if (best < 0) return 0;
    Title *s = &L.t[best];
    static float sc[4096];
    int n = 0;
    int cand[4096];
    for (int i = 0; i < L.nt && n < 4096; i++) {
        Title *t = &L.t[i];
        if (i == best || !visible(t, secret) || t->kind == KIND_BOOK) continue;
        if (t->progress >= 0.999f) continue;
        float v = 0;
        v += genre_overlap(s->genres, t->genres) * 3.0f;
        if (t->kind == s->kind) v += 1.5f;
        if (s->year && t->year) v += fmaxf(0, 2.0f - fabsf((float)(s->year - t->year)) / 8.0f);
        v += t->score * 0.15f + t->rating * 0.4f;
        if (t->favorite) v += 0.8f;
        if (t->last_used == 0) v += 0.6f;  /* new to you */
        /* deterministic daily shuffle so the row feels alive */
        unsigned h = (unsigned)(t->id * 2654435761u) ^ (unsigned)today_ymd();
        v += (h % 100) / 140.0f;
        cand[n] = i;
        sc[i < 4096 ? i : 4095] = v;
        n++;
    }
    /* partial selection sort */
    int m = 0;
    for (; m < max && m < n; m++) {
        int bi = m;
        for (int k = m + 1; k < n; k++)
            if (sc[cand[k] < 4096 ? cand[k] : 4095] > sc[cand[bi] < 4096 ? cand[bi] : 4095]) bi = k;
        int tmp = cand[m];
        cand[m] = cand[bi];
        cand[bi] = tmp;
        out[m] = cand[m];
    }
    return m;
}

static int ci_find(const char *hay, const char *needle) {
    if (!hay || !*needle) return -1;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, needle, n)) return (int)(p - hay);
    return -1;
}

int lib_search(const char *q, int secret, int *out, int max) {
    if (!q || !*q) return 0;
    static int scores[8192];
    int n = 0;
    for (int i = 0; i < L.nt && n < max && n < 8192; i++) {
        Title *t = &L.t[i];
        if (!visible(t, secret)) continue;
        int s = 0, p;
        if ((p = ci_find(t->title, q)) >= 0) s = p == 0 ? 100 : (t->title[p - 1] == ' ' ? 70 : 40);
        else if ((p = ci_find(t->orig_title, q)) >= 0) s = 35;
        else if (t->genres && ci_find(t->genres, q) >= 0) s = 20;
        else {
            char y[8];
            snprintf(y, sizeof(y), "%d", t->year);
            if (t->year && !strcmp(y, q)) s = 30;
            for (int k = t->e0; !s && k < t->e0 + t->ne && k < t->e0 + 200; k++)
                if (ci_find(path_basename(L.e[k].path), q) >= 0 || ci_find(L.e[k].name, q) >= 0) s = 15;
        }
        if (s) {
            if (t->last_used) s += 5;
            scores[n] = s;
            out[n++] = i;
        }
    }
    for (int a = 1; a < n; a++) {
        int v = out[a], sv = scores[a], b = a - 1;
        while (b >= 0 && scores[b] < sv) {
            out[b + 1] = out[b];
            scores[b + 1] = scores[b];
            b--;
        }
        out[b + 1] = v;
        scores[b + 1] = sv;
    }
    return n;
}

int lib_featured(int secret, int *out, int max) {
    int n = 0;
    int tmp[64];
    int c = lib_continue(-1, secret, tmp, 2);
    for (int i = 0; i < c && n < max; i++)
        if (L.t[tmp[i]].kind != KIND_BOOK) out[n++] = tmp[i];
    /* then titles with artwork, rotating daily */
    int pool[512], np = 0;
    for (int i = 0; i < L.nt && np < 512; i++) {
        Title *t = &L.t[i];
        if (!visible(t, secret) || t->kind == KIND_BOOK) continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (out[k] == i) dup = 1;
        if (!dup) pool[np++] = i;
    }
    unsigned seed = (unsigned)today_ymd();
    for (int i = np - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        int j = (int)((seed >> 8) % (unsigned)(i + 1));
        int x = pool[i];
        pool[i] = pool[j];
        pool[j] = x;
    }
    /* prefer ones with a backdrop or poster */
    for (int pass = 0; pass < 2 && n < max; pass++)
        for (int i = 0; i < np && n < max; i++) {
            Title *t = &L.t[pool[i]];
            int art = t->backdrop || t->poster;
            if ((pass == 0) != (art != 0)) continue;
            out[n++] = pool[i];
        }
    return n;
}

/* ---------------- edits ---------------- */

void lib_set_favorite(Title *t, int v) {
    t->favorite = v;
    db_title_set_int(t->id, "favorite", v);
}
void lib_set_rating(Title *t, int v) {
    t->rating = v;
    db_title_set_int(t->id, "rating", v);
}
void lib_set_secret(Title *t, int v) {
    t->secret = v;
    db_title_set_int(t->id, "u_secret", v);
    lib_derive();
}
void lib_set_title(Title *t, const char *s) {
    free(t->title);
    t->title = strdup(s && *s ? s : t->orig_title);
    db_title_set_str(t->id, "u_title", s && *s ? s : NULL);
}
void lib_set_art(Title *t, int which, const char *path) {
    char **f = which == 0 ? &t->poster : which == 1 ? &t->backdrop : &t->logo;
    free(*f);
    *f = path ? strdup(path) : NULL;
    db_title_set_str(t->id, which == 0 ? "u_poster" : which == 1 ? "u_backdrop" : "u_logo", path);
}
void lib_mark_watched(Title *t, int watched) {
    for (int k = t->e0; k < t->e0 + t->ne; k++) {
        Entry *e = &L.e[k];
        e->finished = watched;
        if (!watched) e->pos = 0;
        e->updated = watched ? now_s() : 0;
        db_save_progress(e->path, e->pos, e->dur, watched);
    }
    lib_derive();
}
