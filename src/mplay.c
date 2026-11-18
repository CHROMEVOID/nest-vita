/* The music engine. One thread decodes and plays, and moves through the queue
   on its own, so songs keep going behind the menus, while a video is paused,
   with the screen off, or with Nest in the background. */
#include "nest.h"
#include "music.h"

#include <psp2/audioout.h>
#include <psp2/appmgr.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>

volatile int g_mus_started_track = -1;

static SceUID mtx = -1;
static SceUID th = -1;
static int *qord, *qbase;    /* play order, and the order before shuffling */
static int qn, qbn, qcap, qi = -1;
static volatile int playing;
static volatile int want_load;     /* open qord[qi] */
static volatile int want_seek = -1;
static volatile int shuffle_on, repeat_mode;
static volatile int vol_hw = 32768, vol_gain = 256, vol_pct = 100;
static volatile float level;
static Dec *dec;
static volatile int cur_track = -1;
static volatile int cur_rate = 44100;
static volatile uint64_t cur_frames, cur_pos;
static char from_label[128];
static int from_kind, from_idx;
static volatile int sleep_end_song;
static volatile int64_t sleep_at; /* process time in us, 0 = off */
static int screen_off;

static void lock(void) { if (mtx >= 0) sceKernelLockMutex(mtx, 1, NULL); }
static void unlock(void) { if (mtx >= 0) sceKernelUnlockMutex(mtx, 1); }

static uint32_t rnd_state = 0x2545F491u;
static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void q_reserve(int n) {
    if (n <= qcap) return;
    qcap = n + 64;
    qord = realloc(qord, sizeof(int) * qcap);
    qbase = realloc(qbase, sizeof(int) * qcap);
}

/* shuffle everything after the current song */
static void shuffle_rest(void) {
    for (int i = qn - 1; i > qi + 1; i--) {
        int j = qi + 1 + (int)(rnd() % (uint32_t)(i - qi));
        int t = qord[i];
        qord[i] = qord[j];
        qord[j] = t;
    }
}

/* ---------------- the audio thread ---------------- */

static int16_t outbuf[1024 * 2];
static int16_t decbuf[4096 * 2];

/* move on after a song ends; returns 0 when there's nothing left */
static int advance(int user) {
    if (qn <= 0) return 0;
    if (repeat_mode == REPEAT_ONE && !user) return 1; /* same index again */
    if (qi + 1 < qn) { qi++; return 1; }
    if (repeat_mode == REPEAT_ALL || user) {
        qi = 0;
        if (shuffle_on) { qi = -1; shuffle_rest(); qi = 0; }
        return repeat_mode == REPEAT_ALL || user ? 1 : 0;
    }
    return 0;
}

