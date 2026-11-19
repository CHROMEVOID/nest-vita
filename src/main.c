/* Nest -- a calm media library for PlayStation Vita.
   Main loop, navigation with soft cross-fades, settings and UI sounds. */
#include "nest.h"
#include "music.h"

#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/apputil.h>
#include <psp2/kernel/sysmem.h>

/* keep the heap modest: the hardware video decoder needs room of its own */
int _newlib_heap_size_user = 72 * 1024 * 1024;

Settings S;
float g_time;
int g_secret_unlocked;
int g_completed_tid = -1;

/* ---------------- settings ---------------- */

void settings_load(void) {
    S.theme = db_geti("theme", 0);
    S.poster_size = db_geti("poster_size", 1);
    S.ambient = db_geti("ambient", 0);
    S.autoplay = db_geti("autoplay", 1);
    S.sounds = db_geti("sounds", 0);
    S.sub_size = db_geti("sub_size", 1);
    S.private_session = 0; /* never persists: a private session ends when Nest closes */
    S.show_lofi = db_geti("show_lofi", 1);
    S.manga_rtl = db_geti("manga_rtl", 0);
    S.reader_fit = db_geti("reader_fit", 0);
    S.hero_rotate = db_geti("hero_rotate", 1);
    S.focus_mode = db_geti("focus_mode", 0);
    S.onboarded = db_geti("onboarded", 0);
    S.resume_ask = db_geti("resume_ask", 1);
    S.reduce_motion = db_geti("reduce_motion", 0);
    S.clock = db_geti("clock", 1);
    S.bg_level = db_geti("bg_level", 1);
    S.bg_scene = db_geti("bg_scene", 0);
    S.read_size = db_geti("read_size", 1);
    S.read_spacing = db_geti("read_spacing", 1);
    S.read_margin = db_geti("read_margin", 1);
    S.read_light = db_geti("read_light", 0);
    S.read_anim = db_geti("read_anim", 1);
    S.read_tap = db_geti("read_tap", 1);
    S.thumbs = db_geti("thumbs", 1);
    S.ambient_sound = db_geti("ambient_sound", 0);
    S.video_fit = db_geti("video_fit", 0);
    S.glass = db_geti("glass2", 0); /* off unless you turn it on */
}

void settings_save(void) {
    db_exec("BEGIN");
    db_seti("theme", S.theme);
    db_seti("poster_size", S.poster_size);
    db_seti("ambient", S.ambient);
    db_seti("autoplay", S.autoplay);
    db_seti("sounds", S.sounds);
    db_seti("sub_size", S.sub_size);
    db_seti("show_lofi", S.show_lofi);
    db_seti("manga_rtl", S.manga_rtl);
    db_seti("reader_fit", S.reader_fit);
    db_seti("hero_rotate", S.hero_rotate);
    db_seti("focus_mode", S.focus_mode);
    db_seti("onboarded", S.onboarded);
    db_seti("resume_ask", S.resume_ask);
    db_seti("reduce_motion", S.reduce_motion);
    db_seti("clock", S.clock);
    db_seti("bg_level", S.bg_level);
    db_seti("bg_scene", S.bg_scene);
    db_seti("read_size", S.read_size);
    db_seti("read_spacing", S.read_spacing);
    db_seti("read_margin", S.read_margin);
    db_seti("read_light", S.read_light);
    db_seti("read_anim", S.read_anim);
    db_seti("read_tap", S.read_tap);
    db_seti("thumbs", S.thumbs);
    db_seti("ambient_sound", S.ambient_sound);
    db_seti("video_fit", S.video_fit);
    db_seti("glass2", S.glass);
    db_exec("COMMIT");
}

/* ---------------- UI sounds: tiny synthesized ticks ---------------- */

static volatile int sfx_req = -1;
volatile int g_ambient_kind; /* 0 off, 1 rain, 2 vinyl; set by the main loop */

/* tiny, cheap noise source */
static uint32_t rng = 0x9E3779B9u;
static inline float noise(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (int32_t)rng / 2147483648.0f;
}

/* UI ticks and ambient sound share one quiet mono port. Ambient audio is made
   on the fly: filtered noise for rain with the odd droplet, or a warm hiss
   with crackles for vinyl. Nothing is stored in the VPK. */
