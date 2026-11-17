/* Wi-Fi: Nest talks plain HTTP to "Nest PC Server" (tools/nest_pc_server.py)
   on your computer. A PC library is a normal library folder whose path is
   http://<pc>:<port>/f/...; listing, artwork, nest.json, subtitles and the
   video itself (through AvPlayer's file replacement) all come over HTTP. */
#include "nest.h"

#include <malloc.h>
#include <psp2/avplayer.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

static volatile int state; /* 0 untried, 1 up, -1 failed */
static int tmpl = -1;

int net_is_url(const char *p) { return p && !strncmp(p, "http://", 7); }

int net_up(void) {
    if (state) return state > 0;
    state = -1;
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return 0;
    static void *mem;
    if (!mem) mem = malloc(1024 * 1024);
    if (!mem) return 0;
    SceNetInitParam p;
    p.memory = mem;
    p.size = 1024 * 1024;
    p.flags = 0;
    int r = sceNetInit(&p);
    if (r < 0 && (unsigned)r != 0x80410110u) return 0; /* already initialised is fine */
    sceNetCtlInit();
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP) < 0) return 0;
    sceHttpInit(1024 * 1024);
    tmpl = sceHttpCreateTemplate("Nest/" NEST_VERSION, SCE_HTTP_VERSION_1_1, 1);
    if (tmpl < 0) return 0;
    sceHttpSetResolveTimeOut(tmpl, 4 * 1000 * 1000);
    sceHttpSetConnectTimeOut(tmpl, 4 * 1000 * 1000);
    sceHttpSetSendTimeOut(tmpl, 8 * 1000 * 1000);
    sceHttpSetRecvTimeOut(tmpl, 15 * 1000 * 1000);
    state = 1;
    return 1;
}

int net_wifi_connected(void) {
    if (!net_up()) return 0;
    int st = 0;
    if (sceNetCtlInetGetState(&st) < 0) return 0;
    return st == SCE_NETCTL_STATE_CONNECTED;
}

/* percent-encode the path part; host and port stay as they are */
static void enc(const char *url, const char *query, char *out, int n) {
    const char *host = url + 7;
    const char *path = strchr(host, '/');
    if (!path) path = url + strlen(url);
    int k = snprintf(out, n, "%.*s", (int)(path - url), url);
    for (const unsigned char *s = (const unsigned char *)path; *s && k < n - 4; s++) {
        if (isalnum(*s) || strchr("/-_.~", *s)) out[k++] = (char)*s;
        else k += snprintf(out + k, n - k, "%%%02X", *s);
    }
    out[k] = 0;
    if (query && *query) snprintf(out + k, n - k, "?%s", query);
}

typedef struct {
    int conn, req;
} Http;

/* conn >= 0: reuse that keep-alive connection */
static int http_get(const char *url, const char *query, long long off, long long len, int conn, Http *h, int *status,
                    unsigned long long *clen) {
    if (!net_up()) return -1;
    char e[2200];
    enc(url, query, e, sizeof(e));
    h->conn = conn >= 0 ? conn : sceHttpCreateConnectionWithURL(tmpl, e, 1);
    if (h->conn < 0) return -1;
    h->req = sceHttpCreateRequestWithURL(h->conn, SCE_HTTP_METHOD_GET, e, 0);
    if (h->req < 0) {
        if (conn < 0) sceHttpDeleteConnection(h->conn);
        return -1;
    }
    if (len > 0) {
        char r[64];
        snprintf(r, sizeof(r), "bytes=%lld-%lld", off, off + len - 1);
        sceHttpAddRequestHeader(h->req, "Range", r, SCE_HTTP_HEADER_OVERWRITE);
    }
    if (sceHttpSendRequest(h->req, NULL, 0) < 0) {
        sceHttpDeleteRequest(h->req);
        if (conn < 0) sceHttpDeleteConnection(h->conn);
        return -1;
    }
    *status = 0;
    sceHttpGetStatusCode(h->req, status);
    if (sceHttpGetResponseContentLength(h->req, clen) < 0) *clen = 0;
    return 0;
}

static void http_done(Http *h, int own_conn) {
    sceHttpDeleteRequest(h->req);
    if (own_conn) sceHttpDeleteConnection(h->conn);
}

