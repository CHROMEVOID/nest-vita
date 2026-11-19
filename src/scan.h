#ifndef NEST_SCAN_H
#define NEST_SCAN_H

enum { KIND_MOVIE = 0, KIND_SERIES = 1, KIND_BOOK = 2 };

typedef struct {
    char *path;
    char *name;       /* episode / volume name, may be "" */
    long long size;
    int season, number, number_end;
    int media;        /* MEDIA_VIDEO / MEDIA_BOOK */
    int playable;
    int need_number;
} ScanEntry;

typedef struct {
    char key[320];
    int kind;
    int secret;
    char *title;
    int year;
    char *artdir;     /* folder that owns the artwork */
    char *poster, *backdrop, *logo, *meta; /* local files, may be NULL */
    char *emb_overview, *emb_genres;        /* found inside the video file itself */
    int emb_year;
    ScanEntry *e;
    int ne, cap;
} ScanTitle;

typedef struct {
    ScanTitle *t;
    int nt, cap;
    int movies, shows, episodes, books, unplayable, skipped;
    volatile int files_seen;
    volatile int cancel;
    char current[256];
} ScanResult;

/* Fill r with everything under root. mode_folder=1 sorts strictly by folder
   (used for Secret folders when the user picks "Sort by folder"). */
void scan_folder(const char *root, int secret, int mode_folder, ScanResult *r);
void scan_finish(ScanResult *r); /* sort, number, count */
void scan_set_excludes(char list[][512], int n); /* folders never scanned */
void scan_free(ScanResult *r);

/* directory listing used by the scanner and the folder picker */
typedef void (*fs_cb)(void *ud, const char *name, int is_dir, long long size);
int fs_list(const char *dir, fs_cb cb, void *ud);
void path_join(const char *dir, const char *name, char *out, int n);
int fs_exists(const char *path);

#endif
