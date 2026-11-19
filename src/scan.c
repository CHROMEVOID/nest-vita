/* Recursive library scanner: turns folders of files into Movies, Shows
   (with seasons, even when every season sits in its own folder) and Books. */
#include "scan.h"
#include "mtags.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "parse.h"

#ifdef __vita__
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
int net_list(const char *dir, fs_cb cb, void *ud);
int net_is_url(const char *p);
int fs_list(const char *dir, fs_cb cb, void *ud) {
    if (!strncmp(dir, "http://", 7)) return net_list(dir, cb, ud); /* a folder on the PC */
    SceUID d = sceIoDopen(dir);
    if (d < 0) return -1;
    SceIoDirent ent;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, "..")) continue;
        cb(ud, ent.d_name, SCE_S_ISDIR(ent.d_stat.st_mode), (long long)ent.d_stat.st_size);
    }
    sceIoDclose(d);
    return 0;
}
int fs_exists(const char *path) {
    if (!strncmp(path, "http://", 7)) return 1;
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}
#else
#include <dirent.h>
#include <sys/stat.h>
int fs_list(const char *dir, fs_cb cb, void *ud) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        path_join(dir, e->d_name, p, sizeof(p));
        if (stat(p, &st) != 0) continue;
        cb(ud, e->d_name, S_ISDIR(st.st_mode), (long long)st.st_size);
    }
    closedir(d);
    return 0;
}
int fs_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}
#endif

