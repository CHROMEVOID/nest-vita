/* "Find info in the folder": search the library folder a title came from for
   anything that belongs to it - Kodi/Jellyfin/Plex .nfo files, nest.json or
   <name>.json, posters, fanart and logos (in its own folder, in a folder named
   after it, or named after it anywhere), and per-episode .nfo files - and
   apply it. What's found is remembered by title key and file path, so a
   rescan keeps it.

   The walk runs on a worker thread (it only touches files); the database is
   written on the main thread once it's done. */
#include "nest.h"

#include <psp2/kernel/threadmgr.h>

#define MAX_EPS 2048
#define MAX_FILES 80000
#define MAX_DEPTH 9

typedef struct {
    int season, number;
    char base[192]; /* lower-case file name without extension */
    char *path;
    char *name;     /* found episode title */
    int rel;
} FEp;

typedef struct {
    /* in */
    int tid;
    char key[320];
    char root[512], own[512];
    char n1[128], n2[128]; /* normalised names: shown title, folder title */
    FEp *eps;
    int neps;
    /* out */
    char title[192], overview[4096], genres[256];
    int year;
    float score;
    int meta_rel;
    char art[3][512];
    int art_rel[3], art_pri[3];
    int files, cancel;
} Job;

static Job *job;
static volatile int running, done;
static float spin;

/* ---------- text helpers ---------- */

static void norm(const char *s, char *out, int n) {
    int k = 0;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", s);
    /* drop a trailing year: "Show (2011)", "Show 2011", "Show [2011]" */
    int len = (int)strlen(tmp);
    for (int i = len - 4; i >= 1; i--) {
        if ((tmp[i] == '1' && tmp[i + 1] == '9') || (tmp[i] == '2' && tmp[i + 1] == '0')) {
            if (isdigit((unsigned char)tmp[i + 2]) && isdigit((unsigned char)tmp[i + 3]) &&
                !isdigit((unsigned char)tmp[i - 1]) && !isdigit((unsigned char)tmp[i + 4]) && i > 2) {
                int tail = 1;
                for (int j = i + 4; tmp[j]; j++)
                    if (isalnum((unsigned char)tmp[j])) tail = 0;
                if (tail) { tmp[i] = 0; break; }
            }
        }
    }
    const char *p = tmp;
    if (!strncasecmp(p, "the ", 4)) p += 4;
    for (; *p && k < n - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '&') { if (k < n - 4) { out[k++] = 'a'; out[k++] = 'n'; out[k++] = 'd'; } continue; }
        if (isalnum(c)) out[k++] = (char)tolower(c);
        else if (c >= 0x80) out[k++] = (char)c; /* keep non-latin titles */
    }
    out[k] = 0;
}

