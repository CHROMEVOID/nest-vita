/* Video player on the Vita's hardware decoder (MP4 / H.264 + AAC).
   Soft fading controls, double-tap seeking, external subtitles, Up Next. */
#include "nest.h"
#include "music.h"

#include <malloc.h>
#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/rtc.h>
#include <psp2/io/fcntl.h>

extern int g_completed_tid;

#define ALIGN(x, a) (((x) + ((a)-1)) & ~((a)-1))

static SceAvPlayerHandle av;
static int av_ready, av_started;
static volatile int audio_run;
static volatile int audio_vol = 32768;
static volatile int audio_gain = 256; /* software gain above 100%, 256 = 1.0x */
static int16_t gain_buf[1024 * 2];
static SceUID audio_th = -1;
static vita2d_texture *vtex[2];
static SceAvPlayerFrameInfo vframe[2];
static int vidx, have_frame;
static float file_ar;      /* picture shape written in the MP4 itself (what the Video app uses) */
static int fit_mode;       /* this show's picture size: 0 fit, 1 fill, 2 stretch */
static int mk_is, mk_ie;   /* this show's intro, ms from the start of an episode */
static int mk_cr;          /* credits: ms before the end */
static int intro_vis;      /* "Skip intro" is showing */
static float intro_a;
static void shows_prefs_load(int tid);
static void set_volume(int pct);
static void volume_save(void);

static int eidx = -1;
static int ask;              /* showing the resume prompt */
static int ask_focus;
static uint64_t jump_to;     /* pending seek after start */
static uint64_t duration;
static int paused, ended;
static float ui_a, ui_timer, seek_flash, seek_dir, play_flash, vol_flash;
static int focus_btn = 2;    /* bottom controls focus */
static int ctrl_focus_row;   /* 0 = buttons, 1 = scrubber */
static float save_timer, watched_secs, open_t;
static int speed_i;
static int subs_on = 1;
static float upnext_t = -1;
static int upnext_dismissed;
static int scrubbing;
static float scrub_pos;
static float tap_ripple_t = -1, tap_ripple_x, tap_ripple_y;
static int vol_pct = 100;
static int vol_edit, vol_drag; /* the volume slider is open / being dragged */
static int pending_restart, pending_stop;
/* diagnostics shown if a video fails, so a screenshot says why */
static int diag_init, diag_src, diag_cdram_kb, diag_main_kb, diag_frames;

/* ---------------- subtitles ---------------- */
typedef struct {
    int start, end;
    char *text;
} Cue;
static Cue *cues;
static int ncues;

static void subs_free(void) {
    for (int i = 0; i < ncues; i++) free(cues[i].text);
    free(cues);
    cues = NULL;
    ncues = 0;
}

static int parse_ts(const char *s, int *ms) {
    int h = 0, m = 0, sec = 0, f = 0;
    if (sscanf(s, "%d:%d:%d%*[,.]%d", &h, &m, &sec, &f) == 4) {
        *ms = ((h * 60 + m) * 60 + sec) * 1000 + f;
        return 1;
    }
    if (sscanf(s, "%d:%d%*[,.]%d", &m, &sec, &f) == 3) {
        *ms = (m * 60 + sec) * 1000 + f;
        return 1;
    }
    return 0;
}

static void strip_tags(char *s) {
    char *d = s;
    int in_tag = 0;
    for (char *p = s; *p; p++) {
        if (*p == '<' || *p == '{') in_tag = 1;
        else if ((*p == '>' || *p == '}') && in_tag) in_tag = 0;
        else if (!in_tag && *p != '\r') *d++ = *p;
    }
    *d = 0;
}

static void subs_load(const char *video) {
    subs_free();
    char base[512], path[600];
    snprintf(base, sizeof(base), "%s", video);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    const char *suffix[] = {".srt", ".en.srt", ".eng.srt", ".English.srt", ".vtt", ".en.vtt"};
    FILE *f = NULL;
    for (int i = 0; i < 6 && !f; i++) {
        snprintf(path, sizeof(path), "%s%s", base, suffix[i]);
        if (net_is_url(path)) { /* fetch it once, then read it like a local file */
            size_t n = 0;
            char *txt = net_read_all(path, &n);
            if (!txt) continue;
            FILE *o = fopen(DATA_DIR "/stream_subs.srt", "wb");
            if (o) { fwrite(txt, 1, n, o); fclose(o); }
            free(txt);
            f = fopen(DATA_DIR "/stream_subs.srt", "rb");
        } else {
            f = fopen(path, "rb");
        }
    }
    if (!f) return;
    int cap = 512;
    cues = calloc(cap, sizeof(Cue));
    char line[1024];
    char textbuf[1024];
    int st = 0, en = 0, have = 0;
    textbuf[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        char *arrow = strstr(line, "-->");
        if (arrow) {
            if (have && textbuf[0] && ncues < 20000) {
                if (ncues == cap) {
                    cap *= 2;
                    Cue *nc = realloc(cues, sizeof(Cue) * cap);
                    if (!nc) break;
                    cues = nc;
                }
                strip_tags(textbuf);
                cues[ncues++] = (Cue){st, en, strdup(textbuf)};
            }
            textbuf[0] = 0;
            have = parse_ts(line, &st) && parse_ts(arrow + 3 + strspn(arrow + 3, " "), &en);
            continue;
        }
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!have || n == 0) continue;
        /* a bare number line before the next cue is an index: ignore */
        int digits = 1;
        for (char *p = line; *p; p++)
            if (!isdigit((unsigned char)*p)) digits = 0;
        if (digits && !textbuf[0]) continue;
        if (textbuf[0]) strncat(textbuf, "\n", sizeof(textbuf) - strlen(textbuf) - 1);
        strncat(textbuf, line, sizeof(textbuf) - strlen(textbuf) - 1);
    }
    if (have && textbuf[0] && ncues < cap) {
        strip_tags(textbuf);
        cues[ncues++] = (Cue){st, en, strdup(textbuf)};
    }
    fclose(f);
}

