/* Music decoders: MP3, FLAC, WAV, OGG Vorbis in software, and M4A/AAC through
   the Vita's own AvPlayer. Everything comes out as 16-bit stereo. */
#include "nest.h"
#include "music.h"

#include <malloc.h>
#include <psp2/avplayer.h>
#include <psp2/sysmodule.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD
#include "audio/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#include "audio/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "audio/dr_wav.h"
#undef STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "audio/stb_vorbis.c"
#undef L
#undef R
#undef C

enum { D_MP3, D_FLAC, D_WAV, D_OGG, D_AV };

struct Dec {
    int kind;
    int rate, ch;
    uint64_t frames;   /* total, 0 = unknown */
    uint64_t pos;      /* frames handed out so far */
    drmp3 mp3;
    drflac *flac;
    drwav wav;
    stb_vorbis *ogg;
    /* AvPlayer */
    SceAvPlayerHandle av;
    int16_t *avbuf;
    int avn, avp, av_done;
    float *fbuf;       /* scratch for float sources */
};

static int ext_is(const char *p, const char *e) {
    const char *d = strrchr(p, '.');
    return d && !strcasecmp(d + 1, e);
}

static void *av_alloc(void *p, uint32_t align, uint32_t size) { (void)p; return memalign(align < 16 ? 16 : align, size); }
static void av_free(void *p, void *ptr) { (void)p; free(ptr); }

static int av_open(Dec *d, const char *path) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    SceAvPlayerInitData in_;
    memset(&in_, 0, sizeof(in_));
    in_.memoryReplacement.allocate = av_alloc;
    in_.memoryReplacement.deallocate = av_free;
    in_.memoryReplacement.allocateTexture = av_alloc;
    in_.memoryReplacement.deallocateTexture = av_free;
    in_.basePriority = 0xA0;
    in_.numOutputVideoFrameBuffers = 2;
    in_.autoStart = 1;
    d->av = sceAvPlayerInit(&in_);
    if (!avplayer_ok(d->av)) return 0;
    if (sceAvPlayerAddSource(d->av, path) < 0) {
        sceAvPlayerClose(d->av);
        return 0;
    }
    /* wait for the first audio frame to learn the format */
    SceAvPlayerFrameInfo fr;
    for (int i = 0; i < 400; i++) {
        memset(&fr, 0, sizeof(fr));
        if (sceAvPlayerGetAudioData(d->av, &fr)) {
            d->rate = fr.details.audio.sampleRate;
            d->ch = fr.details.audio.channelCount;
            if (d->ch < 1 || d->ch > 2) d->ch = 2;
            d->avbuf = malloc(1024 * 2 * sizeof(int16_t) * 2);
            memcpy(d->avbuf, fr.pData, 1024 * d->ch * sizeof(int16_t));
            d->avn = 1024;
            d->avp = 0;
            SceAvPlayerStreamInfo si;
            for (uint32_t s = 0; s < 3; s++) {
                memset(&si, 0, sizeof(si));
                if (sceAvPlayerGetStreamInfo(d->av, s, &si) == 0 && si.duration > 0) {
                    d->frames = si.duration * (uint64_t)d->rate / 1000;
                    break;
                }
            }
            return 1;
        }
        if (!sceAvPlayerIsActive(d->av) && i > 50) break;
        sceKernelDelayThread(5000);
    }
    sceAvPlayerClose(d->av);
    return 0;
}

Dec *dec_open(const char *path) {
    Dec *d = calloc(1, sizeof(Dec));
    if (!d) return NULL;
    int ok = 0;
    if (ext_is(path, "mp3")) {
        d->kind = D_MP3;
        if (drmp3_init_file(&d->mp3, path, NULL)) {
            ok = 1;
            d->rate = d->mp3.sampleRate;
            d->ch = d->mp3.channels;
            d->frames = 0; /* counting frames reads the whole file; the tag scan already knows the length */
        }
    } else if (ext_is(path, "flac")) {
        d->kind = D_FLAC;
        d->flac = drflac_open_file(path, NULL);
        if (d->flac) {
            ok = 1;
            d->rate = d->flac->sampleRate;
            d->ch = d->flac->channels;
            d->frames = d->flac->totalPCMFrameCount;
        }
    } else if (ext_is(path, "wav")) {
        d->kind = D_WAV;
        if (drwav_init_file(&d->wav, path, NULL)) {
            ok = 1;
            d->rate = d->wav.sampleRate;
            d->ch = d->wav.channels;
            d->frames = d->wav.totalPCMFrameCount;
        }
    } else if (ext_is(path, "ogg") || ext_is(path, "oga")) {
        d->kind = D_OGG;
        int err = 0;
        d->ogg = stb_vorbis_open_filename(path, &err, NULL);
        if (d->ogg) {
            ok = 1;
            stb_vorbis_info vi = stb_vorbis_get_info(d->ogg);
            d->rate = vi.sample_rate;
            d->ch = vi.channels;
            d->frames = stb_vorbis_stream_length_in_samples(d->ogg);
        }
    } else { /* m4a, aac, mp4 audio */
        d->kind = D_AV;
        ok = av_open(d, path);
    }
    if (!ok || d->rate <= 0 || d->ch <= 0) {
        free(d);
        return NULL;
    }
    d->fbuf = malloc(sizeof(float) * 4096 * (d->ch > 2 ? d->ch : 2));
    return d;
}