static void base_lower(const char *name, char *out, int n) {
    snprintf(out, n, "%s", name);
    char *d = strrchr(out, '.');
    if (d && d != out) *d = 0;
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

static int name_matches(Job *j, const char *nb) {
    const char *ns[2] = {j->n1, j->n2};
    for (int i = 0; i < 2; i++) {
        size_t l = strlen(ns[i]);
        if (!l) continue;
        if (!strcmp(nb, ns[i])) return 1;
        if (l >= 4 && !strncmp(nb, ns[i], l)) return 1;
    }
    return 0;
}

static int skip_dir(const char *name) {
    char l[64];
    snprintf(l, sizeof(l), "%s", name);
    for (char *p = l; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strncmp(l, "sce_", 4) || l[0] == '.' || l[0] == '$') return 1;
    static const char *no[] = {"vitashell", "repatch", "readdcont", "rexdata", "tai", "psp2", "pspemu", "app",
                               "appmeta", "addcont", "patch", "license", "savedata", "cache", "temp", "sys",
                               "plugins", "bgdl", "mms", "xmb", "font", "theme", "nest", NULL};
    for (int i = 0; no[i]; i++)
        if (!strcmp(l, no[i])) return 1;
    return 0;
}

/* ---------- reading files ---------- */

static char *slurp(const char *path, int max) {
    if (net_is_url(path)) {
        size_t n = 0;
        char *b = net_read_all(path, &n);
        if (b && (int)n > max) b[max] = 0;
        return b;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(max + 1);
    if (!b) { fclose(f); return NULL; }
    int n = (int)fread(b, 1, max, f);
    fclose(f);
    b[n < 0 ? 0 : n] = 0;
    return b;
}

static void xml_decode(const char *s, int len, char *out, int n) {
    int k = 0;
    if (len >= 12 && !strncmp(s, "<![CDATA[", 9)) {
        s += 9;
        len -= 9;
        const char *e = strstr(s, "]]>");
        if (e && e - s < len) len = (int)(e - s);
    }
    for (int i = 0; i < len && k < n - 1; i++) {
        if (s[i] == '&') {
            const char *ents[] = {"&amp;", "&", "&lt;", "<", "&gt;", ">", "&quot;", "\"", "&apos;", "'", NULL};
            int hit = 0;
            for (int e = 0; ents[e]; e += 2) {
                int el = (int)strlen(ents[e]);
                if (i + el <= len && !strncmp(s + i, ents[e], el)) {
                    out[k++] = ents[e + 1][0];
                    i += el - 1;
                    hit = 1;
                    break;
                }
            }
            if (hit) continue;
            if (i + 2 < len && s[i + 1] == '#') {
                unsigned u = s[i + 2] == 'x' ? (unsigned)strtoul(s + i + 3, NULL, 16) : (unsigned)strtoul(s + i + 2, NULL, 10);
                const char *semi = memchr(s + i, ';', len - i);
                if (semi && u) {
                    if (u < 0x80) out[k++] = (char)u;
                    else if (u < 0x800 && k < n - 2) { out[k++] = (char)(0xC0 | u >> 6); out[k++] = (char)(0x80 | (u & 63)); }
                    else if (k < n - 3) {
                        out[k++] = (char)(0xE0 | u >> 12);
                        out[k++] = (char)(0x80 | (u >> 6 & 63));
                        out[k++] = (char)(0x80 | (u & 63));
                    }
                    i = (int)(semi - s);
                    continue;
                }
            }
        }
        out[k++] = s[i] == '\r' ? ' ' : s[i];
    }
    out[k] = 0;
    /* trim */
    while (k > 0 && isspace((unsigned char)out[k - 1])) out[--k] = 0;
    int a = 0;
    while (out[a] && isspace((unsigned char)out[a])) a++;
    if (a) memmove(out, out + a, strlen(out + a) + 1);
}

/* text of <tag ...>...</tag> at or after *from; advances *from past it */
static int xml_tag(const char **from, const char *end, const char *tag, char *out, int n) {
    char open[40], close[40];
    snprintf(open, sizeof(open), "<%s", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *p = *from;
    size_t ol = strlen(open);
    for (;;) {
        p = strstr(p, open);
        if (!p || (end && p >= end)) return 0;
        char c = p[ol];
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') break;
        p += ol;
    }
    const char *gt = strchr(p, '>');
    if (!gt) return 0;
    if (gt[-1] == '/') { *from = gt + 1; out[0] = 0; return 1; } /* <tag/> */
    const char *e = strstr(gt + 1, close);
    if (!e || (end && e > end)) return 0;
    xml_decode(gt + 1, (int)(e - gt - 1), out, n);
    *from = e + strlen(close);
    return 1;
}

static int xml_get(const char *s, const char *end, const char *tag, char *out, int n) {
    const char *p = s;
    return xml_tag(&p, end, tag, out, n) && out[0];
}

static int year_of(const char *s) {
    for (const char *p = s; *p; p++)
        if ((p[0] == '1' && p[1] == '9') || (p[0] == '2' && p[1] == '0'))
            if (isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]))
                return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
    return 0;
}

/* ---------- what a file is worth ---------- */

static void take_title_meta(Job *j, const char *body, int rel) {
    if (rel <= j->meta_rel) return;
    char buf[4096];
    int got = 0;
    char t[192] = "", ov[4096] = "", ge[256] = "";
    int yr = 0;
    float sc = 0;
    if (xml_get(body, NULL, "title", buf, sizeof(buf))) snprintf(t, sizeof(t), "%s", buf), got++;
    if (xml_get(body, NULL, "plot", buf, sizeof(buf)) || xml_get(body, NULL, "outline", buf, sizeof(buf)))
        snprintf(ov, sizeof(ov), "%s", buf), got++;
    if (xml_get(body, NULL, "year", buf, sizeof(buf))) yr = atoi(buf);
    if (!yr && (xml_get(body, NULL, "premiered", buf, sizeof(buf)) || xml_get(body, NULL, "aired", buf, sizeof(buf)) ||
                xml_get(body, NULL, "releasedate", buf, sizeof(buf))))
        yr = year_of(buf);
    if (yr < 1880 || yr > 2100) yr = 0;
    if (yr) got++;
    {
        const char *rs = strstr(body, "<ratings");
        if (rs && xml_get(rs, NULL, "value", buf, sizeof(buf))) sc = strtof(buf, NULL);
        else if (xml_get(body, NULL, "rating", buf, sizeof(buf))) sc = strtof(buf, NULL);
        if (sc > 10) sc /= 10; /* some tools write 0..100 */
        if (sc < 0) sc = 0;
        if (sc > 0) got++;
    }
    {
        const char *p = body;
        while (xml_tag(&p, NULL, "genre", buf, sizeof(buf))) {
            if (!buf[0] || strlen(ge) + strlen(buf) + 3 >= sizeof(ge)) continue;
            /* some writers put "Comedy / Animation" in one tag */
            for (char *q = buf; *q; q++) if (*q == '/' || *q == '|') *q = ',';
            if (ge[0]) strcat(ge, ", ");
            strcat(ge, buf);
        }
        if (ge[0]) got++;
    }
    if (!got) return;
    j->meta_rel = rel;
    snprintf(j->title, sizeof(j->title), "%s", t);
    snprintf(j->overview, sizeof(j->overview), "%s", ov);
    snprintf(j->genres, sizeof(j->genres), "%s", ge);
    j->year = yr;
    j->score = sc;
}

static void take_json(Job *j, const char *path, int rel) {
    if (rel <= j->meta_rel) return;
    char *ov = NULL, *ge = NULL, *mt = NULL;
    float sc = 0;
    int yr = 0;
    db_read_meta(path, &ov, &ge, &sc, &mt, &yr);
    if (ov || ge || mt || yr || sc > 0) {
        j->meta_rel = rel;
        snprintf(j->title, sizeof(j->title), "%s", mt ? mt : "");
        snprintf(j->overview, sizeof(j->overview), "%s", ov ? ov : "");
        snprintf(j->genres, sizeof(j->genres), "%s", ge ? ge : "");
        j->year = yr;
        j->score = sc;
    }
    free(ov);
    free(ge);
    free(mt);
}

static void take_episode(Job *j, FEp *e, const char *body, int rel) {
    if (rel <= e->rel) return;
    char buf[256];
    if (!xml_get(body, NULL, "title", buf, sizeof(buf))) return;
    free(e->name);
    e->name = strdup(buf);
    e->rel = rel;
    (void)j;
}

/* 0 poster, 1 backdrop, 2 logo; pri: higher wins among equal relevance */
static int image_role(const char *rest, int *pri) {
    if (strstr(rest, "season") || (strstr(rest, "thumb") && !strstr(rest, "landscape"))) return -1;
    if (strstr(rest, "clearlogo") || strstr(rest, "logo")) { *pri = 3; return 2; }
    if (strstr(rest, "fanart") || strstr(rest, "backdrop") || strstr(rest, "background")) { *pri = 3; return 1; }
    if (strstr(rest, "landscape") || strstr(rest, "banner") || !strcmp(rest, "art")) { *pri = 1; return 1; }
    if (strstr(rest, "poster")) { *pri = 4; return 0; }
    if (strstr(rest, "cover") || strstr(rest, "folder")) { *pri = 3; return 0; }
    if (!strcmp(rest, "show") || !strcmp(rest, "movie") || !strcmp(rest, "default")) { *pri = 2; return 0; }
    if (!rest[0]) { *pri = 1; return 0; } /* an image simply named after the title */
    return -1;
}

static void take_image(Job *j, const char *path, const char *nb_lower, int rel, int named) {
    /* what's left of the name once the title is taken off: "-poster", "fanart" ... */
    const char *rest = nb_lower;
    char strip[192];
    if (named) {
        norm(nb_lower, strip, sizeof(strip));
        size_t l1 = strlen(j->n1), l2 = strlen(j->n2);
        if (l1 && !strncmp(strip, j->n1, l1)) rest = strip + l1;
        else if (l2 && !strncmp(strip, j->n2, l2)) rest = strip + l2;
        else rest = strip;
    }
    int pri = 0, role = image_role(rest, &pri);
    if (role < 0) return;
    if (rel > j->art_rel[role] || (rel == j->art_rel[role] && pri > j->art_pri[role])) {
        snprintf(j->art[role], sizeof(j->art[role]), "%s", path);
        j->art_rel[role] = rel;
        j->art_pri[role] = pri;
    }
}

/* ---------- the walk ---------- */

typedef struct {
    char names[1024][128];
    int dir[1024];
    int n;
} Lst;

static void lst_cb(void *ud, const char *name, int is_dir, long long size) {
    (void)size;
    Lst *l = ud;
    if (l->n >= 1024) return;
    snprintf(l->names[l->n], 128, "%s", name);
    l->dir[l->n++] = is_dir;
}

static int is_inside(const char *path, const char *dir) {
    size_t l = strlen(dir);
    return l && !strncmp(path, dir, l) && (path[l] == '/' || path[l] == 0);
}

static void look_at_file(Job *j, const char *dir, const char *name, int dir_named) {
    char full[1024], lb[192], nb[192];
    path_join(dir, name, full, sizeof(full));
    base_lower(name, lb, sizeof(lb));
    norm(lb, nb, sizeof(nb));
    int own = is_inside(dir, j->own);
    int named = name_matches(j, nb);
    int m = media_type_of(name);
    int nfo = ends_with(name, ".nfo"), json = ends_with(name, ".json");
    if (!nfo && !json && m != MEDIA_IMAGE) return;

    /* a file next to (or named exactly like) one of the episodes */
    if (nfo || m == MEDIA_IMAGE) {
        for (int i = 0; i < j->neps; i++) {
            if (strcmp(j->eps[i].base, lb)) continue;
            if (nfo) {
                char *b = slurp(full, 64 * 1024);
                if (b) {
                    if (strstr(b, "<episodedetails") || strstr(b, "<movie")) {
                        if (j->neps > 1 || strstr(b, "<episodedetails")) take_episode(j, &j->eps[i], b, 3);
                        else take_title_meta(j, b, own ? 4 : 3); /* a movie's own <file>.nfo */
                    }
                    free(b);
                }
            }
            return;
        }
    }
    int rel = own ? 3 : (dir_named || named) ? 2 : 0;
    if (json) {
        if (rel && (!strcmp(lb, "nest") || named || dir_named)) take_json(j, full, rel + (own && !strcmp(lb, "nest")));
        return;
    }
    if (m == MEDIA_IMAGE) {
        if (rel) take_image(j, full, lb, rel, named && !own);
        return;
    }
    /* .nfo: tvshow.nfo / movie.nfo / <title>.nfo, or any nfo whose <title> is this title */
    char *b = slurp(full, 128 * 1024);
    if (!b) return;
    int is_show = strstr(b, "<tvshow") != NULL, is_movie = strstr(b, "<movie") != NULL;
    int is_ep = strstr(b, "<episodedetails") != NULL;
    if (is_show || is_movie) {
        int r = rel;
        if (!r) { /* correlate by what it says inside */
            char t[192], tn[192];
            if (xml_get(b, NULL, "title", t, sizeof(t))) {
                norm(t, tn, sizeof(tn));
                if (tn[0] && (!strcmp(tn, j->n1) || !strcmp(tn, j->n2))) r = 2;
            }
            if (!r && (xml_get(b, NULL, "originaltitle", t, sizeof(t)))) {
                norm(t, tn, sizeof(tn));
                if (tn[0] && (!strcmp(tn, j->n1) || !strcmp(tn, j->n2))) r = 2;
            }
        }
        if (r) take_title_meta(j, b, r + (own && (!strcmp(lb, "tvshow") || !strcmp(lb, "movie"))));
    } else if (is_ep && rel >= 2) {
        /* an episode nfo with numbers, in this show's folders */
        char s[16], e[16];
        if (xml_get(b, NULL, "season", s, sizeof(s)) && xml_get(b, NULL, "episode", e, sizeof(e))) {
            int sn = atoi(s), en = atoi(e);
            for (int i = 0; i < j->neps; i++)
                if (j->eps[i].season == sn && j->eps[i].number == en) take_episode(j, &j->eps[i], b, 2);
        }
    }
    free(b);
}

static void walk(Job *j, const char *dir, int depth, int dir_named) {
    if (j->cancel || depth > MAX_DEPTH || j->files > MAX_FILES) return;
    Lst *l = malloc(sizeof(Lst));
    if (!l) return;
    l->n = 0;
    if (fs_list(dir, lst_cb, l) < 0) { free(l); return; }
    for (int i = 0; i < l->n && !j->cancel; i++) {
        j->files++;
        if (l->dir[i]) continue;
        look_at_file(j, dir, l->names[i], dir_named);
    }
    for (int i = 0; i < l->n && !j->cancel; i++) {
        if (!l->dir[i] || skip_dir(l->names[i])) continue;
        char sub[1024], nb[128];
        path_join(dir, l->names[i], sub, sizeof(sub));
        norm(l->names[i], nb, sizeof(nb));
        int named = dir_named || (nb[0] && (!strcmp(nb, j->n1) || !strcmp(nb, j->n2)));
        walk(j, sub, depth + 1, named);
    }
    free(l);
}

static int worker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    Job *j = job;
    /* its own folder first (and everything under it), then the whole library folder */
    walk(j, j->root[0] ? j->root : j->own, 0, 0);
    if (j->root[0] && !is_inside(j->own, j->root)) walk(j, j->own, 0, 0);
    done = 1;
    sceKernelExitDeleteThread(0);
    return 0;
}

