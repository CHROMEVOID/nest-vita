/* Song info from tags and file names. Plain C + stdio so it also builds and
   gets tested on a PC (tests/test_mtags.c). */
#include "mtags.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static const char *AUDIO_EXT[] = {"mp3", "flac", "ogg", "oga", "wav", "m4a", "aac", "mp4a", NULL};

static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d + 1 : "";
}
int mtags_is_audio(const char *name) {
    if (name[0] == '.') return 0; /* ._ files macOS leaves behind */
    const char *e = ext_of(name);
    for (int i = 0; AUDIO_EXT[i]; i++)
        if (!strcasecmp(e, AUDIO_EXT[i])) return 1;
    return 0;
}
int mtags_is_playlist(const char *name) {
    const char *e = ext_of(name);
    return name[0] != '.' && (!strcasecmp(e, "m3u") || !strcasecmp(e, "m3u8"));
}

unsigned mtags_hash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)tolower((unsigned char)*s)) * 16777619u;
    return h;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | p[1] << 8 | p[2]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static uint32_t syncsafe(const uint8_t *p) { return (p[0] & 127) << 21 | (p[1] & 127) << 14 | (p[2] & 127) << 7 | (p[3] & 127); }

/* ---------------- text ---------------- */

static int put_utf8(char *o, int on, int k, uint32_t c) {
    if (c < 0x80) { if (k + 1 < on) o[k++] = (char)c; }
    else if (c < 0x800) { if (k + 2 < on) { o[k++] = 0xC0 | c >> 6; o[k++] = 0x80 | (c & 63); } }
    else if (c < 0x10000) { if (k + 3 < on) { o[k++] = 0xE0 | c >> 12; o[k++] = 0x80 | (c >> 6 & 63); o[k++] = 0x80 | (c & 63); } }
    else if (k + 4 < on) { o[k++] = 0xF0 | c >> 18; o[k++] = 0x80 | (c >> 12 & 63); o[k++] = 0x80 | (c >> 6 & 63); o[k++] = 0x80 | (c & 63); }
    return k;
}

/* decode one ID3 string; returns bytes used including the terminator */
static int id3_str(int enc, const uint8_t *p, int n, char *o, int on) {
    int k = 0, i = 0;
    if (on > 0) o[0] = 0;
    if (enc == 1 || enc == 2) {
        int big = enc == 2;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { big = 0; i = 2; }
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { big = 1; i = 2; }
        while (i + 1 < n) {
            uint32_t c = big ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
            i += 2;
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
                uint32_t c2 = big ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
                if (c2 >= 0xDC00 && c2 < 0xE000) { i += 2; c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); }
            }
            k = put_utf8(o, on, k, c);
        }
    } else {
        while (i < n) {
            uint8_t c = p[i++];
            if (!c) break;
            if (enc == 3 || c < 0x80) { if (k + 1 < on) o[k++] = c; }
            else k = put_utf8(o, on, k, c); /* Latin-1 */
        }
    }
    if (on > 0) o[k < on ? k : on - 1] = 0;
    return i;
}

static void trim(char *s) {
    char *a = s;
    while (*a && (isspace((unsigned char)*a) || *a == '_')) a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && (isspace((unsigned char)s[n - 1]) || s[n - 1] == '_' || s[n - 1] == '-')) s[--n] = 0;
}

static void set_if(char *dst, int dn, const char *src) {
    if (!dst[0] && src && src[0]) {
        snprintf(dst, dn, "%s", src);
        trim(dst);
    }
}

/* ---------------- cover art ---------------- */

static void art_key(const MTags *t, const char *path, char *out, int on, const char *art_dir, const char *ext) {
    unsigned h;
    if (t->album[0]) {
        char k[600];
        snprintf(k, sizeof(k), "%s|%s", t->album, t->aartist[0] ? t->aartist : t->artist);
        h = mtags_hash(k);
    } else {
        h = mtags_hash(path);
    }
    snprintf(out, on, "%s/%08x.%s", art_dir, h, ext);
}

static void save_art(MTags *t, const char *path, const char *art_dir, const uint8_t *data, int n) {
    if (!art_dir || n < 64 || t->art[0]) return;
    const char *ext = NULL;
    if (data[0] == 0xFF && data[1] == 0xD8) ext = "jpg";
    else if (!memcmp(data, "\x89PNG", 4)) ext = "png";
    if (!ext) return;
    art_key(t, path, t->art, sizeof(t->art), art_dir, ext);
    FILE *f = fopen(t->art, "rb");
    if (f) { fclose(f); return; } /* the album's cover is already saved */
    f = fopen(t->art, "wb");
    if (!f) { t->art[0] = 0; return; }
    fwrite(data, 1, n, f);
    fclose(f);
}

/* scene names come all lower case: "you know my steez" -> "You Know My Steez" */
static void title_case_if_lower(char *s) {
    for (char *p = s; *p; p++)
        if (isupper((unsigned char)*p)) return;
    int start = 1;
    for (char *p = s; *p; p++) {
        if (start && islower((unsigned char)*p)) *p = (char)toupper((unsigned char)*p);
        start = *p == ' ' || *p == '(' || *p == '-';
    }
}

/* ---------------- ID3 ---------------- */

