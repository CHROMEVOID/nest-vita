#ifndef NEST_H
#define NEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <strings.h>

#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "parse.h"
#include "scan.h"

#define SCR_W 960
#define SCR_H 544
#define DATA_DIR "ux0:data/Nest"
#define DB_PATH DATA_DIR "/nest.db"
#define ASSET(p) "app0:assets/" p
#define NEST_VERSION "1.1"

typedef uint32_t col_t;
#define RGBA(r, g, b, a) RGBA8(r, g, b, a)
#define HEX(h) RGBA8(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255, 255)

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline int clampi(int v, int a, int b) { return v < a ? a : v > b ? b : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
/* frame-rate independent smoothing toward a target */
static inline float approach(float cur, float target, float speed, float dt) {
    return cur + (target - cur) * (1.0f - expf(-speed * dt));
}
static inline float ease_out(float t) { t = clampf(t, 0, 1); return 1 - (1 - t) * (1 - t) * (1 - t); }
static inline float ease_inout(float t) { t = clampf(t, 0, 1); return t < .5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }

col_t col_a(col_t c, float a);              /* multiply alpha */
/* AvPlayer hands back a pointer-like handle (0x81xxxxxx), which is negative as an int;
   real errors are 0x806Axxxx codes. Never test it with > 0. */
static inline int avplayer_ok(int h) { return h != 0 && ((unsigned)h & 0xFFFF0000u) != 0x806A0000u; }
col_t col_mix(col_t a, col_t b, float t);   /* blend colours */

/* ---------------- gfx.c ---------------- */
enum { F_REG, F_SEMI, F_BOLD, F_DISPLAY, F_META, F_COUNT };
void gfx_init(void);
void gfx_load_fonts(int display_font, int meta_mono);
void gfx_frame_begin(void);
void gfx_rect(float x, float y, float w, float h, col_t c);
void gfx_rrect(float x, float y, float w, float h, float r, col_t c);
void gfx_rrect_grad(float x, float y, float w, float h, float r, col_t top, col_t bottom);
void gfx_rrect_outline(float x, float y, float w, float h, float r, float t, col_t c);
void gfx_shadow(float x, float y, float w, float h, float r, float spread, col_t c);
void gfx_grad_v(float x, float y, float w, float h, col_t top, col_t bottom);
void gfx_grad_h(float x, float y, float w, float h, col_t left, col_t right);
void gfx_circle(float x, float y, float r, col_t c);
void gfx_ring(float x, float y, float r, float t, col_t c);
void gfx_line(float x0, float y0, float x1, float y1, float t, col_t c);
/* textured, rounded, "cover" crop (ax/ay = focus point 0..1) */
void gfx_tex(vita2d_texture *tex, float x, float y, float w, float h, float r, col_t tint, float ax, float ay);
void gfx_tex_fit(vita2d_texture *tex, float x, float y, float w, float h, col_t tint);
void gfx_clip(float x, float y, float w, float h);
/* frosted glass: see-through tinted panel, glossy sheen, light rim, soft shadow (a = opacity) */
void gfx_glass(float x, float y, float w, float h, float r, float a);
void glass_reset(void); /* each frame */
/* tell the glass what's drawn behind: a (blurry) texture shown "cover" over a rect, and the veil on top of it */
void glass_backdrop(vita2d_texture *tex, float x, float y, float w, float h, float ax, float ay, col_t veil);
void gfx_glass_tint(float x, float y, float w, float h, float r, float a, col_t tint);
void gfx_noclip(void);

void text(int f, int size, float x, float y, col_t c, const char *s);
int text_w(int f, int size, const char *s);
void text_center(int f, int size, float cx, float y, col_t c, const char *s);
void text_right(int f, int size, float rx, float y, col_t c, const char *s);
void text_ellipsis(int f, int size, float x, float y, float maxw, col_t c, const char *s);
int text_wrap(int f, int size, float x, float y, float maxw, float line_h, int max_lines, col_t c, const char *s);
int text_wrap_count(int f, int size, float maxw, const char *s);

enum {
    IC_PLAY, IC_PAUSE, IC_SEARCH, IC_GEAR, IC_LOCK, IC_UNLOCK, IC_HEART, IC_HEART_FILL, IC_STAR, IC_STAR_FILL,
    IC_BACK, IC_PLUS, IC_CHECK, IC_HOME, IC_FILM, IC_TV, IC_BOOK, IC_NEXT, IC_PREV, IC_REW, IC_FWD, IC_VOLUME,
    IC_SUBS, IC_SPEED, IC_FOLDER, IC_INFO, IC_CLOSE, IC_CHEVRON_R, IC_CHEVRON_L, IC_CHEVRON_D, IC_GRID, IC_CHART,
    IC_CALENDAR, IC_SPARK, IC_DOTS, IC_EYE_OFF, IC_REFRESH, IC_TRASH, IC_PENCIL, IC_IMAGE, IC_MOON, IC_DROP,
    IC_SHUFFLE, IC_REPEAT, IC_NOTE, IC_QUEUE, IC_TIMER, IC_SCREEN_OFF, IC_MIC, IC_EQ
};
void icon(int id, float cx, float cy, float s, col_t c);

/* ---------------- input.c ---------------- */
enum {
    B_UP = 1 << 0, B_DOWN = 1 << 1, B_LEFT = 1 << 2, B_RIGHT = 1 << 3, B_OK = 1 << 4, B_BACK = 1 << 5,
    B_TRI = 1 << 6, B_SQR = 1 << 7, B_L = 1 << 8, B_R = 1 << 9, B_START = 1 << 10, B_SELECT = 1 << 11
};
typedef struct {
    uint32_t held, pressed, repeat; /* repeat = pressed + auto-repeat for navigation */
    int touching;
    float tx, ty;                  /* current touch */
    float sx, sy;                  /* touch start */
    int tap, long_press, double_tap;
    float tap_x, tap_y;
    int dragging;
    float ddx, ddy;                /* drag delta this frame */
    float vx, vy;                  /* fling velocity on release (px/s) */
    int released;
    int any;                       /* any input this frame (for idle timers) */
    float lstick_y;
} Input;
extern Input in;
void input_init(void);
void input_update(float dt);
int hit(float x, float y, float w, float h); /* tap landed in rect */
int hit_long(float x, float y, float w, float h);
int touch_in(float x, float y, float w, float h);
/* iPhone-style scrolling. pos/tgt are a screen's scroll position and where the d-pad wants it.
   grab: this list owns the finger right now; d: finger movement on its axis (in.ddy or in.ddx);
   v: release speed on that axis (in.vy or in.vx). Follows the finger exactly, a flick glides
   and slows down by itself, and the ends stretch a little and spring back. */
typedef struct { float vel; int held; } KScroll;
void kscroll(float *pos, float *tgt, KScroll *k, float max, int grab, float d, float v, float dt);
void input_consume_touch(void);

/* ---------------- theme.c ---------------- */
typedef struct {
    const char *name, *tagline;
    col_t bg, bg2, surface, surface2, line, text, text2, text3, accent, accent2, on_accent, focus, shadow;
    float radius, radius_sm;
    int focus_style;   /* 0 ring + lift, 1 glow, 2 underline + lift */
    int display_font;  /* 0 Inter Bold, 1 DM Serif Display, 2 Space Mono */
    int meta_mono;
    int light;
    int nav_style;     /* 0 floating pill, 1 underline */
    const char *scene; /* lofi scene used across this theme */
    float scene_alpha;
    float grain;       /* subtle texture lines in hero areas */
} Theme;
#define THEME_COUNT 6
extern const Theme THEMES[THEME_COUNT];
extern const Theme *T;
void theme_apply(int idx);

/* ---------------- lofi.c ---------------- */
typedef struct Lofi Lofi;
Lofi *lofi_get(const char *scene);           /* cached */
void lofi_tick(float dt);                    /* advance all visible scenes */
void lofi_draw(Lofi *l, float x, float y, float w, float h, float r, col_t tint, float ax, float ay);
vita2d_texture *lofi_soft(const char *scene);/* blurred still */
void lofi_trim(void);
void lofi_release_all(void); /* free every scene at the next frame start */

/* ---------------- net.c: the PC over Wi-Fi ---------------- */
typedef struct NetFile NetFile;
int net_is_url(const char *p);
int net_up(void);
int net_wifi_connected(void);
void *net_read_all(const char *url, size_t *len); /* malloc'd, NUL-terminated */
int net_list(const char *dir, fs_cb cb, void *ud);
NetFile *netf_open(const char *url);
int netf_read(NetFile *f, uint8_t *dst, uint64_t pos, uint32_t len);
uint64_t netf_size(NetFile *f);
void netf_close(NetFile *f);
void net_avplayer_files(void *replacement); /* fills a SceAvPlayerFileReplacement */

/* boost.c: volume past 100% through a clean limiter */
void boost_apply(int16_t *buf, int frames, int ch, float gain, float *state);

/* ---------------- media.c ---------------- */
float media_aspect(const char *path); /* display shape from the MP4 header, 0 if unknown */

/* ---------------- thumb.c ---------------- */
void thumb_tick(float dt, int allowed);
void thumb_reset(void);                        /* free scenes not drawn lately */
extern const char *LOFI_SCENES[];
extern const int LOFI_SCENE_COUNT;

/* ---------------- zip.c ---------------- */
typedef struct Zip Zip;
Zip *zip_open(const char *path);
void zip_close(Zip *z);
int zip_count(Zip *z);
const char *zip_name(Zip *z, int i);
int zip_find(Zip *z, const char *name);
void *zip_read(Zip *z, int i, size_t *size); /* malloc'd */

/* ---------------- img.c ---------------- */
void img_init(void);
/* returns texture when ready (NULL while loading). fade receives 0..1 */
vita2d_texture *img_get(const char *src, int w, int h, float *fade);
void img_frame_end(void);
void img_drop(const char *src);
void img_flush(void);
int img_pending(void);
/* decode helper usable from other threads: returns RGBA buffer */
uint8_t *img_decode_file(const char *src, int maxw, int maxh, int *ow, int *oh);
vita2d_texture *img_upload(uint8_t *rgba, int w, int h);

/* ---------------- library (lib.c) ---------------- */
typedef struct {
    int id;
    int tidx;
    int season, number, number_end;
    char *name, *path;
    long long size;
    int playable;
    int media;
    int pos, dur, finished; /* ms for video, pages for books */
    int64_t updated;
    float aspect;           /* picture shape stored by the scanner, 0 = unknown */
} Entry;

typedef struct {
    int id;
    int kind, secret;
    char *title, *orig_title;
    int year;
    char *overview, *genres;
    char *poster, *backdrop, *logo;
    char *key;
    int favorite, rating;
    float score;            /* from nest.json (TMDB style 0..10) */
    int64_t added;
    int e0, ne;             /* entries [e0, e0+ne) */
    /* derived */
    int64_t last_used;
    int resume;             /* entry index to continue, -1 none */
    int watched, seasons;
    float progress;         /* 0..1 of whole title */
    int present;
} Title;

typedef struct {
    Title *t; int nt;
    Entry *e; int ne;
    int nmovies, nshows, nbooks, nepisodes;
} Library;
extern Library L;

enum { SORT_RECENT, SORT_AZ, SORT_YEAR, SORT_RATING, SORT_PROGRESS };

int lib_load(void);
void lib_derive(void);
Title *lib_title_by_id(int id);
int lib_title_index(int id);
void lib_progress(Entry *e, int pos, int dur, int finished, int seconds);
int lib_next_entry(int eidx);     /* next episode / volume or -1 */
int lib_prev_entry(int eidx);
int lib_query(int kind, int secret, int sort, int filter, int *out, int max); /* filter: 0 all,1 unwatched,2 fav */
int lib_continue(int kind, int secret, int *out, int max);
int lib_recent(int kind, int secret, int *out, int max);
int lib_favorites(int secret, int *out, int max);
int lib_because(int secret, int *seed, int *out, int max);
int lib_search(const char *q, int secret, int *out, int max);
int lib_featured(int secret, int *out, int max);
const char *lib_entry_label(Entry *e, char *buf, int n); /* "S2 · E4" */
void lib_set_favorite(Title *t, int v);
void lib_set_rating(Title *t, int v);
void lib_set_secret(Title *t, int v);
void lib_set_title(Title *t, const char *s);
void lib_set_art(Title *t, int which, const char *path); /* 0 poster 1 backdrop 2 logo */
void lib_mark_watched(Title *t, int watched);
const char *title_poster(Title *t); /* poster or cbz cover source */

/* ---------------- db.c ---------------- */
int db_open(void);
void db_close(void);
int db_exec(const char *sql);
const char *db_get(const char *k, const char *def);
int db_geti(const char *k, int def);
void db_set(const char *k, const char *v);
void db_seti(const char *k, int v);
int db_import(ScanResult *r, const char *folder);
void db_add_folder(const char *path, int secret, int mode);
void db_remove_folder(const char *path);
void db_delete_entry(const char *path);       /* forget one file and its progress */
void db_delete_title(int id);
void db_title_folder(int id, char *out, int n);
/* remove.c: delete from the memory card itself */
int remove_entry_files(int e, const char *library_folder);
int remove_title_files(Title *t, int *failed);

/* metafind.c: "Find info in the folder" */
void metafind_start(int tid);
void metafind_frame(float dt);
int metafind_busy(void);
void detail_refresh(void);
void db_read_meta(const char *path, char **overview, char **genres, float *score, char **title, int *year);
void db_found_save(const char *key, const char *title, int year, const char *overview, const char *genres,
                   float score, const char *poster, const char *backdrop, const char *logo);
void db_found_episode(const char *path, const char *name);
void db_found_apply(void);
int db_folders(char paths[][512], int *secret, int *mode, int max);
void db_save_progress(const char *path, int pos, int dur, int finished);
void db_add_activity(int title_id, int kind, int secret, int seconds);
int db_activity_days(int secret, int *days, int *secs, int max);
void db_title_set_int(int id, const char *field, int v);
void db_title_set_str(int id, const char *field, const char *v);
int db_collections(int secret, int *ids, char names[][64], int max);
int db_collection_items(int cid, int *tids, int max);
int db_collection_create(const char *name, int secret);
void db_collection_toggle(int cid, int tid);
int db_collection_has(int cid, int tid);
void db_collection_delete(int cid);
void db_search_add(const char *q);
int db_search_recent(char out[][64], int max);
void db_clear_history(void);
void db_clear_library(void); /* forget every scanned title and folder; progress and settings stay */
int db_excludes(char out[][512], int max);
void db_add_exclude(const char *path);
void db_remove_exclude(const char *path);
void scan_load_excludes(void);
int db_views(int *ids, char names[][64], int *kinds, int *sorts, int *filters, int max); /* Smart Views */
void db_view_create(const char *name, int kind, int sort, int filter);
void db_view_delete(int id);
void db_stats(int secret, int *video_secs, int *read_secs, int *streak, int *best_streak, int *titles_finished);
int64_t now_s(void);
int today_ymd(void);

/* ---------------- settings ---------------- */
typedef struct {
    int theme, poster_size, ambient, autoplay, sounds, sub_size, private_session, show_lofi;
    int manga_rtl, reader_fit, hero_rotate, focus_mode, onboarded, resume_ask, reduce_motion, clock;
    int bg_level, bg_scene;                 /* scene behind the app: 0 off 1 soft 2 vivid; 0 = theme's scene */
    int read_size, read_spacing, read_margin, read_light, read_anim, read_tap; /* book reading */
    int thumbs;                             /* grab a frame from videos that have no artwork */
    int ambient_sound;                      /* 0 off, 1 rain, 2 vinyl */
    int video_fit;                          /* 0 fit, 1 fill, 2 stretch */
    int glass;                              /* glassy panels (on by default) */
} Settings;
extern Settings S;
void settings_load(void);
void settings_save(void);
float poster_w(void);

/* ---------------- app / navigation (main.c) ---------------- */
enum {
    SCR_ONBOARD, SCR_MAIN, SCR_DETAIL, SCR_PLAYER, SCR_READER, SCR_SEARCH, SCR_SETTINGS, SCR_STATS,
    SCR_PICKER, SCR_PIN, SCR_SCAN, SCR_SPOTLIGHT, SCR_HISTORY, SCR_STORAGE, SCR_MPAGE, SCR_NOWPLAYING, SCR_COUNT
};
void nav_push(int scr);
void nav_pop(void);
void nav_replace(int scr);
int nav_top(void);
void toast(const char *msg);
void sfx(int kind); /* 0 move 1 select 2 back */
extern float g_time;
extern int g_secret_unlocked;
extern int g_enter_cross;

/* screens */
void onboard_enter(void);   void onboard_frame(float dt);
void main_enter(void);      void main_frame(float dt);
void detail_open(int tid);  void detail_frame(float dt);
void player_open(int eidx, int ask_resume); void player_frame(float dt); void player_close(void);
void reader_open(int eidx); void reader_frame(float dt); void reader_close(void);
void search_enter(void);    void search_frame(float dt);
void settings_enter(void);  void settings_frame(float dt);
void stats_enter(void);     void stats_frame(float dt);
void picker_open(int purpose); void picker_frame(float dt);
void pin_open(int purpose); void pin_frame(float dt);
void scan_open(int purpose); void scan_frame(float dt);
void spotlight_open(int tid); void spotlight_frame(float dt);
void history_enter(void);   void history_frame(float dt);
void storage_enter(void);   void storage_frame(float dt);

/* picker purposes */
enum { PICK_ONBOARD, PICK_LIBRARY, PICK_SECRET, PICK_POSTER, PICK_LOGO, PICK_BACKDROP, PICK_EXCLUDE, PICK_MUSIC };
/* pin purposes */
enum { PIN_CREATE, PIN_UNLOCK, PIN_CHANGE };
/* scan purposes */
enum { SCAN_ONBOARD, SCAN_ADD, SCAN_RESCAN };

/* shared widgets (widgets.c) */
void draw_backdrop_scene(float alpha);
void draw_scene_bg(float strength); /* the chosen lofi scene behind a screen */
const char *bg_scene_id(void);
void draw_poster(Title *t, float x, float y, float w, float h, float focus, float alpha);
void draw_wide_card(Title *t, int eidx, float x, float y, float w, float h, float focus, float alpha);
void draw_focus(float x, float y, float w, float h, float r, float f);
void draw_button(float x, float y, float w, float h, int ic, const char *label, int primary, float focus);
void draw_chip(float x, float y, const char *label, int active, float focus, float *out_w);
void draw_progress(float x, float y, float w, float h, float p);
void draw_hint_bar(const char *const *items, int n); /* {"✕", "Select", ...} */
void draw_stars(float x, float y, float s, int v, col_t on, col_t off);
void draw_placeholder(Title *t, float x, float y, float w, float h, float r);
extern int g_poster_caption; /* set while drawing posters that have a caption below */
void draw_spinner(float cx, float cy, float r, col_t c);
void draw_toasts(float dt);

/* modal sheets (modal.c) */
typedef void (*modal_cb)(int choice, void *ud);
void modal_menu(const char *title, const char *const *items, int n, modal_cb cb, void *ud);
void modal_confirm(const char *title, const char *body, const char *yes, const char *no, modal_cb cb, void *ud);
void modal_rate(Title *t);
void modal_rate_ex(Title *t, const char *header);
void modal_collections(Title *t);
void modal_quick(Title *t);
int modal_active(void);
void modal_frame(float dt);
typedef void (*ime_cb)(const char *text, void *ud); /* text is NULL if cancelled */
void ime_request(const char *title, const char *initial, int max, ime_cb cb, void *ud);
void ime_pump(void); /* called by the main loop between frames */

#endif
