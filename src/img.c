/* Background image loading: posters, backdrops, logos, comic pages and book
   covers decode on a worker thread, then upload on the main thread a few per
   frame so scrolling never hitches. Textures fade in when they arrive. */
#include "nest.h"

#include <setjmp.h>
#include <psp2/kernel/threadmgr.h>
#include <jpeglib.h>
#include <webp/decode.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#include "../third_party/stb_image.h"

#include "book.h"
#include "cbr.h"

/* ---------------- archive helpers (shared with reader) ---------------- */

static int ext_is(const char *path, const char *e) {
    const char *d = strrchr(path, '.');
    return d && !strcasecmp(d + 1, e);
}

static int natcmp(const char *p, const char *q) {
    while (*p && *q) {
        if (*p >= '0' && *p <= '9' && *q >= '0' && *q <= '9') {
            long a = strtol(p, (char **)&p, 10), b = strtol(q, (char **)&q, 10);
            if (a != b) return a < b ? -1 : 1;
            continue;
        }
        int a = tolower((unsigned char)*p), b = tolower((unsigned char)*q);
        if (a != b) return a - b;
        p++, q++;
    }
    return (unsigned char)*p - (unsigned char)*q;
}
static int natcmp_ptr(const void *a, const void *b) { return natcmp(*(char *const *)a, *(char *const *)b); }

int book_list_images(const char *path, char ***out) {
    *out = NULL;
    int n = 0;
    char **names = NULL;
    if (ext_is(path, "cbr")) {
        n = rar_list(path, &names);
    } else if (ext_is(path, "cbz") || ext_is(path, "zip") || ext_is(path, "epub")) {
        Zip *z = zip_open(path);
        if (!z) return 0;
        names = malloc(sizeof(char *) * (zip_count(z) + 1));
        for (int i = 0; i < zip_count(z); i++) {
            const char *nm = zip_name(z, i);
            const char *b = strrchr(nm, '/');
            b = b ? b + 1 : nm;
            if (b[0] == '.' || strstr(nm, "__MACOSX")) continue;
            if (media_type_of(nm) == MEDIA_IMAGE) names[n++] = strdup(nm);
        }
        zip_close(z);
    }
    if (n > 1) qsort(names, n, sizeof(char *), natcmp_ptr);
    *out = names;
    return n;
}

void book_free_list(char **names, int n) {
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

void *book_read_entry(const char *path, const char *name, size_t *size) {
    if (ext_is(path, "cbr")) return rar_read(path, name, size);
    Zip *z = zip_open(path);
    if (!z) return NULL;
    void *d = zip_read(z, zip_find(z, name), size);
    zip_close(z);
    return d;
}

static void *read_file(const char *path, size_t *len) {
    if (net_is_url(path)) return net_read_all(path, len);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024 * 1024) { fclose(f); return NULL; }
    void *b = malloc(n);
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* cover for cbz/cbr/epub: an image named like "cover", else the first page */
static void *book_cover(const char *path, size_t *len) {
    char **names;
    int n = book_list_images(path, &names);
    if (n <= 0) return NULL;
    int pick = 0;
    for (int i = 0; i < n; i++) {
        char low[256];
        snprintf(low, sizeof(low), "%s", names[i]);
        for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(low, "cover")) { pick = i; break; }
    }
    if (ext_is(path, "epub") && pick == 0) {
        /* prefer the largest image early in an epub (often the cover) */
    }
    void *d = book_read_entry(path, names[pick], len);
    book_free_list(names, n);
    return d;
}

/* ---------------- decoding ---------------- */

struct jerr_t {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr_t *)c->err)->jb, 1); }