void path_join(const char *dir, const char *name, char *out, int n) {
    size_t l = strlen(dir);
    if (l && (dir[l - 1] == '/' || dir[l - 1] == ':'))
        snprintf(out, n, "%s%s%s", dir, dir[l - 1] == ':' ? "/" : "", name);
    else
        snprintf(out, n, "%s/%s", dir, name);
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---------- directory listing into arrays ---------- */

typedef struct {
    char name[256];
    long long size;
    int is_dir;
    int media;
} DEnt;

typedef struct {
    DEnt *v;
    int n, cap;
} DList;

static void dl_cb(void *ud, const char *name, int is_dir, long long size) {
    DList *l = ud;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        DEnt *nv = realloc(l->v, sizeof(DEnt) * nc);
        if (!nv) return;
        l->v = nv;
        l->cap = nc;
    }
    DEnt *e = &l->v[l->n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->is_dir = is_dir;
    e->size = size;
    e->media = is_dir ? MEDIA_NONE : media_type_of(name);
}

static int dent_cmp(const void *a, const void *b) {
    const DEnt *x = a, *y = b;
    /* natural-ish ordering: compare digit runs numerically */
    const char *p = x->name, *q = y->name;
    while (*p && *q) {
        if (isdigit((unsigned char)*p) && isdigit((unsigned char)*q)) {
            long a1 = strtol(p, (char **)&p, 10), b1 = strtol(q, (char **)&q, 10);
            if (a1 != b1) return a1 < b1 ? -1 : 1;
            continue;
        }
        int ca = tolower((unsigned char)*p), cb = tolower((unsigned char)*q);
        if (ca != cb) return ca - cb;
        p++, q++;
    }
    return (unsigned char)*p - (unsigned char)*q;
}

/* ---------- artwork in a folder ---------- */

typedef struct {
    char dir[512];
    char poster[512], backdrop[512], logo[512], meta[512];
} DirArt;

static void lower_base_noext(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s", name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

static void find_dir_art(const char *dir, DList *l, DirArt *a) {
    memset(a, 0, sizeof(*a));
    snprintf(a->dir, sizeof(a->dir), "%s", dir);
    for (int i = 0; i < l->n; i++) {
        DEnt *e = &l->v[i];
        if (e->is_dir) continue;
        char full[512];
        if (e->media == MEDIA_META) {
            path_join(dir, e->name, full, sizeof(full));
            snprintf(a->meta, sizeof(a->meta), "%s", full);
            continue;
        }
        if (e->media != MEDIA_IMAGE) continue;
        char b[256];
        lower_base_noext(e->name, b, sizeof(b));
        path_join(dir, e->name, full, sizeof(full));
        if (!strcmp(b, "poster") || !strcmp(b, "cover") || !strcmp(b, "folder") || !strcmp(b, "show") ||
            !strcmp(b, "movie") || !strcmp(b, "default") || !strcmp(b, "nest-poster")) {
            if (!a->poster[0] || !strcmp(b, "poster") || !strcmp(b, "nest-poster"))
                snprintf(a->poster, sizeof(a->poster), "%s", full);
        } else if (!strcmp(b, "fanart") || !strcmp(b, "backdrop") || !strcmp(b, "background") ||
                   !strcmp(b, "art") || !strcmp(b, "landscape") || !strcmp(b, "nest-backdrop")) {
            if (!a->backdrop[0] || !strcmp(b, "nest-backdrop")) snprintf(a->backdrop, sizeof(a->backdrop), "%s", full);
        } else if (!strcmp(b, "logo") || !strcmp(b, "clearlogo") || !strcmp(b, "nest-logo")) {
            snprintf(a->logo, sizeof(a->logo), "%s", full);
        }
    }
}

/* image next to a specific file: "<base>-poster.jpg", "<base>.jpg" ... */
static int find_file_art(const char *dir, DList *l, const char *filename, const char *suffix, char *out, size_t n) {
    char want[256];
    lower_base_noext(filename, want, sizeof(want));
    if (suffix) strncat(want, suffix, sizeof(want) - strlen(want) - 1);
    for (int i = 0; i < l->n; i++) {
        if (l->v[i].media != MEDIA_IMAGE) continue;
        char b[256];
        lower_base_noext(l->v[i].name, b, sizeof(b));
        if (!strcmp(b, want)) {
            char full[512];
            path_join(dir, l->v[i].name, full, sizeof(full));
            snprintf(out, n, "%s", full);
            return 1;
        }
    }
    return 0;
}

/* ---------- titles ---------- */

static ScanTitle *get_title(ScanResult *r, const char *key, int kind, int secret, const char *title, int year) {
    for (int i = 0; i < r->nt; i++)
        if (!strcmp(r->t[i].key, key)) {
            ScanTitle *t = &r->t[i];
            if (year && !t->year) t->year = year;
            return t;
        }
    if (r->nt == r->cap) {
        int nc = r->cap ? r->cap * 2 : 128;
        ScanTitle *nv = realloc(r->t, sizeof(ScanTitle) * nc);
        if (!nv) return NULL;
        r->t = nv;
        r->cap = nc;
    }
    ScanTitle *t = &r->t[r->nt++];
    memset(t, 0, sizeof(*t));
    snprintf(t->key, sizeof(t->key), "%s", key);
    t->kind = kind;
    t->secret = secret;
    t->title = xstrdup(title);
    t->year = year;
    return t;
}

static ScanEntry *add_entry(ScanTitle *t, const char *path, const char *name, long long size, int season, int num,
                            int num_end, int media) {
    for (int i = 0; i < t->ne; i++)
        if (!strcmp(t->e[i].path, path)) return &t->e[i];
    if (t->ne == t->cap) {
        int nc = t->cap ? t->cap * 2 : 8;
        ScanEntry *nv = realloc(t->e, sizeof(ScanEntry) * nc);
        if (!nv) return NULL;
        t->e = nv;
        t->cap = nc;
    }
    ScanEntry *e = &t->e[t->ne++];
    memset(e, 0, sizeof(*e));
    e->path = xstrdup(path);
    e->name = xstrdup(name ? name : "");
    e->size = size;
    e->season = season;
    e->number = num;
    e->number_end = num_end >= num ? num_end : num;
    e->media = media;
    /* a video smaller than 256 KB is a placeholder or a broken copy */
    e->playable = media == MEDIA_VIDEO ? (video_is_playable(path) && size >= 256 * 1024) : 1;
    return e;
}

static void set_art(ScanTitle *t, const DirArt *a, const DirArt *fallback) {
    const DirArt *src[2] = {a, fallback};
    for (int i = 0; i < 2; i++) {
        const DirArt *d = src[i];
        if (!d) continue;
        if (!t->poster && d->poster[0]) t->poster = xstrdup(d->poster);
        if (!t->backdrop && d->backdrop[0]) t->backdrop = xstrdup(d->backdrop);
        if (!t->logo && d->logo[0]) t->logo = xstrdup(d->logo);
        if (!t->meta && d->meta[0]) t->meta = xstrdup(d->meta);
        if (!t->artdir && d->dir[0]) t->artdir = xstrdup(d->dir);
    }
}

static int generic_title(const char *t) {
    char k[256];
    parse_norm_key(t, k, sizeof(k));
    if (!k[0]) return 1;
    static const char *g[] = {"movie", "video", "film", "episode", "untitled", "new", "downloads", "videos",
                              "movies", "tv", "tvshows", "shows", "series", "media", "complete", NULL};
    for (int i = 0; g[i]; i++)
        if (!strcmp(k, g[i])) return 1;
    return 0;
}

/* ---------- what never gets scanned ---------- */

static char excl[64][512];
static int nexcl;

void scan_set_excludes(char list[][512], int n) {
    nexcl = n > 64 ? 64 : n;
    for (int i = 0; i < nexcl; i++) snprintf(excl[i], sizeof(excl[i]), "%s", list[i]);
}

static int same_path_prefix(const char *path, const char *pre) {
    size_t n = strlen(pre);
    while (n && pre[n - 1] == '/') n--;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)pre[i])) return 0;
    return path[n] == 0 || path[n] == '/';
}

