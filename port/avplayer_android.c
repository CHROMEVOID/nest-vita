/* Nest port (Android): the Vita's AvPlayer on top of the device's hardware decoders
   (NDK MediaExtractor + MediaCodec). MP4, MKV and WebM; H.264, H.265, VP9, AV1 where
   the chip supports them; AAC, AC-3/E-AC-3, Opus, MP3 audio. Nest's player gets
   exactly what it got on the Vita: video frames as Y + interleaved VU planes,
   audio in 1024-sample chunks, a clock in milliseconds, seeking and pausing. */
#ifdef __ANDROID__
#include "port.h"
#undef fopen
#undef main

#include <SDL.h>
#include <android/log.h>
#include <android/native_window.h>
#include <fcntl.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include "psp2_all.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "Nest", __VA_ARGS__)
#define RING_FRAMES (48000 * 3)
#define NFRAMEBUF 3

typedef struct {
    int used;
    char path[1200];
    int fd_v, fd_a;
    off64_t fsize;
    /* video */
    AMediaExtractor *vex;
    AMediaCodec *vdec;
    AImageReader *reader;
    ANativeWindow *win;
    int vw, vh;
    uint8_t *fbuf[NFRAMEBUF];
    size_t fbuf_size;
    int fb_write;                    /* next buffer to fill */
    volatile int fb_ready;           /* buffer with the newest frame, -1 none */
    volatile unsigned int fb_serial; /* goes up with each frame */
    unsigned int fb_served;
    int64_t fb_pts;
    int frame_w, frame_h;
    /* audio */
    AMediaExtractor *aex;
    AMediaCodec *adec;
    int arate, ach;                  /* decoded format */
    int16_t *ring;                   /* stereo (or mono) frames */
    int ring_ch;
    volatile int rd, wr, count;
    int64_t ring_pts;                /* pts (us) of the frame at rd; -1 unknown */
    int16_t out[1024 * 2];
    /* clock */
    SDL_mutex *mx;
    volatile int paused, speed;
    int64_t base_us;                 /* clock when the wall clock started counting */
    Uint64 wall_ms;
    volatile int64_t audio_clock;    /* pts of audio handed to Nest */
    int has_audio, has_video;
    int64_t dur_us;
    /* control */
    volatile int run;
    volatile int vdone, adone;
    volatile int64_t seek_us;        /* -1 none */
    volatile int seek_gen;
    SDL_Thread *vth, *ath;
} AV;

static AV players[4];

static int64_t now_wall(void) { return (int64_t)SDL_GetTicks64(); }

/* the master clock in microseconds */
static int64_t clock_us(AV *a) {
    if (a->has_audio && a->speed == 100 && a->audio_clock >= 0) {
        /* audio Nest has taken is still in the output queue: about 90 ms */
        int64_t c = a->audio_clock - 90000;
        return c < 0 ? 0 : c;
    }
    if (a->paused) return a->base_us;
    return a->base_us + (now_wall() - (int64_t)a->wall_ms) * 1000 * a->speed / 100;
}

static void restart_wall(AV *a, int64_t at) {
    a->base_us = at;
    a->wall_ms = (Uint64)now_wall();
}

/* ---------------------------------------------------------------- opening */

static AMediaExtractor *open_extractor(AV *a, int *fd_out) {
    AMediaExtractor *ex = AMediaExtractor_new();
    if (!ex) return NULL;
    media_status_t st;
    if (!strncmp(a->path, "http://", 7) || !strncmp(a->path, "https://", 8)) {
        *fd_out = -1;
        st = AMediaExtractor_setDataSource(ex, a->path);
    } else {
        int fd = open(a->path, O_RDONLY);
        if (fd < 0) { AMediaExtractor_delete(ex); return NULL; }
        struct stat s;
        fstat(fd, &s);
        a->fsize = s.st_size;
        *fd_out = fd;
        st = AMediaExtractor_setDataSourceFd(ex, fd, 0, s.st_size);
    }
    if (st != AMEDIA_OK) {
        LOG("extractor failed (%d) for %s", st, a->path);
        AMediaExtractor_delete(ex);
        if (*fd_out >= 0) close(*fd_out);
        *fd_out = -1;
        return NULL;
    }
    return ex;
}