static void target_size(int sw, int sh, int maxw, int maxh, int *dw, int *dh) {
    float s;
    if (maxw <= 0 && maxh <= 0) s = 1;
    else if (maxh <= 0) s = (float)maxw / sw;
    else if (maxw <= 0) s = (float)maxh / sh;
    else s = fmaxf((float)maxw / sw, (float)maxh / sh); /* cover */
    if (s > 1) s = 1;
    *dw = (int)(sw * s + 0.5f);
    *dh = (int)(sh * s + 0.5f);
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
    /* GPU texture limit */
    while (*dw > 2048 || *dh > 2048) { *dw /= 2; *dh /= 2; }
}

static uint8_t *downscale(uint8_t *src, int sw, int sh, int dw, int dh) {
    if (dw == sw && dh == sh) return src;
    uint8_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return src;
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            int stepy = (y1 - y0) > 4 ? (y1 - y0) / 4 : 1, stepx = (x1 - x0) > 4 ? (x1 - x0) / 4 : 1;
            for (int yy = y0; yy < y1; yy += stepy)
                for (int xx = x0; xx < x1; xx += stepx) {
                    const uint8_t *p = src + ((size_t)yy * sw + xx) * 4;
                    r += p[0], g += p[1], b += p[2], a += p[3], n++;
                }
            uint8_t *q = dst + ((size_t)y * dw + x) * 4;
            q[0] = r / n, q[1] = g / n, q[2] = b / n, q[3] = a / n;
        }
    }
    free(src);
    return dst;
}

static uint8_t *decode_mem(const uint8_t *buf, size_t len, int maxw, int maxh, int *ow, int *oh) {
    if (len < 12) return NULL;
    int w = 0, h = 0;
    uint8_t *px = NULL;
    if (buf[0] == 0xFF && buf[1] == 0xD8) {
        struct jpeg_decompress_struct ci;
        struct jerr_t je;
        ci.err = jpeg_std_error(&je.pub);
        je.pub.error_exit = jerr_exit;
        if (setjmp(je.jb)) {
            jpeg_destroy_decompress(&ci);
            free(px);
            return NULL;
        }
        jpeg_create_decompress(&ci);
        jpeg_mem_src(&ci, (unsigned char *)buf, len);
        jpeg_read_header(&ci, TRUE);
        int dw, dh;
        target_size(ci.image_width, ci.image_height, maxw, maxh, &dw, &dh);
        int denom = 1;
        while (denom < 8 && (int)ci.image_width / (denom * 2) >= dw && (int)ci.image_height / (denom * 2) >= dh)
            denom *= 2;
        ci.scale_num = 1;
        ci.scale_denom = denom;
        ci.out_color_space = JCS_EXT_RGBA;
        ci.dct_method = JDCT_IFAST;
        jpeg_start_decompress(&ci);
        w = ci.output_width;
        h = ci.output_height;
        px = malloc((size_t)w * h * 4);
        if (!px) {
            jpeg_destroy_decompress(&ci);
            return NULL;
        }
        while (ci.output_scanline < ci.output_height) {
            JSAMPROW row = px + (size_t)ci.output_scanline * w * 4;
            jpeg_read_scanlines(&ci, &row, 1);
        }
        jpeg_finish_decompress(&ci);
        jpeg_destroy_decompress(&ci);
    } else if (!memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WEBP", 4)) {
        uint8_t *d = WebPDecodeRGBA(buf, len, &w, &h);
        if (!d) return NULL;
        px = malloc((size_t)w * h * 4);
        if (px) memcpy(px, d, (size_t)w * h * 4);
        WebPFree(d);
    } else {
        int n;
        px = stbi_load_from_memory(buf, (int)len, &w, &h, &n, 4);
    }
    if (!px) return NULL;
    int dw, dh;
    target_size(w, h, maxw, maxh, &dw, &dh);
    px = downscale(px, w, h, dw, dh);
    *ow = dw;
    *oh = dh;
    return px;
}

