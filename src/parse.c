/* Release-name cleaning and episode detection.
   Goal: "Shameless.S02E04.1080p.BluRay.x265.DDP5.1.mkv" -> Shameless, S2 E4. */
#include "parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXTOK 96

static int strncasecmp_portable(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

typedef struct {
    char t[96];   /* original text */
    char l[96];   /* lowercase */
    int brk;      /* a removed [..] / {..} group sat here */
} Tok;

static const char *VIDEO_EXT[] = {"mp4", "m4v", "mkv", "avi", "mov", "wmv", "ts", "m2ts", "webm",
                                  "mpg", "mpeg", "flv", "3gp", "vob", "ogv", NULL};
static const char *BOOK_EXT[] = {"cbz", "cbr", "pdf", "epub", "txt", NULL};
static const char *IMAGE_EXT[] = {"jpg", "jpeg", "png", "webp", "bmp", "gif", NULL};
static const char *SUB_EXT[] = {"srt", "vtt", NULL};

static const char *HARD_JUNK[] = {
    "bluray", "bdrip", "brrip", "bdremux", "remux", "webrip", "webdl", "webdlrip", "hdtv", "hdtvrip",
    "hdrip", "dvdrip", "dvdscr", "dvd", "dvd5", "dvd9", "hdcam", "telesync", "x264", "x265", "h264",
    "h265", "hevc", "avc", "xvid", "divx", "av1", "vp9", "vc1", "mpeg2", "10bit", "8bit", "12bit", "hdr",
    "hdr10", "hdr10plus", "dovi", "sdr", "aac", "ac3", "eac3", "eac", "ddp", "dts", "dtshd", "dtsx",
    "truehd", "atmos", "flac", "mp3", "opus", "pcm", "lpcm", "2ch", "6ch", "8ch", "4k", "uhd", "fhd",
    "amzn", "nf", "dsnp", "hmax", "atvp", "hulu", "pcok", "pmtp", "stan", "crav", "itunes", "rarbg",
    "yts", "yify", "eztv", "ettv", "tgx", "psa", "qxr", "pahe", "rartv", "galaxyrg", "mkv", "mp4",
    "avi", "m4v", "esub", "esubs", "hc", "hdtc", "bdmv", "webhd", "dl", "ddp51", "dd51", "aac20",
    "aac51", "nvenc", "x26", "hdr10+", "dv", "imax", NULL};

static const char *SOFT_JUNK[] = {"proper", "repack", "extended", "unrated", "remastered", "internal",
                                  "limited", "multi", "dual", "audio", "complete", "uncut", "directors",
                                  "theatrical", "hybrid", "readnfo", "subbed", "dubbed", "rerip", "cam",
                                  "ts", "hd", "sd", "eng", "english", "ita", "fre", "ger",
                                  "spa", "multisub", "subs", "sub", "edition", "criterion", "open", "matte",
                                  "ws", "fs", NULL};

static int in_list(const char *s, const char **list) {
    for (int i = 0; list[i]; i++)
        if (!strcmp(s, list[i])) return 1;
    return 0;
}

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static const char *ext_of(const char *name) {
    const char *dot = strrchr(name, '.');
    const char *slash = strrchr(name, '/');
    if (!dot || (slash && dot < slash) || dot == name) return "";
    return dot + 1;
}

static int ext_in(const char *name, const char **list) {
    char e[16];
    const char *x = ext_of(name);
    size_t n = strlen(x);
    if (n == 0 || n >= sizeof(e)) return 0;
    for (size_t i = 0; i <= n; i++) e[i] = (char)tolower((unsigned char)x[i]);
    return in_list(e, list);
}

int media_type_of(const char *filename) {
    if (ext_in(filename, VIDEO_EXT)) return MEDIA_VIDEO;
    if (ext_in(filename, BOOK_EXT)) return MEDIA_BOOK;
    if (ext_in(filename, IMAGE_EXT)) return MEDIA_IMAGE;
    if (ext_in(filename, SUB_EXT)) return MEDIA_SUB;
    {
        const char *b = path_basename(filename);
        if (!strcmp(b, "nest.json")) return MEDIA_META;
    }
    return MEDIA_NONE;
}

int video_is_playable(const char *filename) {
    static const char *ok[] = {"mp4", "m4v", NULL};
    return ext_in(filename, ok);
}

const char *path_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void path_dirname(const char *path, char *out, size_t n) {
    const char *s = strrchr(path, '/');
    size_t len = s ? (size_t)(s - path) : 0;
    if (len >= n) len = n - 1;
    memcpy(out, path, len);
    out[len] = 0;
}

/* ---------- token helpers ---------- */

static int is_year(const char *l) {
    if (strlen(l) != 4 || !all_digits(l)) return 0;
    int y = atoi(l);
    return y >= 1900 && y <= 2035;
}

/* 720p, 1080p, 2160p, 1080i, 480p ... */
static int is_resolution(const char *l) {
    size_t n = strlen(l);
    if (n < 4 || n > 5) return 0;
    if (l[n - 1] != 'p' && l[n - 1] != 'i') return 0;
    for (size_t i = 0; i + 1 < n; i++)
        if (!isdigit((unsigned char)l[i])) return 0;
    int v = atoi(l);
    return v == 240 || v == 360 || v == 480 || v == 540 || v == 576 || v == 720 || v == 1080 ||
           v == 1440 || v == 2160 || v == 4320;
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* ddp5, dd5, aac2, ac3, eac3, dts-hd, 10bit ... */
static int is_audio_like(const char *l) {
    static const char *pre[] = {"ddp", "dd", "aac", "eac3", "ac3", "dts", "truehd", "atmos", "opus",
                                "flac", NULL};
    for (int i = 0; pre[i]; i++) {
        size_t k = strlen(pre[i]);
        if (starts(l, pre[i]) && (l[k] == 0 || isdigit((unsigned char)l[k]))) return 1;
    }
    size_t n = strlen(l);
    if (n > 3 && !strcmp(l + n - 3, "bit") && isdigit((unsigned char)l[0])) return 1;
    return 0;
}

/* strip hyphens: web-dl -> webdl, blu-ray -> bluray */
static void squash(const char *l, char *out) {
    while (*l) {
        if (*l != '-' && *l != '+') *out++ = *l;
        l++;
    }
    *out = 0;
}

static int hard_junk(const char *l, const char *next) {
    char sq[96];
    squash(l, sq);
    if (!*sq) return 0;
    if (in_list(sq, HARD_JUNK) || in_list(l, HARD_JUNK)) {
        if (!strcmp(sq, "dl")) return 0; /* only with web, handled below */
        if (!strcmp(sq, "dv") || !strcmp(sq, "imax")) return next == NULL || next[0] == 0 ? 0 : 1;
        return 1;
    }
    if (!strcmp(sq, "bluray") || !strcmp(sq, "blu")) return next && (!strcmp(next, "ray") || !strcmp(sq, "bluray"));
    if (is_resolution(sq) || is_audio_like(sq)) return 1;
    if (!strcmp(sq, "h") && next && (!strcmp(next, "264") || !strcmp(next, "265"))) return 1;
    if (!strcmp(sq, "web") && next &&
        (!strcmp(next, "dl") || !strcmp(next, "rip") || !strcmp(next, "dlrip") || is_resolution(next) ||
         !strcmp(next, "h") || !strncmp(next, "x26", 3) || !strncmp(next, "h26", 3)))
        return 1;
    if (!strcmp(sq, "br") && next && !strcmp(next, "rip")) return 1;
    /* x265-GROUP, web-dl etc: check part before the first hyphen */
    const char *h = strchr(l, '-');
    if (h && h != l) {
        char a[96];
        size_t k = (size_t)(h - l);
        if (k < sizeof(a)) {
            memcpy(a, l, k);
            a[k] = 0;
            if (in_list(a, HARD_JUNK) && strcmp(a, "dl") != 0) return 1;
            if (is_resolution(a) || is_audio_like(a)) return 1;
        }
    }
    return 0;
}

static int soft_junk(const char *l) {
    char sq[96];
    squash(l, sq);
    return in_list(sq, SOFT_JUNK);
}

/* S01E02, s1e2, S01E01E02, S01E01-E03, S01E01-03 */
static int match_sxxeyy(const char *l, int *s, int *e, int *e2) {
    if (l[0] != 's' || !isdigit((unsigned char)l[1])) return 0;
    const char *p = l + 1;
    int sv = 0, digits = 0;
    while (isdigit((unsigned char)*p) && digits < 3) sv = sv * 10 + (*p++ - '0'), digits++;
    if (*p == ' ' || *p == '.') p++;
    if (*p != 'e' || !isdigit((unsigned char)p[1])) return 0;
    p++;
    int ev = 0;
    digits = 0;
    while (isdigit((unsigned char)*p) && digits < 4) ev = ev * 10 + (*p++ - '0'), digits++;
    int last = ev;
    while (*p == 'e' || *p == '-') {
        const char *q = p + 1;
        if (*q == 'e') q++;
        if (!isdigit((unsigned char)*q)) break;
        int v = 0;
        while (isdigit((unsigned char)*q)) v = v * 10 + (*q++ - '0');
        if (v > last && v - last < 10) last = v;
        p = q;
    }
    if (*p != 0 && *p != '-') return 0;
    *s = sv;
    *e = ev;
    *e2 = last;
    return 1;
}

/* 1x02, 01x02 */
static int match_nxnn(const char *l, int *s, int *e) {
    const char *x = strchr(l, 'x');
    if (!x || x == l || x - l > 2) return 0;
    for (const char *p = l; p < x; p++)
        if (!isdigit((unsigned char)*p)) return 0;
    const char *q = x + 1;
    size_t n = strlen(q);
    if (n < 2 || n > 3 || !all_digits(q)) return 0;
    *s = atoi(l);
    *e = atoi(q);
    return 1;
}

/* s01 / s1 (season pack) */
static int match_sxx(const char *l, int *s) {
    if (l[0] != 's') return 0;
    size_t n = strlen(l + 1);
    if (n < 1 || n > 2 || !all_digits(l + 1)) return 0;
    *s = atoi(l + 1);
    return 1;
}

/* e05, ep05, ep.5 */
static int match_ep(const char *l, int *e) {
    const char *p = l;
    if (starts(p, "episode")) p += 7;
    else if (starts(p, "ep")) p += 2;
    else if (p[0] == 'e') p += 1;
    else return 0;
    if (!all_digits(p) || strlen(p) > 4) return 0;
    *e = atoi(p);
    return 1;
}

/* e01e02, e01-e02, e01-02 (multi-episode without a season) */
static int match_exey(const char *l, int *e, int *e2) {
    if (l[0] != 'e' || !isdigit((unsigned char)l[1])) return 0;
    const char *p = l + 1;
    int a = 0, b = 0, n = 0;
    while (isdigit((unsigned char)*p) && n < 4) a = a * 10 + (*p++ - '0'), n++;
    if (*p == '-') p++;
    if (*p == 'e') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    n = 0;
    while (isdigit((unsigned char)*p) && n < 4) b = b * 10 + (*p++ - '0'), n++;
    if (*p || b < a || b - a > 9) return 0;
    *e = a;
    *e2 = b;
    return 1;
}

static int is_season_word(const char *l) {
    return !strcmp(l, "season") || !strcmp(l, "series") || !strcmp(l, "staffel") || !strcmp(l, "saison") ||
           !strcmp(l, "temporada") || !strcmp(l, "stagione") || !strcmp(l, "seizoen");
}

static int is_episode_word(const char *l) {
    return !strcmp(l, "episode") || !strcmp(l, "ep") || !strcmp(l, "episodio") || !strcmp(l, "folge") ||
           !strcmp(l, "e");
}

/* Split a name into tokens. Bracketed groups are removed (they are almost
   always release junk); a year inside (..) or [..] is remembered. */
static int tokenize(const char *in, Tok *toks, int *year_in_brackets, int keep_parens_text) {
    char buf[512];
    size_t bl = 0;
    int nt = 0;
    int pending_brk = 0;
    *year_in_brackets = 0;

    /* drop leading "www.site.com - " */
    if (!strncasecmp_portable(in, "www.", 4)) {
        const char *d = strstr(in, " - ");
        if (d) in = d + 3;
    }

    for (const char *p = in;; p++) {
        char c = *p;
        int sep = (c == 0 || c == ' ' || c == '.' || c == '_' || c == '\t' || c == ',');
        if (c == '[' || c == '{' || c == '(' || c == '\xe3') {
            /* bracket group */
            char close = c == '[' ? ']' : c == '{' ? '}' : ')';
            const char *end = strchr(p + 1, close);
            if (c == '\xe3' || !end) {
                if (c == '\xe3') { /* stray UTF-8 CJK bracket start: treat as separator */
                    sep = 1;
                } else {
                    continue;
                }
            } else {
                char inner[256];
                size_t k = (size_t)(end - p - 1);
                if (k >= sizeof(inner)) k = sizeof(inner) - 1;
                memcpy(inner, p + 1, k);
                inner[k] = 0;
                int y = 0;
                {
                    char low[256];
                    for (size_t i = 0; i <= k; i++) low[i] = (char)tolower((unsigned char)inner[i]);
                    if (is_year(low)) y = atoi(low);
                    /* keep short plain text in parentheses: "Shameless (US)" */
                    int keep = 0;
                    if (c == '(' && !y && keep_parens_text && k > 0 && k <= 12) {
                        keep = 1;
                        char t2[256];
                        strcpy(t2, low);
                        for (char *q = t2; *q; q++)
                            if (*q == '.' || *q == '_' || *q == '-') *q = ' ';
                        char *save = NULL;
                        for (char *w = strtok_r(t2, " ", &save); w; w = strtok_r(NULL, " ", &save))
                            if (hard_junk(w, NULL) || soft_junk(w) || all_digits(w)) keep = 0;
                    }
                    if (keep) {
                        if (bl) {
                            buf[bl] = 0;
                            if (nt < MAXTOK) {
                                snprintf(toks[nt].t, sizeof(toks[nt].t), "%s", buf);
                                toks[nt].brk = pending_brk;
                                nt++;
                            }
                            bl = 0;
                            pending_brk = 0;
                        }
                        if (nt < MAXTOK) {
                            snprintf(toks[nt].t, sizeof(toks[nt].t), "(%s)", inner);
                            toks[nt].brk = pending_brk;
                            pending_brk = 0;
                            nt++;
                        }
                        p = end;
                        continue;
                    }
                }
                if (y) {
                    if (!*year_in_brackets) *year_in_brackets = y;
                    /* treat a bracketed year like a bare year token */
                    if (bl) {
                        buf[bl] = 0;
                        if (nt < MAXTOK) {
                            snprintf(toks[nt].t, sizeof(toks[nt].t), "%s", buf);
                            toks[nt].brk = pending_brk;
                            nt++;
                        }
                        bl = 0;
                        pending_brk = 0;
                    }
                    if (nt < MAXTOK) {
                        snprintf(toks[nt].t, sizeof(toks[nt].t), "%d", y);
                        toks[nt].brk = pending_brk;
                        pending_brk = 0;
                        nt++;
                    }
                } else {
                    pending_brk = 1;
                    sep = 1;
                }
                p = end;
                if (!sep) continue;
                c = ' ';
            }
        }
        if (sep) {
            if (bl) {
                buf[bl] = 0;
                if (nt < MAXTOK) {
                    snprintf(toks[nt].t, sizeof(toks[nt].t), "%s", buf);
                    toks[nt].brk = pending_brk;
                    pending_brk = 0;
                    nt++;
                }
                bl = 0;
            }
            if (c == 0) break;
            continue;
        }
        if (bl < sizeof(buf) - 1 && bl < 95) buf[bl++] = c;
    }
    for (int i = 0; i < nt; i++) {
        size_t n = strlen(toks[i].t);
        for (size_t k = 0; k <= n; k++) toks[i].l[k] = (char)tolower((unsigned char)toks[i].t[k]);
    }
    return nt;
}

static void strip_ext(const char *in, char *out, size_t n) {
    snprintf(out, n, "%s", in);
    int mt = media_type_of(in);
    if (mt != MEDIA_NONE) {
        char *dot = strrchr(out, '.');
        if (dot) *dot = 0;
    }
}

static void join_tokens(Tok *toks, int a, int b, char *out, size_t n) {
    out[0] = 0;
    size_t len = 0;
    for (int i = a; i < b; i++) {
        size_t k = strlen(toks[i].t);
        if (len + k + 2 >= n) break;
        if (len) out[len++] = ' ';
        memcpy(out + len, toks[i].t, k);
        len += k;
        out[len] = 0;
    }
}

static void trim_title(char *s) {
    /* trim separators at both ends */
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '-' || s[n - 1] == ':' || s[n - 1] == ',' ||
                 s[n - 1] == '+' || s[n - 1] == '~' || s[n - 1] == '&' || s[n - 1] == '('))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '-' || s[i] == ':' || s[i] == ',' || s[i] == '~' || s[i] == '+')
        i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
    /* collapse " - - " leftovers and double spaces */
    char *d = s;
    for (char *p = s; *p; p++) {
        if (*p == ' ' && (d == s || d[-1] == ' ')) continue;
        *d++ = *p;
    }
    *d = 0;
    n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
}

