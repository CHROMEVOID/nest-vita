/* Video thumbnails: for titles with no artwork, quietly decode one frame a
   little way into the video and save it as a JPEG backdrop. Runs between
   frames, one video at a time, only while browsing. */
#include "nest.h"

#include <malloc.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <jpeglib.h>

#define THUMB_DIR DATA_DIR "/thumbs"
#define TW 480
#define TH 270
#define ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))

static SceAvPlayerHandle av;
static int active;           /* a video is open */
static int tidx = -1;        /* title being thumbnailed */
static float t_open;
static int jumped;
static uint64_t target_ms;
static unsigned char *tried; /* per title index: already attempted */
static int tried_n;
static vita2d_texture vtex;
static SceAvPlayerFrameInfo frame;
static int idle_frames;

static void *t_alloc(void *p, uint32_t a, uint32_t s) { (void)p; return memalign(a < 16 ? 16 : a, s); }
static void t_free(void *p, void *ptr) { (void)p; free(ptr); }
static void *t_galloc(void *p, uint32_t align, uint32_t size) {
    (void)p;
    void *res = NULL;
    if (align < 0x40000) align = 0x40000;
    size = ALIGN(size, align);
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = align;
    SceUID b = sceKernelAllocMemBlock("nest_thumb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    if (b < 0) return NULL;
    sceKernelGetMemBlockBase(b, &res);
    sceGxmMapMemory(res, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    return res;
}
static void t_gfree(void *p, void *ptr) {
    (void)p;
    SceUID b = sceKernelFindMemBlockByAddr(ptr, 0);
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(b);
}

static void close_video(void) {
    if (!active) return;
    sceAvPlayerStop(av);
    vita2d_wait_rendering_done();
    sceAvPlayerClose(av);
    active = 0;
}

static int write_jpeg(const char *path, const uint8_t *rgba, int w, int h, int stride) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, f);
    c.image_width = w;
    c.image_height = h;
    c.input_components = 4;
    c.in_color_space = JCS_EXT_RGBA;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 86, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = (JSAMPROW)(rgba + (size_t)c.next_scanline * stride);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    fclose(f);
    return 1;
}

/* is this frame worth keeping? skip near-black or flat frames */
static int interesting(const uint8_t *p, int w, int h, int stride) {
    long sum = 0, sum2 = 0, n = 0;
    for (int y = 0; y < h; y += 9)
        for (int x = 0; x < w; x += 9) {
            const uint8_t *q = p + (size_t)y * stride + x * 4;
            int l = (q[0] * 3 + q[1] * 6 + q[2]) / 10;
            sum += l;
            sum2 += l * l;
            n++;
        }
    if (!n) return 0;
    long mean = sum / n, var = sum2 / n - mean * mean;
    return mean > 24 && var > 180;
}

static void capture(void) {
    Title *t = &L.t[tidx];
    vita2d_texture *rt = vita2d_create_empty_texture_rendertarget(TW, TH, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!rt) return;
    float vw = frame.details.video.width, vh = frame.details.video.height;
    vita2d_start_drawing_advanced(rt, 0);
    vita2d_clear_screen();
    float s = fmaxf(TW / vw, TH / vh);
    vita2d_draw_texture_scale(&vtex, (TW - vw * s) / 2, (TH - vh * s) / 2, s, s);
    vita2d_end_drawing();
    vita2d_wait_rendering_done();
    const uint8_t *px = vita2d_texture_get_datap(rt);
    int stride = vita2d_texture_get_stride(rt);
    int good = interesting(px, TW, TH, stride);
    if (good) {
        sceIoMkdir(THUMB_DIR, 0777);
        char path[128];
        snprintf(path, sizeof(path), THUMB_DIR "/%d.jpg", t->id);
        if (write_jpeg(path, px, TW, TH, stride)) {
            db_title_set_str(t->id, "backdrop", path);
            free(t->backdrop);
            t->backdrop = strdup(path);
        }
    }
    vita2d_free_texture(rt);
}