uint8_t *img_decode_file(const char *src, int maxw, int maxh, int *ow, int *oh) {
    size_t len = 0;
    void *buf = NULL;
    if (!strncmp(src, "cover:", 6)) {
        buf = book_cover(src + 6, &len);
    } else if (!strncmp(src, "arc:", 4)) {
        /* arc:<path>\x01<entry> */
        const char *sep = strchr(src + 4, '\x01');
        if (!sep) return NULL;
        char path[512];
        size_t k = (size_t)(sep - (src + 4));
        if (k >= sizeof(path)) return NULL;
        memcpy(path, src + 4, k);
        path[k] = 0;
        buf = book_read_entry(path, sep + 1, &len);
    } else {
        buf = read_file(src, &len);
    }
    if (!buf) return NULL;
    uint8_t *px = decode_mem(buf, len, maxw, maxh, ow, oh);
    free(buf);
    return px;
}

vita2d_texture *img_upload(uint8_t *rgba, int w, int h) {
    vita2d_texture *t = vita2d_create_empty_texture(w, h);
    if (!t) return NULL;
    uint8_t *d = vita2d_texture_get_datap(t);
    unsigned stride = vita2d_texture_get_stride(t);
    for (int y = 0; y < h; y++) memcpy(d + (size_t)y * stride, rgba + (size_t)y * w * 4, (size_t)w * 4);
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return t;
}

/* ---------------- cache + worker ---------------- */

enum { ST_QUEUED, ST_LOADING, ST_DECODED, ST_READY, ST_FAILED };

typedef struct {
    char *src;
    int w, h;
    int state;
    uint8_t *rgba;
    int rw, rh;
    vita2d_texture *tex;
    unsigned last_frame;
    float born;
    size_t bytes;
    int drop;
} Slot;

#define MAXSLOTS 512
#define BUDGET (40u * 1024 * 1024)
static Slot slots[MAXSLOTS];
static int nslots_used;
static SceUID mtx, sema;
static unsigned frame_no;
static size_t total_bytes;
static int uploads_this_frame;

static void lock(void) { sceKernelLockMutex(mtx, 1, NULL); }
static void unlock(void) { sceKernelUnlockMutex(mtx, 1); }

static int worker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (;;) {
        SceUInt timeout = 200000;
        sceKernelWaitSema(sema, 1, &timeout);
        for (;;) {
            lock();
            int best = -1;
            for (int i = 0; i < nslots_used; i++)
                if (slots[i].src && slots[i].state == ST_QUEUED &&
                    (best < 0 || slots[i].last_frame > slots[best].last_frame))
                    best = i;
            if (best < 0) {
                unlock();
                break;
            }
            /* skip requests nobody looked at for a while */
            if (frame_no - slots[best].last_frame > 90) {
                free(slots[best].src);
                slots[best].src = NULL;
                unlock();
                continue;
            }
            Slot *s = &slots[best];
            s->state = ST_LOADING;
            char *src = strdup(s->src);
            int w = s->w, h = s->h;
            unlock();
            int ow = 0, oh = 0;
            uint8_t *px = src ? img_decode_file(src, w, h, &ow, &oh) : NULL;
            lock();
            /* the slot may have been dropped meanwhile */
            if (s->src && src && !strcmp(s->src, src) && s->state == ST_LOADING) {
                if (px) {
                    s->rgba = px;
                    s->rw = ow;
                    s->rh = oh;
                    s->state = ST_DECODED;
                } else {
                    s->state = ST_FAILED;
                }
            } else {
                free(px);
            }
            unlock();
            free(src);
        }
    }
    return 0;
}