static int sfx_thread(SceSize a, void *p) {
    (void)a;
    (void)p;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 256, 48000, SCE_AUDIO_OUT_MODE_MONO);
    if (port < 0) return 0;
    static int16_t buf[256];
    int tick = -1, tick_pos = 0, tick_len = 0;
    float ph = 0, f0 = 0, f1 = 0;
    float lp = 0, lp2 = 0, amb_gain = 0;
    float drop_env = 0, drop_ph = 0, drop_f = 0, crackle = 0;
    for (;;) {
        int k = sfx_req;
        if (k >= 0) {
            sfx_req = -1;
            tick = k;
            tick_pos = 0;
            ph = 0;
            f0 = k == 1 ? 880 : k == 2 ? 520 : 1320;
            f1 = k == 1 ? 1320 : k == 2 ? 390 : 1320;
            tick_len = (int)(48000 * (k == 0 ? 0.022f : 0.06f));
        }
        int amb = g_ambient_kind;
        if (tick < 0 && amb == 0 && amb_gain < 0.001f) {
            sceKernelDelayThread(8000);
            continue;
        }
        for (int i = 0; i < 256; i++) {
            float out = 0, tick_out = 0;
            /* ambient bed, faded in and out gently */
            amb_gain += ((amb ? 1.0f : 0.0f) - amb_gain) * 0.00004f;
            if (amb_gain > 0.0005f) {
                float n = noise();
                if (amb == 2) { /* vinyl: soft hiss and crackle */
                    lp += (n - lp) * 0.18f;
                    float v = lp * 0.22f;
                    if (noise() > 0.9993f) crackle = 0.6f + 0.4f * noise();
                    v += crackle * noise();
                    crackle *= 0.92f;
                    out += v * amb_gain;
                } else { /* rain: low rumble + brighter patter + droplets */
                    lp += (n - lp) * 0.05f;
                    lp2 += (n - lp2) * 0.35f;
                    float v = lp * 0.9f + (lp2 - lp) * 0.25f;
                    if (noise() > 0.99985f) {
                        drop_env = 0.35f;
                        drop_f = 1800 + 1400 * fabsf(noise());
                    }
                    drop_ph += 2 * (float)M_PI * drop_f / 48000;
                    v += sinf(drop_ph) * drop_env;
                    drop_env *= 0.9975f;
                    out += v * amb_gain * 0.8f;
                }
            }
            /* UI tick */
            if (tick >= 0) {
                float t = (float)tick_pos / tick_len;
                if (t <= 1) {
                    float f = f0 + (f1 - f0) * t;
                    ph += 2 * (float)M_PI * f / 48000;
                    float env = (1 - t) * (1 - t) * fminf(1, tick_pos / 60.0f);
                    tick_out = sinf(ph) * env;
                    tick_pos++;
                } else {
                    tick = -1;
                }
            }
            float v = (out * 0.30f + tick_out * 0.08f) * 32767.0f;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            buf[i] = (int16_t)v;
        }
        sceAudioOutOutput(port, buf);
    }
    return 0;
}

void sfx(int kind) {
    if (S.sounds) sfx_req = kind;
}

/* ---------------- navigation ---------------- */

static int stack[16];
static int sp;
static int pending = -1, pending_mode; /* 0 push 1 pop 2 replace */
static float fade = 1;                  /* 1 = fully covered */
static int fading_out;
static int apply_now;

int nav_top(void) { return sp > 0 ? stack[sp - 1] : SCR_MAIN; }

static void begin(int mode, int scr) {
    pending = scr;
    pending_mode = mode;
    fading_out = 1;
}

void nav_push(int scr) { begin(0, scr); }
void nav_pop(void) { begin(1, -1); }
void nav_replace(int scr) { begin(2, scr); }

static void apply_pending(void) {
    if (pending_mode == 0 && sp < 16) stack[sp++] = pending;
    else if (pending_mode == 1 && sp > 1) {
        int leaving = stack[sp - 1];
        if (leaving == SCR_PLAYER) player_close();
        if (leaving == SCR_READER) reader_close();
        sp--;
        if (stack[sp - 1] == SCR_MAIN) main_enter();
    } else if (pending_mode == 2 && sp > 0) {
        int leaving = stack[sp - 1];
        if (leaving == SCR_PLAYER && pending != SCR_PLAYER) player_close();
        if (leaving == SCR_READER && pending != SCR_READER) reader_close();
        stack[sp - 1] = pending;
        if (pending == SCR_MAIN) main_enter();
    }
    pending = -1;
}

static void frame_for(int scr, float dt) {
    switch (scr) {
    case SCR_ONBOARD: onboard_frame(dt); break;
    case SCR_MAIN: main_frame(dt); break;
    case SCR_DETAIL: detail_frame(dt); break;
    case SCR_PLAYER: player_frame(dt); break;
    case SCR_READER: reader_frame(dt); break;
    case SCR_SEARCH: search_frame(dt); break;
    case SCR_SETTINGS: settings_frame(dt); break;
    case SCR_STATS: stats_frame(dt); break;
    case SCR_PICKER: picker_frame(dt); break;
    case SCR_PIN: pin_frame(dt); break;
    case SCR_SCAN: scan_frame(dt); break;
    case SCR_SPOTLIGHT: spotlight_frame(dt); break;
    case SCR_HISTORY: history_frame(dt); break;
    case SCR_STORAGE: storage_frame(dt); break;
    case SCR_MPAGE: mpage_frame(dt); break;
    case SCR_NOWPLAYING: nowplaying_frame(dt); break;
    }
}