static int has_lower(const char *s) {
    for (; *s; s++)
        if (islower((unsigned char)*s)) return 1;
    return 0;
}
static int has_upper(const char *s) {
    for (; *s; s++)
        if (isupper((unsigned char)*s)) return 1;
    return 0;
}

void parse_title_case(char *s) {
    static const char *small[] = {"a", "an", "the", "and", "or", "of", "in", "on", "at", "to", "for",
                                  "vs", "by", "with", "from", "nor", "but", NULL};
    int all_caps = !has_lower(s) && strlen(s) > 4;
    int all_low = !has_upper(s);
    if (!all_caps && !all_low) return;
    int word = 0;
    char *p = s;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *w = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - w);
        char low[64];
        size_t k = len < 63 ? len : 63;
        for (size_t i = 0; i < k; i++) low[i] = (char)tolower((unsigned char)w[i]);
        low[k] = 0;
        int is_last = (*p == 0);
        int keep_small = word > 0 && !is_last && in_list(low, small);
        int paren_short = w[0] == '(' && len <= 5;
        for (size_t i = 0; i < len; i++) {
            if (paren_short)
                w[i] = (char)toupper((unsigned char)w[i]);
            else if (i == 0 && !keep_small)
                w[i] = (char)toupper((unsigned char)w[i]);
            else if (i == 1 && w[0] == '(')
                w[i] = (char)toupper((unsigned char)w[i]);
            else
                w[i] = (char)tolower((unsigned char)w[i]);
        }
        /* roman numerals II, III, IV stay caps */
        if (len >= 2 && len <= 4) {
            int roman = 1;
            for (size_t i = 0; i < len; i++)
                if (!strchr("ivxIVX", w[i])) roman = 0;
            if (roman)
                for (size_t i = 0; i < len; i++) w[i] = (char)toupper((unsigned char)w[i]);
        }
        word++;
    }
}