static int is_excluded(const char *path) {
    for (int i = 0; i < nexcl; i++)
        if (same_path_prefix(path, excl[i])) return 1;
    return 0;
}

/* system, game and homebrew folders that never hold your media */
static int is_system_dir(const char *parent, const char *name) {
    size_t pl = strlen(parent);
    int at_device_root = pl && (parent[pl - 1] == ':' || (pl >= 2 && parent[pl - 1] == '/' && parent[pl - 2] == ':'));
    char l[256];
    size_t n = strlen(name);
    if (n >= sizeof(l)) n = sizeof(l) - 1;
    for (size_t i = 0; i < n; i++) l[i] = (char)tolower((unsigned char)name[i]);
    l[n] = 0;
    if (!strncmp(l, "sce_", 4) || l[0] == '.' || l[0] == '$') return 1;
    static const char *always[] = {"vitashell", "repatch", "readdcont", "rexdata", "tai", "psp2", "pspemu",
                                   "appmeta", "addcont", "patch", "license", NULL};
    for (int i = 0; always[i]; i++)
        if (!strcmp(l, always[i])) return 1;
    if (!at_device_root) return 0;
    static const char *rootonly[] = {"app", "data", "cache", "temp", "user", "savedata", "picture", "music",
                                     "theme", "vpk", "bgdl", "mms", "font", "xmb", "plugins", "sys", "app_tmp",
                                     "vitadb", "iso", "cso", "pkg", NULL};
    for (int i = 0; rootonly[i]; i++)
        if (!strcmp(l, rootonly[i])) return 1;
    return 0;
}

/* text files that are release notes or homebrew leftovers, not books */
static int junk_text(const char *name, long long size) {
    const char *d = strrchr(name, '.');
    if (!d || strcasecmp(d, ".txt")) return 0;
    if (size < 40 * 1024) return 1;
    char l[256];
    snprintf(l, sizeof(l), "%s", name);
    for (char *p = l; *p; p++) *p = (char)tolower((unsigned char)*p);
    static const char *bad[] = {"readme", "read me", "license", "licence", "changelog", "credits", "rarbg", "done",
                                "install", "notes", "nfo", "torrent", "manual", NULL};
    for (int i = 0; bad[i]; i++)
        if (strstr(l, bad[i])) return 1;
    return 0;
}

/* ---------- walker ---------- */

typedef struct {
    ScanResult *r;
    const char *root;
    int secret;
    int mode_folder;
    DirArt art[16]; /* ancestors' artwork, by depth */
    char folder_key[320]; /* folder mode: current top-level folder */
    char folder_name[256];
} Walk;

static void key_for(char *out, size_t n, const char *prefix, int secret, const char *title, int year) {
    char k[256];
    parse_norm_key(title, k, sizeof(k));
    if (year > 0)
        snprintf(out, n, "%s%s:%s:%d", secret ? "x" : "", prefix, k, year);
    else
        snprintf(out, n, "%s%s:%s", secret ? "x" : "", prefix, k);
}