static void id3_frame(MTags *t, const char *id, const uint8_t *d, int n, int ver, const char *path, const char *art_dir) {
    char v[512];
    if (n < 2) return;
    int is = ver == 2;
#define IS(a, b) (!strcmp(id, is ? (b) : (a)))
    if (id[0] == 'T' && !IS("TXXX", "TXX")) {
        id3_str(d[0], d + 1, n - 1, v, sizeof(v));
        if (IS("TIT2", "TT2")) set_if(t->title, sizeof(t->title), v);
        else if (IS("TPE1", "TP1")) set_if(t->artist, sizeof(t->artist), v);
        else if (IS("TPE2", "TP2")) set_if(t->aartist, sizeof(t->aartist), v);
        else if (IS("TALB", "TAL")) set_if(t->album, sizeof(t->album), v);
        else if (IS("TRCK", "TRK")) { if (!t->track) t->track = atoi(v); }
        else if (IS("TPOS", "TPA")) { if (!t->disc) t->disc = atoi(v); }
        else if (IS("TYER", "TYE") || IS("TDRC", "TDR") || IS("TDOR", "TOR")) { if (!t->year) t->year = atoi(v); }
        else if (IS("TCON", "TCO")) {
            char *g = v;
            if (*g == '(') { char *c = strchr(g, ')'); if (c && c[1]) g = c + 1; }
            set_if(t->genre, sizeof(t->genre), g);
        }
        else if (IS("TLEN", "TLE")) { if (!t->dur) t->dur = atoi(v); }
    } else if (IS("USLT", "ULT") && t->lyrics && !t->lyrics[0] && n > 5) {
        int enc = d[0];
        char desc[128];
        int used = id3_str(enc, d + 4, n - 4, desc, sizeof(desc)); /* after the 3-letter language */
        if (4 + used < n) id3_str(enc, d + 4 + used, n - 4 - used, t->lyrics, t->lyrics_cap);
    } else if (IS("APIC", "PIC") && !t->art[0]) {
        int enc = d[0], i = 1;
        if (is) i += 3;
        else { while (i < n && d[i]) i++; i++; }   /* mime */
        if (i >= n) return;
        int ptype = d[i++];
        (void)ptype;
        char desc[64];
        i += id3_str(enc, d + i, n - i, desc, sizeof(desc));
        if (i < n) save_art(t, path, art_dir, d + i, n - i);
    }
#undef IS
}

/* returns where the audio starts */
static long read_id3v2(FILE *f, MTags *t, const char *path, const char *art_dir) {
    uint8_t h[10];
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return 0;
    int ver = h[3], flags = h[5];
    uint32_t size = syncsafe(h + 6);
    long end = 10 + size + ((flags & 0x10) ? 10 : 0);
    if (ver < 2 || ver > 4 || size > 64u * 1024 * 1024) return end;
    uint8_t *b = malloc(size + 1);
    if (!b) return end;
    if (fread(b, 1, size, f) != size) { free(b); return end; }
    if ((flags & 0x80) && ver < 4) { /* whole-tag unsynchronisation */
        uint32_t o = 0;
        for (uint32_t i = 0; i < size; i++) {
            b[o++] = b[i];
            if (b[i] == 0xFF && i + 1 < size && b[i + 1] == 0) i++;
        }
        size = o;
    }
    uint32_t pos = 0;
    if ((flags & 0x40) && ver >= 3 && size >= 4) pos = ver == 3 ? be32(b) + 4 : syncsafe(b);
    int hdr = ver == 2 ? 6 : 10;
    while (pos + hdr <= size) {
        char id[5] = {0};
        memcpy(id, b + pos, ver == 2 ? 3 : 4);
        if (!isalnum((unsigned char)id[0])) break;
        uint32_t fs = ver == 2 ? be24(b + pos + 3) : ver == 4 ? syncsafe(b + pos + 4) : be32(b + pos + 4);
        if (ver == 4 && fs > size - pos - hdr) fs = be32(b + pos + 4); /* some taggers write plain sizes in 2.4 */
        if (fs == 0 || pos + hdr + fs > size) break;
        uint8_t *d = b + pos + hdr;
        int n = fs;
        int fflags = ver == 2 ? 0 : (b[pos + 8] << 8 | b[pos + 9]);
        int skip = 0;
        if (ver == 4) {
            if (fflags & 0x0008) skip = 1; /* compressed */
            if (fflags & 0x0001) { d += 4; n -= 4; }
            if (fflags & 0x0002) { /* per-frame unsync */
                int o = 0;
                for (int i = 0; i < n; i++) {
                    d[o++] = d[i];
                    if (d[i] == 0xFF && i + 1 < n && d[i + 1] == 0) i++;
                }
                n = o;
            }
        } else if (ver == 3 && (fflags & 0x00C0)) skip = 1; /* compressed / encrypted */
        if (!skip && n > 0) id3_frame(t, id, d, n, ver, path, art_dir);
        pos += hdr + fs;
    }
    free(b);
    return end;
}

static void read_id3v1(FILE *f, long fsize, MTags *t) {
    uint8_t b[128];
    if (fsize < 256) return;
    fseek(f, fsize - 128, SEEK_SET);
    if (fread(b, 1, 128, f) != 128 || memcmp(b, "TAG", 3)) return;
    char v[64];
#define FIELD(off, len, dst)                                  \
    do {                                                      \
        id3_str(0, b + (off), (len), v, sizeof(v));           \
        set_if(dst, sizeof(dst), v);                          \
    } while (0)
    FIELD(3, 30, t->title);
    FIELD(33, 30, t->artist);
    FIELD(63, 30, t->album);
    char y[5];
    memcpy(y, b + 93, 4);
    y[4] = 0;
    if (!t->year) t->year = atoi(y);
    if (!t->track && b[125] == 0 && b[126]) t->track = b[126];
#undef FIELD
}

static const int BR1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int BR2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
static const int SR[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};