/* ---------- core scanner ---------- */

typedef struct {
    int title_end;   /* tokens [0, title_end) form the title */
    int year;
    int season;
    int episode, episode_end;
    int ep_tok_end;  /* first token after the episode marker */
} Scan;

static void scan_tokens(Tok *t, int nt, Scan *sc) {
    sc->title_end = nt;
    sc->year = 0;
    sc->season = -1;
    sc->episode = -1;
    sc->episode_end = -1;
    sc->ep_tok_end = -1;

    int any_hard = 0;
    for (int i = 0; i < nt; i++)
        if (hard_junk(t[i].l, i + 1 < nt ? t[i + 1].l : NULL)) any_hard = 1;

    int cut = nt;
    int last_year_idx = -1;
    for (int i = 0; i < nt; i++) {
        const char *l = t[i].l;
        const char *nx = i + 1 < nt ? t[i + 1].l : NULL;
        int s, e, e2;
        if (t[i].brk && i > 0) { cut = i; break; }
        if (match_sxxeyy(l, &s, &e, &e2)) {
            sc->season = s; sc->episode = e; sc->episode_end = e2; sc->ep_tok_end = i + 1;
            cut = i; break;
        }
        if (match_nxnn(l, &s, &e) && i > 0) {
            sc->season = s; sc->episode = e; sc->episode_end = e; sc->ep_tok_end = i + 1;
            cut = i; break;
        }
        /* "S01 E02" written apart */
        if (match_sxx(l, &s) && nx && match_ep(nx, &e) && i > 0) {
            sc->season = s; sc->episode = e; sc->episode_end = e; sc->ep_tok_end = i + 2;
            cut = i; break;
        }
        if (match_sxx(l, &s) && i > 0) { sc->season = s; cut = i; break; }
        if (is_season_word(l) && nx && all_digits(nx) && strlen(nx) <= 2) {
            sc->season = atoi(nx);
            cut = i;
            int j = i + 2;
            if (j + 1 < nt && is_episode_word(t[j].l) && all_digits(t[j + 1].l)) {
                sc->episode = sc->episode_end = atoi(t[j + 1].l);
                sc->ep_tok_end = j + 2;
            } else if (j < nt && match_ep(t[j].l, &e)) {
                sc->episode = sc->episode_end = e;
                sc->ep_tok_end = j + 1;
            }
            break;
        }
        if (is_episode_word(l) && strcmp(l, "e") != 0 && nx && all_digits(nx) && strlen(nx) <= 4) {
            sc->episode = sc->episode_end = atoi(nx); sc->ep_tok_end = i + 2;
            cut = i; break;
        }
        if (i > 0 && match_exey(l, &e, &e2)) {
            sc->episode = e; sc->episode_end = e2; sc->ep_tok_end = i + 1;
            cut = i; break;
        }
        if (i > 0 && match_ep(l, &e) && l[0] == 'e' && strcmp(l, "e") != 0) {
            sc->episode = sc->episode_end = e; sc->ep_tok_end = i + 1;
            cut = i; break;
        }
        if (hard_junk(l, nx)) { cut = i; break; }
        if (soft_junk(l) && (any_hard || last_year_idx >= 0) && i > 0) { cut = i; break; }
        if (i > 0 && is_year(l)) last_year_idx = i;
    }
    if (last_year_idx >= 0 && last_year_idx < cut) {
        sc->year = atoi(t[last_year_idx].l);
        cut = last_year_idx;
    } else if (cut < nt) {
        /* a year can follow the episode marker: Show.S01E01.2019... ignore */
    }
    sc->title_end = cut;
}