static void handle_videos(Walk *w, const char *dir, int depth, DList *l) {
    ScanResult *r = w->r;
    const char *dname = path_basename(dir);
    int at_root = depth == 0;
    int season_dir = at_root ? -1 : parse_season_folder(dname);

    /* count real videos in this folder */
    int nvid = 0;
    for (int i = 0; i < l->n; i++)
        if (l->v[i].media == MEDIA_VIDEO && !(parse_is_sample(l->v[i].name) && l->v[i].size < 400LL * 1024 * 1024))
            nvid++;

    /* show / folder names from context */
    char folder_title[256] = "", parent_title[256] = "";
    int folder_year = 0, parent_year = 0;
    if (!at_root) parse_clean(dname, folder_title, sizeof(folder_title), &folder_year, NULL);
    if (depth >= 1) {
        char pdir[512];
        path_dirname(dir, pdir, sizeof(pdir));
        parse_clean(path_basename(pdir), parent_title, sizeof(parent_title), &parent_year, NULL);
    }
    /* "Shameless.S02.1080p" -> folder_title already "Shameless"; "Season 2" -> "" or "Season" */
    if (season_dir >= 0) {
        char k[256];
        parse_norm_key(folder_title, k, sizeof(k));
        if (!strncmp(k, "season", 6) || !strncmp(k, "series", 6) || !strncmp(k, "specials", 8) ||
            !strncmp(k, "staffel", 7) || !strncmp(k, "saison", 6) || !k[0] ||
            (k[0] == 's' && isdigit((unsigned char)k[1])))
            folder_title[0] = 0;
    }

    /* pre-pass: are the non-SxxEyy files in this folder numbered siblings? */
    int numbered = 0;
    for (int i = 0; i < l->n; i++) {
        if (l->v[i].media != MEDIA_VIDEO) continue;
        ParsedName p;
        parse_filename(l->v[i].name, &p);
        if (p.episode >= 0) numbered++;
    }

    for (int i = 0; i < l->n; i++) {
        DEnt *de = &l->v[i];
        if (de->media != MEDIA_VIDEO) continue;
        r->files_seen++;
        if (parse_is_sample(de->name) && de->size < 400LL * 1024 * 1024) {
            r->skipped++;
            continue;
        }
        char full[512];
        path_join(dir, de->name, full, sizeof(full));
        ParsedName p;
        parse_filename(de->name, &p);

        if (w->mode_folder) {
            const char *name = w->folder_name[0] ? w->folder_name : "Unsorted";
            ScanTitle *t = get_title(r, w->folder_key[0] ? w->folder_key : (w->secret ? "xf:root" : "f:root"), KIND_SERIES, w->secret, name, 0);
            if (!t) continue;
            char nice[256];
            parse_clean(de->name, nice, sizeof(nice), NULL, NULL);
            ScanEntry *e = add_entry(t, full, nice[0] ? nice : de->name, de->size, 1, 0, 0, MEDIA_VIDEO);
            if (e) e->need_number = 1;
            set_art(t, &w->art[depth < 16 ? depth : 15], NULL);
            continue;
        }

        int is_ep = 0;
        if (p.is_episode && !(p.absolute && nvid < 2 && season_dir < 0)) is_ep = 1;
        else if (season_dir >= 0) is_ep = 1;
        else if (p.episode >= 0 && numbered >= 2 && nvid >= 2) is_ep = 1;

        if (is_ep) {
            /* decide the show name */
            char show[256] = "";
            int year = p.year;
            const char *ctx = season_dir >= 0 ? (folder_title[0] ? folder_title : parent_title) : folder_title;
            int ctx_year = season_dir >= 0 ? (folder_title[0] ? folder_year : parent_year) : folder_year;
            char kf[256], kc[256];
            parse_norm_key(p.title, kf, sizeof(kf));
            parse_norm_key(ctx, kc, sizeof(kc));
            if (p.title[0] && !generic_title(p.title)) {
                /* prefer the folder spelling when it is the same show */
                if (ctx[0] && !strcmp(kf, kc))
                    snprintf(show, sizeof(show), "%s", ctx);
                else
                    snprintf(show, sizeof(show), "%s", p.title);
            } else if (ctx[0] && !generic_title(ctx)) {
                snprintf(show, sizeof(show), "%s", ctx);
                if (!year) year = ctx_year;
            } else if (folder_title[0]) {
                snprintf(show, sizeof(show), "%s", folder_title);
            } else {
                parse_clean(path_basename(w->root), show, sizeof(show), NULL, NULL);
                if (!show[0]) snprintf(show, sizeof(show), "Unsorted Episodes");
            }
            int season = p.season >= 0 ? p.season : (season_dir >= 0 ? season_dir : 1);
            char key[320];
            key_for(key, sizeof(key), "s", w->secret, show, 0);
            ScanTitle *t = get_title(r, key, KIND_SERIES, w->secret, show, year ? year : ctx_year);
            if (!t) continue;
            int num = p.episode;
            ScanEntry *e = add_entry(t, full, p.ep_title, de->size, season, num >= 0 ? num : 0,
                                     p.episode_end, MEDIA_VIDEO);
            if (e && num < 0) e->need_number = 1;
            /* artwork: show root first, then season folder */
            int show_depth = season_dir >= 0 ? depth - 1 : depth;
            if (show_depth < 0) show_depth = 0;
            set_art(t, &w->art[show_depth < 16 ? show_depth : 15], &w->art[depth < 16 ? depth : 15]);
        } else {
            char title[256];
            int year = p.year;
            int owns_folder = !at_root && nvid == 1;
            if (p.title[0] && !generic_title(p.title)) {
                snprintf(title, sizeof(title), "%s", p.title);
                if (owns_folder && folder_title[0]) {
                    char a[256], b[256];
                    parse_norm_key(p.title, a, sizeof(a));
                    parse_norm_key(folder_title, b, sizeof(b));
                    if (!strcmp(a, b)) snprintf(title, sizeof(title), "%s", folder_title);
                    if (!year) year = folder_year;
                }
            } else if (folder_title[0]) {
                snprintf(title, sizeof(title), "%s", folder_title);
                if (!year) year = folder_year;
            } else {
                snprintf(title, sizeof(title), "%s", de->name);
            }
            char key[320];
            key_for(key, sizeof(key), "m", w->secret, title, year);
            ScanTitle *t = get_title(r, key, KIND_MOVIE, w->secret, title, year);
            if (!t) continue;
            add_entry(t, full, "", de->size, 0, 1, 1, MEDIA_VIDEO);
            char tmp[512];
            if (!t->poster && (find_file_art(dir, l, de->name, "-poster", tmp, sizeof(tmp)) ||
                               find_file_art(dir, l, de->name, NULL, tmp, sizeof(tmp))))
                t->poster = xstrdup(tmp);
            if (!t->backdrop && (find_file_art(dir, l, de->name, "-fanart", tmp, sizeof(tmp)) ||
                                 find_file_art(dir, l, de->name, "-backdrop", tmp, sizeof(tmp))))
                t->backdrop = xstrdup(tmp);
            if (!t->logo && find_file_art(dir, l, de->name, "-logo", tmp, sizeof(tmp))) t->logo = xstrdup(tmp);
            if (!t->meta) { /* "<movie>.json" from Nest Metadata, for movies sharing a folder */
                char want[300];
                snprintf(want, sizeof(want), "%s", de->name);
                char *dot = strrchr(want, '.');
                if (dot) *dot = 0;
                strncat(want, ".json", sizeof(want) - strlen(want) - 1);
                for (int k = 0; k < l->n; k++)
                    if (!l->v[k].is_dir && !strcasecmp(l->v[k].name, want)) {
                        path_join(dir, l->v[k].name, tmp, sizeof(tmp));
                        t->meta = xstrdup(tmp);
                        break;
                    }
            }
            if (owns_folder) set_art(t, &w->art[depth < 16 ? depth : 15], NULL);
        }
    }
}

