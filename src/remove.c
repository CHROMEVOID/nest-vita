/* Deleting from the Vita itself (not just from Nest's memory): an episode,
   a book, or a whole show/movie with its artwork folder. Folders are only
   removed when nothing else playable or readable is left inside them, and
   never the library folder you picked or a device root. */
#include "nest.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

/* ---------- small file-system helpers ---------- */

static int is_root(const char *p) {
    const char *c = strchr(p, ':');
    return !c || c[1] == 0 || (c[1] == '/' && c[2] == 0);
}

static void parent_of(const char *p, char *out, int n) {
    snprintf(out, n, "%s", p);
    char *s = strrchr(out, '/');
    if (s && s > strchr(out, ':') + 1) *s = 0;
    else if (s) s[1] = 0; /* "ux0:/" */
}

/* does dir (recursively) still hold any video or book? */
static int has_media(const char *dir, int depth) {
    if (depth > 12) return 1; /* be safe: assume yes */
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    SceIoDirent de;
    int found = 0;
    while (!found) {
        memset(&de, 0, sizeof(de));
        if (sceIoDread(d, &de) <= 0) break;
        if (!strcmp(de.d_name, ".") || !strcmp(de.d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, de.d_name);
        if (SCE_S_ISDIR(de.d_stat.st_mode)) found = has_media(p, depth + 1);
        else {
            int m = media_type_of(de.d_name);
            found = m == MEDIA_VIDEO || m == MEDIA_BOOK;
        }
    }
    sceIoDclose(d);
    return found;
}

static int delete_tree(const char *dir, int depth) {
    if (depth > 12 || is_root(dir)) return 0;
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    SceIoDirent de;
    for (;;) {
        memset(&de, 0, sizeof(de));
        if (sceIoDread(d, &de) <= 0) break;
        if (!strcmp(de.d_name, ".") || !strcmp(de.d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, de.d_name);
        if (SCE_S_ISDIR(de.d_stat.st_mode)) delete_tree(p, depth + 1);
        else sceIoRemove(p);
    }
    sceIoDclose(d);
    return sceIoRmdir(dir) >= 0;
}

/* remove now-empty folders from dir upwards, stopping at the library folder */
static void prune_up(const char *dir, const char *stop) {
    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", dir);
    for (int i = 0; i < 12; i++) {
        if (is_root(cur) || (stop && *stop && !strcmp(cur, stop))) return;
        if (sceIoRmdir(cur) < 0) return; /* not empty: stop */
        char up[1024];
        parent_of(cur, up, sizeof(up));
        if (!strcmp(up, cur)) return;
        snprintf(cur, sizeof(cur), "%s", up);
    }
}

static void remove_subs(const char *video) {
    char base[1024], p[1100];
    snprintf(base, sizeof(base), "%s", video);
    char *dot = strrchr(base, '.');
    char *sl = strrchr(base, '/');
    if (dot && (!sl || dot > sl)) *dot = 0;
    const char *suffix[] = {".srt", ".en.srt", ".eng.srt", ".English.srt", ".vtt", ".en.vtt", ".ass", ".ssa"};
    for (int i = 0; i < 8; i++) {
        snprintf(p, sizeof(p), "%s%s", base, suffix[i]);
        sceIoRemove(p);
    }
}

static int remove_file(const char *path) {
    int r = sceIoRemove(path);
    if (r < 0 && fs_exists(path)) return 0;
    remove_subs(path);
    return 1;
}

/* shrink common to the deepest folder that also contains d */
static void common_dir(char *common, const char *d) {
    int i = 0;
    while (common[i] && d[i] && common[i] == d[i]) i++;
    if (common[i] == 0 && (d[i] == 0 || d[i] == '/')) return;
    int j = i;
    while (j > 0 && common[j] != '/') j--;
    if (common[j] != '/') { common[0] = 0; return; }
    if (j > 0 && common[j - 1] == ':') common[j + 1] = 0;
    else common[j] = 0;
}

/* ---------- public ---------- */

int remove_entry_files(int e, const char *library_folder) {
    if (e < 0 || e >= L.ne) return 0;
    char path[1024], dir[1024];
    snprintf(path, sizeof(path), "%s", L.e[e].path);
    if (!remove_file(path)) return 0;
    db_delete_entry(path);
    parent_of(path, dir, sizeof(dir));
    prune_up(dir, library_folder);
    return 1;
}

int remove_title_files(Title *t, int *failed) {
    char folder[512] = "";
    db_title_folder(t->id, folder, sizeof(folder));
    /* the folder all of its files share */
    char common[1024] = "";
    for (int k = t->e0; k < t->e0 + t->ne; k++) {
        char d[1024];
        parent_of(L.e[k].path, d, sizeof(d));
        if (!common[0]) snprintf(common, sizeof(common), "%s", d);
        else common_dir(common, d);
    }
    int n = 0, bad = 0;
    char **paths = calloc(t->ne + 1, sizeof(char *));
    int np = 0;
    for (int k = t->e0; k < t->e0 + t->ne; k++)
        if (paths) paths[np++] = strdup(L.e[k].path);
    for (int i = 0; i < np; i++) {
        if (remove_file(paths[i])) {
            db_delete_entry(paths[i]);
            n++;
        } else {
            bad++;
        }
        free(paths[i]);
    }
    free(paths);
    /* its own folder (artwork, nest.json, empty seasons) goes too, but only if
       it sits inside the library folder and has nothing else to watch or read */
    size_t fl = strlen(folder);
    int inside = common[0] && !is_root(common) && fl && strlen(common) > fl && !strncmp(common, folder, fl) &&
                 common[fl] == '/';
    if (!bad && inside && !has_media(common, 0)) {
        char up[1024];
        parent_of(common, up, sizeof(up));
        delete_tree(common, 0);
        prune_up(up, folder);
    } else if (common[0]) {
        prune_up(common, folder);
    }
    if (!bad) db_delete_title(t->id);
    if (failed) *failed = bad;
    return n;
}