static void mp3_duration(FILE *f, long start, long fsize, MTags *t) {
    uint8_t b[16384];
    fseek(f, start, SEEK_SET);
    int n = (int)fread(b, 1, sizeof(b), f);
    for (int i = 0; i + 4 < n; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) continue;
        int vid = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3;
        int bri = b[i + 2] >> 4, sri = (b[i + 2] >> 2) & 3, mode = b[i + 3] >> 6;
        if (vid == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) continue; /* layer III only */
        int v = vid == 3 ? 0 : vid == 2 ? 1 : 2;
        int sr = SR[v][sri];
        int br = v == 0 ? BR1[bri] : BR2[bri];
        int spf = v == 0 ? 1152 : 576;
        int side = v == 0 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17);
        int x = i + 4 + side;
        if (x + 12 < n && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4))) {
            uint32_t fl = be32(b + x + 4);
            if (fl & 1) {
                uint32_t frames = be32(b + x + 8);
                if (frames) { t->dur = (int)((double)frames * spf * 1000 / sr); return; }
            }
        }
        if (i + 36 + 18 < n && !memcmp(b + i + 36, "VBRI", 4)) {
            uint32_t frames = be32(b + i + 36 + 14);
            if (frames) { t->dur = (int)((double)frames * spf * 1000 / sr); return; }
        }
        if (br) t->dur = (int)((double)(fsize - start - i) * 8 / br);
        return;
    }
}

/* ---------------- Vorbis comments (FLAC, OGG) ---------------- */

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void flac_picture(MTags *t, const uint8_t *p, int n, const char *path, const char *art_dir) {
    if (n < 32) return;
    uint32_t ml = be32(p + 4);
    if (8 + ml + 4 > (uint32_t)n) return;
    uint32_t dl = be32(p + 8 + ml);
    uint32_t o = 8 + ml + 4 + dl + 16;
    if (o + 4 > (uint32_t)n) return;
    uint32_t len = be32(p + o);
    if (o + 4 + len > (uint32_t)n) return;
    save_art(t, path, art_dir, p + o + 4, len);
}

static void vorbis_comments(MTags *t, const uint8_t *p, int n, const char *path, const char *art_dir) {
    if (n < 8) return;
    uint32_t vl = le32(p);
    if (vl > (uint32_t)n - 8) return;
    int o = 4 + vl;
    uint32_t cnt = le32(p + o);
    o += 4;
    for (uint32_t c = 0; c < cnt && o + 4 <= n; c++) {
        uint32_t l = le32(p + o);
        o += 4;
        if (l > (uint32_t)(n - o)) break;
        const char *s = (const char *)p + o;
        const char *eq = memchr(s, '=', l);
        if (eq) {
            int kl = (int)(eq - s), vlen = (int)(l - kl - 1);
            char key[32];
            snprintf(key, sizeof(key), "%.*s", MIN(kl, 31), s);
            if ((!strcasecmp(key, "LYRICS") || !strcasecmp(key, "UNSYNCEDLYRICS")) && t->lyrics && !t->lyrics[0]) {
                int k2 = vlen < t->lyrics_cap - 1 ? vlen : t->lyrics_cap - 1;
                memcpy(t->lyrics, eq + 1, k2);
                t->lyrics[k2] = 0;
            } else if (!strcasecmp(key, "METADATA_BLOCK_PICTURE")) {
                if (!t->art[0] && art_dir) {
                    uint8_t *bin = malloc(vlen);
                    int bo = 0, acc = 0, bits = 0;
                    for (int i = 0; i < vlen && bin; i++) {
                        int v = b64v((unsigned char)eq[1 + i]);
                        if (v < 0) continue;
                        acc = acc << 6 | v;
                        bits += 6;
                        if (bits >= 8) { bits -= 8; bin[bo++] = (acc >> bits) & 255; }
                    }
                    if (bin) flac_picture(t, bin, bo, path, art_dir);
                    free(bin);
                }
            } else {
                char v[512];
                snprintf(v, sizeof(v), "%.*s", MIN(vlen, 511), eq + 1);
                if (!strcasecmp(key, "TITLE")) set_if(t->title, sizeof(t->title), v);
                else if (!strcasecmp(key, "ARTIST")) set_if(t->artist, sizeof(t->artist), v);
                else if (!strcasecmp(key, "ALBUMARTIST") || !strcasecmp(key, "ALBUM ARTIST") || !strcasecmp(key, "ALBUM_ARTIST"))
                    set_if(t->aartist, sizeof(t->aartist), v);
                else if (!strcasecmp(key, "ALBUM")) set_if(t->album, sizeof(t->album), v);
                else if (!strcasecmp(key, "TRACKNUMBER") || !strcasecmp(key, "TRACK")) { if (!t->track) t->track = atoi(v); }
                else if (!strcasecmp(key, "DISCNUMBER")) { if (!t->disc) t->disc = atoi(v); }
                else if (!strcasecmp(key, "DATE") || !strcasecmp(key, "YEAR") || !strcasecmp(key, "ORIGINALDATE")) { if (!t->year) t->year = atoi(v); }
                else if (!strcasecmp(key, "GENRE")) set_if(t->genre, sizeof(t->genre), v);
            }
        }
        o += l;
    }
}