static void handle_books(Walk *w, const char *dir, int depth, DList *l) {
    ScanResult *r = w->r;
    char folder_title[256] = "";
    if (depth > 0) parse_clean(path_basename(dir), folder_title, sizeof(folder_title), NULL, NULL);
    for (int i = 0; i < l->n; i++) {
        DEnt *de = &l->v[i];
        if (de->media != MEDIA_BOOK) continue;
        r->files_seen++;
        if (junk_text(de->name, de->size)) {
            r->skipped++;
            continue;
        }
        char full[512];
        path_join(dir, de->name, full, sizeof(full));
        if (w->mode_folder) {
            const char *name = w->folder_name[0] ? w->folder_name : "Unsorted";
            char key[320];
            snprintf(key, sizeof(key), "%sb", w->folder_key[0] ? w->folder_key : (w->secret ? "xf:root" : "f:root"));
            ScanTitle *t = get_title(r, key, KIND_BOOK, w->secret, name, 0);
            if (!t) continue;
            ParsedName p;
            parse_book(de->name, &p);
            ScanEntry *e = add_entry(t, full, p.title, de->size, 0, 0, 0, MEDIA_BOOK);
            if (e) e->need_number = 1;
            set_art(t, &w->art[depth < 16 ? depth : 15], NULL);
            continue;
        }
        ParsedName p;
        parse_book(de->name, &p);
        char key[320];
        const char *title = p.title[0] ? p.title : de->name;
        if (p.volume >= 0) {
            key_for(key, sizeof(key), "b", w->secret, title, 0);
        } else {
            /* standalone book: keep separate even when names collide across folders */
            char k[256];
            parse_norm_key(title, k, sizeof(k));
            snprintf(key, sizeof(key), "%sb1:%s:%s", w->secret ? "x" : "", k, dir);
        }
        ScanTitle *t = get_title(r, key, KIND_BOOK, w->secret, title, p.year);
        if (!t) continue;
        ScanEntry *e = add_entry(t, full, p.volume >= 0 ? "" : title, de->size, 0, p.volume >= 0 ? p.volume : 0, 0,
                                 MEDIA_BOOK);
        if (e && p.volume < 0) e->need_number = 1;
        char tmp[512];
        if (!t->poster && find_file_art(dir, l, de->name, NULL, tmp, sizeof(tmp))) t->poster = xstrdup(tmp);
        if (depth > 0 && p.volume >= 0) set_art(t, &w->art[depth < 16 ? depth : 15], NULL);
    }
    (void)folder_title;
}

