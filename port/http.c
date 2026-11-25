/* Nest port: the small part of the Vita's HTTP library Nest uses to talk to
   "Nest PC Server" on your computer: GET with an optional Range, status, length, body. */
#include "port.h"
#undef fopen
#undef main

#include <SDL.h>
#include "psp2_all.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#define BADSOCK INVALID_SOCKET
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSESOCK close
#define BADSOCK (-1)
#endif

typedef struct {
    int used;
    char host[256];
    char port[8];
} Conn;
typedef struct {
    int used, conn;
    char path[2048];
    char headers[512];
    sock_t s;
    int status;
    long long clen, left; /* -1: until the server closes */
    char buf[8192];
    int bpos, blen;
} Req;
static Conn conns[32];
static Req reqs[32];
static unsigned int recv_timeout_us = 15000000;

int sceHttpInit(unsigned int pool) {
    (void)pool;
#ifdef _WIN32
    static int ws;
    if (!ws) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); ws = 1; }
#endif
    return 0;
}
int sceHttpCreateTemplate(const char *ua, int ver, int proxy) { (void)ua; (void)ver; (void)proxy; return 1; }
int sceHttpSetResolveTimeOut(int id, unsigned int us) { (void)id; (void)us; return 0; }
int sceHttpSetConnectTimeOut(int id, unsigned int us) { (void)id; (void)us; return 0; }
int sceHttpSetSendTimeOut(int id, unsigned int us) { (void)id; (void)us; return 0; }
int sceHttpSetRecvTimeOut(int id, unsigned int us) { (void)id; recv_timeout_us = us; return 0; }

/* http://host:port/path -> host, port, path */
static int split_url(const char *url, char *host, int hn, char *port, int pn, char *path, int pathn) {
    if (strncmp(url, "http://", 7)) return -1;
    const char *h = url + 7;
    const char *slash = strchr(h, '/');
    const char *colon = strchr(h, ':');
    if (colon && slash && colon > slash) colon = NULL;
    const char *hend = colon ? colon : slash ? slash : h + strlen(h);
    snprintf(host, hn, "%.*s", (int)(hend - h), h);
    if (colon) snprintf(port, pn, "%.*s", (int)((slash ? slash : colon + strlen(colon)) - colon - 1), colon + 1);
    else snprintf(port, pn, "80");
    snprintf(path, pathn, "%s", slash ? slash : "/");
    return 0;
}

int sceHttpCreateConnectionWithURL(int tmpl, const char *url, int keep) {
    (void)tmpl; (void)keep;
    for (int i = 1; i < 32; i++)
        if (!conns[i].used) {
            char path[16];
            if (split_url(url, conns[i].host, sizeof(conns[i].host), conns[i].port, sizeof(conns[i].port), path, sizeof(path)) < 0)
                return -1;
            conns[i].used = 1;
            return i;
        }
    return -1;
}
int sceHttpDeleteConnection(int id) {
    if (id > 0 && id < 32) conns[id].used = 0;
    return 0;
}

int sceHttpCreateRequestWithURL(int conn, int method, const char *url, unsigned long long clen) {
    (void)method; (void)clen;
    if (conn <= 0 || conn >= 32 || !conns[conn].used) return -1;
    for (int i = 1; i < 32; i++)
        if (!reqs[i].used) {
            Req *r = &reqs[i];
            memset(r, 0, sizeof(*r));
            char host[256], port[8];
            if (split_url(url, host, sizeof(host), port, sizeof(port), r->path, sizeof(r->path)) < 0) return -1;
            r->used = 1;
            r->conn = conn;
            r->s = BADSOCK;
            return i;
        }
    return -1;
}

int sceHttpDeleteRequest(int id) {
    if (id <= 0 || id >= 32 || !reqs[id].used) return -1;
    if (reqs[id].s != BADSOCK) CLOSESOCK(reqs[id].s);
    reqs[id].used = 0;
    return 0;
}

int sceHttpAddRequestHeader(int id, const char *name, const char *value, unsigned int mode) {
    (void)mode;
    if (id <= 0 || id >= 32 || !reqs[id].used) return -1;
    size_t l = strlen(reqs[id].headers);
    snprintf(reqs[id].headers + l, sizeof(reqs[id].headers) - l, "%s: %s\r\n", name, value);
    return 0;
}

static int fill(Req *r) {
    int n = (int)recv(r->s, r->buf, sizeof(r->buf), 0);
    if (n <= 0) return 0;
    r->bpos = 0;
    r->blen = n;
    return n;
}
static int getline_(Req *r, char *out, int n) {
    int k = 0;
    for (;;) {
        if (r->bpos >= r->blen && !fill(r)) break;
        char c = r->buf[r->bpos++];
        if (c == '\n') break;
        if (c != '\r' && k < n - 1) out[k++] = c;
    }
    out[k] = 0;
    return k;
}

int sceHttpSendRequest(int id, const void *post, unsigned int size) {
    (void)post; (void)size;
    if (id <= 0 || id >= 32 || !reqs[id].used) return -1;
    Req *r = &reqs[id];
    Conn *c = &conns[r->conn];
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(c->host, c->port, &hints, &res) != 0 || !res) return -1;
    r->s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (r->s == BADSOCK) { freeaddrinfo(res); return -1; }
#ifdef _WIN32
    DWORD tv = recv_timeout_us / 1000;
    setsockopt(r->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv = {(time_t)(recv_timeout_us / 1000000), (suseconds_t)(recv_timeout_us % 1000000)};
    setsockopt(r->s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    int one = 1;
    setsockopt(r->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    if (connect(r->s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        CLOSESOCK(r->s);
        r->s = BADSOCK;
        return -1;
    }
    freeaddrinfo(res);
    char req[3000];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s:%s\r\nUser-Agent: Nest\r\nConnection: close\r\n%s\r\n", r->path,
                     c->host, c->port, r->headers);
    if (send(r->s, req, n, 0) != n) return -1;
    char line[1024];
    if (!getline_(r, line, sizeof(line))) return -1;
    sscanf(line, "HTTP/%*s %d", &r->status);
    r->clen = -1;
    while (getline_(r, line, sizeof(line)) > 0)
        if (!strncasecmp(line, "Content-Length:", 15)) r->clen = atoll(line + 15);
    r->left = r->clen;
    return 0;
}

int sceHttpGetStatusCode(int id, int *code) {
    if (id <= 0 || id >= 32 || !reqs[id].used) return -1;
    *code = reqs[id].status;
    return 0;
}
int sceHttpGetResponseContentLength(int id, unsigned long long *len) {
    if (id <= 0 || id >= 32 || !reqs[id].used || reqs[id].clen < 0) return -1;
    *len = (unsigned long long)reqs[id].clen;
    return 0;
}
int sceHttpReadData(int id, void *data, unsigned int size) {
    if (id <= 0 || id >= 32 || !reqs[id].used) return -1;
    Req *r = &reqs[id];
    if (r->left == 0) return 0;
    if (r->bpos >= r->blen && !fill(r)) return 0;
    int n = r->blen - r->bpos;
    if ((unsigned)n > size) n = (int)size;
    if (r->left > 0 && n > r->left) n = (int)r->left;
    memcpy(data, r->buf + r->bpos, n);
    r->bpos += n;
    if (r->left > 0) r->left -= n;
    return n;
}