static const char *sub_at(int ms) {
    int a = 0, b = ncues - 1, found = -1;
    while (a <= b) {
        int m = (a + b) / 2;
        if (cues[m].start <= ms) { found = m; a = m + 1; }
        else b = m - 1;
    }
    for (int i = found; i >= 0 && i > found - 4; i--)
        if (cues[i].start <= ms && ms < cues[i].end) return cues[i].text;
    return NULL;
}

/* ---------------- AvPlayer plumbing ---------------- */

static void *mem_alloc(void *p, uint32_t align, uint32_t size) {
    (void)p;
    return memalign(align < 16 ? 16 : align, size);
}
static void mem_free(void *p, void *ptr) { (void)p; free(ptr); }

static void *gpu_alloc(void *p, uint32_t align, uint32_t size) {
    (void)p;
    void *res = NULL;
    if (align < 0x40000) align = 0x40000;
    size = ALIGN(size, align);
    SceKernelAllocMemBlockOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = align;
    SceUID blk = sceKernelAllocMemBlock("nest_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, &opt);
    if (blk < 0) return NULL;
    sceKernelGetMemBlockBase(blk, &res);
    sceGxmMapMemory(res, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    return res;
}
static void gpu_free(void *p, void *ptr) {
    (void)p;
    SceUID blk = sceKernelFindMemBlockByAddr(ptr, 0);
    sceGxmUnmapMemory(ptr);
    sceKernelFreeMemBlock(blk);
}

static int audio_thread(SceSize a, void *arg) {
    (void)a;
    (void)arg;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) return sceKernelExitDeleteThread(0);
    int cur_vol = -1, cur_rate = 48000, cur_ch = 2;
    SceAvPlayerFrameInfo fr;
    memset(&fr, 0, sizeof(fr));
    float lim_state = 1;
    while (audio_run) {
        if (cur_vol != audio_vol) {
            int v[2] = {audio_vol, audio_vol};
            sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, v);
            cur_vol = audio_vol;
        }
        if (av_ready && sceAvPlayerIsActive(av) && sceAvPlayerGetAudioData(av, &fr)) {
            int rate = fr.details.audio.sampleRate, ch = fr.details.audio.channelCount;
            if (rate != cur_rate || ch != cur_ch) {
                sceAudioOutSetConfig(port, -1, rate, ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO);
                cur_rate = rate;
                cur_ch = ch;
            }
            void *out = fr.pData;
            int g = audio_gain;
            if (g != 256) { /* boost through the limiter: loud, never crackly */
                int nch = ch == 1 ? 1 : 2;
                memcpy(gain_buf, fr.pData, 1024 * nch * sizeof(int16_t));
                boost_apply(gain_buf, 1024, nch, g / 256.0f, &lim_state);
                out = gain_buf;
            }
            sceAudioOutOutput(port, out);
        } else {
            sceKernelDelayThread(1000);
        }
    }
    sceAudioOutReleasePort(port);
    return sceKernelExitDeleteThread(0);
}

static void av_start(void) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    SceAvPlayerInitData d;
    memset(&d, 0, sizeof(d));
    d.memoryReplacement.allocate = mem_alloc;
    d.memoryReplacement.deallocate = mem_free;
    d.memoryReplacement.allocateTexture = gpu_alloc;
    d.memoryReplacement.deallocateTexture = gpu_free;
    d.basePriority = 0xA0;
    /* 4 buffers: the decoder never writes into a picture still being shown */
    d.numOutputVideoFrameBuffers = 4;
    d.autoStart = 1;
    if (net_is_url(L.e[eidx].path)) net_avplayer_files(&d.fileReplacement); /* streamed from the PC */
    file_ar = L.e[eidx].aspect > 0 ? L.e[eidx].aspect : media_aspect(L.e[eidx].path);
    {
        SceKernelFreeMemorySizeInfo fm;
        memset(&fm, 0, sizeof(fm));
        fm.size = sizeof(fm);
        if (sceKernelGetFreeMemorySize(&fm) >= 0) {
            diag_cdram_kb = fm.size_cdram / 1024;
            diag_main_kb = fm.size_user / 1024;
        }
    }
    diag_frames = 0;
    av = sceAvPlayerInit(&d);
    diag_init = av;
    av_ready = avplayer_ok(av);
    if (!av_ready) return;
    for (int i = 0; i < 2; i++) {
        vtex[i] = calloc(1, sizeof(vita2d_texture));
        memset(&vframe[i], 0, sizeof(vframe[i]));
    }
    have_frame = 0;
    diag_src = sceAvPlayerAddSource(av, L.e[eidx].path);
    sceAvPlayerSetTrickSpeed(av, 100);
    audio_run = 1;
    audio_th = sceKernelCreateThread("nest_audio", audio_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (audio_th >= 0) sceKernelStartThread(audio_th, 0, NULL);
    av_started = 1;
}

static void av_stop(void) {
    if (!av_started) return;
    audio_run = 0;
    if (audio_th >= 0) {
        SceUInt to = 500000;
        sceKernelWaitThreadEnd(audio_th, NULL, &to);
        audio_th = -1;
    }
    if (av_ready) {
        sceAvPlayerStop(av);
        vita2d_wait_rendering_done();
        sceAvPlayerClose(av);
    }
    av_ready = 0;
    for (int i = 0; i < 2; i++) {
        free(vtex[i]);
        vtex[i] = NULL;
    }
    have_frame = 0;
    av_started = 0;
}

static uint64_t now_ms(void) { return av_ready ? sceAvPlayerCurrentTime(av) : 0; }

static void save_progress(int final) {
    if (eidx < 0) return;
    Entry *e = &L.e[eidx];
    uint64_t pos = now_ms();
    if (!duration && !pos) return;
    int fin = duration > 0 && (pos >= duration * 0.94 || (duration > 20 * 60000 && duration - pos < 120000 && final));
    int secs = (int)watched_secs;
    watched_secs -= secs;
    lib_progress(e, fin ? 0 : (int)pos, (int)duration, fin, secs);
}