static int http_read(Http *h, void *buf, int n) {
    int got = 0;
    while (got < n) {
        int r = sceHttpReadData(h->req, (char *)buf + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    return got;
}

static void *read_all_q(const char *url, const char *query, size_t *len) {
    Http h;
    int st;
    unsigned long long cl;
    if (http_get(url, query, 0, 0, -1, &h, &st, &cl) < 0) return NULL;
    if (st != 200 || cl > 64ULL * 1024 * 1024) {
        http_done(&h, 1);
        return NULL;
    }
    size_t cap = cl ? (size_t)cl + 1 : 65536, n = 0;
    char *b = malloc(cap);
    while (b) {
        if (n + 1 >= cap) {
            if (cap >= 64 * 1024 * 1024) break;
            char *nb = realloc(b, cap * 2);
            if (!nb) break;
            b = nb;
            cap *= 2;
        }
        int r = sceHttpReadData(h.req, b + n, (unsigned)(cap - 1 - n));
        if (r <= 0) break;
        n += r;
    }
    http_done(&h, 1);
    if (!b) return NULL;
    b[n] = 0;
    if (len) *len = n;
    return b;
}

void *net_read_all(const char *url, size_t *len) { return read_all_q(url, NULL, len); }

/* server answers a folder with lines: "d|f <TAB> size <TAB> name" */
int net_list(const char *dir, fs_cb cb, void *ud) {
    char url[1100];
    size_t l = strlen(dir);
    snprintf(url, sizeof(url), "%s%s", dir, l && dir[l - 1] == '/' ? "" : "/");
    size_t n = 0;
    char *t = read_all_q(url, "list", &n);
    if (!t) return -1;
    if (strncmp(t, "NEST-LIST", 9)) { free(t); return -1; }
    char *p = strchr(t, '\n');
    while (p && *++p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *t1 = strchr(p, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2 && t2[1]) {
            int isdir = p[0] == 'd';
            long long size = strtoll(t1 + 1, NULL, 10);
            cb(ud, t2 + 1, isdir, size);
        }
        if (!nl) break;
        p = nl;
    }
    free(t);
    return 0;
}

/* ---------------- streamed file with a small read-ahead cache ---------------- */

#define NB 4
#define BS (256 * 1024)

struct NetFile {
    char url[1100];
    int conn;
    uint64_t size;
    uint8_t *buf[NB];
    uint64_t off[NB];
    int len[NB];
    int next;
};

NetFile *netf_open(const char *url) {
    if (!net_up()) return NULL;
    NetFile *f = calloc(1, sizeof(NetFile));
    if (!f) return NULL;
    snprintf(f->url, sizeof(f->url), "%s", url);
    char e[2200];
    enc(url, NULL, e, sizeof(e));
    f->conn = sceHttpCreateConnectionWithURL(tmpl, e, 1);
    size_t n = 0;
    char *s = read_all_q(url, "size", &n);
    if (s) f->size = strtoull(s, NULL, 10);
    free(s);
    if (f->conn < 0 || !f->size) {
        if (f->conn >= 0) sceHttpDeleteConnection(f->conn);
        free(f);
        return NULL;
    }
    for (int i = 0; i < NB; i++) f->off[i] = (uint64_t)-1;
    return f;
}

uint64_t netf_size(NetFile *f) { return f ? f->size : 0; }

static int fetch(NetFile *f, uint64_t off) {
    int slot = f->next;
    f->next = (f->next + 1) % NB;
    if (!f->buf[slot]) f->buf[slot] = malloc(BS);
    if (!f->buf[slot]) return -1;
    long long want = (long long)(f->size - off < BS ? f->size - off : BS);
    for (int attempt = 0; attempt < 2; attempt++) {
        Http h;
        int st;
        unsigned long long cl;
        if (http_get(f->url, NULL, (long long)off, want, f->conn, &h, &st, &cl) == 0) {
            int got = (st == 206 || st == 200) ? http_read(&h, f->buf[slot], (int)want) : 0;
            http_done(&h, 0);
            if (got > 0 && st == 206) {
                f->off[slot] = off;
                f->len[slot] = got;
                return slot;
            }
        }
        /* the keep-alive socket may have gone stale: reconnect once */
        char e[2200];
        enc(f->url, NULL, e, sizeof(e));
        sceHttpDeleteConnection(f->conn);
        f->conn = sceHttpCreateConnectionWithURL(tmpl, e, 1);
        if (f->conn < 0) return -1;
    }
    return -1;
}

int netf_read(NetFile *f, uint8_t *dst, uint64_t pos, uint32_t len) {
    if (!f || pos >= f->size) return 0;
    if (pos + len > f->size) len = (uint32_t)(f->size - pos);
    uint32_t done = 0;
    while (done < len) {
        uint64_t p = pos + done;
        int slot = -1;
        for (int i = 0; i < NB; i++)
            if (f->off[i] != (uint64_t)-1 && p >= f->off[i] && p < f->off[i] + f->len[i]) slot = i;
        if (slot < 0) slot = fetch(f, p - p % BS);
        if (slot < 0) return done ? (int)done : -1;
        uint32_t in_blk = (uint32_t)(p - f->off[slot]);
        uint32_t take = (uint32_t)f->len[slot] - in_blk;
        if (take > len - done) take = len - done;
        memcpy(dst + done, f->buf[slot] + in_blk, take);
        done += take;
    }
    return (int)done;
}

void netf_close(NetFile *f) {
    if (!f) return;
    if (f->conn >= 0) sceHttpDeleteConnection(f->conn);
    for (int i = 0; i < NB; i++) free(f->buf[i]);
    free(f);
}

/* ---------------- AvPlayer reads the video through these ---------------- */

static NetFile *av_nf;
static int avf_open(void *p, const char *fn) {
    (void)p;
    netf_close(av_nf);
    av_nf = netf_open(fn);
    return av_nf ? 0 : -1;
}
static int avf_close(void *p) {
    (void)p;
    netf_close(av_nf);
    av_nf = NULL;
    return 0;
}
static int avf_read(void *p, uint8_t *b, uint64_t pos, uint32_t len) {
    (void)p;
    return av_nf ? netf_read(av_nf, b, pos, len) : -1;
}
static uint64_t avf_size(void *p) {
    (void)p;
    return av_nf ? av_nf->size : 0;
}

void net_avplayer_files(void *replacement) {
    SceAvPlayerFileReplacement *r = replacement;
    r->objectPointer = NULL;
    r->open = avf_open;
    r->close = avf_close;
    r->readOffset = avf_read;
    r->size = avf_size;
}