void parse_clean(const char *in, char *out, size_t n, int *year, int *season) {
    char base[512];
    strip_ext(in, base, sizeof(base));
    Tok t[MAXTOK];
    int yb = 0;
    int nt = tokenize(base, t, &yb, 1);
    Scan sc;
    scan_tokens(t, nt, &sc);
    join_tokens(t, 0, sc.title_end, out, n);
    trim_title(out);
    /* title made only of a year (e.g. "2012") -> keep it */
    if (!out[0] && nt > 0 && is_year(t[0].l)) snprintf(out, n, "%s", t[0].t);
    parse_title_case(out);
    if (year) *year = sc.year ? sc.year : yb;
    if (season) *season = sc.episode < 0 ? sc.season : -1;
}

static int is_number_tok(const char *l, int *v) {
    /* 05, 05v2, 123 */
    char tmp[16];
    size_t n = strlen(l);
    if (n == 0 || n > 8) return 0;
    snprintf(tmp, sizeof(tmp), "%s", l);
    char *vv = strchr(tmp, 'v');
    if (vv && vv != tmp && all_digits(vv + 1)) *vv = 0;
    if (!all_digits(tmp) || strlen(tmp) > 4) return 0;
    *v = atoi(tmp);
    return 1;
}

void parse_filename(const char *filename, ParsedName *p) {
    memset(p, 0, sizeof(*p));
    p->season = -1;
    p->episode = -1;
    p->episode_end = -1;
    p->volume = -1;

    char base[512];
    strip_ext(path_basename(filename), base, sizeof(base));
    Tok t[MAXTOK];
    int yb = 0;
    int nt = tokenize(base, t, &yb, 1);
    Scan sc;
    scan_tokens(t, nt, &sc);

    int title_end = sc.title_end;
    if (sc.episode >= 0) {
        p->is_episode = 1;
        p->season = sc.season;
        p->episode = sc.episode;
        p->episode_end = sc.episode_end >= sc.episode ? sc.episode_end : sc.episode;
        /* episode title: tokens after the marker until junk */
        int a = sc.ep_tok_end, b = a;
        while (b < nt) {
            const char *nx = b + 1 < nt ? t[b + 1].l : NULL;
            if (t[b].brk && b > a) break;
            if (hard_junk(t[b].l, nx) || is_year(t[b].l)) break;
            if (soft_junk(t[b].l) && b > a) break;
            b++;
        }
        join_tokens(t, a, b, p->ep_title, sizeof(p->ep_title));
        trim_title(p->ep_title);
        parse_title_case(p->ep_title);
    } else {
        p->season = sc.season;
        /* anime: "Title - 05" or "Title - 05v2" */
        int v;
        if (title_end >= 2 && is_number_tok(t[title_end - 1].l, &v) && !is_year(t[title_end - 1].l) &&
            (!strcmp(t[title_end - 2].t, "-") || !strcmp(t[title_end - 2].t, "\xe2\x80\x93"))) {
            p->is_episode = 1;
            p->absolute = 1;
            p->episode = p->episode_end = v;
            title_end -= 2;
        } else if (title_end == 1 && is_number_tok(t[0].l, &v) && !is_year(t[0].l) && nt >= 1) {
            /* bare "05.mkv" -- the caller decides from folder context */
            p->episode = p->episode_end = v;
            title_end = 0;
        } else if (title_end >= 2 && is_number_tok(t[title_end - 1].l, &v) && strlen(t[title_end - 1].l) >= 2 &&
                   t[title_end - 1].l[0] == '0') {
            /* "Naruto 001" -- leading zero numbering is a strong episode signal */
            p->episode = p->episode_end = v;
            p->absolute = 1;
            title_end -= 1;
        }
        /* "05 - Episode Name" (number first) */
        if (p->episode < 0 && nt >= 2 && is_number_tok(t[0].l, &v) && !is_year(t[0].l) &&
            (!strcmp(t[1].t, "-") || strlen(t[0].l) <= 3)) {
            if (strlen(t[0].l) <= 3 && v < 1000) {
                p->episode = p->episode_end = v;
                int a = !strcmp(t[1].t, "-") ? 2 : 1;
                join_tokens(t, a, title_end > a ? title_end : a, p->ep_title, sizeof(p->ep_title));
                trim_title(p->ep_title);
                parse_title_case(p->ep_title);
                title_end = 0;
            }
        }
    }
    join_tokens(t, 0, title_end, p->title, sizeof(p->title));
    trim_title(p->title);
    if (!p->title[0] && nt > 0 && is_year(t[0].l) && !p->is_episode && p->episode < 0)
        snprintf(p->title, sizeof(p->title), "%s", t[0].t);
    parse_title_case(p->title);
    p->year = sc.year ? sc.year : yb;
}