void img_init(void) {
    mtx = sceKernelCreateMutex("nest_img", 0, 0, NULL);
    sema = sceKernelCreateSema("nest_img_s", 0, 0, 64, NULL);
    SceUID th = sceKernelCreateThread("nest_img", worker, 0x10000110, 512 * 1024, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

static int gpu_waited; /* one GPU wait per frame end, not one per texture */

static void free_slot(Slot *s) {
    if (s->tex) {
        if (!gpu_waited) {
            vita2d_wait_rendering_done();
            gpu_waited = 1;
        }
        vita2d_free_texture(s->tex);
        s->tex = NULL;
    }
    free(s->rgba);
    s->rgba = NULL;
    free(s->src);
    s->src = NULL;
    total_bytes -= s->bytes;
    s->bytes = 0;
    s->state = ST_FAILED;
}

static void evict(void) {
    while (total_bytes > BUDGET) {
        int victim = -1;
        for (int i = 0; i < nslots_used; i++)
            if (slots[i].src && slots[i].tex && slots[i].last_frame + 2 < frame_no &&
                (victim < 0 || slots[i].last_frame < slots[victim].last_frame))
                victim = i;
        if (victim < 0) break;
        free_slot(&slots[victim]);
    }
}

vita2d_texture *img_get(const char *src, int w, int h, float *fade) {
    if (fade) *fade = 0;
    if (!src || !*src) return NULL;
    lock();
    Slot *s = NULL;
    int free_i = -1;
    for (int i = 0; i < nslots_used; i++) {
        if (slots[i].src && slots[i].w == w && slots[i].h == h && !strcmp(slots[i].src, src)) {
            s = &slots[i];
            break;
        }
        if (!slots[i].src && free_i < 0 && slots[i].state != ST_LOADING) free_i = i;
    }
    if (!s) {
        if (free_i < 0) {
            if (nslots_used < MAXSLOTS) free_i = nslots_used++;
            else {
                /* no free slot: mark the stalest one to be freed after this frame
                   (freeing a texture mid-frame stalls the GPU and can glitch) */
                int v = -1;
                for (int i = 0; i < nslots_used; i++)
                    if (slots[i].state != ST_LOADING && slots[i].last_frame < frame_no &&
                        (v < 0 || slots[i].last_frame < slots[v].last_frame))
                        v = i;
                if (v >= 0) slots[v].drop = 1;
                unlock();
                return NULL; /* asked again next frame */
            }
        }
        s = &slots[free_i];
        memset(s, 0, sizeof(*s));
        s->src = strdup(src);
        s->w = w;
        s->h = h;
        s->state = ST_QUEUED;
        s->last_frame = frame_no;
        unlock();
        sceKernelSignalSema(sema, 1);
        return NULL;
    }
    s->last_frame = frame_no;
    s->drop = 0;
    if (s->state == ST_DECODED && uploads_this_frame < 2) {
        uint8_t *px = s->rgba;
        int rw = s->rw, rh = s->rh;
        s->rgba = NULL;
        s->state = ST_READY;
        unlock();
        vita2d_texture *t = img_upload(px, rw, rh);
        free(px);
        lock();
        uploads_this_frame++;
        s->tex = t;
        if (!t) s->state = ST_FAILED;
        s->born = g_time;
        s->bytes = (size_t)rw * rh * 4;
        total_bytes += s->bytes;
    }
    vita2d_texture *t = s->state == ST_READY ? s->tex : NULL;
    if (t && fade) *fade = clampf((g_time - s->born) / 0.35f, 0, 1);
    unlock();
    return t;
}

void img_frame_end(void) {
    lock();
    gpu_waited = 0;
    for (int i = 0; i < nslots_used; i++)
        if (slots[i].src && slots[i].drop && slots[i].state != ST_LOADING) free_slot(&slots[i]);
    frame_no++;
    uploads_this_frame = 0;
    evict();
    unlock();
}

void img_drop(const char *src) {
    lock();
    for (int i = 0; i < nslots_used; i++)
        if (slots[i].src && !strcmp(slots[i].src, src)) slots[i].drop = 1; /* freed after this frame */
    unlock();
}

void img_flush(void) {
    lock();
    for (int i = 0; i < nslots_used; i++)
        if (slots[i].src) slots[i].drop = 1;
    unlock();
}

int img_pending(void) {
    int n = 0;
    lock();
    for (int i = 0; i < nslots_used; i++)
        if (slots[i].src && (slots[i].state == ST_QUEUED || slots[i].state == ST_LOADING)) n++;
    unlock();
    return n;
}