static int play_thread(SceSize a, void *arg) {
    (void)a;
    (void)arg;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) return sceKernelExitDeleteThread(0);
    int port_rate = 48000, port_vol = -1, decim = 1;
    float lim_state = 1;
    SceUInt64 last_tick = 0;
    for (;;) {
        lock();
        if (want_load) {
            want_load = 0;
            if (dec) { dec_close(dec); dec = NULL; }
            cur_track = -1;
            if (qi >= 0 && qi < qn && qord[qi] >= 0 && qord[qi] < M.nt) {
                int t = qord[qi];
                dec = dec_open(M.t[t].path);
                if (dec) {
                    cur_track = t;
                    int r = dec_rate(dec);
                    decim = r > 48000 ? (r + 47999) / 48000 : 1;
                    int pr = r / decim;
                    static const int ok_rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
                    int best = 48000;
                    for (int i = 0; i < 9; i++)
                        if (abs(ok_rates[i] - pr) < abs(best - pr)) best = ok_rates[i];
                    if (best != port_rate) {
                        sceAudioOutSetConfig(port, -1, best, SCE_AUDIO_OUT_MODE_STEREO);
                        port_rate = best;
                    }
                    cur_rate = r;
                    cur_frames = dec_frames(dec);
                    if (!cur_frames && M.t[t].dur > 0) cur_frames = (uint64_t)M.t[t].dur * r / 1000;
                    cur_pos = 0;
                    g_mus_started_track = t;
                } else {
                    /* can't open this one: skip it */
                    if (advance(0) && qn > 1) want_load = 1;
                    else playing = 0;
                }
            }
        }
        if (want_seek >= 0 && dec) {
            uint64_t f = (uint64_t)want_seek * cur_rate / 1000;
            if (cur_frames && f >= cur_frames) f = cur_frames - 1;
            dec_seek(dec, f);
            cur_pos = dec_pos(dec);
            want_seek = -1;
        }
        int go = playing && dec;
        unlock();

        if (port_vol != vol_hw) {
            int v[2] = {vol_hw, vol_hw};
            sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, v);
            port_vol = vol_hw;
        }
        if (!go) {
            level = 0;
            sceKernelDelayThread(8000);
            continue;
        }
        /* keep the Vita from going to sleep while music plays */
        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now - last_tick > 4000000) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            last_tick = now;
        }
        if (sleep_at && (int64_t)now >= sleep_at) {
            sleep_at = 0;
            playing = 0;
            continue;
        }
        /* decode one buffer */
        int need = 1024 * decim;
        int got = 0;
        lock();
        while (got < need && dec) {
            int n = dec_read(dec, decbuf + got * 2, need - got);
            if (n <= 0) break;
            got += n;
        }
        cur_pos = dec ? dec_pos(dec) : 0;
        int ended = got < need;
        unlock();
        if (decim > 1) {
            int outn = got / decim;
            for (int i = 0; i < outn; i++) {
                int l = 0, r = 0;
                for (int k = 0; k < decim; k++) {
                    l += decbuf[(i * decim + k) * 2];
                    r += decbuf[(i * decim + k) * 2 + 1];
                }
                outbuf[i * 2] = (int16_t)(l / decim);
                outbuf[i * 2 + 1] = (int16_t)(r / decim);
            }
            got = outn;
        } else {
            memcpy(outbuf, decbuf, got * 4);
        }
        if (got < 1024) memset(outbuf + got * 2, 0, (1024 - got) * 4);
        int g = vol_gain, peak = 0;
        if (g != 256) boost_apply(outbuf, 1024, 2, g / 256.0f, &lim_state);
        for (int i = 0; i < 2048; i++) {
            int v = outbuf[i] < 0 ? -outbuf[i] : outbuf[i];
            if (v > peak) peak = v;
        }
        level = level * 0.6f + (peak / 32768.0f) * 0.4f;
        if (got > 0) sceAudioOutOutput(port, outbuf);
        if (ended) {
            lock();
            if (sleep_end_song) {
                sleep_end_song = 0;
                playing = 0;
                if (advance(0)) want_load = 1;
            } else if (advance(0)) want_load = 1;
            else {
                playing = 0;
                if (dec) { dec_close(dec); dec = NULL; }
                cur_track = qi >= 0 && qi < qn ? qord[qi] : -1;
                cur_pos = 0;
                want_load = 1; /* ready to play the last song again */
            }
            unlock();
        }
    }
    return 0;
}

