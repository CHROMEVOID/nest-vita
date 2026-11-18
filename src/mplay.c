/* The music engine. One thread decodes and plays, and moves through the queue
   on its own, so songs keep going behind the menus, while a video is paused,
   with the screen off, or with Nest in the background. */
#include "nest.h"
#include "music.h"
#include <sqlite3.h>
sqlite3 *db_handle(void);

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
static volatile uint64_t cur_frames;
static char from_label[128];
static int from_kind, from_idx;
static volatile int sleep_end_song;
static volatile int64_t sleep_at; /* process time in us, 0 = off */
static int screen_off;
static volatile int release_req, port_released;

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

/* ---------------- the two audio threads ----------------
   A decoder thread keeps about a second and a half of music ready in a ring,
   and the output thread plays from it. Slow memory card moments (a scan,
   a big cover image) never reach your ears. */

#define RING 65536 /* frames, a power of two */
static int16_t ring[RING * 2];
static volatile unsigned rd, wr;      /* free-running frame counters */
static volatile int ring_eof;         /* the decoder reached the end of this song */
static volatile int ring_rate = 48000;/* rate of what's in the ring */
static volatile int gen;              /* goes up on every load / seek / stop */
static volatile int want_stop;
static volatile uint64_t out_pos;     /* source frames played so far */
static volatile int decim = 1;
static SceUID th_dec = -1;
static int16_t outbuf[1024 * 2];
static int16_t decbuf[4096 * 2];
static int16_t dtmp[1024 * 2];

/* move on after a song ends; returns 0 when there's nothing left */
static int advance(int user) {
    if (qn <= 0) return 0;
    if (repeat_mode == REPEAT_ONE && !user) return 1; /* same index again */
    if (qi + 1 < qn) { qi++; return 1; }
    if (repeat_mode == REPEAT_ALL || user) {
        qi = 0;
        if (shuffle_on) { qi = -1; shuffle_rest(); qi = 0; }
        return 1;
    }
    return 0;
}

static void ring_reset(void) {
    rd = wr = 0;
    ring_eof = 0;
    gen++;
}

static int decode_thread(SceSize a, void *arg) {
    (void)a;
    (void)arg;
    static char path[1024];
    for (;;) {
        lock();
        if (want_stop) {
            want_stop = 0;
            Dec *old = dec;
            dec = NULL;
            cur_track = -1;
            ring_reset();
            out_pos = 0;
            unlock();
            if (old) dec_close(old);
            continue;
        }
        if (want_load) {
            want_load = 0;
            Dec *old = dec;
            dec = NULL;
            cur_track = -1;
            ring_reset();
            out_pos = 0;
            int t = qi >= 0 && qi < qn && qord[qi] >= 0 && qord[qi] < M.nt ? qord[qi] : -1;
            if (t >= 0) snprintf(path, sizeof(path), "%s", M.t[t].path);
            unlock();
            if (old) dec_close(old);
            if (t < 0) continue;
            Dec *d = dec_open(path); /* outside the lock: the menus never wait on a slow file */
            lock();
            if (want_load || want_stop) { /* something else was picked meanwhile */
                unlock();
                if (d) dec_close(d);
                continue;
            }
            if (!d) { /* this one won't open: skip it */
                if (advance(0) && qn > 1) want_load = 1;
                else playing = 0;
                unlock();
                continue;
            }
            dec = d;
            cur_track = t;
            int r = dec_rate(d);
            decim = r > 48000 ? (r + 47999) / 48000 : 1;
            int pr = r / decim;
            static const int ok_rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
            int best = 48000;
            for (int i = 0; i < 9; i++)
                if (abs(ok_rates[i] - pr) < abs(best - pr)) best = ok_rates[i];
            ring_rate = best;
            cur_rate = r;
            cur_frames = dec_frames(d);
            if (!cur_frames && t < M.nt && M.t[t].dur > 0) cur_frames = (uint64_t)M.t[t].dur * r / 1000;
            g_mus_started_track = t;
            unlock();
            continue;
        }
        if (want_seek >= 0 && dec) {
            uint64_t f = (uint64_t)want_seek * cur_rate / 1000;
            if (cur_frames && f >= cur_frames) f = cur_frames - 1;
            dec_seek(dec, f);
            ring_reset();
            out_pos = dec_pos(dec);
            want_seek = -1;
        }
        int space = RING - (int)(wr - rd);
        int can = dec && !ring_eof && space >= 1100;
        int my_gen = gen, dm = decim;
        Dec *d = dec;
        unlock();
        if (!can) {
            sceKernelDelayThread(6000);
            continue;
        }
        /* only this thread ever closes the decoder, so it is safe to use without the lock */
        int need = 1024 * dm, got = 0;
        while (got < need) {
            int n = dec_read(d, decbuf + got * 2, need - got);
            if (n <= 0) break;
            got += n;
        }
        int outn = got;
        const int16_t *src = decbuf;
        if (dm > 1) {
            outn = got / dm;
            for (int i = 0; i < outn; i++) {
                int l = 0, r = 0;
                for (int k = 0; k < dm; k++) {
                    l += decbuf[(i * dm + k) * 2];
                    r += decbuf[(i * dm + k) * 2 + 1];
                }
                dtmp[i * 2] = (int16_t)(l / dm);
                dtmp[i * 2 + 1] = (int16_t)(r / dm);
            }
            src = dtmp;
        }
        lock();
        if (gen == my_gen) {
            for (int i = 0; i < outn; i++) {
                unsigned p = (wr + i) & (RING - 1);
                ring[p * 2] = src[i * 2];
                ring[p * 2 + 1] = src[i * 2 + 1];
            }
            wr += outn;
            if (got < need) ring_eof = 1;
        }
        unlock();
    }
    return 0;
}