static int find_track(AMediaExtractor *ex, const char *kind, AMediaFormat **fmt_out) {
    size_t n = AMediaExtractor_getTrackCount(ex);
    for (size_t i = 0; i < n; i++) {
        AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
        const char *mime = NULL;
        if (f && AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && mime && !strncmp(mime, kind, strlen(kind))) {
            *fmt_out = f;
            return (int)i;
        }
        if (f) AMediaFormat_delete(f);
    }
    return -1;
}

/* ---------------------------------------------------------------- video thread */

/* copy an Android YUV_420_888 image into the Vita's layout: Y plane, then V,U interleaved */
static void copy_image(AV *a, AImage *img, uint8_t *dst) {
    int32_t w = 0, h = 0;
    AImage_getWidth(img, &w);
    AImage_getHeight(img, &h);
    AImageCropRect crop = {0, 0, w, h};
    AImage_getCropRect(img, &crop);
    int cw = (crop.right - crop.left) & ~1, ch = (crop.bottom - crop.top) & ~1;
    if (cw <= 0 || ch <= 0) return;
    if (cw > a->frame_w || ch > a->frame_h || !dst) return;
    uint8_t *y, *u, *v;
    int ylen, ulen, vlen, ys, us, vs, ups, vps;
    AImage_getPlaneData(img, 0, &y, &ylen);
    AImage_getPlaneData(img, 1, &u, &ulen);
    AImage_getPlaneData(img, 2, &v, &vlen);
    AImage_getPlaneRowStride(img, 0, &ys);
    AImage_getPlaneRowStride(img, 1, &us);
    AImage_getPlaneRowStride(img, 2, &vs);
    AImage_getPlanePixelStride(img, 1, &ups);
    AImage_getPlanePixelStride(img, 2, &vps);
    int W = a->frame_w;
    for (int r = 0; r < ch; r++) memcpy(dst + (size_t)r * W, y + (size_t)(r + crop.top) * ys + crop.left, cw);
    uint8_t *vu = dst + (size_t)W * a->frame_h;
    int cx = crop.left / 2, cy = crop.top / 2;
    for (int r = 0; r < ch / 2; r++) {
        uint8_t *o = vu + (size_t)r * W;
        const uint8_t *vr = v + (size_t)(r + cy) * vs + cx * vps;
        const uint8_t *ur = u + (size_t)(r + cy) * us + cx * ups;
        if (vps == 2 && ups == 2 && ur == vr + 1) {
            memcpy(o, vr, cw); /* already V,U,V,U (NV21) */
        } else {
            for (int i = 0; i < cw / 2; i++) {
                o[i * 2] = vr[i * vps];
                o[i * 2 + 1] = ur[i * ups];
            }
        }
    }
}