static void read_flac(FILE *f, MTags *t, const char *path, const char *art_dir) {
    uint8_t h[4];
    fseek(f, 0, SEEK_SET);
    long start = 0;
    if (fread(h, 1, 4, f) != 4) return;
    if (!memcmp(h, "ID3", 3)) { /* rare, but some rippers do it */
        start = read_id3v2(f, t, path, art_dir);
        fseek(f, start, SEEK_SET);
        if (fread(h, 1, 4, f) != 4) return;
    }
    if (memcmp(h, "fLaC", 4)) return;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t bh[4];
        if (fread(bh, 1, 4, f) != 4) return;
        int last = bh[0] & 0x80, type = bh[0] & 0x7F;
        uint32_t len = be24(bh + 1);
        if (type == 0 && len >= 18) {
            uint8_t s[34];
            if (fread(s, 1, MIN(len, 34), f) != MIN(len, 34)) return;
            if (len > 34) fseek(f, len - 34, SEEK_CUR);
            uint32_t sr = (s[10] << 12) | (s[11] << 4) | (s[12] >> 4);
            uint64_t total = ((uint64_t)(s[13] & 0x0F) << 32) | be32(s + 14);
            if (sr && total && !t->dur) t->dur = (int)(total * 1000 / sr);
        } else if ((type == 4 || (type == 6 && art_dir && !t->art[0])) && len < 32u * 1024 * 1024) {
            uint8_t *b = malloc(len);
            if (!b || fread(b, 1, len, f) != len) { free(b); return; }
            if (type == 4) vorbis_comments(t, b, len, path, art_dir);
            else flac_picture(t, b, len, path, art_dir);
            free(b);
        } else {
            fseek(f, len, SEEK_CUR);
        }
        if (last) break;
    }
}

static void read_ogg(FILE *f, long fsize, MTags *t, const char *path, const char *art_dir) {
    int cap = (int)MIN(fsize, 2 * 1024 * 1024);
    uint8_t *b = malloc(cap);
    if (!b) return;
    fseek(f, 0, SEEK_SET);
    int n = (int)fread(b, 1, cap, f);
    /* rebuild the first two packets from the pages */
    uint8_t *pk = malloc(cap);
    int pn = 0, packet = 0, rate = 0;
    int o = 0;
    while (pk && o + 27 <= n && packet < 2) {
        if (memcmp(b + o, "OggS", 4)) break;
        int ns = b[o + 26];
        if (o + 27 + ns > n) break;
        const uint8_t *seg = b + o + 27;
        int d = o + 27 + ns;
        for (int s = 0; s < ns && packet < 2; s++) {
            int l = seg[s];
            if (d + l > n) { packet = 9; break; }
            memcpy(pk + pn, b + d, l);
            pn += l;
            d += l;
            if (l < 255) { /* packet ends */
                if (packet == 0 && pn >= 16 && !memcmp(pk, "\x01vorbis", 7)) rate = (int)le32(pk + 12);
                if (packet == 1 && pn > 7 && !memcmp(pk, "\x03vorbis", 7)) vorbis_comments(t, pk + 7, pn - 7, path, art_dir);
                packet++;
                pn = 0;
            }
        }
        o = d;
    }
    free(pk);
    free(b);
    if (rate > 0 && !t->dur) { /* length: the last page's granule position */
        uint8_t e[65536];
        long from = fsize > (long)sizeof(e) ? fsize - (long)sizeof(e) : 0;
        fseek(f, from, SEEK_SET);
        int m = (int)fread(e, 1, sizeof(e), f);
        for (int i = m - 14; i >= 0; i--) {
            if (!memcmp(e + i, "OggS", 4)) {
                uint64_t g = (uint64_t)le32(e + i + 10) << 32 | le32(e + i + 6);
                if (g && g != (uint64_t)-1) t->dur = (int)(g * 1000 / rate);
                break;
            }
        }
    }
}

/* ---------------- MP4 / M4A ---------------- */

static const uint8_t *child(const uint8_t *p, uint32_t n, const char *type, uint32_t *out_n) {
    uint32_t o = 0;
    while (o + 8 <= n) {
        uint32_t sz = be32(p + o);
        if (sz < 8 || o + sz > n) break;
        if (!memcmp(p + o + 4, type, 4)) { *out_n = sz - 8; return p + o + 8; }
        o += sz;
    }
    return NULL;
}

static void read_mp4(FILE *f, long fsize, MTags *t, const char *path, const char *art_dir) {
    uint8_t h[16];
    long o = 0;
    while (o + 8 <= fsize) {
        fseek(f, o, SEEK_SET);
        if (fread(h, 1, 16, f) < 8) return;
        uint64_t sz = be32(h);
        int hl = 8;
        if (sz == 1) { sz = (uint64_t)be32(h + 8) << 32 | be32(h + 12); hl = 16; }
        else if (sz == 0) sz = fsize - o;
        if (sz < 8) return;
        if (!memcmp(h + 4, "moov", 4)) {
            uint32_t n = (uint32_t)(sz - hl);
            if (n > 48u * 1024 * 1024) return;
            uint8_t *m = malloc(n);
            fseek(f, o + hl, SEEK_SET);
            if (!m || fread(m, 1, n, f) != n) { free(m); return; }
            uint32_t ln;
            const uint8_t *mv = child(m, n, "mvhd", &ln);
            if (mv && ln >= 24 && !t->dur) {
                uint32_t ts, d32;
                if (mv[0] == 1 && ln >= 32) {
                    ts = be32(mv + 20);
                    uint64_t d = (uint64_t)be32(mv + 24) << 32 | be32(mv + 28);
                    if (ts) t->dur = (int)(d * 1000 / ts);
                } else {
                    ts = be32(mv + 12);
                    d32 = be32(mv + 16);
                    if (ts) t->dur = (int)((uint64_t)d32 * 1000 / ts);
                }
            }
            uint32_t un, mn, iln;
            const uint8_t *ud = child(m, n, "udta", &un);
            const uint8_t *me = ud ? child(ud, un, "meta", &mn) : child(m, n, "meta", &mn);
            const uint8_t *il = me && mn > 4 ? child(me + 4, mn - 4, "ilst", &iln) : NULL;
            uint32_t io = 0;
            while (il && io + 8 <= iln) {
                uint32_t isz = be32(il + io);
                if (isz < 8 || io + isz > iln) break;
                const uint8_t *name = il + io + 4;
                uint32_t dn;
                const uint8_t *da = child(il + io + 8, isz - 8, "data", &dn);
                if (da && dn > 8) {
                    const uint8_t *v = da + 8;
                    uint32_t vn = dn - 8;
                    char s[512];
                    snprintf(s, sizeof(s), "%.*s", (int)MIN(vn, 511), (const char *)v);
                    if (!memcmp(name, "\xa9nam", 4)) set_if(t->title, sizeof(t->title), s);
                    else if (!memcmp(name, "\xa9" "ART", 4)) set_if(t->artist, sizeof(t->artist), s);
                    else if (!memcmp(name, "aART", 4)) set_if(t->aartist, sizeof(t->aartist), s);
                    else if (!memcmp(name, "\xa9" "alb", 4)) set_if(t->album, sizeof(t->album), s);
                    else if (!memcmp(name, "\xa9gen", 4)) set_if(t->genre, sizeof(t->genre), s);
                    else if (!memcmp(name, "\xa9" "day", 4)) { if (!t->year) t->year = atoi(s); }
                    else if (!memcmp(name, "trkn", 4) && vn >= 4) { if (!t->track) t->track = v[2] << 8 | v[3]; }
                    else if (!memcmp(name, "disk", 4) && vn >= 4) { if (!t->disc) t->disc = v[2] << 8 | v[3]; }
                    else if (!memcmp(name, "covr", 4)) save_art(t, path, art_dir, v, vn);
                    else if (!memcmp(name, "\xa9lyr", 4) && t->lyrics && !t->lyrics[0]) {
                        int k2 = (int)vn < t->lyrics_cap - 1 ? (int)vn : t->lyrics_cap - 1;
                        memcpy(t->lyrics, v, k2);
                        t->lyrics[k2] = 0;
                    }
                }
                io += isz;
            }
            free(m);
            return;
        }
        o += (long)sz;
    }
}

