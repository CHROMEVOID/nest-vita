#ifndef NEST_PARSE_H
#define NEST_PARSE_H
#include <stddef.h>

/* Everything here is plain C with no Vita dependencies so it can be unit
   tested on a PC (see tests/test_parse.c). */

typedef struct {
    char title[256];     /* cleaned show / movie / book title ("" if none) */
    char ep_title[256];  /* cleaned episode name, if any */
    int year;            /* 0 if unknown */
    int season;          /* -1 if unknown */
    int episode;         /* -1 if not an episode */
    int episode_end;     /* last episode for multi-episode files, else = episode */
    int is_episode;      /* 1 when the name itself proves it is an episode */
    int absolute;        /* 1 for anime style "Title - 05" numbering */
    int volume;          /* books: volume / chapter number, -1 if none */
} ParsedName;

enum { MEDIA_NONE = 0, MEDIA_VIDEO, MEDIA_BOOK, MEDIA_IMAGE, MEDIA_SUB, MEDIA_META };

int media_type_of(const char *filename);
int video_is_playable(const char *filename); /* hardware decoder: mp4/m4v */

/* Clean a release name ("Shameless.S02.1080p.BluRay.x265") to "Shameless".
   year receives a detected year (or 0). season receives a season-pack
   number when present (or -1). */
void parse_clean(const char *in, char *out, size_t n, int *year, int *season);

void parse_filename(const char *filename, ParsedName *p);
void parse_book(const char *filename, ParsedName *p);

/* "Season 2", "S02", "Series 2", "Staffel 2", "Specials" -> number, else -1 */
int parse_season_folder(const char *dirname);
int parse_is_extras_folder(const char *dirname);
int parse_is_sample(const char *filename);

/* lowercase alnum-only key used to group seasons of the same show */
void parse_norm_key(const char *title, char *out, size_t n);

void parse_title_case(char *s);
const char *path_basename(const char *path);
void path_dirname(const char *path, char *out, size_t n);

#endif