static int video_thread(void *p) {
    AV *a = p;
    int gen = a->seek_gen;
    int64_t drop_before = -1;
    int in_eos = 0;
    while (a->run) {
        if (gen != a->seek_gen) { /* jump */
            gen = a->seek_gen;
            int64_t to = a->seek_us;
            AMediaExtractor_seekTo(a->vex, to, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
            AMediaCodec_flush(a->vdec);
            drop_before = to;
            in_eos = 0;
            a->vdone = 0;
        }
        /* feed */
        if (!in_eos) {
            ssize_t ii = AMediaCodec_dequeueInputBuffer(a->vdec, 2000);
            if (ii >= 0) {
                size_t cap;
                uint8_t *buf = AMediaCodec_getInputBuffer(a->vdec, ii, &cap);
                ssize_t n = AMediaExtractor_readSampleData(a->vex, buf, cap);
                if (n < 0) {
                    AMediaCodec_queueInputBuffer(a->vdec, ii, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    in_eos = 1;
                } else {
                    AMediaCodec_queueInputBuffer(a->vdec, ii, 0, n, AMediaExtractor_getSampleTime(a->vex), 0);
                    AMediaExtractor_advance(a->vex);
                }
            }
        }
        /* output */
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(a->vdec, &info, 4000);
        if (oi >= 0) {
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) a->vdone = 1;
            int64_t pts = info.presentationTimeUs;
            int show = info.size > 0 || !(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            if (drop_before >= 0 && pts < drop_before - 1000) show = 0; /* before the jump point */
            else drop_before = -1;
            /* wait for its moment */
            while (show && a->run && gen == a->seek_gen) {
                if (a->paused && a->fb_ready >= 0) { SDL_Delay(5); continue; }
                int64_t c = clock_us(a);
                if (pts <= c + 8000) break;
                int64_t wait = (pts - c) / 1000;
                SDL_Delay(wait > 10 ? 10 : (Uint32)(wait > 1 ? wait : 1));
            }
            if (gen != a->seek_gen) { AMediaCodec_releaseOutputBuffer(a->vdec, oi, false); continue; }
            /* far behind (a slow moment): skip it rather than lag */
            if (show && a->fb_ready >= 0 && clock_us(a) - pts > 120000 && !a->paused) show = 0;
            AMediaCodec_releaseOutputBuffer(a->vdec, oi, show ? true : false);
            if (show) {
                AImage *img = NULL;
                for (int k = 0; k < 20 && a->run; k++) {
                    if (AImageReader_acquireLatestImage(a->reader, &img) == AMEDIA_OK && img) break;
                    img = NULL;
                    SDL_Delay(1);
                }
                if (img) {
                    int w = a->fb_write;
                    copy_image(a, img, a->fbuf[w]);
                    AImage_delete(img);
                    SDL_LockMutex(a->mx);
                    a->fb_ready = w;
                    a->fb_pts = pts;
                    a->fb_serial++;
                    a->fb_write = (w + 1) % NFRAMEBUF;
                    SDL_UnlockMutex(a->mx);
                }
            }
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *f = AMediaCodec_getOutputFormat(a->vdec);
            if (f) AMediaFormat_delete(f);
        } else if (in_eos && a->vdone) {
            SDL_Delay(10);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- audio thread */

static void ring_push(AV *a, const int16_t *s, int frames, int src_ch, int64_t pts) {
    int och = a->ring_ch;
    for (int i = 0; i < frames && a->run;) {
        if (a->count >= RING_FRAMES - 1) { /* full: Nest hasn't taken it yet */
            SDL_Delay(4);
            continue;
        }
        SDL_LockMutex(a->mx);
        if (a->count == 0) a->ring_pts = pts + (int64_t)i * 1000000 / (a->arate ? a->arate : 48000);
        while (i < frames && a->count < RING_FRAMES - 1) {
            const int16_t *f = s + (size_t)i * src_ch;
            int16_t *o = a->ring + (size_t)a->wr * och;
            if (och == 1) o[0] = f[0];
            else if (src_ch == 1) o[0] = o[1] = f[0];
            else if (src_ch == 2) { o[0] = f[0]; o[1] = f[1]; }
            else { /* 5.1 / 7.1 down to stereo: FL FR C LFE BL BR ... */
                int c = src_ch > 2 ? f[2] : 0;
                int bl = src_ch > 4 ? f[4] : 0, br = src_ch > 5 ? f[5] : 0;
                int l = f[0] + c * 7 / 10 + bl * 7 / 10, r = f[1] + c * 7 / 10 + br * 7 / 10;
                o[0] = (int16_t)SDL_clamp(l * 6 / 10, -32768, 32767);
                o[1] = (int16_t)SDL_clamp(r * 6 / 10, -32768, 32767);
            }
            a->wr = (a->wr + 1) % RING_FRAMES;
            a->count++;
            i++;
        }
        SDL_UnlockMutex(a->mx);
    }
}

static int audio_thread(void *p) {
    AV *a = p;
    int gen = a->seek_gen;
    int in_eos = 0;
    while (a->run) {
        if (gen != a->seek_gen) {
            gen = a->seek_gen;
            AMediaExtractor_seekTo(a->aex, a->seek_us, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
            AMediaCodec_flush(a->adec);
            SDL_LockMutex(a->mx);
            a->rd = a->wr = a->count = 0;
            a->ring_pts = -1;
            SDL_UnlockMutex(a->mx);
            in_eos = 0;
            a->adone = 0;
        }
        if (!in_eos) {
            ssize_t ii = AMediaCodec_dequeueInputBuffer(a->adec, 2000);
            if (ii >= 0) {
                size_t cap;
                uint8_t *buf = AMediaCodec_getInputBuffer(a->adec, ii, &cap);
                ssize_t n = AMediaExtractor_readSampleData(a->aex, buf, cap);
                if (n < 0) {
                    AMediaCodec_queueInputBuffer(a->adec, ii, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    in_eos = 1;
                } else {
                    AMediaCodec_queueInputBuffer(a->adec, ii, 0, n, AMediaExtractor_getSampleTime(a->aex), 0);
                    AMediaExtractor_advance(a->aex);
                }
            }
        }
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(a->adec, &info, 4000);
        if (oi >= 0) {
            if (info.size > 0) {
                size_t sz;
                uint8_t *buf = AMediaCodec_getOutputBuffer(a->adec, oi, &sz);
                if (buf && a->ach > 0) {
                    int frames = info.size / (2 * a->ach);
                    ring_push(a, (const int16_t *)(buf + info.offset), frames, a->ach, info.presentationTimeUs);
                }
            }
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) a->adone = 1;
            AMediaCodec_releaseOutputBuffer(a->adec, oi, false);
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *f = AMediaCodec_getOutputFormat(a->adec);
            if (f) {
                int32_t v;
                if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v)) a->arate = v;
                if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v)) a->ach = v;
                AMediaFormat_delete(f);
            }
        } else if (in_eos && a->adone) {
            SDL_Delay(10);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- the Vita interface */

static AV *get(SceAvPlayerHandle h) {
    if (h <= 0 || h > 4 || !players[h - 1].used) return NULL;
    return &players[h - 1];
}

SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData *init) {
    (void)init;
    for (int i = 0; i < 4; i++)
        if (!players[i].used) {
            AV *a = &players[i];
            memset(a, 0, sizeof(*a));
            a->used = 1;
            a->fd_v = a->fd_a = -1;
            a->fb_ready = -1;
            a->speed = 100;
            a->seek_us = -1;
            a->audio_clock = -1;
            a->ring_pts = -1;
            a->vdone = a->adone = 1;
            a->mx = SDL_CreateMutex();
            return i + 1;
        }
    return 0;
}

int sceAvPlayerAddSource(SceAvPlayerHandle h, const char *filename) {
    AV *a = get(h);
    if (!a) return -1;
    port_path(filename, a->path, sizeof(a->path));
    /* video */
    a->vex = open_extractor(a, &a->fd_v);
    if (!a->vex) return -1;
    AMediaFormat *vf = NULL, *af = NULL;
    int vt = find_track(a->vex, "video/", &vf);
    if (vt >= 0) {
        AMediaExtractor_selectTrack(a->vex, vt);
        const char *mime = NULL;
        AMediaFormat_getString(vf, AMEDIAFORMAT_KEY_MIME, &mime);
        int32_t w = 0, hh = 0;
        AMediaFormat_getInt32(vf, AMEDIAFORMAT_KEY_WIDTH, &w);
        AMediaFormat_getInt32(vf, AMEDIAFORMAT_KEY_HEIGHT, &hh);
        int64_t d = 0;
        if (AMediaFormat_getInt64(vf, AMEDIAFORMAT_KEY_DURATION, &d)) a->dur_us = d;
        a->vw = w;
        a->vh = hh;
        a->frame_w = (w + 1) & ~1;
        a->frame_h = (hh + 1) & ~1;
        a->fbuf_size = (size_t)a->frame_w * a->frame_h * 3 / 2;
        for (int k = 0; k < NFRAMEBUF; k++) a->fbuf[k] = calloc(1, a->fbuf_size);
        if (AImageReader_new(w, hh, AIMAGE_FORMAT_YUV_420_888, 4, &a->reader) == AMEDIA_OK &&
            AImageReader_getWindow(a->reader, &a->win) == AMEDIA_OK) {
            a->vdec = mime ? AMediaCodec_createDecoderByType(mime) : NULL;
            if (a->vdec && AMediaCodec_configure(a->vdec, vf, a->win, NULL, 0) == AMEDIA_OK && AMediaCodec_start(a->vdec) == AMEDIA_OK) {
                a->has_video = 1;
                a->vdone = 0;
            } else LOG("no video decoder for %s", mime ? mime : "?");
        }
        AMediaFormat_delete(vf);
    }
    /* audio: its own reader so the two never wait on each other */
    a->aex = open_extractor(a, &a->fd_a);
    if (a->aex) {
        int at = find_track(a->aex, "audio/", &af);
        if (at >= 0) {
            AMediaExtractor_selectTrack(a->aex, at);
            const char *mime = NULL;
            AMediaFormat_getString(af, AMEDIAFORMAT_KEY_MIME, &mime);
            int32_t v;
            if (AMediaFormat_getInt32(af, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v)) a->arate = v;
            if (AMediaFormat_getInt32(af, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v)) a->ach = v;
            int64_t d = 0;
            if (!a->dur_us && AMediaFormat_getInt64(af, AMEDIAFORMAT_KEY_DURATION, &d)) a->dur_us = d;
            a->adec = mime ? AMediaCodec_createDecoderByType(mime) : NULL;
            if (a->adec && AMediaCodec_configure(a->adec, af, NULL, NULL, 0) == AMEDIA_OK && AMediaCodec_start(a->adec) == AMEDIA_OK) {
                a->has_audio = 1;
                a->adone = 0;
                a->ring_ch = a->ach == 1 ? 1 : 2;
                a->ring = calloc(RING_FRAMES, sizeof(int16_t) * 2);
                a->audio_clock = 0;
            } else LOG("no audio decoder for %s", mime ? mime : "?");
            AMediaFormat_delete(af);
        }
    }
    if (!a->has_video && !a->has_audio) return -1;
    a->run = 1;
    restart_wall(a, 0);
    if (a->has_video) a->vth = SDL_CreateThread(video_thread, "nest_vdec", a);
    if (a->has_audio) a->ath = SDL_CreateThread(audio_thread, "nest_adec", a);
    return 0;
}

static void stop_threads(AV *a) {
    a->run = 0;
    if (a->vth) { SDL_WaitThread(a->vth, NULL); a->vth = NULL; }
    if (a->ath) { SDL_WaitThread(a->ath, NULL); a->ath = NULL; }
}

int sceAvPlayerStop(SceAvPlayerHandle h) {
    AV *a = get(h);
    if (!a) return -1;
    stop_threads(a);
    return 0;
}

int sceAvPlayerClose(SceAvPlayerHandle h) {
    AV *a = get(h);
    if (!a) return -1;
    stop_threads(a);
    if (a->vdec) { AMediaCodec_stop(a->vdec); AMediaCodec_delete(a->vdec); }
    if (a->adec) { AMediaCodec_stop(a->adec); AMediaCodec_delete(a->adec); }
    if (a->reader) AImageReader_delete(a->reader);
    if (a->vex) AMediaExtractor_delete(a->vex);
    if (a->aex) AMediaExtractor_delete(a->aex);
    if (a->fd_v >= 0) close(a->fd_v);
    if (a->fd_a >= 0) close(a->fd_a);
    for (int k = 0; k < NFRAMEBUF; k++) free(a->fbuf[k]);
    free(a->ring);
    SDL_DestroyMutex(a->mx);
    memset(a, 0, sizeof(*a));
    return 0;
}

int sceAvPlayerIsActive(SceAvPlayerHandle h) {
    AV *a = get(h);
    if (!a || !a->run) return 0;
    int audio_left = a->has_audio && (!a->adone || a->count > 0);
    int video_left = a->has_video && !a->vdone;
    return audio_left || video_left;
}

SceBool sceAvPlayerGetVideoData(SceAvPlayerHandle h, SceAvPlayerFrameInfo *info) {
    AV *a = get(h);
    if (!a || !a->has_video) return 0;
    SDL_LockMutex(a->mx);
    int ok = a->fb_ready >= 0 && a->fb_serial != a->fb_served;
    if (ok) {
        a->fb_served = a->fb_serial;
        memset(info, 0, sizeof(*info));
        info->pData = a->fbuf[a->fb_ready];
        info->timeStamp = (uint64_t)(a->fb_pts / 1000);
        info->details.video.width = a->frame_w;
        info->details.video.height = a->frame_h;
        info->details.video.aspectRatio = 1.0f;
    }
    SDL_UnlockMutex(a->mx);
    return ok;
}

SceBool sceAvPlayerGetAudioData(SceAvPlayerHandle h, SceAvPlayerFrameInfo *info) {
    AV *a = get(h);
    if (!a || !a->has_audio || a->paused) return 0;
    if (a->speed != 100) { /* fast-forward is silent, like on the Vita */
        SDL_LockMutex(a->mx);
        int64_t c = clock_us(a);
        while (a->count > 0 && a->ring_pts >= 0 && a->ring_pts < c) {
            a->rd = (a->rd + 1) % RING_FRAMES;
            a->count--;
            a->ring_pts += 1000000 / (a->arate ? a->arate : 48000);
        }
        SDL_UnlockMutex(a->mx);
        return 0;
    }
    SDL_LockMutex(a->mx);
    if (a->count < 1024 && !(a->adone && a->count > 0)) {
        SDL_UnlockMutex(a->mx);
        return 0;
    }
    int ch = a->ring_ch, n = a->count < 1024 ? a->count : 1024;
    for (int i = 0; i < n; i++) {
        const int16_t *s = a->ring + (size_t)a->rd * ch;
        a->out[i * ch] = s[0];
        if (ch == 2) a->out[i * ch + 1] = s[1];
        a->rd = (a->rd + 1) % RING_FRAMES;
    }
    if (n < 1024) memset(a->out + n * ch, 0, (size_t)(1024 - n) * ch * 2);
    a->count -= n;
    int64_t pts = a->ring_pts;
    if (a->ring_pts >= 0) a->ring_pts += (int64_t)n * 1000000 / (a->arate ? a->arate : 48000);
    a->audio_clock = a->ring_pts >= 0 ? a->ring_pts : a->audio_clock;
    SDL_UnlockMutex(a->mx);
    memset(info, 0, sizeof(*info));
    info->pData = (uint8_t *)a->out;
    info->timeStamp = pts >= 0 ? (uint64_t)(pts / 1000) : 0;
    info->details.audio.channelCount = (uint16_t)ch;
    info->details.audio.sampleRate = a->arate ? a->arate : 48000;
    info->details.audio.size = 1024 * ch * 2;
    return 1;
}

int sceAvPlayerGetStreamInfo(SceAvPlayerHandle h, uint32_t id, SceAvPlayerStreamInfo *info) {
    AV *a = get(h);
    if (!a || id > 1) return -1;
    memset(info, 0, sizeof(*info));
    info->type = id;
    info->duration = (uint64_t)(a->dur_us / 1000);
    if (id == 0) {
        info->details.video.width = a->vw;
        info->details.video.height = a->vh;
        info->details.video.aspectRatio = 1.0f;
    } else {
        info->details.audio.sampleRate = a->arate;
        info->details.audio.channelCount = (uint16_t)a->ach;
    }
    return 0;
}

uint64_t sceAvPlayerCurrentTime(SceAvPlayerHandle h) {
    AV *a = get(h);
    if (!a) return 0;
    int64_t c = clock_us(a);
    return c > 0 ? (uint64_t)(c / 1000) : 0;
}

int sceAvPlayerJumpToTime(SceAvPlayerHandle h, uint64_t ms) {
    AV *a = get(h);
    if (!a) return -1;
    int64_t to = (int64_t)ms * 1000;
    if (a->dur_us && to > a->dur_us - 500000) to = a->dur_us - 500000;
    if (to < 0) to = 0;
    a->seek_us = to;
    a->audio_clock = a->has_audio ? to : -1;
    restart_wall(a, to);
    a->seek_gen++;
    return 0;
}

int sceAvPlayerPause(SceAvPlayerHandle h) {
    AV *a = get(h);
    if (!a) return -1;
    restart_wall(a, clock_us(a));
    a->paused = 1;
    return 0;
}

int sceAvPlayerResume(SceAvPlayerHandle h) {
    AV *a = get(h);
    if (!a) return -1;
    restart_wall(a, a->base_us);
    a->paused = 0;
    return 0;
}

int sceAvPlayerSetTrickSpeed(SceAvPlayerHandle h, int32_t speed) {
    AV *a = get(h);
    if (!a) return -1;
    restart_wall(a, clock_us(a));
    a->speed = speed > 0 ? speed : 100;
    if (a->speed == 100 && a->has_audio) {
        /* back to normal: start the sound from here */
        a->seek_us = a->base_us;
        a->audio_clock = a->base_us;
        a->seek_gen++;
    }
    return 0;
}
#endif
