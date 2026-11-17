/* The picture shape an MP4 is meant to be shown at, read from its video
   track header ('tkhd'). This is what the Vita's own Video app goes by;
   AvPlayer's aspect value is unreliable for anamorphic encodes (960x544
   with slightly non-square pixels came out 4:3). Works on files on the
   memory card and on files streamed from the PC. */
#include "nest.h"

#include <psp2/io/fcntl.h>

typedef struct {
    SceUID fd;
    NetFile *nf;
} Src;

static int rd(Src *s, int64_t off, void *buf, int n) {
    if (s->nf) return netf_read(s->nf, buf, (uint64_t)off, (uint32_t)n) == n;
    if (sceIoLseek(s->fd, off, SCE_SEEK_SET) != off) return 0;
    return sceIoRead(s->fd, buf, n) == n;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static float walk(Src *s, int64_t start, int64_t end, int depth) {
    int64_t pos = start;
    int boxes = 0;
    while (pos + 8 <= end && depth < 6 && boxes++ < 4096) {
        uint8_t h[16];
        if (!rd(s, pos, h, 8)) return 0;
        uint64_t size = be32(h);
        int hl = 8;
        if (size == 1) {
            if (!rd(s, pos + 8, h + 8, 8)) return 0;
            size = (uint64_t)be32(h + 8) << 32 | be32(h + 12);
            hl = 16;
        } else if (size == 0) {
            size = (uint64_t)(end - pos);
        }
        if (size < (uint64_t)hl) return 0;
        if (!memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "trak", 4)) {
            float r = walk(s, pos + hl, pos + (int64_t)size, depth + 1);
            if (r > 0) return r;
        } else if (!memcmp(h + 4, "tkhd", 4) && size >= (uint64_t)hl + 84 && size <= 256) {
            uint8_t b[256];
            int n = (int)size - hl;
            if (!rd(s, pos + hl, b, n)) return 0;
            float w = be32(b + n - 8) / 65536.0f, hh = be32(b + n - 4) / 65536.0f;
            if (w >= 16 && hh >= 16) return w / hh; /* audio tracks are 0 x 0 */
        }
        pos += (int64_t)size;
    }
    return 0;
}

float media_aspect(const char *path) {
    if (!path || !video_is_playable(path)) return 0;
    Src s = {-1, NULL};
    int64_t end;
    if (net_is_url(path)) {
        s.nf = netf_open(path);
        if (!s.nf) return 0;
        end = (int64_t)netf_size(s.nf);
    } else {
        s.fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (s.fd < 0) return 0;
        end = sceIoLseek(s.fd, 0, SCE_SEEK_END);
    }
    float r = end > 0 ? walk(&s, 0, end, 0) : 0;
    if (s.nf) netf_close(s.nf);
    else sceIoClose(s.fd);
    return (r > 0.3f && r < 4.0f) ? r : 0;
}
