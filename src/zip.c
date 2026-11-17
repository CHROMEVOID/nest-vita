/* Tiny read-only ZIP reader (stored + deflate) for CBZ and EPUB. */
#include "nest.h"

#include <zlib.h>

typedef struct {
    char *name;
    uint32_t comp, size, offset;
    uint16_t method;
} ZEnt;

struct Zip {
    FILE *f;
    ZEnt *e;
    int n;
};

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

Zip *zip_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    long tail = fsize < 66000 ? fsize : 66000;
    uint8_t *buf = malloc(tail);
    if (!buf) { fclose(f); return NULL; }
    fseek(f, fsize - tail, SEEK_SET);
    if (fread(buf, 1, tail, f) != (size_t)tail) { free(buf); fclose(f); return NULL; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(buf); fclose(f); return NULL; }
    int count = rd16(buf + eocd + 10);
    uint32_t cd_size = rd32(buf + eocd + 12), cd_off = rd32(buf + eocd + 16);
    free(buf);
    if (cd_size > 64 * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t *cd = malloc(cd_size);
    if (!cd) { fclose(f); return NULL; }
    fseek(f, cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, f) != cd_size) { free(cd); fclose(f); return NULL; }
    Zip *z = calloc(1, sizeof(Zip));
    z->f = f;
    z->e = calloc(count ? count : 1, sizeof(ZEnt));
    uint8_t *p = cd, *end = cd + cd_size;
    for (int i = 0; i < count && p + 46 <= end; i++) {
        if (rd32(p) != 0x02014b50) break;
        uint16_t nl = rd16(p + 28), xl = rd16(p + 30), cl = rd16(p + 32);
        if (p + 46 + nl > end) break;
        ZEnt *e = &z->e[z->n];
        e->method = rd16(p + 10);
        e->comp = rd32(p + 20);
        e->size = rd32(p + 24);
        e->offset = rd32(p + 42);
        e->name = malloc(nl + 1);
        memcpy(e->name, p + 46, nl);
        e->name[nl] = 0;
        if (nl && e->name[nl - 1] != '/') z->n++;
        else free(e->name);
        p += 46 + nl + xl + cl;
    }
    free(cd);
    return z;
}

void zip_close(Zip *z) {
    if (!z) return;
    for (int i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    if (z->f) fclose(z->f);
    free(z);
}

int zip_count(Zip *z) { return z ? z->n : 0; }
const char *zip_name(Zip *z, int i) { return z && i >= 0 && i < z->n ? z->e[i].name : ""; }

int zip_find(Zip *z, const char *name) {
    if (!z) return -1;
    for (int i = 0; i < z->n; i++)
        if (!strcmp(z->e[i].name, name)) return i;
    for (int i = 0; i < z->n; i++)
        if (!strcasecmp(z->e[i].name, name)) return i;
    return -1;
}

void *zip_read(Zip *z, int i, size_t *size) {
    if (!z || i < 0 || i >= z->n) return NULL;
    ZEnt *e = &z->e[i];
    if (e->size > 96 * 1024 * 1024) return NULL;
    uint8_t lh[30];
    fseek(z->f, e->offset, SEEK_SET);
    if (fread(lh, 1, 30, z->f) != 30 || rd32(lh) != 0x04034b50) return NULL;
    fseek(z->f, e->offset + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
    uint8_t *comp = malloc(e->comp ? e->comp : 1);
    if (!comp) return NULL;
    if (fread(comp, 1, e->comp, z->f) != e->comp) { free(comp); return NULL; }
    uint8_t *out = malloc(e->size + 1);
    if (!out) { free(comp); return NULL; }
    if (e->method == 0) {
        memcpy(out, comp, e->size);
    } else if (e->method == 8) {
        z_stream s;
        memset(&s, 0, sizeof(s));
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(comp); free(out); return NULL; }
        s.next_in = comp;
        s.avail_in = e->comp;
        s.next_out = out;
        s.avail_out = e->size;
        int r = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        if (r != Z_STREAM_END && s.total_out != e->size) { free(comp); free(out); return NULL; }
    } else {
        free(comp);
        free(out);
        return NULL;
    }
    free(comp);
    out[e->size] = 0;
    if (size) *size = e->size;
    return out;
}