void player_open(int e, int allow_ask) {
    if (e < 0 || e >= L.ne) return;
    /* give the hardware decoder as much memory as possible */
    img_flush();
    lofi_release_all();
    mus_release_audio(); /* music stops for a video and hands over the sound channel */
    int restart = 0;
    if (av_started) {
        save_progress(1);
        restart = 1; /* stop the old stream at the start of the next frame, not mid-draw */
    }
    eidx = e;
    Entry *en = &L.e[e];
    duration = en->dur;
    paused = 0;
    ended = 0;
    ui_a = 1;
    ui_timer = 0;
    focus_btn = 2;
    ctrl_focus_row = 0;
    speed_i = 0;
    upnext_t = -1;
    upnext_dismissed = 0;
    watched_secs = 0;
    save_timer = 0;
    open_t = 0;
    jump_to = 0;
    scrubbing = 0;
    subs_load(en->path);
    set_volume(db_geti("player_vol", 100));
    vol_flash = 0;
    vol_edit = vol_drag = 0;
    shows_prefs_load(L.t[en->tidx].id);
    ask = allow_ask && S.resume_ask && !en->finished && en->pos > 30000;
    ask_focus = 0;
    if (!ask) {
        if (allow_ask && !en->finished && en->pos > 30000) jump_to = en->pos - 3000; /* a few seconds back, so you don't miss a line */
        pending_restart = 1; /* start on the player's first frame, after any thumbnail work has stopped */
        (void)restart;
    } else if (restart) {
        pending_stop = 1;
    }
    if (nav_top() == SCR_PLAYER) nav_replace(SCR_PLAYER);
    else nav_push(SCR_PLAYER);
}

void player_close(void) {
    pending_restart = pending_stop = 0;
    volume_save();
    save_progress(1);
    av_stop();
    subs_free();
    Entry *e = eidx >= 0 ? &L.e[eidx] : NULL;
    if (e && e->finished) {
        Title *t = &L.t[e->tidx];
        if (t->kind == KIND_SERIES && lib_next_entry(eidx) < 0 && t->watched == t->ne) g_completed_tid = t->id;
    }
    eidx = -1;
}

static void fmt_time(uint64_t ms, char *b, int n) {
    int s = (int)(ms / 1000);
    if (s >= 3600) snprintf(b, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, n, "%d:%02d", s / 60, s % 60);
}

static void seek_by(int sec) {
    if (!av_ready) return;
    int64_t t = (int64_t)now_ms() + sec * 1000LL;
    if (t < 0) t = 0;
    if (duration && t > (int64_t)duration - 1000) t = duration - 1000;
    sceAvPlayerJumpToTime(av, (uint64_t)t);
    seek_flash = 1;
    seek_dir = sec;
    if (save_timer < 2.4f) save_timer = 2.4f; /* remember the new spot straight after */
}

static void toggle_pause(void) {
    if (!av_ready) return;
    paused = !paused;
    if (paused) {
        sceAvPlayerPause(av);
        save_progress(0); /* pausing is often the last thing before switching off */
    }
    else sceAvPlayerResume(av);
    play_flash = 1;
}

static void go_next(void) {
    int nx = lib_next_entry(eidx);
    if (nx < 0) return;
    save_progress(1);
    if (L.e[nx].playable) player_open(nx, 0);
    else {
        toast("The next episode can't play here \xe2\x80\x94 it needs converting");
        nav_pop();
    }
}

static void go_prev(void) {
    if (now_ms() > 8000) {
        sceAvPlayerJumpToTime(av, 0);
        return;
    }
    int pv = lib_prev_entry(eidx);
    if (pv >= 0 && L.e[pv].playable) player_open(pv, 0);
}

#define VOL_MAX 300
static void set_volume(int pct) {
    vol_pct = clampi(pct, 0, VOL_MAX);
    /* 100% is a touch louder than the Vita's own maximum (x1.2), and the top end
       reaches x3.6: the hardware does what it can, software does the rest, and
       the limiter keeps it clean */
    float eff = vol_pct * 1.2f / 100.0f;
    if (vol_pct > 150) eff = 1.8f * powf(vol_pct / 150.0f, 1.75f); /* past 150% it climbs fast: 300% is about x6 */
    if (eff <= 1.0f) {
        audio_vol = (int)(32768 * eff);
        audio_gain = 256;
    } else {
        audio_vol = 32768;
        audio_gain = (int)(256 * eff);
    }
    vol_flash = 1;
}
static void volume_save(void) { db_seti("player_vol", vol_pct); }

static const int SPEEDS[] = {100, 200, 400};
static const char *SPEED_LABEL[] = {"1\xc3\x97", "2\xc3\x97", "4\xc3\x97"};

static int cur_tid(void) { return eidx >= 0 ? L.t[L.e[eidx].tidx].id : -1; }

static void shows_prefs_load(int tid) {
    char k[32];
    snprintf(k, sizeof(k), "fit:%d", tid);
    fit_mode = clampi(db_geti(k, S.video_fit), 0, 2);
    mk_is = mk_ie = mk_cr = 0;
    snprintf(k, sizeof(k), "mk:%d", tid);
    const char *v = db_get(k, NULL);
    if (v) sscanf(v, "%d,%d,%d", &mk_is, &mk_ie, &mk_cr);
    intro_vis = 0;
    intro_a = 0;
}

static void markers_save(void) {
    char k[32], v[64];
    snprintf(k, sizeof(k), "mk:%d", cur_tid());
    snprintf(v, sizeof(v), "%d,%d,%d", mk_is, mk_ie, mk_cr);
    db_set(k, v);
}

static void cycle_fit(void) {
    static const char *names[3] = {"Fit: whole picture", "Fill: no black bars", "Stretch to screen"};
    fit_mode = (fit_mode + 1) % 3;
    char k[32], m[96];
    snprintf(k, sizeof(k), "fit:%d", cur_tid());
    db_seti(k, fit_mode); /* remembered for this show */
    snprintf(m, sizeof(m), "%s \xc2\xb7 remembered for this show", names[fit_mode]);
    toast(m);
}