static void walk(Walk *w, const char *dir, int depth) {
    ScanResult *r = w->r;
    if (r->cancel || depth > 14) return;
    snprintf(r->current, sizeof(r->current), "%s", dir);
    DList l = {0};
    if (fs_list(dir, dl_cb, &l) < 0) return;
    qsort(l.v, l.n, sizeof(DEnt), dent_cmp);

    DirArt *a = &w->art[depth < 16 ? depth : 15];
    find_dir_art(dir, &l, a);

    handle_videos(w, dir, depth, &l);
    handle_books(w, dir, depth, &l);

    for (int i = 0; i < l.n && !r->cancel; i++) {
        if (!l.v[i].is_dir) continue;
        if (parse_is_extras_folder(l.v[i].name)) continue;
        if (is_system_dir(dir, l.v[i].name)) continue;
        char sub[512];
        path_join(dir, l.v[i].name, sub, sizeof(sub));
        if (is_excluded(sub)) continue;
        if (w->mode_folder && depth == 0) {
            snprintf(w->folder_name, sizeof(w->folder_name), "%s", l.v[i].name);
            snprintf(w->folder_key, sizeof(w->folder_key), "%sf:%s", w->secret ? "x" : "", sub);
        }
        walk(w, sub, depth + 1);
        if (w->mode_folder && depth == 0) {
            w->folder_name[0] = 0;
            w->folder_key[0] = 0;
        }
    }
    free(l.v);
}

void scan_folder(const char *root, int secret, int mode_folder, ScanResult *r) {
    Walk *w = calloc(1, sizeof(Walk));
    if (!w) return;
    w->r = r;
    w->root = root;
    w->secret = secret;
    w->mode_folder = mode_folder;
    if (mode_folder) {
        parse_clean(path_basename(root), w->folder_name, sizeof(w->folder_name), NULL, NULL);
        snprintf(w->folder_key, sizeof(w->folder_key), "%sf:%s", secret ? "x" : "", root);
    }
    walk(w, root, 0);
    free(w);
}