/* ---------------- WAV ---------------- */

static void read_wav(FILE *f, long fsize, MTags *t) {
    uint8_t h[12];
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return;
    long o = 12;
    int rate = 0, align = 0;
    while (o + 8 <= fsize) {
        uint8_t c[8];
        fseek(f, o, SEEK_SET);
        if (fread(c, 1, 8, f) != 8) return;
        uint32_t len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            uint8_t fm[16];
            if (fread(fm, 1, 16, f) != 16) return;
            rate = (int)le32(fm + 4);
            align = fm[12] | fm[13] << 8;
        } else if (!memcmp(c, "data", 4)) {
            if (rate && align && !t->dur) t->dur = (int)((uint64_t)len / align * 1000 / rate);
        } else if (!memcmp(c, "LIST", 4) && len < 1 << 20) {
            uint8_t *b = malloc(len);
            if (b && fread(b, 1, len, f) == len && !memcmp(b, "INFO", 4)) {
                uint32_t i = 4;
                while (i + 8 <= len) {
                    uint32_t l = le32(b + i + 4);
                    if (i + 8 + l > len) break;
                    char v[256];
                    snprintf(v, sizeof(v), "%.*s", (int)MIN(l, 255), (const char *)b + i + 8);
                    if (!memcmp(b + i, "INAM", 4)) set_if(t->title, sizeof(t->title), v);
                    else if (!memcmp(b + i, "IART", 4)) set_if(t->artist, sizeof(t->artist), v);
                    else if (!memcmp(b + i, "IPRD", 4)) set_if(t->album, sizeof(t->album), v);
                    else if (!memcmp(b + i, "ICRD", 4)) { if (!t->year) t->year = atoi(v); }
                    else if (!memcmp(b + i, "ITRK", 4) || !memcmp(b + i, "IPRT", 4)) { if (!t->track) t->track = atoi(v); }
                    else if (!memcmp(b + i, "IGNR", 4)) set_if(t->genre, sizeof(t->genre), v);
                    i += 8 + l + (l & 1);
                }
            }
            free(b);
        }
        o += 8 + len + (len & 1);
    }
}

void mtags_read(const char *path, long long size, MTags *t, const char *art_dir) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    const char *e = ext_of(path);
    long fs = (long)size;
    if (fs <= 0) { fseek(f, 0, SEEK_END); fs = ftell(f); }
    if (!strcasecmp(e, "mp3")) {
        long start = read_id3v2(f, t, path, art_dir);
        read_id3v1(f, fs, t);
        if (!t->dur) mp3_duration(f, start, fs, t);
    } else if (!strcasecmp(e, "flac")) read_flac(f, t, path, art_dir);
    else if (!strcasecmp(e, "ogg") || !strcasecmp(e, "oga")) read_ogg(f, fs, t, path, art_dir);
    else if (!strcasecmp(e, "wav")) read_wav(f, fs, t);
    else if (!strcasecmp(e, "aac")) read_id3v2(f, t, path, art_dir);
    else read_mp4(f, fs, t, path, art_dir);
    fclose(f);
    trim(t->title); trim(t->artist); trim(t->aartist); trim(t->album);
}

/* ---------------- names ---------------- */

/* words that mean a bracket is download junk, not part of the title */
static const char *JUNK[] = {"kbps", "320", "256", "192", "128", "flac", "mp3", "m4a", "aac", "lossless", "24bit", "24-bit",
                             "16bit", "web", "cd", "vinyl", "official", "lyric", "audio", "video", "visualizer", "hq", "hd",
                             "4k", "explicit", "clean version", "free download", "www.", ".com", ".net", ".org", "ytmp3",
                             "y2mate", "spotdl", "music video", "mv", "remaster", "itunes", "deezer", "rip", "scene", NULL};