void parse_book(const char *filename, ParsedName *p) {
    memset(p, 0, sizeof(*p));
    p->season = -1;
    p->episode = -1;
    p->episode_end = -1;
    p->volume = -1;
    char base[512];
    strip_ext(path_basename(filename), base, sizeof(base));
    Tok t[MAXTOK];
    int yb = 0;
    int nt = tokenize(base, t, &yb, 0);
    int end = nt;
    for (int i = 0; i < nt; i++) {
        const char *l = t[i].l;
        const char *nx = i + 1 < nt ? t[i + 1].l : NULL;
        int v;
        if (i > 0 && t[i].brk) { end = i; break; }
        if (i > 0 && (!strcmp(l, "vol") || !strcmp(l, "volume") || !strcmp(l, "v") || !strcmp(l, "ch") ||
                      !strcmp(l, "chapter") || !strcmp(l, "book") || !strcmp(l, "tome") || !strcmp(l, "issue")) &&
            nx && is_number_tok(nx, &v)) {
            p->volume = v;
            end = i;
            break;
        }
        if (i > 0 && (l[0] == 'v' || l[0] == 'c' || l[0] == '#') && is_number_tok(l + 1, &v) && strlen(l) >= 2) {
            p->volume = v;
            end = i;
            break;
        }
        if (i > 0 && is_number_tok(l, &v) && !is_year(l)) {
            p->volume = v;
            end = i;
            break;
        }
        if (i > 0 && is_year(l)) { p->year = atoi(l); end = i; break; }
        if (i > 0 && (!strcmp(l, "digital") || !strcmp(l, "webrip") || hard_junk(l, nx))) { end = i; break; }
    }
    join_tokens(t, 0, end, p->title, sizeof(p->title));
    trim_title(p->title);
    if (!p->title[0]) {
        join_tokens(t, 0, nt, p->title, sizeof(p->title));
        trim_title(p->title);
    }
    parse_title_case(p->title);
    if (!p->year) p->year = yb;
}