static void ensure_thread(void) {
    if (th >= 0) return;
    mtx = sceKernelCreateMutex("nest_mus", 0, 0, NULL);
    sceAppMgrAcquireBgmPort(); /* keep playing when Nest isn't in front */
    th = sceKernelCreateThread("nest_music", play_thread, 0x10000100 - 20, 0x8000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    rnd_state ^= (uint32_t)sceKernelGetProcessTimeWide();
    mus_set_volume(db_geti("music_vol", 100));
    shuffle_on = db_geti("music_shuffle", 0);
    repeat_mode = db_geti("music_repeat", REPEAT_OFF);
}

/* ---------------- controls (main thread) ---------------- */

static void set_from(int kind, int idx, const char *label) {
    from_kind = kind;
    from_idx = idx;
    snprintf(from_label, sizeof(from_label), "%s", label ? label : "");
    if (kind == FROM_ALBUM) music_note_recent(0, idx);
    else if (kind == FROM_ARTIST) music_note_recent(1, idx);
    else if (kind == FROM_PLAYLIST || kind == FROM_LIKED) music_note_recent(2, idx);
}

void mus_play(const int *tracks, int n, int start, int fk, int fi, const char *fl) {
    if (n <= 0) return;
    ensure_thread();
    lock();
    q_reserve(n);
    memcpy(qbase, tracks, sizeof(int) * n);
    memcpy(qord, tracks, sizeof(int) * n);
    qn = qbn = n;
    qi = clampi(start, 0, n - 1);
    if (shuffle_on) {
        /* the chosen song first, the rest shuffled */
        int t = qord[0];
        qord[0] = qord[qi];
        qord[qi] = t;
        qi = 0;
        shuffle_rest();
    }
    want_load = 1;
    want_seek = -1;
    playing = 1;
    unlock();
    set_from(fk, fi, fl);
}

void mus_play_shuffled(const int *tracks, int n, int fk, int fi, const char *fl) {
    if (n <= 0) return;
    ensure_thread();
    shuffle_on = 1;
    db_seti("music_shuffle", 1);
    mus_play(tracks, n, (int)(rnd() % (uint32_t)n), fk, fi, fl);
}

void mus_pause(void) { playing = 0; }
void mus_resume(void) {
    if (qn <= 0) return;
    ensure_thread();
    playing = 1;
}
void mus_toggle(void) {
    if (playing) mus_pause();
    else mus_resume();
}
void mus_stop(void) {
    lock();
    playing = 0;
    qn = qbn = 0;
    qi = -1;
    if (dec) { dec_close(dec); dec = NULL; }
    cur_track = -1;
    unlock();
}

void mus_next(void) {
    if (qn <= 0) return;
    lock();
    int rm = repeat_mode;
    if (rm == REPEAT_ONE) repeat_mode = REPEAT_ALL; /* skipping leaves the song */
    if (advance(1)) {
        want_load = 1;
        playing = 1;
    }
    if (rm == REPEAT_ONE) repeat_mode = REPEAT_ALL;
    unlock();
    if (rm == REPEAT_ONE) db_seti("music_repeat", repeat_mode);
}

void mus_prev(void) {
    if (qn <= 0) return;
    if (mus_pos_ms() > 3000) { mus_seek(0); return; } /* first press: back to the start */
    lock();
    if (qi > 0) qi--;
    else if (repeat_mode == REPEAT_ALL) qi = qn - 1;
    want_load = 1;
    playing = 1;
    unlock();
}

void mus_seek(int ms) {
    if (ms < 0) ms = 0;
    want_seek = ms;
}

void mus_set_shuffle(int on) {
    lock();
    if (on && !shuffle_on) {
        shuffle_on = 1;
        shuffle_rest();
    } else if (!on && shuffle_on) {
        shuffle_on = 0;
        /* back to the original order, staying on this song */
        int cur = qi >= 0 && qi < qn ? qord[qi] : -1;
        int keep_n = qn;
        if (qbn > 0) {
            memcpy(qord, qbase, sizeof(int) * qbn);
            qn = qbn;
            for (int i = 0; i < qn; i++)
                if (qord[i] == cur) { qi = i; break; }
        }
        (void)keep_n;
    }
    unlock();
    db_seti("music_shuffle", shuffle_on);
}

void mus_cycle_repeat(void) {
    repeat_mode = (repeat_mode + 1) % 3;
    db_seti("music_repeat", repeat_mode);
}

void mus_queue_add(int ti, int next) {
    if (ti < 0 || ti >= M.nt) return;
    ensure_thread();
    lock();
    q_reserve(qn + 1);
    int at = next ? (qi < 0 ? 0 : qi + 1) : qn;
    if (at > qn) at = qn;
    memmove(qord + at + 1, qord + at, sizeof(int) * (qn - at));
    qord[at] = ti;
    qn++;
    /* keep the unshuffled order in step */
    q_reserve(qbn + 1);
    qbase[qbn++] = ti;
    if (qi < 0) {
        qi = 0;
        want_load = 1;
        playing = 1;
    }
    unlock();
}

void mus_queue_remove(int k) {
    lock();
    if (k > qi && k < qn) {
        memmove(qord + k, qord + k + 1, sizeof(int) * (qn - k - 1));
        qn--;
    }
    unlock();
}

void mus_queue_jump(int k) {
    lock();
    if (k >= 0 && k < qn) {
        qi = k;
        want_load = 1;
        playing = 1;
    }
    unlock();
}

void mus_queue_move(int from, int to) {
    lock();
    if (from > qi && to > qi && from < qn && to < qn && from != to) {
        int v = qord[from];
        if (from < to) memmove(qord + from, qord + from + 1, sizeof(int) * (to - from));
        else memmove(qord + to + 1, qord + to, sizeof(int) * (from - to));
        qord[to] = v;
    }
    unlock();
}

void mus_set_volume(int pct) {
    vol_pct = clampi(pct, 0, 200);
    if (vol_pct <= 100) {
        vol_hw = 32768 * vol_pct / 100;
        vol_gain = 256;
    } else {
        vol_hw = 32768;
        vol_gain = 256 * vol_pct / 100;
    }
}
int mus_volume(void) { return vol_pct; }

int mus_active(void) { return qn > 0 && (cur_track >= 0 || (qi >= 0 && qi < qn)); }
int mus_playing(void) { return playing && qn > 0; }
int mus_current(void) {
    if (cur_track >= 0) return cur_track;
    return qi >= 0 && qi < qn ? qord[qi] : -1;
}
int mus_pos_ms(void) {
    if (want_seek >= 0) return want_seek;
    return cur_rate > 0 ? (int)(cur_pos * 1000 / cur_rate) : 0;
}
int mus_dur_ms(void) {
    if (cur_frames && cur_rate) return (int)(cur_frames * 1000 / cur_rate);
    int t = mus_current();
    return t >= 0 && t < M.nt ? M.t[t].dur : 0;
}
int mus_shuffle(void) { return shuffle_on; }
int mus_repeat(void) { return repeat_mode; }
int mus_queue(int *out, int max, int *cur) {
    lock();
    int n = qn < max ? qn : max;
    if (n > 0) memcpy(out, qord, sizeof(int) * n);
    if (cur) *cur = qi;
    unlock();
    return n;
}
const char *mus_from_label(void) { return from_label; }
int mus_from_kind(void) { return from_kind; }
int mus_from_idx(void) { return from_idx; }
float mus_level(void) { return playing ? level : 0; }

void mus_sleep_timer(int minutes) {
    sleep_end_song = minutes == -1;
    sleep_at = minutes > 0 ? (int64_t)sceKernelGetProcessTimeWide() + (int64_t)minutes * 60 * 1000000 : 0;
}
int mus_sleep_left(void) {
    if (sleep_end_song) return -1;
    if (!sleep_at) return 0;
    int64_t left = sleep_at - (int64_t)sceKernelGetProcessTimeWide();
    return left > 0 ? (int)(left / 1000000) + 1 : 0;
}

void mus_screen_off(void) {
    screen_off = 1;
    scePowerRequestDisplayOff();
}
int mus_screen_is_off(void) { return screen_off; }
void mus_screen_wake(void) {
    if (!screen_off) return;
    screen_off = 0;
    scePowerRequestDisplayOn();
}

/* ---------------- surviving a library reload ---------------- */
/* song numbers change when the library is reloaded: hold the queue as paths meanwhile */
static char **rm_ord, **rm_base, *rm_cur;

static int find_path(const char *p) {
    for (int i = 0; i < M.nt; i++)
        if (!strcmp(M.t[i].path, p)) return i;
    return -1;
}

void mus_remap_begin(void) {
    lock();
    rm_ord = malloc(sizeof(char *) * (qn + 1));
    rm_base = malloc(sizeof(char *) * (qbn + 1));
    for (int i = 0; i < qn; i++) rm_ord[i] = qord[i] >= 0 && qord[i] < M.nt ? strdup(M.t[qord[i]].path) : NULL;
    for (int i = 0; i < qbn; i++) rm_base[i] = qbase[i] >= 0 && qbase[i] < M.nt ? strdup(M.t[qbase[i]].path) : NULL;
    rm_cur = cur_track >= 0 && cur_track < M.nt ? strdup(M.t[cur_track].path) : NULL;
    /* stays locked until mus_remap_end */
}

void mus_remap_end(void) {
    int n = 0, cur_qi = qi;
    for (int i = 0; i < qn; i++) {
        int t = rm_ord[i] ? find_path(rm_ord[i]) : -1;
        if (t >= 0) qord[n++] = t;
        else if (i < qi) cur_qi--;
        free(rm_ord[i]);
    }
    qn = n;
    qi = qn ? clampi(cur_qi, 0, qn - 1) : -1;
    n = 0;
    for (int i = 0; i < qbn; i++) {
        int t = rm_base[i] ? find_path(rm_base[i]) : -1;
        if (t >= 0) qbase[n++] = t;
        free(rm_base[i]);
    }
    qbn = n;
    cur_track = rm_cur ? find_path(rm_cur) : -1;
    free(rm_cur);
    free(rm_ord);
    free(rm_base);
    rm_ord = rm_base = NULL;
    rm_cur = NULL;
    if (qn == 0) {
        playing = 0;
        if (dec) { dec_close(dec); dec = NULL; }
    }
    g_mus_started_track = -1;
    unlock();
}