static int is_junk(const char *s, int n) {
    char low[128];
    snprintf(low, sizeof(low), "%.*s", MIN(n, 127), s);
    for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
    /* keep things that are clearly part of a song name */
    if (strstr(low, "feat") || strstr(low, "ft.") || strstr(low, "remix") || strstr(low, "live") || strstr(low, "acoustic") ||
        strstr(low, "version") || strstr(low, "edit") || strstr(low, "mix") || strstr(low, "demo") || strstr(low, "instrumental"))
        return strstr(low, "official") || strstr(low, "lyric") ? 1 : 0;
    int digits = 1;
    for (char *p = low; *p; p++)
        if (!isdigit((unsigned char)*p)) digits = 0;
    if (digits && strlen(low) == 4) return 1; /* (2019) */
    for (int i = 0; JUNK[i]; i++)
        if (strstr(low, JUNK[i])) return 1;
    return 0;
}

void mtags_clean(char *s) {
    /* "_" or "." used as spaces */
    if (!strchr(s, ' ')) {
        int dots = 0;
        for (char *p = s; *p; p++) dots += *p == '.';
        for (char *p = s; *p; p++)
            if (*p == '_' || (dots >= 2 && *p == '.')) *p = ' ';
    } else {
        for (char *p = s; *p; p++)
            if (*p == '_') *p = ' ';
    }
    /* drop junk brackets */
    char out[512];
    int k = 0;
    for (int i = 0; s[i] && k < 510;) {
        char c = s[i];
        if (c == '(' || c == '[' || c == '{') {
            char close = c == '(' ? ')' : c == '[' ? ']' : '}';
            const char *e = strchr(s + i + 1, close);
            if (e && is_junk(s + i + 1, (int)(e - s - i - 1))) {
                i = (int)(e - s) + 1;
                continue;
            }
        }
        out[k++] = c;
        i++;
    }
    out[k] = 0;
    /* collapse spaces */
    k = 0;
    for (int i = 0; out[i]; i++) {
        if (out[i] == ' ' && (k == 0 || s[k - 1] == ' ')) continue;
        s[k++] = out[i];
    }
    s[k] = 0;
    trim(s);
    /* "www.something.com - " at the front */
    if (!strncasecmp(s, "www.", 4)) {
        char *sp = strstr(s, " - ");
        if (sp) memmove(s, sp + 3, strlen(sp + 3) + 1);
    }
    trim(s);
}

static int is_disc_folder(const char *n, int *disc) {
    const char *p = n;
    if (!strncasecmp(p, "cd", 2)) p += 2;
    else if (!strncasecmp(p, "disc", 4)) p += 4;
    else if (!strncasecmp(p, "disk", 4)) p += 4;
    else return 0;
    while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    int d = atoi(p);
    while (isdigit((unsigned char)*p)) p++;
    while (*p == ' ') p++;
    if (*p && *p != '(' && *p != '[' && *p != '-') return 0;
    if (disc) *disc = d;
    return 1;
}

static const char *GENERIC[] = {"music", "my music", "mp3", "mp3s", "songs", "audio", "downloads", "download", "new folder",
                                "ux0:", "uma0:", "ur0:", "imc0:", "xmc0:", "grw0:", "data", "nest", "playlists", "tracks",
                                "singles", "unsorted", "misc", "various", "spotify", "spotdl", NULL};
static int is_generic(const char *n) {
    if (!n[0] || strchr(n, ':')) return 1;
    for (int i = 0; GENERIC[i]; i++)
        if (!strcasecmp(n, GENERIC[i])) return 1;
    return 0;
}

/* "Artist - Album (2004) [FLAC]" -> artist, album, year */
static void parse_album_folder(const char *folder, char *artist, int an, char *album, int bn, int *year) {
    char f[256];
    snprintf(f, sizeof(f), "%s", folder);
    /* a year in brackets or at the front */
    for (char *p = f; *p; p++) {
        if ((*p == '(' || *p == '[') && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[4]) && (p[5] == ')' || p[5] == ']')) {
            int y = atoi(p + 1);
            if (y > 1900 && y < 2100 && !*year) *year = y;
        }
    }
    if (isdigit((unsigned char)f[0]) && isdigit((unsigned char)f[3]) && !isdigit((unsigned char)f[4])) {
        int y = atoi(f);
        if (y > 1900 && y < 2100) {
            char *q = f + 4;
            while (*q == ' ' || *q == '-' || *q == '.' || *q == '_' || *q == ')' || *q == ']') q++;
            if (*q) {
                if (!*year) *year = y;
                memmove(f, q, strlen(q) + 1);
            }
        }
    }
    { /* scene release suffix: "-1998-FTD", "-WEB-2019-GRP", "-(CD)-" */
        char *p = f;
        while ((p = strchr(p, '-'))) {
            if (isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[4]) && (p[5] == '-' || !p[5])) {
                int y = atoi(p + 1);
                if (y > 1900 && y < 2100) {
                    if (!*year) *year = y;
                    *p = 0;
                    break;
                }
            }
            p++;
        }
        size_t n = strlen(f);
        while (n > 3 && (!strcasecmp(f + n - 4, "-web") || !strcasecmp(f + n - 3, "-cd") || !strcasecmp(f + n - 6, "-vinyl"))) {
            *strrchr(f, '-') = 0;
            n = strlen(f);
        }
    }
    mtags_clean(f);
    char *sep = strstr(f, " - ");
    if (sep) {
        *sep = 0;
        snprintf(artist, an, "%s", f);
        snprintf(album, bn, "%s", sep + 3);
        /* "Album - 2004" leftovers */
        trim(album);
    } else {
        artist[0] = 0;
        snprintf(album, bn, "%s", f);
    }
    trim(artist);
    trim(album);
}