static void fatal_screen(const char *msg) {
    for (;;) {
        input_update(1 / 60.0f);
        vita2d_start_drawing();
        vita2d_clear_screen();
        text_center(F_BOLD, 22, SCR_W / 2, 250, T->text, "Nest couldn't start");
        text_center(F_REG, 15, SCR_W / 2, 284, T->text2, msg);
        text_center(F_REG, 13, SCR_W / 2, 320, T->text3, "Press START to exit");
        vita2d_end_drawing();
        vita2d_swap_buffers();
        if (in.pressed & B_START) break;
    }
}

int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);

    gfx_init();
    /* textures live in main memory so video memory stays free for the decoder */
    vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    input_init();
    theme_apply(0);
    gfx_frame_begin();

    if (db_open() != 0) {
        fatal_screen("The library database at ux0:data/Nest could not be opened.");
        vita2d_fini();
        sceKernelExitProcess(0);
        return 0;
    }
    settings_load();
    theme_apply(S.theme);
    gfx_frame_begin();
    img_init();
    lib_load();
    music_init();
    /* a library on the PC needs the network up before anything loads from it */
    for (int i = 0; i < L.ne; i++)
        if (net_is_url(L.e[i].path)) { net_up(); break; }

    SceUID th = sceKernelCreateThread("nest_sfx", sfx_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    if (S.onboarded) {
        stack[sp++] = SCR_MAIN;
        main_enter();
    } else {
        stack[sp++] = SCR_ONBOARD;
        onboard_enter();
    }

    SceUInt64 last = sceKernelGetProcessTimeWide();
    float trim_timer = 0;
    for (;;) {
        SceUInt64 now = sceKernelGetProcessTimeWide();
        float dt = (now - last) / 1000000.0f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        g_time += dt;

        input_update(dt);
        /* screen turned off for music: any button or touch wakes it, and does nothing else */
        if (mus_screen_is_off() && (in.pressed || in.touching)) {
            mus_screen_wake();
            in.pressed = in.repeat = 0;
            in.tap = in.double_tap = in.long_press = 0;
        }
        /* keep the screen awake while watching or reading */
        int top = nav_top();
        if (top == SCR_PLAYER || top == SCR_READER || top == SCR_SCAN) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

        if (apply_now) {
            apply_now = 0;
            apply_pending();
        }
        ime_pump(); /* the system keyboard runs between frames, never inside one */
        {
            int top_now = nav_top();
            int browsing = (top_now == SCR_MAIN || top_now == SCR_DETAIL || top_now == SCR_SEARCH) &&
                           !modal_active() && pending < 0 && !fading_out;
            thumb_tick(dt, browsing);
        }
        gfx_frame_begin();
        lofi_tick(dt);
        {
            /* ambient sound: menus and reading only, never over a video */
            int top_a = nav_top();
            g_ambient_kind = (top_a == SCR_PLAYER || top_a == SCR_PIN || mus_playing()) ? 0 : S.ambient_sound;
        }
        vita2d_start_drawing();
        vita2d_clear_screen();

        glass_reset();
        Input saved = in;
        int frozen = modal_active() || pending >= 0 || apply_now;
        if (frozen) {
            in.pressed = in.repeat = 0;
            in.tap = in.long_press = in.double_tap = 0;
            in.ddx = in.ddy = 0;
            in.dragging = 0;
        }
        if (miniplayer_owns_tap(nav_top())) { /* taps on the music bar are for the bar only */
            in.tap = in.long_press = in.double_tap = 0;
        }
        frame_for(nav_top(), dt);
        in = saved;
        if (pending >= 0) {
            in.pressed = in.repeat = 0;
            in.tap = in.long_press = 0;
        }
        metafind_frame(dt);
        music_frame(dt);
        miniplayer_draw(dt, nav_top());
        modal_frame(dt);
        draw_toasts(dt);

        /* cross-fade between screens */
        float speed = S.reduce_motion ? 22 : 17; /* quick and clean between screens */
        if (fading_out) {
            fade = approach(fade, 1.05f, speed * 1.4f, dt);
            if (fade >= 0.98f) {
                fade = 1;
                fading_out = 0;
                apply_now = 1; /* switch screens at the start of the next frame */
            }
        } else {
            fade = approach(fade, -0.05f, speed, dt);
            if (fade < 0) fade = 0;
        }
        if (fade > 0.002f) gfx_rect(0, 0, SCR_W, SCR_H, col_a(T->bg, fade));

        vita2d_end_drawing();
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        img_frame_end();

        trim_timer += dt;
        if (trim_timer > 4) {
            trim_timer = 0;
            lofi_trim();
        }
    }
    return 0;
}