static int play_thread(SceSize a, void *arg) {
    (void)a;
    (void)arg;
    /* the Vita has one music (BGM) channel: hold it only while songs play, so videos get it */
    int port = -1;
    int port_rate = 48000, port_vol = -1;
    DspState dsp = {1};
    SceUInt64 last_tick = 0;
    for (;;) {
        int go = playing && (cur_track >= 0 || want_load);
        if (release_req || (!playing && port >= 0)) {
            if (port >= 0) {
                sceAudioOutReleasePort(port);
                port = -1;
            }
            port_released = 1;
            release_req = 0;
        }
        if (!go) {
            level = 0;
            sceKernelDelayThread(8000);
            continue;
        }
        if (port < 0) {
            port_rate = ring_rate;
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, port_rate, SCE_AUDIO_OUT_MODE_STEREO);
            if (port < 0) { /* a video still has it */
                sceKernelDelayThread(50000);
                continue;
            }
            port_released = 0;
            port_vol = -1;
        }
        if (port_vol != vol_hw) {
            int v[2] = {vol_hw, vol_hw};
            sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, v);
            port_vol = vol_hw;
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
        /* take a buffer from the ring */
        lock();
        int avail = (int)(wr - rd), eof = ring_eof, n = 0;
        if (cur_track >= 0 && (avail >= 1024 || (eof && avail > 0))) {
            n = avail < 1024 ? avail : 1024;
            for (int i = 0; i < n; i++) {
                unsigned p = (rd + i) & (RING - 1);
                outbuf[i * 2] = ring[p * 2];
                outbuf[i * 2 + 1] = ring[p * 2 + 1];
            }
            rd += n;
            out_pos += (uint64_t)n * decim;
        }
        int drained = cur_track >= 0 && eof && wr == rd;
        int rate_now = ring_rate;
        unlock();
        if (n > 0) {
            if (rate_now != port_rate) {
                sceAudioOutSetConfig(port, -1, rate_now, SCE_AUDIO_OUT_MODE_STEREO);
                port_rate = rate_now;
            }
            if (n < 1024) memset(outbuf + n * 2, 0, (1024 - n) * 4);
            dsp_run(outbuf, 1024, port_rate, vol_gain / 256.0f, &dsp); /* EQ, loud mode, boost, limiter */
            int peak = 0;
            for (int i = 0; i < 2048; i += 4) {
                int v = outbuf[i] < 0 ? -outbuf[i] : outbuf[i];
                if (v > peak) peak = v;
            }
            level = level * 0.6f + (peak / 32768.0f) * 0.4f;
            sceAudioOutOutput(port, outbuf);
        } else if (!drained) {
            sceKernelDelayThread(3000); /* the decoder is catching up (or opening the next song) */
        }
        if (drained) { /* the song is over */
            lock();
            if (sleep_end_song) {
                sleep_end_song = 0;
                playing = 0;
                if (advance(0)) want_load = 1;
            } else if (advance(0)) want_load = 1;
            else { /* end of the queue: stop on the last song, ready to play it again */
                playing = 0;
                want_load = 1;
            }
            ring_eof = 0; /* handled */
            unlock();
        }
    }
    return 0;
}