int dec_rate(Dec *d) { return d->rate; }
uint64_t dec_frames(Dec *d) { return d->frames; }
uint64_t dec_pos(Dec *d) { return d->pos; }

/* downmix any channel layout to interleaved stereo */
static void to_stereo_s16(const int16_t *src, int ch, int n, int16_t *out) {
    if (ch == 2) { memcpy(out, src, n * 4); return; }
    for (int i = 0; i < n; i++) {
        if (ch == 1) out[i * 2] = out[i * 2 + 1] = src[i];
        else { out[i * 2] = src[i * ch]; out[i * 2 + 1] = src[i * ch + 1]; }
    }
}

int dec_read(Dec *d, int16_t *out, int frames) {
    if (frames > 4096) frames = 4096;
    int got = 0;
    static int16_t tmp[4096 * 8];
    int ch = d->ch > 8 ? 8 : d->ch;
    switch (d->kind) {
    case D_MP3:
        got = (int)drmp3_read_pcm_frames_s16(&d->mp3, frames, tmp);
        break;
    case D_FLAC:
        got = (int)drflac_read_pcm_frames_s16(d->flac, frames, tmp);
        break;
    case D_WAV:
        got = (int)drwav_read_pcm_frames_s16(&d->wav, frames, tmp);
        break;
    case D_OGG:
        got = stb_vorbis_get_samples_short_interleaved(d->ogg, 2, out, frames * 2);
        d->pos += got;
        return got; /* already stereo */
    case D_AV:
        while (got < frames && !d->av_done) {
            if (d->avp >= d->avn) {
                SceAvPlayerFrameInfo fr;
                memset(&fr, 0, sizeof(fr));
                int tries = 0;
                while (!sceAvPlayerGetAudioData(d->av, &fr)) {
                    if (!sceAvPlayerIsActive(d->av) || ++tries > 300) { d->av_done = 1; break; }
                    sceKernelDelayThread(2000);
                }
                if (d->av_done) break;
                int c = fr.details.audio.channelCount;
                if (c < 1 || c > 2) c = 2;
                to_stereo_s16((const int16_t *)fr.pData, c, 1024, d->avbuf);
                d->avn = 1024;
                d->avp = 0;
                d->ch = 2;
            }
            int take = d->avn - d->avp;
            if (take > frames - got) take = frames - got;
            memcpy(out + got * 2, d->avbuf + d->avp * 2, take * 4);
            d->avp += take;
            got += take;
        }
        d->pos += got;
        return got;
    }
    if (got > 0) to_stereo_s16(tmp, ch, got, out);
    d->pos += got;
    return got;
}

int dec_seek(Dec *d, uint64_t frame) {
    int ok = 0;
    switch (d->kind) {
    case D_MP3: ok = drmp3_seek_to_pcm_frame(&d->mp3, frame); break;
    case D_FLAC: ok = drflac_seek_to_pcm_frame(d->flac, frame); break;
    case D_WAV: ok = drwav_seek_to_pcm_frame(&d->wav, frame); break;
    case D_OGG: ok = stb_vorbis_seek(d->ogg, (unsigned)frame); break;
    case D_AV:
        ok = sceAvPlayerJumpToTime(d->av, frame * 1000 / d->rate) >= 0;
        d->avp = d->avn; /* drop what was buffered */
        break;
    }
    if (ok) d->pos = frame;
    return ok;
}

void dec_close(Dec *d) {
    if (!d) return;
    switch (d->kind) {
    case D_MP3: drmp3_uninit(&d->mp3); break;
    case D_FLAC: drflac_close(d->flac); break;
    case D_WAV: drwav_uninit(&d->wav); break;
    case D_OGG: stb_vorbis_close(d->ogg); break;
    case D_AV:
        sceAvPlayerStop(d->av);
        sceAvPlayerClose(d->av);
        free(d->avbuf);
        break;
    }
    free(d->fbuf);
    free(d);
}
