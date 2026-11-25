/* Nest port: Android keeps Nest's fonts and scenes inside the APK. On first launch
   (and after each update) they're copied to the app's private folder, where Nest
   reads them like normal files. files.txt (made at build time) lists them, with
   a build stamp on its first line. */
#include "port.h"
#undef fopen
#undef main

#include <SDL.h>
#include <sys/stat.h>

const char *port_asset_root(void);

static void mkdirs_for(const char *file) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", file);
    for (char *c = tmp + 1; *c; c++)
        if (*c == '/') {
            *c = 0;
#ifdef _WIN32
            mkdir(tmp);
#else
            mkdir(tmp, 0777);
#endif
            *c = '/';
        }
}

void port_assets_extract(void) {
#ifdef __ANDROID__
    SDL_RWops *lst = SDL_RWFromFile("files.txt", "rb");
    if (!lst) return;
    Sint64 n = SDL_RWsize(lst);
    char *list = malloc((size_t)n + 1);
    if (!list) { SDL_RWclose(lst); return; }
    SDL_RWread(lst, list, 1, (size_t)n);
    list[n] = 0;
    SDL_RWclose(lst);
    char *nl = strchr(list, '\n');
    if (!nl) { free(list); return; }
    *nl = 0;
    const char *stamp = list; /* first line */
    char stamp_path[1024];
    snprintf(stamp_path, sizeof(stamp_path), "%s/.stamp", port_asset_root());
    FILE *sf = fopen(stamp_path, "rb");
    if (sf) {
        char have[128] = "";
        size_t k = fread(have, 1, sizeof(have) - 1, sf);
        have[k] = 0;
        fclose(sf);
        if (!strcmp(have, stamp)) { free(list); return; } /* already up to date */
    }
    char *line = nl + 1;
    static char buf[256 * 1024];
    while (*line) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[--l] = 0;
        if (l) {
            SDL_RWops *in = SDL_RWFromFile(line, "rb");
            if (in) {
                char out[1024];
                snprintf(out, sizeof(out), "%s/%s", port_asset_root(), line);
                mkdirs_for(out);
                FILE *o = fopen(out, "wb");
                if (o) {
                    size_t r;
                    while ((r = SDL_RWread(in, buf, 1, sizeof(buf))) > 0) fwrite(buf, 1, r, o);
                    fclose(o);
                }
                SDL_RWclose(in);
            }
        }
        if (!end) break;
        line = end + 1;
    }
    mkdirs_for(stamp_path);
    sf = fopen(stamp_path, "wb");
    if (sf) {
        fputs(stamp, sf);
        fclose(sf);
    }
    free(list);
#endif
}