/* ---------- main-thread side ---------- */

int metafind_busy(void) { return running; }

void metafind_start(int tid) {
    if (running) { toast("Already looking \xe2\x80\x94 one moment"); return; }
    Title *t = lib_title_by_id(tid);
    if (!t || t->ne <= 0) return;
    Job *j = calloc(1, sizeof(Job));
    if (!j) return;
    j->tid = tid;
    snprintf(j->key, sizeof(j->key), "%s", t->key ? t->key : "");
    db_title_folder(tid, j->root, sizeof(j->root));
    norm(t->title ? t->title : "", j->n1, sizeof(j->n1));
    norm(t->orig_title ? t->orig_title : "", j->n2, sizeof(j->n2));
    if (!strcmp(j->n1, j->n2)) j->n2[0] = 0;
    j->eps = calloc(t->ne, sizeof(FEp));
    for (int k = t->e0; k < t->e0 + t->ne && j->eps; k++) {
        FEp *e = &j->eps[j->neps++];
        e->season = L.e[k].season;
        e->number = L.e[k].number;
        e->path = strdup(L.e[k].path);
        base_lower(path_basename(L.e[k].path), e->base, sizeof(e->base));
        /* the folder all of its files share = its own folder */
        char d[512];
        snprintf(d, sizeof(d), "%s", L.e[k].path);
        char *s = strrchr(d, '/');
        if (s) *s = 0;
        if (!j->own[0]) snprintf(j->own, sizeof(j->own), "%s", d);
        else {
            int i = 0;
            while (j->own[i] && d[i] == j->own[i]) i++;
            if (!(j->own[i] == 0 && (d[i] == 0 || d[i] == '/'))) {
                while (i > 0 && j->own[i] != '/') i--;
                j->own[i] = 0;
            }
        }
    }
    /* a movie loose in the library folder has no folder of its own */
    if (j->root[0] && !strcmp(j->own, j->root)) j->own[0] = 0;
    /* "Show/Season 1": the show's folder is one up */
    if (j->own[0] && t->kind == KIND_SERIES) {
        const char *b = path_basename(j->own);
        if (!strncasecmp(b, "season", 6) || !strncasecmp(b, "series", 6) || !strncasecmp(b, "specials", 8) ||
            ((b[0] == 'S' || b[0] == 's') && isdigit((unsigned char)b[1]))) {
            char *s = strrchr(j->own, '/');
            if (s && (!j->root[0] || (int)(s - j->own) > (int)strlen(j->root))) *s = 0;
        }
    }
    job = j;
    done = 0;
    running = 1;
    SceUID th = sceKernelCreateThread("nest_metafind", worker, 0x10000110, 256 * 1024, 0, 0, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) {
        running = 0;
        toast("Couldn't start looking");
        return;
    }
    toast("Looking through the folder\xe2\x80\xa6");
}