static int pick_next(void) {
    if (tried_n < L.nt) {
        unsigned char *n = realloc(tried, L.nt);
        if (!n) return -1;
        memset(n + tried_n, 0, L.nt - tried_n);
        tried = n;
        tried_n = L.nt;
    }
    for (int i = 0; i < L.nt; i++) {
        Title *t = &L.t[i];
        if (tried[i] || t->kind == KIND_BOOK || t->poster || t->backdrop || t->ne <= 0) continue;
        if (t->secret && !g_secret_unlocked) continue;
        tried[i] = 1;
        /* prefer the episode you're on, else the first playable file */
        int e = t->resume >= 0 ? t->resume : t->e0;
#define THUMB_OK(k) (L.e[k].playable && L.e[k].size > 2 * 1024 * 1024 && !net_is_url(L.e[k].path))
        if (!THUMB_OK(e))
            for (int k = t->e0; k < t->e0 + t->ne; k++)
                if (THUMB_OK(k)) { e = k; break; }
        if (!THUMB_OK(e)) continue; /* empty or broken files are never opened */
        tidx = i;
        return e;
    }
    return -1;
}

void thumb_reset(void) {
    free(tried);
    tried = NULL;
    tried_n = 0;
}

/* call once per frame, before drawing starts */
/* Emulators such as Vita3K run without the system video module and crash
   when asked to decode in the background; real hardware always has it. */
static int decoder_present(void) {
    static int known = -1;
    if (known < 0) {
        SceIoStat st;
        known = sceIoGetstat("vs0:sys/external/libsceavplayer.suprx", &st) >= 0;
    }
    return known;
}

void thumb_tick(float dt, int allowed) {
    if (!S.thumbs || !allowed || !decoder_present()) {
        close_video();
        idle_frames = 0;
        return;
    }
    if (!active) {
        /* wait for a calm moment: no input for a little while */
        idle_frames = in.any ? 0 : idle_frames + 1;
        if (idle_frames < 45) return;
        int e = pick_next();
        if (e < 0) return;
        sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        SceAvPlayerInitData d;
        memset(&d, 0, sizeof(d));
        d.memoryReplacement.allocate = t_alloc;
        d.memoryReplacement.deallocate = t_free;
        d.memoryReplacement.allocateTexture = t_galloc;
        d.memoryReplacement.deallocateTexture = t_gfree;
        d.basePriority = 0xA0;
        d.numOutputVideoFrameBuffers = 2;
        d.autoStart = 1;
        av = sceAvPlayerInit(&d);
        if (!avplayer_ok(av)) return;
        sceAvPlayerAddSource(av, L.e[e].path);
        active = 1;
        t_open = 0;
        jumped = 0;
        target_ms = 0;
        return;
    }
    t_open += dt;
    if (t_open > 10) { /* give up on this one */
        close_video();
        return;
    }
    if (!sceAvPlayerIsActive(av)) return;
    if (!jumped) {
        SceAvPlayerStreamInfo si;
        uint64_t dur = 0;
        for (uint32_t i = 0; i < 3 && !dur; i++) {
            memset(&si, 0, sizeof(si));
            if (sceAvPlayerGetStreamInfo(av, i, &si) == 0) dur = si.duration;
        }
        if (!dur && t_open < 2) return;
        /* a little way in: past black frames and studio logos */
        target_ms = dur ? dur * 12 / 100 : 60000;
        if (target_ms > 240000) target_ms = 240000;
        if (target_ms < 3000) target_ms = dur / 2;
        sceAvPlayerJumpToTime(av, target_ms);
        jumped = 1;
        return;
    }
    /* drain audio so the decoder never stalls */
    SceAvPlayerFrameInfo af;
    while (sceAvPlayerGetAudioData(av, &af)) {}
    if (sceAvPlayerGetVideoData(av, &frame)) {
        if (frame.timeStamp + 1500 < target_ms && t_open < 7) return; /* still before the jump point */
        memset(&vtex, 0, sizeof(vtex));
        sceGxmTextureInitLinear(&vtex.gxm_tex, frame.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                frame.details.video.width, frame.details.video.height, 0);
        capture();
        close_video();
    }
}
