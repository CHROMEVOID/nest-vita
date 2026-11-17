/* CBR (RAR comic) support through the unrar library. */
#include "nest.h"

#include <wchar.h>
#define _UNIX
#include <unrar.h>

#include "cbr.h"

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int ok;
} Sink;

static int CALLBACK rar_cb(UINT msg, LPARAM user, LPARAM p1, LPARAM p2) {
    if (msg != UCM_PROCESSDATA) return msg == UCM_NEEDPASSWORD || msg == UCM_NEEDPASSWORDW ? -1 : 1;
    Sink *s = (Sink *)user;
    size_t n = (size_t)p2;
    if (s->len + n > s->cap) {
        size_t nc = (s->cap ? s->cap * 2 : 1 << 20);
        while (nc < s->len + n) nc *= 2;
        if (nc > 96u * 1024 * 1024) { s->ok = 0; return -1; }
        uint8_t *nb = realloc(s->buf, nc);
        if (!nb) { s->ok = 0; return -1; }
        s->buf = nb;
        s->cap = nc;
    }
    memcpy(s->buf + s->len, (void *)p1, n);
    s->len += n;
    return 1;
}

static HANDLE open_rar(const char *path, Sink *sink) {
    struct RAROpenArchiveDataEx d;
    memset(&d, 0, sizeof(d));
    d.ArcName = (char *)path;
    d.OpenMode = RAR_OM_EXTRACT;
    d.Callback = rar_cb;
    d.UserData = (LPARAM)sink;
    HANDLE h = RAROpenArchiveEx(&d);
    if (!h || d.OpenResult != 0) {
        if (h) RARCloseArchive(h);
        return NULL;
    }
    return h;
}

int rar_list(const char *path, char ***names_out) {
    Sink s = {0};
    HANDLE h = open_rar(path, &s);
    if (!h) return 0;
    int n = 0, cap = 0;
    char **names = NULL;
    struct RARHeaderDataEx hd;
    memset(&hd, 0, sizeof(hd));
    while (RARReadHeaderEx(h, &hd) == 0) {
        if (!(hd.Flags & RHDF_DIRECTORY) && media_type_of(hd.FileName) == MEDIA_IMAGE) {
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                char **nn = realloc(names, sizeof(char *) * cap);
                if (!nn) break;
                names = nn;
            }
            names[n++] = strdup(hd.FileName);
        }
        if (RARProcessFile(h, RAR_SKIP, NULL, NULL) != 0) break;
    }
    RARCloseArchive(h);
    *names_out = names;
    return n;
}

void *rar_read(const char *path, const char *name, size_t *size) {
    Sink s = {0};
    s.ok = 1;
    HANDLE h = open_rar(path, &s);
    if (!h) return NULL;
    struct RARHeaderDataEx hd;
    memset(&hd, 0, sizeof(hd));
    int found = 0;
    while (RARReadHeaderEx(h, &hd) == 0) {
        if (!strcmp(hd.FileName, name)) {
            found = RARProcessFile(h, RAR_TEST, NULL, NULL) == 0;
            break;
        }
        if (RARProcessFile(h, RAR_SKIP, NULL, NULL) != 0) break;
    }
    RARCloseArchive(h);
    if (!found || !s.ok) {
        free(s.buf);
        return NULL;
    }
    if (size) *size = s.len;
    return s.buf;
}