static void ensure_thread(void) {
    if (th >= 0) return;
    mtx = sceKernelCreateMutex("nest_mus", 0, 0, NULL);
    sceAppMgrAcquireBgmPort(); /* keep playing when Nest isn't in front */
    th_dec = sceKernelCreateThread("nest_mdec", decode_thread, 0x10000100 - 10, 0x40000, 0, 0x40000, NULL);
    if (th_dec >= 0) sceKernelStartThread(th_dec, 0, NULL);
    th = sceKernelCreateThread("nest_music", play_thread, 0x10000100 - 20, 0x10000, 0, 0x40000, NULL);
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

/* a video is starting: stop and give back the music channel straight away */
void mus_release_audio(void) {
    playing = 0;
    if (th < 0) return;
    release_req = 1;
    for (int i = 0; i < 40 && !port_released; i++) sceKernelDelayThread(5000);
}
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
    want_stop = 1;
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
    vol_pct = clampi(pct, 0, 300);
    /* 100% is a touch louder than the Vita's own maximum (x1.2), and the top end
       reaches x3.6: the hardware does what it can, software does the rest, and
       the limiter keeps it clean */
    float eff = vol_pct * 1.2f / 100.0f;
    if (vol_pct > 150) eff = 1.8f * powf(vol_pct / 150.0f, 1.75f); /* past 150% it climbs fast: 300% is about x6 */
    if (eff <= 1.0f) {
        vol_hw = (int)(32768 * eff);
        vol_gain = 256;
    } else {
        vol_hw = 32768;
        vol_gain = (int)(256 * eff);
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
    return cur_rate > 0 ? (int)(out_pos * 1000 / cur_rate) : 0;
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
        want_stop = 1;
    }
    g_mus_started_track = -1;
    unlock();
}

/* ---------------- remembering where you were ---------------- */

static int saved_qn = -1, saved_qhash;
static float state_t;

static int queue_hash(void) {
    unsigned h = 2166136261u;
    for (int i = 0; i < qn; i++) h = (h ^ (unsigned)qord[i]) * 16777619u;
    return (int)(h ^ (unsigned)qn);
}

static void save_state(void) {
    if (qn <= 0 || qi < 0 || qi >= qn) return;
    sqlite3 *db = db_handle();
    if (!db) return;
    int h = queue_hash();
    if (qn != saved_qn || h != saved_qhash) { /* the queue itself changed: write it out */
        db_exec("BEGIN; CREATE TABLE IF NOT EXISTS mqueue(pos INT PRIMARY KEY, path TEXT); DELETE FROM mqueue;");
        sqlite3_stmt *s = NULL;
        sqlite3_prepare_v2(db, "INSERT INTO mqueue(pos, path) VALUES(?, ?)", -1, &s, NULL);
        for (int i = 0; s && i < qn; i++) {
            if (qord[i] < 0 || qord[i] >= M.nt) continue;
            sqlite3_reset(s);
            sqlite3_bind_int(s, 1, i);
            sqlite3_bind_text(s, 2, M.t[qord[i]].path, -1, SQLITE_STATIC);
            sqlite3_step(s);
        }
        if (s) sqlite3_finalize(s);
        db_exec("COMMIT");
        saved_qn = qn;
        saved_qhash = h;
    }
    char v[300];
    snprintf(v, sizeof(v), "%d,%d,%d,%d,%s", qi, mus_pos_ms(), from_kind, from_idx, from_label);
    db_set("mus_state", v);
}

void mus_state_tick(float dt) {
    static int was_playing;
    int p = mus_playing();
    state_t += dt;
    /* every 5 s while playing, and right away on pause */
    if ((p && state_t > 5) || (was_playing && !p)) {
        state_t = 0;
        save_state();
    }
    was_playing = p;
}

void mus_state_restore(void) {
    const char *v = db_get("mus_state", NULL);
    if (!v || !M.nt) return;
    int sqi = 0, spos = 0, fk = 0, fi = 0;
    char label[200] = "";
    if (sscanf(v, "%d,%d,%d,%d,%199[^\n]", &sqi, &spos, &fk, &fi, label) < 4) return;
    sqlite3 *db = db_handle();
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT path FROM mqueue ORDER BY pos", -1, &s, NULL) != SQLITE_OK) return;
    /* path -> song */
    int n = 0, cap = 256;
    int *list = malloc(sizeof(int) * cap);
    int cur = -1;
    int row = 0;
    while (list && sqlite3_step(s) == SQLITE_ROW) {
        const char *pth = (const char *)sqlite3_column_text(s, 0);
        int t = pth ? music_track_by_path(pth) : -1;
        if (t >= 0) {
            if (n == cap) { cap *= 2; list = realloc(list, sizeof(int) * cap); }
            if (row == sqi) cur = n;
            list[n++] = t;
        }
        row++;
    }
    sqlite3_finalize(s);
    if (n > 0 && cur >= 0) {
        ensure_thread();
        lock();
        q_reserve(n);
        memcpy(qord, list, sizeof(int) * n);
        memcpy(qbase, list, sizeof(int) * n);
        qn = qbn = n;
        qi = cur;
        playing = 0;      /* ready, not blasting music at start-up */
        want_load = 1;
        want_seek = spos > 2000 ? spos - 2000 : 0;
        unlock();
        from_kind = fk;
        from_idx = fi;
        snprintf(from_label, sizeof(from_label), "%s", label);
        saved_qn = qn;
        saved_qhash = queue_hash();
    }
    free(list);
}