static void marker_cb(int c, void *ud) {
    (void)ud;
    if (eidx < 0 || c < 0) return;
    int p = (int)now_ms();
    switch (c) {
    case 0:
        mk_is = p;
        if (mk_ie <= mk_is) mk_ie = 0;
        toast("Intro start marked \xe2\x80\x94 now mark where it ends");
        break;
    case 1:
        if (p <= mk_is + 2000) { toast("Mark where the intro starts first"); return; }
        mk_ie = p;
        toast("Intro saved: \"Skip intro\" shows on every episode (press R)");
        break;
    case 2:
        if (!duration || (uint64_t)p >= duration) return;
        mk_cr = (int)(duration - p);
        toast("Credits marked: Up Next appears here on every episode");
        break;
    case 3:
        mk_is = mk_ie = mk_cr = 0;
        toast("Markers cleared for this show");
        break;
    }
    markers_save();
}

static void open_markers(void) {
    static const char *items[] = {"The intro starts here", "The intro ends here", "The credits start here",
                                  "Clear this show's markers"};
    modal_menu("Mark this show", items, 4, marker_cb, NULL);
}

static void button_action(int b) {
    switch (b) {
    case 0: go_prev(); break;
    case 1: seek_by(-10); break;
    case 2: toggle_pause(); break;
    case 3: seek_by(10); break;
    case 4: go_next(); break;
    case 5:
        if (!ncues) toast("No subtitle file found next to this video");
        else {
            subs_on = !subs_on;
            toast(subs_on ? "Subtitles on" : "Subtitles off");
        }
        break;
    case 6:
        speed_i = (speed_i + 1) % 3;
        if (av_ready) sceAvPlayerSetTrickSpeed(av, SPEEDS[speed_i]);
        toast(speed_i ? "Fast forward (sound mutes above 1\xc3\x97)" : "Normal speed");
        break;
    case 7: vol_edit = !vol_edit; vol_flash = 1; if (!vol_edit) volume_save(); break;
    case 8: cycle_fit(); break;
    }
}

static void draw_resume_prompt(float dt) {
    (void)dt;
    Entry *e = &L.e[eidx];
    Title *t = &L.t[e->tidx];
    const char *src = t->backdrop ? t->backdrop : title_poster(t);
    float fade = 0;
    vita2d_texture *tex = src ? img_get(src, SCR_W, SCR_H, &fade) : NULL;
    if (tex) gfx_tex(tex, 0, 0, SCR_W, SCR_H, 0, col_a(0xFFFFFFFF, 0.45f * fade), 0.5f, 0.3f);
    else draw_backdrop_scene(0.5f);
    gfx_rect(0, 0, SCR_W, SCR_H, col_a(0xFF000000, 0.55f));
    char ts[32], lab[64], el[32];
    fmt_time(e->pos, ts, sizeof(ts));
    text_center(F_DISPLAY, T->display_font == 2 ? 24 : 30, SCR_W / 2, 210, 0xFFFFFFFF, t->title);
    if (t->kind == KIND_SERIES) text_center(F_META, 14, SCR_W / 2, 238, col_a(0xFFFFFFFF, 0.7f), lib_entry_label(e, el, sizeof(el)));
    snprintf(lab, sizeof(lab), "Resume from %s", ts);
    float bw = 220, gap = 14, x0 = SCR_W / 2 - bw - gap / 2;
    draw_button(x0, 280, bw, 48, IC_PLAY, lab, 1, ask_focus == 0);
    draw_button(x0 + bw + gap, 280, bw, 48, IC_REFRESH, "Start over", 0, ask_focus == 1);
    if (e->dur > 0) draw_progress(SCR_W / 2 - 160, 356, 320, 4, (float)e->pos / e->dur);
    int choose = -1;
    if (in.repeat & B_LEFT) ask_focus = 0;
    if (in.repeat & B_RIGHT) ask_focus = 1;
    if (in.pressed & B_OK) choose = ask_focus;
    if (hit(x0, 280, bw, 48)) choose = 0;
    if (hit(x0 + bw + gap, 280, bw, 48)) choose = 1;
    if (in.pressed & B_BACK) {
        ask = 0;
        nav_pop();
        return;
    }
    if (choose >= 0) {
        jump_to = choose == 0 ? (uint64_t)(e->pos > 3000 ? e->pos - 3000 : e->pos) : 0;
        ask = 0;
        av_start();
    }
}

