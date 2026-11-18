/* Reading song info: ID3, FLAC/Vorbis comments, MP4 atoms, WAV INFO,
   and a careful guess from the file and folder names when tags are missing. */
#ifndef NEST_MTAGS_H
#define NEST_MTAGS_H

typedef struct {
    char title[256], artist[256], aartist[256], album[256], genre[64];
    int track, disc, year, dur; /* dur in ms */
    char art[160];              /* cached cover file, "" if none */
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
#endif