int parse_season_folder(const char *dirname) {
    char l[256];
    size_t n = strlen(dirname);
    if (n >= sizeof(l)) n = sizeof(l) - 1;
    for (size_t i = 0; i < n; i++) l[i] = (char)tolower((unsigned char)dirname[i]);
    l[n] = 0;
    if (!strcmp(l, "specials") || !strcmp(l, "special") || !strcmp(l, "season 0") || !strcmp(l, "season 00"))
        return 0;
    Tok t[MAXTOK];
    int yb;
    int nt = tokenize(dirname, t, &yb, 0);
    if (nt == 0) return -1;
    int s;
    if (is_season_word(t[0].l)) {
        if (nt >= 2 && all_digits(t[1].l) && strlen(t[1].l) <= 2) return atoi(t[1].l);
        /* season01 */
    }
    for (const char *w = t[0].l; *w; w++) {
        (void)w;
        break;
    }
    if (!strncmp(t[0].l, "season", 6) && all_digits(t[0].l + 6) && t[0].l[6]) return atoi(t[0].l + 6);
    if (match_sxx(t[0].l, &s)) return s;
    /* "Shameless.S02.1080p" style season packs */
    int season = -1;
    char tmp[256];
    parse_clean(dirname, tmp, sizeof(tmp), NULL, &season);
    if (season >= 0) return season;
    return -1;
}

