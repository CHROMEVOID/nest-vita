/* Music: library, playback engine and screens. */
#ifndef NEST_MUSIC_H
#define NEST_MUSIC_H
#include <stdint.h>

/* ---- decoders (mdec.c) ---- */
typedef struct Dec Dec;
Dec *dec_open(const char *path);
int dec_rate(Dec *d);
uint64_t dec_frames(Dec *d);
uint64_t dec_pos(Dec *d);
int dec_read(Dec *d, int16_t *out_stereo, int frames);
int dec_seek(Dec *d, uint64_t frame);
void dec_close(Dec *d);

/* ---- library (music.c) ---- */
typedef struct {
    char *path, *title, *artist, *album_artist, *album, *genre, *art;
    int track, disc, year, dur;   /* dur in ms */
    int album_i, artist_i;        /* derived */
    int liked, plays;
    int64_t last_played, added;
} MTrack;

typedef struct {
    char *name, *artist, *art;
    int year, artist_i;
    int *tracks; int nt;          /* in disc/track order */
    int dur;
    int64_t last_played, added;
} MAlbum;

typedef struct {
    char *name, *art;
    int *albums; int nal;         /* newest first */
    int *tracks; int nt;          /* every song: album order */
    int64_t last_played;
} MArtist;

typedef struct {
    int id;
    char *name, *desc, *cover, *source; /* source: m3u path, or NULL for made in Nest */
    int *tracks; int nt;
    int missing;                  /* tracks in the file we couldn't find */
    int dur;
    int64_t last_played, created;
    int liked_list;               /* the built-in Liked Songs list */
} MPlaylist;

typedef struct {
    MTrack *t; int nt;
    MAlbum *a; int na;
    MArtist *r; int nr;
    MPlaylist *p; int np;
    int gen;                      /* goes up every time the library is reloaded */
    int scanning;                 /* a scan is running */
    int scan_found;
    char scan_now[128];
} MusicLib;
extern MusicLib M;

void music_init(void);           /* load the saved library, start the engine */
void music_scan_start(void);     /* walk the folders in the background */
void music_frame(float dt);      /* main loop: finish scans, save stats */
int music_has_library(void);
void music_reload(void);

int music_search(const char *q, int *tracks, int *nt, int *albums, int *na, int *artists, int *nr, int max);
void music_set_liked(int ti, int v);
int music_playlist_create(const char *name);
void music_playlist_add(int pid_i, int ti);
void music_playlist_remove(int pid_i, int pos);
void music_playlist_rename(int pid_i, const char *name);
void music_playlist_delete(int pid_i);
void music_playlist_move(int pid_i, int from, int to);
int music_recents(int *kinds, int *idx, int max); /* 0 album 1 artist 2 playlist 3 track */
void music_note_recent(int kind, int idx);

/* ---- engine ---- */
enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };
enum { FROM_NONE, FROM_ALBUM, FROM_ARTIST, FROM_PLAYLIST, FROM_SONGS, FROM_SEARCH, FROM_LIKED };
void mus_play(const int *tracks, int n, int start, int from_kind, int from_idx, const char *from_label);
void mus_play_shuffled(const int *tracks, int n, int from_kind, int from_idx, const char *from_label);
void mus_toggle(void);
void mus_pause(void);
void mus_resume(void);
void mus_next(void);
void mus_prev(void);
void mus_seek(int ms);
void mus_stop(void);
void mus_set_shuffle(int on);
void mus_cycle_repeat(void);
void mus_queue_add(int ti, int next);
void mus_queue_remove(int qi);
void mus_queue_jump(int qi);
void mus_queue_move(int from, int to);
void mus_set_volume(int pct);     /* 0..200 */
int mus_volume(void);
int mus_active(void);            /* something is loaded */
int mus_playing(void);
int mus_current(void);           /* track index or -1 */
int mus_pos_ms(void);
int mus_dur_ms(void);
int mus_shuffle(void);
int mus_repeat(void);
int mus_queue(int *out, int max, int *cur);  /* the play order, returns length */
const char *mus_from_label(void);
int mus_from_kind(void);
int mus_from_idx(void);
float mus_level(void);           /* 0..1 loudness for the little visualiser */
void mus_sleep_timer(int minutes); /* 0 cancels, -1 end of song */
int mus_sleep_left(void);        /* seconds, 0 none, -1 end of song */
void mus_screen_off(void);
int mus_screen_is_off(void);
void mus_screen_wake(void);

/* ---- screens (music_ui.c) ---- */
void music_tab_frame(float dt, int focused, int *want_nav); /* the Music tab inside the main screen */
void music_tab_enter(void);
void mpage_frame(float dt);      /* album / artist / playlist pages */
void mpage_open(int kind, int idx); /* kind FROM_ALBUM / FROM_ARTIST / FROM_PLAYLIST / FROM_LIKED */
void nowplaying_open(void);
void nowplaying_frame(float dt);
void miniplayer_draw(float dt, int top_screen); /* small bar over the menus */
int miniplayer_height(void);
void music_track_menu(int ti, int pl_i, int pl_pos);
void art_draw(const char *art, const char *seed, float x, float y, float w, float h, float r, float a);
#endif
