/* Builds a fake messy library on disk and checks the scanner groups it right. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "parse.h"
#include "scan.h"

static char ROOT[512];
static int fails = 0;

static void mkdirs(const char *rel) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
    for (char *s = p + strlen(ROOT) + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            MKDIR(p);
            *s = '/';
        }
    MKDIR(p);
}

static void touch(const char *rel, int kb) {
    char p[1024], d[1024];
    snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
    snprintf(d, sizeof(d), "%s", rel);
    char *s = strrchr(d, '/');
    if (s) {
        *s = 0;
        mkdirs(d);
    }
    FILE *f = fopen(p, "wb");
    if (!f) { printf("cannot create %s\n", p); exit(2); }
    static char buf[1024];
    for (int i = 0; i < kb; i++) fwrite(buf, 1, sizeof(buf), f);
    fclose(f);
}

static ScanTitle *find(ScanResult *r, const char *title, int kind) {
    for (int i = 0; i < r->nt; i++)
        if (!strcmp(r->t[i].title, title) && r->t[i].kind == kind) return &r->t[i];
    return NULL;
}

static void expect(int cond, const char *what) {
    if (!cond) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

int main(int argc, char **argv) {
    snprintf(ROOT, sizeof(ROOT), "%s", argc > 1 ? argv[1] : "nest_scan_test");
    MKDIR(ROOT);
    /* show with each season in its own differently-named folder */
    touch("TV/Shameless.S01.1080p.BluRay.x265-GROUP/Shameless.S01E01.1080p.BluRay.x265.mkv", 1);
    touch("TV/Shameless.S01.1080p.BluRay.x265-GROUP/Shameless.S01E02.1080p.BluRay.x265.mkv", 1);
    touch("TV/Shameless S02 [720p WEBRip]/shameless.s02e01.720p.webrip.mp4", 1);
    touch("TV/Shameless S02 [720p WEBRip]/shameless.s02e02.720p.webrip.mp4", 1);
    touch("TV/Shameless S02 [720p WEBRip]/Sample/shameless.s02e01.sample.mkv", 1);
    /* show folder + Season N folders + bare episode files */
    touch("TV/The Bear (2022)/Season 1/01 - System.mp4", 1);
    touch("TV/The Bear (2022)/Season 1/02 - Hands.mp4", 1);
    touch("TV/The Bear (2022)/Season 2/Episode 1.mp4", 1);
    touch("TV/The Bear (2022)/poster.jpg", 1);
    /* anime absolute numbering */
    touch("Anime/Frieren/[SubsPlease] Frieren - 01 (1080p).mkv", 1);
    touch("Anime/Frieren/[SubsPlease] Frieren - 02 (1080p).mkv", 1);
    /* movies: in own folder, loose, and a remake with same name */
    touch("Movies/Inception (2010)/Inception.2010.1080p.BluRay.x264.mp4", 1);
    touch("Movies/Inception (2010)/poster.jpg", 1);
    touch("Movies/Dune.Part.Two.2024.2160p.WEB-DL.DDP5.1.Atmos.mkv", 1);
    touch("Movies/The.Matrix.1999.1080p.mp4", 1);
    touch("Movies/Dune.1984.720p.mp4", 1);
    touch("Movies/Dune.2021.1080p.mp4", 1);
    touch("Movies/Extras/Making Of.mp4", 1);
    /* books */
    touch("Books/Berserk/Berserk v01.cbz", 1);
    touch("Books/Berserk/Berserk v02.cbz", 1);
    touch("Books/The Hobbit.epub", 1);

    /* things that must be ignored */
    touch("TV/Shameless.S01.1080p.BluRay.x265-GROUP/RARBG.txt", 1);
    touch("Books/done.txt", 1);
    touch("Private/Hidden Movie 2020.mp4", 1);
    char ex[1][512];
    snprintf(ex[0], sizeof(ex[0]), "%s/Private", ROOT);
    scan_set_excludes(ex, 1);

    ScanResult r = {0};
    scan_folder(ROOT, 0, 0, &r);
    scan_finish(&r);

    for (int i = 0; i < r.nt; i++) {
        ScanTitle *t = &r.t[i];
        printf("%-6s %-28s y%-4d n=%d%s\n", t->kind == KIND_MOVIE ? "movie" : t->kind == KIND_SERIES ? "show" : "book",
               t->title, t->year, t->ne, t->poster ? " [poster]" : "");
        for (int k = 0; k < t->ne && t->kind != KIND_MOVIE; k++)
            printf("         S%02d E%02d %s\n", t->e[k].season, t->e[k].number, t->e[k].name);
    }

    ScanTitle *t;
    t = find(&r, "Shameless", KIND_SERIES);
    expect(t && t->ne == 4, "Shameless grouped across season folders with 4 episodes (sample skipped)");
    if (t) expect(t->e[0].season == 1 && t->e[3].season == 2 && t->e[3].number == 2, "Shameless order");
    t = find(&r, "The Bear", KIND_SERIES);
    expect(t && t->ne == 3, "The Bear 3 episodes");
    if (t) {
        expect(t->poster != NULL, "The Bear poster from show folder");
        expect(t->e[0].number == 1 && !strcmp(t->e[0].name, "System"), "The Bear ep1 named System");
        expect(t->e[2].season == 2 && t->e[2].number == 1, "The Bear S2E1");
        expect(t->year == 2022, "The Bear year");
    }
    t = find(&r, "Frieren", KIND_SERIES);
    expect(t && t->ne == 2, "Frieren absolute numbering");
    t = find(&r, "Inception", KIND_MOVIE);
    expect(t && t->year == 2010 && t->poster, "Inception movie with poster");
    expect(find(&r, "Dune Part Two", KIND_MOVIE) != NULL, "Dune Part Two");
    expect(find(&r, "The Matrix", KIND_MOVIE) != NULL, "The Matrix");
    int dunes = 0;
    for (int i = 0; i < r.nt; i++)
        if (!strcmp(r.t[i].title, "Dune") && r.t[i].kind == KIND_MOVIE) dunes++;
    expect(dunes == 2, "Dune 1984 and Dune 2021 stay separate");
    expect(find(&r, "Making Of", KIND_MOVIE) == NULL, "Extras folder skipped");
    t = find(&r, "Berserk", KIND_BOOK);
    expect(t && t->ne == 2, "Berserk volumes grouped");
    expect(find(&r, "The Hobbit", KIND_BOOK) != NULL, "The Hobbit book");
    expect(find(&r, "RARBG", KIND_BOOK) == NULL && find(&r, "Done", KIND_BOOK) == NULL, "junk .txt files ignored");
    expect(find(&r, "Hidden Movie", KIND_MOVIE) == NULL, "excluded folder skipped");

    printf("movies %d shows %d episodes %d books %d unplayable %d\n", r.movies, r.shows, r.episodes, r.books,
           r.unplayable);
    printf(fails ? "SCAN TESTS FAILED (%d)\n" : "scan tests passed\n", fails);
    scan_free(&r);
    return fails ? 1 : 0;
}
