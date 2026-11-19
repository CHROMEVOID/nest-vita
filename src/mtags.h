/* Reading song info: ID3, FLAC/Vorbis comments, MP4 atoms, WAV INFO,
   and a careful guess from the file and folder names when tags are missing. */
#ifndef NEST_MTAGS_H
#define NEST_MTAGS_H

typedef struct {
    char title[256], artist[256], aartist[256], album[256], genre[64];
    int track, disc, year, dur; /* dur in ms */
    char art[160];              /* cached cover file, "" if none */
    char *lyrics;               /* when set (with lyrics_cap), lyrics are copied here too */
    int lyrics_cap;
} MTags;

int mtags_is_audio(const char *name);
int mtags_is_playlist(const char *name);
/* read the tags of one file. art_dir: where embedded covers are saved */
void mtags_read(const char *path, long long size, MTags *t, const char *art_dir);
/* fill anything still missing from the path (track number, title, album, artist) */
void mtags_guess(const char *path, MTags *t);
/* tidy a title or artist pulled from a file name ("_" to spaces, junk like [320kbps] removed) */
void mtags_clean(char *s);
unsigned mtags_hash(const char *s);
/* lyrics for a song: a .lrc file next to it, or the lyrics stored inside it. Returns the length (0 = none) */
int mtags_lyrics(const char *path, char *out, int cap);
/* what a video file says about itself (MP4 tags): description, genre, year, show name,
   and its cover picture saved to cover_base + ".jpg"/".png" (cover_out gets the path). Returns 1 if anything was found */
int mtags_video(const char *path, char *desc, int dn, char *genre, int gn, int *year, char *show, int sn,
                const char *cover_base, char *cover_out, int con);
#endif