static int entry_cmp(const void *a, const void *b) {
    const ScanEntry *x = a, *y = b;
    if (x->season != y->season) {
        /* specials (season 0) go last */
        if (x->season == 0) return 1;
        if (y->season == 0) return -1;
        return x->season - y->season;
    }
    if (x->need_number != y->need_number) return x->need_number - y->need_number;
    if (!x->need_number && x->number != y->number) return x->number - y->number;
    DEnt da, db;
    snprintf(da.name, sizeof(da.name), "%s", path_basename(x->path));
    snprintf(db.name, sizeof(db.name), "%s", path_basename(y->path));
    return dent_cmp(&da, &db);
}

static int title_cmp(const void *a, const void *b) {
    const ScanTitle *x = a, *y = b;
    const char *p = x->title, *q = y->title;
    if (!strncmp(p, "The ", 4)) p += 4;
    if (!strncmp(q, "The ", 4)) q += 4;
    for (; *p && *q; p++, q++) {
        int c = tolower((unsigned char)*p) - tolower((unsigned char)*q);
        if (c) return c;
    }
    return (unsigned char)*p - (unsigned char)*q;
}

void scan_finish(ScanResult *r) {
    r->movies = r->shows = r->episodes = r->books = r->unplayable = 0;
    for (int i = 0; i < r->nt; i++) {
        ScanTitle *t = &r->t[i];
        qsort(t->e, t->ne, sizeof(ScanEntry), entry_cmp);
        /* number files that had no number, continuing after the numbered ones */
        int last_season = -999, next = 1;
        for (int k = 0; k < t->ne; k++) {
            ScanEntry *e = &t->e[k];
            if (e->season != last_season) {
                last_season = e->season;
                next = 1;
            }
            if (e->need_number) {
                e->number = e->number_end = next;
            }
            if (e->number_end >= next) next = e->number_end + 1;
            if (e->media == MEDIA_VIDEO && !e->playable) r->unplayable++;
        }
#ifdef __vita__
        /* nothing about it on the card? the video file may say something about itself */
        if (!t->meta && t->kind != KIND_BOOK && t->ne > 0 && !net_is_url(t->e[0].path) && !r->cancel) {
            char desc[1200], genre[96], show[160], cover[256], base[200];
            int year = 0;
            unsigned hsh = 2166136261u;
            for (const char *c = t->key; *c; c++) hsh = (hsh ^ (unsigned char)*c) * 16777619u;
            snprintf(base, sizeof(base), "ux0:data/Nest/embedded/%08x", hsh);
            sceIoMkdir("ux0:data/Nest/embedded", 0777);
            if (mtags_video(t->e[0].path, desc, sizeof(desc), genre, sizeof(genre), &year, show, sizeof(show),
                            t->poster ? NULL : base, cover, sizeof(cover))) {
                if (desc[0]) t->emb_overview = xstrdup(desc);
                if (genre[0]) t->emb_genres = xstrdup(genre);
                if (year && !t->year) t->emb_year = year;
                if (cover[0] && !t->poster) t->poster = xstrdup(cover);
            }
        }
#endif
        if (t->kind == KIND_MOVIE) r->movies++;
        else if (t->kind == KIND_SERIES) r->shows++, r->episodes += t->ne;
        else r->books += t->ne;
    }
    qsort(r->t, r->nt, sizeof(ScanTitle), title_cmp);
}

void scan_free(ScanResult *r) {
    for (int i = 0; i < r->nt; i++) {
        ScanTitle *t = &r->t[i];
        for (int k = 0; k < t->ne; k++) {
            free(t->e[k].path);
            free(t->e[k].name);
        }
        free(t->e);
        free(t->title);
        free(t->artdir);
        free(t->poster);
        free(t->backdrop);
        free(t->logo);
        free(t->meta);
        free(t->emb_overview);
        free(t->emb_genres);
    }
    free(r->t);
    memset(r, 0, sizeof(*r));
}