void player_frame(float dt) {
    if (eidx < 0) {
        nav_pop();
        return;
    }
    if (pending_restart || pending_stop) {
        av_stop();
        if (pending_restart) av_start();
        pending_restart = pending_stop = 0;
    }
    gfx_rect(0, 0, SCR_W, SCR_H, 0xFF000000);
    if (ask) {
        draw_resume_prompt(dt);
        return;
    }
    Entry *e = &L.e[eidx];
    Title *t = &L.t[e->tidx];
    open_t += dt;

    /* ---------- video frame ---------- */
    if (av_ready && sceAvPlayerIsActive(av)) {
        if (sceAvPlayerGetVideoData(av, &vframe[vidx])) {
            vita2d_texture *vt = vtex[vidx];
            sceGxmTextureInitLinear(&vt->gxm_tex, vframe[vidx].pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                    vframe[vidx].details.video.width, vframe[vidx].details.video.height, 0);
            have_frame = vidx + 1;
            diag_frames++;
            vidx ^= 1;
        }
        if (!duration) {
            SceAvPlayerStreamInfo si;
            for (uint32_t i = 0; i < 3; i++) {
                memset(&si, 0, sizeof(si));
                if (sceAvPlayerGetStreamInfo(av, i, &si) == 0 && si.duration > 0) {
                    duration = si.duration;
                    break;
                }
            }
        }
        if (jump_to && have_frame) {
            sceAvPlayerJumpToTime(av, jump_to);
            jump_to = 0;
        }
        if (!paused) watched_secs += dt;
    } else if (av_ready && av_started && have_frame && open_t > 1.0f) {
        ended = 1;
    }
    if (have_frame) {
        vita2d_texture *vt = vtex[have_frame - 1];
        float vw = vita2d_texture_get_width(vt), vh = vita2d_texture_get_height(vt);
        /* AvPlayer reports the PIXEL aspect ratio (1.0 = square pixels), not the
           picture's: the picture shape is width/height times that. A value that
           already looks like a picture ratio is used as is. */
        float fw = vframe[have_frame - 1].details.video.width, fh = vframe[have_frame - 1].details.video.height;
        if (fw < 16 || fh < 16) { fw = vw; fh = vh; }
        float base = fw / fh;
        float par = vframe[have_frame - 1].details.video.aspectRatio;
        float ar = base;
        if (par > 0.3f && par < 3.0f) ar = fabsf(par - base) < 0.06f ? par : base * par;
        if (file_ar > 0) ar = file_ar; /* the file knows best */
        /* draw only the real picture area of the frame buffer */
        vw = fw;
        vh = fh;
        float dw, dh;
        if (fit_mode == 2) { /* stretch to the whole screen */
            dw = SCR_W;
            dh = SCR_H;
        } else if (fit_mode == 1) { /* fill: zoom until there are no bars */
            if ((float)SCR_W / SCR_H > ar) { dw = SCR_W; dh = SCR_W / ar; }
            else { dh = SCR_H; dw = SCR_H * ar; }
        } else { /* fit: the whole picture, bars if the shapes differ */
            dw = SCR_W;
            dh = SCR_W / ar;
            if (dh > SCR_H) { dh = SCR_H; dw = SCR_H * ar; }
        }
        vita2d_draw_texture_scale(vt, (SCR_W - dw) / 2, (SCR_H - dh) / 2, dw / vw, dh / vh);
    } else if (!ended) {
        draw_spinner(SCR_W / 2, SCR_H / 2, 16, 0xFFFFFFFF);
        if (!av_ready || open_t > 14) {
            text_center(F_SEMI, 16, SCR_W / 2, SCR_H / 2 + 50, 0xFFFFFFFF, "This video couldn't be played");
            char dg[160];
            snprintf(dg, sizeof(dg), "init %08X  source %08X  frames %d  video mem %d KB  main mem %d KB",
                     (unsigned)diag_init, (unsigned)diag_src, diag_frames, diag_cdram_kb, diag_main_kb);
            text_center(F_META, 11, SCR_W / 2, SCR_H - 24, col_a(0xFFFFFFFF, 0.45f), dg);
            text_center(F_REG, 13, SCR_W / 2, SCR_H / 2 + 74, col_a(0xFFFFFFFF, 0.7f),
                        MSG_PLAY_FAIL);
        } else if (open_t > 5) {
            text_center(F_REG, 14, SCR_W / 2, SCR_H / 2 + 50, col_a(0xFFFFFFFF, 0.7f),
                        "Still opening\xe2\x80\xa6 large files can take a moment");
        }
    }
    uint64_t pos = scrubbing ? (uint64_t)(scrub_pos * duration) : now_ms();

    /* ---------- end of file ---------- */
    int nx = lib_next_entry(eidx);
    if (ended) {
        save_timer = 0;
        e->finished = 1;
        lib_progress(e, 0, (int)duration, 1, (int)watched_secs);
        watched_secs = 0;
        if (nx >= 0 && S.autoplay && L.e[nx].playable && !upnext_dismissed) go_next();
        else nav_pop();
        return;
    }

    /* ---------- skip intro ---------- */
    intro_vis = mk_ie > mk_is + 3000 && pos + 500 >= (uint64_t)mk_is && pos + 1500 < (uint64_t)mk_ie && !scrubbing;
    intro_a = approach(intro_a, intro_vis ? 1 : 0, 10, dt);

    /* ---------- input ---------- */
    if (in.any) {
        if (in.pressed || in.tap) ui_timer = 0;
    }
    ui_timer += dt;
    int visible = !S.focus_mode && (ui_timer < 3.5f || paused);
    if (vol_edit) { /* the slider has the d-pad until it's closed */
        ui_timer = 0;
        if (in.repeat & (B_UP | B_RIGHT)) set_volume(vol_pct + 5);
        if (in.repeat & (B_DOWN | B_LEFT)) set_volume(vol_pct - 5);
        if (in.pressed & (B_OK | B_BACK)) { vol_edit = 0; volume_save(); }
        vol_flash = 1;
        in.repeat = 0;
        in.pressed &= ~(B_OK | B_BACK | B_SQR);
    }
    if (in.pressed & B_BACK) {
        sfx(2);
        nav_pop();
        return;
    }
    if (in.pressed & B_TRI) {
        S.focus_mode = !S.focus_mode;
        toast(S.focus_mode ? "Focus mode: controls stay hidden" : "Focus mode off");
        settings_save();
    }
    if (upnext_t >= 0 && (in.pressed & B_OK)) {
        go_next();
        return;
    }
    if (ui_a > 0.4f && !S.focus_mode) {
        if (in.repeat & B_UP) ctrl_focus_row = 1;
        if (in.repeat & B_DOWN) ctrl_focus_row = 0;
        if (ctrl_focus_row == 0) {
            if (in.repeat & B_LEFT && focus_btn > 0) focus_btn--;
            if (in.repeat & B_RIGHT && focus_btn < 8) focus_btn++;
            if (in.pressed & B_OK) {
                static const int slot_to_btn[9] = {0, 1, 2, 3, 4, 8, 5, 6, 7};
                button_action(slot_to_btn[clampi(focus_btn, 0, 8)]);
            }
        } else {
            if (in.repeat & B_LEFT) seek_by(-10);
            if (in.repeat & B_RIGHT) seek_by(10);
            if (in.pressed & B_OK) toggle_pause();
        }
    } else {
        if (in.repeat & B_LEFT) seek_by(-10);
        if (in.repeat & B_RIGHT) seek_by(10);
        if (in.repeat & B_UP) { set_volume(vol_pct + 10); volume_save(); }
        if (in.repeat & B_DOWN) { set_volume(vol_pct - 10); volume_save(); }
        if (in.pressed & B_OK) {
            ui_timer = 0;
            if (S.focus_mode) toggle_pause();
        }
    }
    if (in.pressed & B_SQR) toggle_pause();
    if (in.pressed & B_SELECT) cycle_fit();
    if (in.pressed & B_START) open_markers();
    if (in.pressed & B_L) seek_by(-30);
    if (in.pressed & B_R) {
        if (intro_vis) { sceAvPlayerJumpToTime(av, (uint64_t)mk_ie); intro_vis = 0; seek_flash = 1; seek_dir = 1; }
        else seek_by(30);
    }
    /* touch: the volume slider, while it's on screen */
    float vs_x = SCR_W - 64, vs_y = 116, vs_h = 250;
    int vol_shown = vol_edit || vol_flash > 0.3f;
    if (in.touching && (vol_drag || (vol_shown && in.sx > vs_x - 34 && in.sx < vs_x + 50 && in.sy > vs_y - 20 && in.sy < vs_y + vs_h + 20))) {
        vol_drag = 1;
        int pct = (int)roundf((1 - clampf((in.ty - vs_y) / vs_h, 0, 1)) * VOL_MAX / 5) * 5;
        if (pct != vol_pct) set_volume(pct);
        vol_flash = 1;
        ui_timer = 0;
    }
    if (vol_drag && !in.touching) { vol_drag = 0; volume_save(); in.tap = in.double_tap = 0; }
    if (vol_drag) in.tap = in.double_tap = 0;
    /* touch: double tap zones, single tap toggles controls */
    float bar_y = SCR_H - 92;
    if (in.double_tap) {
        tap_ripple_t = 0;
        tap_ripple_x = in.tap_x;
        tap_ripple_y = in.tap_y;
        if (in.tap_x < SCR_W * 0.33f) seek_by(-10);
        else if (in.tap_x > SCR_W * 0.67f) seek_by(10);
        else toggle_pause();
    } else if (in.tap && !(ui_a > 0.4f && in.tap_y > bar_y - 20) && !(ui_a > 0.4f && in.tap_y < 70)) {
        ui_timer = ui_a > 0.5f ? 10 : 0;
    }
    if (in.touching && ui_a > 0.4f && fabsf(in.sy - (bar_y + 6)) < 20 && duration) {
        scrubbing = 1;
        scrub_pos = clampf((in.tx - 40) / (SCR_W - 80), 0, 1);
        ui_timer = 0;
    }
    if (in.released && scrubbing) {
        scrubbing = 0;
        sceAvPlayerJumpToTime(av, (uint64_t)(scrub_pos * duration));
    }
    ui_a = approach(ui_a, visible ? 1 : 0, visible ? 14 : 5, dt);

    /* periodic save */
    save_timer += dt;
    if (save_timer > 3) { /* every few seconds: switching off never loses more than that */
        save_timer = 0;
        save_progress(0);
    }

    /* ---------- subtitles ---------- */
    if (subs_on && ncues) {
        const char *s = sub_at((int)pos);
        if (s) {
            int sz = S.sub_size == 0 ? 17 : S.sub_size == 1 ? 20 : 24;
            int lines = 1;
            for (const char *p = s; *p; p++)
                if (*p == '\n') lines++;
            float y = SCR_H - 34 - (lines - 1) * (sz + 6) - ui_a * 74;
            const char *p = s;
            for (int l = 0; l < lines; l++) {
                char ln[256];
                const char *nl = strchr(p, '\n');
                size_t k = nl ? (size_t)(nl - p) : strlen(p);
                if (k > 255) k = 255;
                memcpy(ln, p, k);
                ln[k] = 0;
                float w = text_w(F_SEMI, sz, ln);
                gfx_rrect(SCR_W / 2 - w / 2 - 10, y + l * (sz + 6) - sz - 2, w + 20, sz + 10, 6, col_a(0xFF000000, 0.55f));
                text_center(F_SEMI, sz, SCR_W / 2, y + l * (sz + 6), 0xFFFFFFFF, ln);
                p = nl ? nl + 1 : p + k;
            }
        }
    }

    /* ---------- feedback flashes ---------- */
    seek_flash = approach(seek_flash, 0, 4, dt);
    if (seek_flash > 0.02f) {
        float cx = seek_dir < 0 ? SCR_W * 0.2f : SCR_W * 0.8f;
        gfx_circle(cx, SCR_H / 2, 44, col_a(0xFF000000, 0.35f * seek_flash));
        icon(seek_dir < 0 ? IC_REW : IC_FWD, cx, SCR_H / 2 - 6, 30, col_a(0xFFFFFFFF, seek_flash));
        char sl[16];
        snprintf(sl, sizeof(sl), "%d s", (int)fabsf(seek_dir));
        text_center(F_SEMI, 12, cx, SCR_H / 2 + 24, col_a(0xFFFFFFFF, seek_flash), sl);
    }
    play_flash = approach(play_flash, 0, 4, dt);
    if (play_flash > 0.02f && ui_a < 0.5f) {
        float s = 1 + (1 - play_flash) * 0.3f;
        gfx_circle(SCR_W / 2, SCR_H / 2, 40 * s, col_a(0xFF000000, 0.35f * play_flash));
        icon(paused ? IC_PAUSE : IC_PLAY, SCR_W / 2 + (paused ? 0 : 2), SCR_H / 2, 30 * s, col_a(0xFFFFFFFF, play_flash));
    }
    if (tap_ripple_t >= 0) {
        tap_ripple_t += dt;
        float k = tap_ripple_t / 0.5f;
        if (k >= 1) tap_ripple_t = -1;
        else gfx_ring(tap_ripple_x, tap_ripple_y, 20 + 60 * ease_out(k), 2, col_a(0xFFFFFFFF, 0.5f * (1 - k)));
    }
    if (!vol_edit && !vol_drag) vol_flash = approach(vol_flash, 0, 1.6f, dt);
    if (vol_flash > 0.02f) { /* volume slider: 0-100% normal, 100-300% boost */
        float a = clampf(vol_flash * 1.6f, 0, 1);
        float x = vs_x, y = vs_y, h = vs_h, w = 16;
        float k = (float)vol_pct / VOL_MAX, mid = y + h * (1 - 100.0f / VOL_MAX);
        gfx_rrect(x - 22, y - 46, w + 44, h + 92, 22, col_a(0xFF0E0E12, 0.55f * a));
        gfx_glass_tint(x - 22, y - 46, w + 44, h + 92, 22, a, 0xFF202028);
        gfx_rrect(x, y, w, h, w / 2, col_a(0xFFFFFFFF, 0.18f * a));
        float fy = y + h * (1 - k);
        if (vol_pct > 100) { /* boost part glows in the accent colour */
            gfx_rrect(x, fy, w, mid - fy + w / 2, w / 2, col_a(T->accent, a));
            gfx_rrect(x, mid, w, y + h - mid, w / 2, col_a(0xFFFFFFFF, 0.92f * a));
        } else if (vol_pct > 0) {
            gfx_rrect(x, fy, w, y + h - fy, w / 2, col_a(0xFFFFFFFF, 0.92f * a));
        }
        gfx_rect(x - 5, mid - 1, w + 10, 2, col_a(0xFFFFFFFF, 0.55f * a)); /* the 100% line */
        gfx_circle(x + w / 2, fy, vol_edit || vol_drag ? 12 : 10, col_a(0xFFFFFFFF, a));
        if (vol_edit || vol_drag) gfx_ring(x + w / 2, fy, 15, 2, col_a(T->accent, a));
        char vl[16];
        snprintf(vl, sizeof(vl), "%d%%", vol_pct);
        text_center(F_SEMI, 14, x + w / 2, y - 18, col_a(vol_pct > 100 ? T->accent : 0xFFFFFFFF, a), vl);
        icon(vol_pct == 0 ? IC_VOLUME : IC_VOLUME, x + w / 2, y + h + 24, 18, col_a(0xFFFFFFFF, 0.9f * a));
        if (vol_pct == 0) gfx_line(x + w / 2 - 10, y + h + 34, x + w / 2 + 10, y + h + 14, 2, col_a(0xFFFFFFFF, a));
        if (vol_pct > 100) text_center(F_META, 9, x + w / 2, y + h + 46, col_a(T->accent, 0.9f * a), "BOOST");
        if (vol_edit) text_right(F_META, 11, x - 34, y + h / 2 + 4, col_a(0xFFFFFFFF, 0.6f * a), "Up / Down to change");
    }

    /* ---------- Up Next ---------- */
    uint64_t credits_ms = mk_cr > 25000 && (uint64_t)mk_cr < duration ? (uint64_t)mk_cr : 25000;
    if (nx >= 0 && duration > 60000 && pos + credits_ms >= duration && !upnext_dismissed) {
        if (upnext_t < 0) upnext_t = 0;
        upnext_t += dt;
        Entry *ne = &L.e[nx];
        float w = 300, h = 96, x = SCR_W - w - 28, y = SCR_H - h - (ui_a > 0.3f ? 120 : 28);
        float a = clampf(upnext_t / 0.4f, 0, 1);
        gfx_shadow(x, y + 6, w, h, T->radius, 20, col_a(0xFF000000, 0.5f * a));
        gfx_rrect(x, y, w, h, T->radius, col_a(0xFF141418, 0.92f * a));
        text(F_META, 11, x + 16, y + 24, col_a(T->accent, a), "UP NEXT");
        char el[48];
        lib_entry_label(ne, el, sizeof(el));
        char line[200];
        snprintf(line, sizeof(line), "%s%s%s", el, ne->name[0] ? "  \xc2\xb7  " : "", ne->name);
        text_ellipsis(F_SEMI, 15, x + 16, y + 46, w - 32, col_a(0xFFFFFFFF, a), line);
        float cd = 10 - upnext_t;
        char bl[32];
        if (S.autoplay && ne->playable) snprintf(bl, sizeof(bl), "Play in %d", (int)ceilf(fmaxf(cd, 0)));
        else snprintf(bl, sizeof(bl), "Play now");
        draw_button(x + 16, y + 56, 130, 30, IC_PLAY, bl, 1, 0);
        draw_button(x + 156, y + 56, 90, 30, -1, "Hide", 0, 0);
        if (S.autoplay && ne->playable && cd <= 0) {
            go_next();
            return;
        }
        if (hit(x + 16, y + 56, 130, 30)) { go_next(); return; }
        if (hit(x + 156, y + 56, 90, 30)) upnext_dismissed = 1;
    } else {
        upnext_t = -1;
    }

    if (intro_a > 0.02f) {
        float w = 176, h = 42, x = SCR_W - w - 28, y = SCR_H - h - (ui_a > 0.3f ? 128 : 30);
        gfx_shadow(x, y + 4, w, h, h / 2, 14, col_a(0xFF000000, 0.45f * intro_a));
        gfx_rrect(x, y, w, h, h / 2, col_a(0xFFFFFFFF, 0.94f * intro_a));
        text(F_SEMI, 15, x + 20, y + 27, col_a(0xFF101010, intro_a), "Skip intro");
        gfx_rrect(x + w - 50, y + 10, 32, 22, 6, col_a(0xFF101010, 0.85f * intro_a));
        text_center(F_SEMI, 12, x + w - 34, y + 26, col_a(0xFFFFFFFF, intro_a), "R");
        if (intro_vis && hit(x, y, w, h)) { sceAvPlayerJumpToTime(av, (uint64_t)mk_ie); intro_vis = 0; }
    }

    /* ---------- controls ---------- */
    if (ui_a > 0.01f) {
        float a = ui_a;
        gfx_grad_v(0, 0, SCR_W, 110, col_a(0xFF000000, 0.75f * a), 0);
        gfx_grad_v(0, SCR_H - 160, SCR_W, 160, 0, col_a(0xFF000000, 0.85f * a));
        /* top */
        icon(IC_CHEVRON_L, 44, 40, 22, col_a(0xFFFFFFFF, a));
        if (hit(20, 14, 60, 52)) { nav_pop(); return; }
        text_ellipsis(F_SEMI, 17, 70, 38, 600, col_a(0xFFFFFFFF, a), t->title);
        icon(IC_DOTS, SCR_W - 150, 40, 18, col_a(0xFFFFFFFF, 0.85f * a));
        if (hit(SCR_W - 175, 16, 50, 48)) open_markers();
        if (t->kind != KIND_MOVIE) {
            char el[48], line[200];
            lib_entry_label(e, el, sizeof(el));
            snprintf(line, sizeof(line), "%s%s%s", el, e->name[0] ? "  \xc2\xb7  " : "", e->name);
            text_ellipsis(F_META, 13, 70, 58, 600, col_a(0xFFFFFFFF, 0.7f * a), line);
        }
        if (S.clock) {
            SceDateTime dtm;
            sceRtcGetCurrentClockLocalTime(&dtm);
            char cl[16];
            snprintf(cl, sizeof(cl), "%d:%02d", dtm.hour, dtm.minute);
            text_right(F_SEMI, 15, SCR_W - 36, 38, col_a(0xFFFFFFFF, 0.85f * a), cl);
            if (duration > pos) {
                SceDateTime endt = dtm;
                int add = (int)((duration - pos) / 60000);
                int mins = dtm.minute + add;
                int hrs = (dtm.hour + mins / 60) % 24;
                snprintf(cl, sizeof(cl), "Ends %d:%02d", hrs, mins % 60);
                (void)endt;
                text_right(F_META, 12, SCR_W - 36, 58, col_a(0xFFFFFFFF, 0.6f * a), cl);
            }
        }
        /* scrubber */
        float sx = 40, sw = SCR_W - 80, sy2 = bar_y;
        float p = duration ? clampf((float)pos / duration, 0, 1) : 0;
        gfx_rrect(sx, sy2, sw, 5, 2.5f, col_a(0xFFFFFFFF, 0.25f * a));
        gfx_rrect(sx, sy2, fmaxf(5, sw * p), 5, 2.5f, col_a(T->accent, a));
        float kr = ctrl_focus_row == 1 || scrubbing ? 9 : 6;
        gfx_circle(sx + sw * p, sy2 + 2.5f, kr, col_a(0xFFFFFFFF, a));
        char tl[32], tr[32];
        fmt_time(pos, tl, sizeof(tl));
        fmt_time(duration > pos ? duration - pos : 0, tr + 1, sizeof(tr) - 1);
        tr[0] = '-';
        text(F_META, 13, sx, sy2 + 26, col_a(0xFFFFFFFF, 0.85f * a), tl);
        text_right(F_META, 13, sx + sw, sy2 + 26, col_a(0xFFFFFFFF, 0.85f * a), duration ? tr : "");
        if (scrubbing) {
            float bx = sx + sw * p;
            gfx_rrect(bx - 34, sy2 - 40, 68, 26, 8, col_a(0xFF000000, 0.7f));
            text_center(F_SEMI, 13, bx, sy2 - 22, 0xFFFFFFFF, tl);
        }
        /* buttons: on a glass bar */
        gfx_rrect(SCR_W / 2 - 200, SCR_H - 66, 400, 56, 28, col_a(0xFF000000, 0.35f * a));
        gfx_glass_tint(SCR_W / 2 - 200, SCR_H - 66, 400, 56, 28, 0.8f * a, 0xFF202028);
        int ics[9] = {IC_PREV, IC_REW, paused ? IC_PLAY : IC_PAUSE, IC_FWD, IC_NEXT, IC_SUBS, IC_SPEED, IC_VOLUME, IC_GRID};
        float cy = SCR_H - 38;
        float cxs[9] = {SCR_W / 2 - 150, SCR_W / 2 - 80, SCR_W / 2, SCR_W / 2 + 80, SCR_W / 2 + 150, SCR_W - 200, SCR_W - 150, SCR_W - 100, SCR_W - 50};
        int order[9] = {0, 1, 2, 3, 4, 8, 5, 6, 7}; /* screen-fit sits before subtitles */
        for (int j = 0; j < 9; j++) {
            int i = order[j];
            float bxc = cxs[j];
            int disabled = (i == 4 && nx < 0) || (i == 5 && !ncues);
            float f = (ctrl_focus_row == 0 && focus_btn == j && !in.touching) ? 1 : 0;
            float r = i == 2 ? 26 : 19;
            if (i == 2) gfx_circle(bxc, cy, r, col_a(0xFFFFFFFF, 0.95f * a));
            else if (f) gfx_circle(bxc, cy, r, col_a(0xFFFFFFFF, 0.2f * a));
            if (f && i == 2) gfx_ring(bxc, cy, r + 4, 2, col_a(T->accent, a));
            else if (f) gfx_ring(bxc, cy, r, 1.5f, col_a(0xFFFFFFFF, 0.6f * a));
            col_t c = i == 2 ? col_a(0xFF101010, a) : col_a(0xFFFFFFFF, (disabled ? 0.35f : 0.95f) * a);
            icon(ics[i], bxc + (i == 2 && paused ? 1.5f : 0), cy, i == 2 ? 24 : 20, c);
            if (i == 5 && ncues && !subs_on) gfx_line(bxc - 12, cy + 12, bxc + 12, cy - 12, 2, c);
            if (i == 6 && speed_i) text_center(F_SEMI, 10, bxc, cy + 24, col_a(0xFFFFFFFF, a), SPEED_LABEL[speed_i]);
            if (i == 8) {
                static const char *fl[3] = {"FIT", "FILL", "FULL"};
                text_center(F_META, 10, bxc, cy + 24, col_a(0xFFFFFFFF, 0.7f * a), fl[clampi(fit_mode, 0, 2)]);
            }
            if (i == 7) {
                char vl[8];
                snprintf(vl, sizeof(vl), "%d", vol_pct);
                text_center(F_META, 10, bxc, cy + 24, col_a(0xFFFFFFFF, 0.7f * a), vl);
            }
            if (hit(bxc - r - 6, cy - r - 6, r * 2 + 12, r * 2 + 12)) {
                focus_btn = j;
                button_action(i);
            }
        }
        if (paused) {
            gfx_circle(SCR_W / 2, SCR_H / 2 - 10, 40, col_a(0xFF000000, 0.35f * a));
            icon(IC_PAUSE, SCR_W / 2, SCR_H / 2 - 10, 30, col_a(0xFFFFFFFF, 0.9f * a));
        }
    }
}