static void apply(Job *j) {
    char what[160] = "";
    int nf = 0, neps = 0;
#define ADD(s) do { if (nf++) strncat(what, ", ", sizeof(what) - strlen(what) - 1); strncat(what, s, sizeof(what) - strlen(what) - 1); } while (0)
    if (j->meta_rel) {
        if (j->overview[0]) ADD("description");
        if (j->year) ADD("year");
        if (j->genres[0]) ADD("genres");
        if (j->score > 0) ADD("rating");
        if (j->title[0] && !nf) ADD("title");
    }
    const char *art_names[3] = {"poster", "backdrop", "logo"};
    for (int r = 0; r < 3; r++)
        if (j->art[r][0]) ADD(art_names[r]);
    for (int i = 0; i < j->neps; i++)
        if (j->eps[i].name) neps++;
    db_found_save(j->key, j->meta_rel && j->title[0] ? j->title : NULL, j->meta_rel ? j->year : 0,
                  j->meta_rel && j->overview[0] ? j->overview : NULL, j->meta_rel && j->genres[0] ? j->genres : NULL,
                  j->meta_rel ? j->score : 0, j->art[0][0] ? j->art[0] : NULL, j->art[1][0] ? j->art[1] : NULL,
                  j->art[2][0] ? j->art[2] : NULL);
    for (int i = 0; i < j->neps; i++)
        if (j->eps[i].name) db_found_episode(j->eps[i].path, j->eps[i].name);
    /* art you asked for now beats art picked earlier by hand */
    for (int r = 0; r < 3; r++)
        if (j->art[r][0]) db_title_set_str(j->tid, r == 0 ? "u_poster" : r == 1 ? "u_backdrop" : "u_logo", NULL);
    db_found_apply();
    lib_load();
    detail_refresh();
    char m[220];
    if (!nf && !neps) snprintf(m, sizeof(m), "Nothing about this in the folder (looked at %d files)", j->files);
    else if (neps && nf) snprintf(m, sizeof(m), "Found %s and %d episode name%s", what, neps, neps == 1 ? "" : "s");
    else if (neps) snprintf(m, sizeof(m), "Found %d episode name%s", neps, neps == 1 ? "" : "s");
    else snprintf(m, sizeof(m), "Found %s", what);
    toast(m);
#undef ADD
}

void metafind_frame(float dt) {
    if (!running) return;
    if (done) {
        Job *j = job;
        running = 0;
        job = NULL;
        apply(j);
        for (int i = 0; i < j->neps; i++) {
            free(j->eps[i].path);
            free(j->eps[i].name);
        }
        free(j->eps);
        free(j);
        return;
    }
    /* small "looking" pill at the top while the walk runs */
    spin += dt;
    float w = 250, x = SCR_W / 2 - w / 2, y = 14;
    gfx_rrect(x, y, w, 36, 18, col_a(0xFF000000, 0.72f));
    draw_spinner(x + 22, y + 18, 8, 0xFFFFFFFF);
    char b[64];
    snprintf(b, sizeof(b), "Looking for info\xe2\x80\xa6 %d", job ? job->files : 0);
    text(F_META, 13, x + 40, y + 23, 0xFFFFFFFF, b);
}