void mtags_guess(const char *path, MTags *t) {
    char p[1024];
    snprintf(p, sizeof(p), "%s", path);
    for (char *c = p; *c; c++)
        if (*c == '\\') *c = '/';
    /* split into folders */
    char *parts[64];
    int np = 0;
    char *s = p;
    for (char *c = p; *c; c++) {
        if (*c == '/') {
            *c = 0;
            if (np < 63) parts[np++] = s;
            s = c + 1;
        }
    }
    char name[512];
    snprintf(name, sizeof(name), "%s", s);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;

    /* ---- file name: track number, artist, title ---- */
    char work[512];
    snprintf(work, sizeof(work), "%s", name);
    int track = 0, disc = 0, track_word = 0;
    /* scene style "04-gang_starr-you_know_my_steez": dashes without spaces */
    if (!strchr(work, ' ') && !strstr(work, " - ") && strchr(work, '-')) {
        char tmp[600];
        int k = 0;
        for (char *c = work; *c && k < 590; c++) {
            if (*c == '-' && c != work) { memcpy(tmp + k, " - ", 3); k += 3; }
            else tmp[k++] = *c;
        }
        tmp[k] = 0;
        snprintf(work, sizeof(work), "%s", tmp);
    }
    char *w = work;
    while (*w == ' ' || *w == '_') w++;
    if (!strncasecmp(w, "track", 5) && (w[5] == ' ' || w[5] == '_' || isdigit((unsigned char)w[5]))) {
        char *q = w + 5;
        while (*q == ' ' || *q == '_') q++;
        if (isdigit((unsigned char)*q)) { w = q; track_word = 1; }
    }
    /* 1-03, 1.03, 103 (disc + track) and 03 / 3 */
    int nd = 0;
    while (isdigit((unsigned char)w[nd])) nd++;
    if (nd >= 1 && nd <= 3 && (w[nd] == '-' || w[nd] == '.') && isdigit((unsigned char)w[nd + 1]) && isdigit((unsigned char)w[nd + 2]) &&
        !isdigit((unsigned char)w[nd + 3]) && nd == 1) {
        disc = w[0] - '0';
        track = atoi(w + nd + 1);
        w += nd + 3;
    } else if (nd >= 1 && nd <= 3) {
        char after = w[nd];
        int sep_ok = after == ' ' || after == '-' || after == '.' || after == '_' || after == ')' || after == ']' || after == 0;
        /* "1979" or "22 Acacia Avenue": only a number followed by a separator counts, and never a 4-digit one */
        if (sep_ok && (after != 0 || track_word)) {
            int v = atoi(w);
            if (nd == 3 && v >= 101 && v <= 999 && v % 100 >= 1 && v % 100 <= 40 && !disc) {
                disc = v / 100;
                track = v % 100;
            } else track = v;
            w += nd;
        }
    }
    if (track) {
        while (*w == ' ' || *w == '-' || *w == '.' || *w == '_' || *w == ')' || *w == ']') w++;
    }
    char rest[512];
    snprintf(rest, sizeof(rest), "%s", w);
    mtags_clean(rest);
    /* "Artist - Title", "Artist - Album - 03 - Title" */
    char fart[256] = "", ftitle[256] = "";
    char *seg[8];
    int ns = 0;
    char *r = rest;
    while (ns < 8) {
        char *x = strstr(r, " - ");
        seg[ns++] = r;
        if (!x) break;
        *x = 0;
        r = x + 3;
    }
    if (ns == 1) snprintf(ftitle, sizeof(ftitle), "%s", seg[0]);
    else {
        /* a bare number segment is the track */
        int ti = ns - 1;
        for (int i = 1; i < ns - 1; i++) {
            int dg = 1;
            for (char *c = seg[i]; *c; c++)
                if (!isdigit((unsigned char)*c)) dg = 0;
            if (dg && strlen(seg[i]) <= 3 && !track) track = atoi(seg[i]);
        }
        snprintf(ftitle, sizeof(ftitle), "%s", seg[ti]);
        snprintf(fart, sizeof(fart), "%s", seg[0]);
        /* "03 - Title" left after a numeric artist segment */
        int dg = 1;
        for (char *c = fart; *c; c++)
            if (!isdigit((unsigned char)*c)) dg = 0;
        if (dg) { if (!track) track = atoi(fart); fart[0] = 0; }
    }
    trim(ftitle);
    trim(fart);
    if (track_word && !ftitle[0]) snprintf(ftitle, sizeof(ftitle), "%s", name);
    title_case_if_lower(ftitle);
    title_case_if_lower(fart);

    /* ---- folders: album and artist ---- */
    int ai = np - 1; /* album folder */
    int fdisc = 0;
    if (ai >= 0 && is_disc_folder(parts[ai], &fdisc)) {
        if (!disc) disc = fdisc;
        ai--;
    }
    char dart[256] = "", dalb[256] = "";
    int dyear = 0;
    if (ai >= 0 && !is_generic(parts[ai])) {
        parse_album_folder(parts[ai], dart, sizeof(dart), dalb, sizeof(dalb), &dyear);
        if (!dart[0] && ai - 1 >= 0 && !is_generic(parts[ai - 1])) {
            snprintf(dart, sizeof(dart), "%s", parts[ai - 1]);
            mtags_clean(dart);
        }
    }

    /* ---- fill gaps, tags first ---- */
    int title_bad = !t->title[0] || !strncasecmp(t->title, "track ", 6) || !strcasecmp(t->title, name);
    if (title_bad) {
        if (ftitle[0]) snprintf(t->title, sizeof(t->title), "%s", ftitle);
        else if (!t->title[0]) snprintf(t->title, sizeof(t->title), "%s", name);
    } else {
        mtags_clean(t->title);
    }
    if (!t->track) t->track = track;
    if (!t->disc) t->disc = disc;
    if (!t->artist[0]) {
        if (fart[0]) snprintf(t->artist, sizeof(t->artist), "%s", fart);
        else if (t->aartist[0]) snprintf(t->artist, sizeof(t->artist), "%s", t->aartist);
        else if (dart[0]) snprintf(t->artist, sizeof(t->artist), "%s", dart);
    }
    if (!t->album[0] && dalb[0]) {
        /* a folder full of loose songs isn't an album: only use it when it looks like one */
        snprintf(t->album, sizeof(t->album), "%s", dalb);
        if (!t->aartist[0] && dart[0]) snprintf(t->aartist, sizeof(t->aartist), "%s", dart);
    }
    if (!t->year) t->year = dyear;
    if (t->year < 1000 || t->year > 2200) t->year = 0;
    if (t->track < 0 || t->track > 999) t->track = 0;
    if (t->disc < 0 || t->disc > 99) t->disc = 0;
}