int parse_is_extras_folder(const char *dirname) {
    static const char *names[] = {"extras", "extra", "featurettes", "featurette", "behind the scenes",
                                  "deleted scenes", "trailers", "trailer", "interviews", "scenes", "shorts",
                                  "samples", "sample", "bonus", "other", "subs", "subtitles", "artwork",
                                  "@eadir", "$recycle.bin", "system volume information", NULL};
    char l[256];
    size_t n = strlen(dirname);
    if (n >= sizeof(l)) n = sizeof(l) - 1;
    for (size_t i = 0; i < n; i++) l[i] = (char)tolower((unsigned char)dirname[i]);
    l[n] = 0;
    if (l[0] == '.') return 1;
    return in_list(l, names);
}

int parse_is_sample(const char *filename) {
    char base[512];
    strip_ext(path_basename(filename), base, sizeof(base));
    Tok t[MAXTOK];
    int yb;
    int nt = tokenize(base, t, &yb, 0);
    for (int i = 0; i < nt; i++)
        if (!strcmp(t[i].l, "sample") || !strcmp(t[i].l, "trailer")) return 1;
    return 0;
}

void parse_norm_key(const char *title, char *out, size_t n) {
    size_t k = 0;
    const char *p = title;
    /* ignore a leading "the " so "The Office" and "Office" group */
    if (!strncasecmp_portable(p, "the ", 4)) p += 4;
    for (; *p && k + 4 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '&') {
            memcpy(out + k, "and", 3);
            k += 3;
        } else if (isalnum(c)) {
            out[k++] = (char)tolower(c);
        } else if (c >= 0x80) {
            out[k++] = (char)c;
        }
    }
    out[k] = 0;
}