/* ---------------- lyrics ---------------- */

int mtags_lyrics(const char *path, char *out, int cap) {
    if (!out || cap < 2) return 0;
    out[0] = 0;
    /* "Song.lrc" next to "Song.mp3" wins: people add those on purpose */
    char lrc[1100];
    snprintf(lrc, sizeof(lrc), "%s", path);
    char *dot = strrchr(lrc, '.');
    if (dot) {
        strcpy(dot, ".lrc");
        FILE *f = fopen(lrc, "rb");
        if (f) {
            int n = (int)fread(out, 1, cap - 1, f);
            fclose(f);
            out[n > 0 ? n : 0] = 0;
            if (n > 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
                memmove(out, out + 3, n - 2);
            if (out[0]) return (int)strlen(out);
        }
    }
    MTags t;
    memset(&t, 0, sizeof(t));
    t.lyrics = out;
    t.lyrics_cap = cap;
    mtags_read(path, 0, &t, NULL);
    return (int)strlen(out);
}

/* ---------------- video files ---------------- */

int mtags_video(const char *path, char *desc, int dn, char *genre, int gn, int *year, char *show, int sn,
                const char *cover_base, char *cover_out, int con) {
    if (dn) desc[0] = 0;
    if (gn) genre[0] = 0;
    if (sn) show[0] = 0;
    if (con) cover_out[0] = 0;
    *year = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    uint8_t h[16];
    long o = 0;
    int got = 0;
    while (o + 8 <= fsize) {
        fseek(f, o, SEEK_SET);
        if (fread(h, 1, 16, f) < 8) break;
        uint64_t sz = be32(h);
        int hl = 8;
        if (sz == 1) { sz = (uint64_t)be32(h + 8) << 32 | be32(h + 12); hl = 16; }
        else if (sz == 0) sz = fsize - o;
        if (sz < 8) break;
        if (!memcmp(h + 4, "moov", 4)) {
            uint32_t n = (uint32_t)(sz - hl);
            if (n > 24u * 1024 * 1024) break;
            uint8_t *m = malloc(n);
            fseek(f, o + hl, SEEK_SET);
            if (!m || fread(m, 1, n, f) != n) { free(m); break; }
            uint32_t un, mn, iln;
            const uint8_t *ud = child(m, n, "udta", &un);
            const uint8_t *me = ud ? child(ud, un, "meta", &mn) : child(m, n, "meta", &mn);
            const uint8_t *il = me && mn > 4 ? child(me + 4, mn - 4, "ilst", &iln) : NULL;
            uint32_t io = 0;
            while (il && io + 8 <= iln) {
                uint32_t isz = be32(il + io);
                if (isz < 8 || io + isz > iln) break;
                const uint8_t *name = il + io + 4;
                uint32_t dn2;
                const uint8_t *da = child(il + io + 8, isz - 8, "data", &dn2);
                if (da && dn2 > 8) {
                    const uint8_t *v = da + 8;
                    uint32_t vn = dn2 - 8;
                    if ((!memcmp(name, "ldes", 4) || (!memcmp(name, "desc", 4) && !desc[0])) && dn) {
                        snprintf(desc, dn, "%.*s", (int)MIN(vn, (uint32_t)dn - 1), (const char *)v);
                        got = 1;
                    } else if (!memcmp(name, "\xa9gen", 4) && gn) {
                        snprintf(genre, gn, "%.*s", (int)MIN(vn, (uint32_t)gn - 1), (const char *)v);
                        got = 1;
                    } else if (!memcmp(name, "\xa9" "day", 4)) {
                        char y[8];
                        snprintf(y, sizeof(y), "%.*s", (int)MIN(vn, 7u), (const char *)v);
                        int yy = atoi(y);
                        if (yy > 1900 && yy < 2100) { *year = yy; got = 1; }
                    } else if (!memcmp(name, "tvsh", 4) && sn) {
                        snprintf(show, sn, "%.*s", (int)MIN(vn, (uint32_t)sn - 1), (const char *)v);
                        got = 1;
                    } else if (!memcmp(name, "covr", 4) && cover_base && con && vn > 64) {
                        const char *ext = v[0] == 0xFF && v[1] == 0xD8 ? "jpg" : !memcmp(v, "\x89PNG", 4) ? "png" : NULL;
                        if (ext) {
                            snprintf(cover_out, con, "%s.%s", cover_base, ext);
                            FILE *cf = fopen(cover_out, "wb");
                            if (cf) {
                                fwrite(v, 1, vn, cf);
                                fclose(cf);
                                got = 1;
                            } else cover_out[0] = 0;
                        }
                    }
                }
                io += isz;
            }
            free(m);
            break;
        }
        o += (long)sz;
    }
    fclose(f);
    return got;
}
