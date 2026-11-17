/* Animated lofi scenes. Frames are 8-bit palette images, zlib compressed,
   inflated straight into a palette texture (pixel art stays crisp). */
#include "nest.h"

#include <zlib.h>

const char *LOFI_SCENES[] = {"window", "cafe", "city", "plants", "lake", "alley", "rooftop", "study",
                             "ramen", "dreamer", "forest", "garden", "castle", "koi", "rainy", "ruins"};
const int LOFI_SCENE_COUNT = sizeof(LOFI_SCENES) / sizeof(LOFI_SCENES[0]);

typedef struct {
    uint16_t delay;
    uint32_t size;
    const uint8_t *data;
} LFrame;

struct Lofi {
    char id[24];
    int w, h, n, pixel;
    uint8_t *file;
    LFrame *fr;
    vita2d_texture *tex[2];
    int shown, cur, decoded;
    float acc;
    float last_draw;
    int ok;
};

#define MAXL 4
static Lofi *cache[MAXL];

typedef struct {
    char id[24];
    vita2d_texture *tex;
    float last;
} Soft;
#define NSOFT 20
static Soft soft[NSOFT];

static void lofi_free(Lofi *l) {
    if (!l) return;
    vita2d_wait_rendering_done();
    for (int i = 0; i < 2; i++)
        if (l->tex[i]) vita2d_free_texture(l->tex[i]);
    free(l->fr);
    free(l->file);
    free(l);
}

static void decode(Lofi *l, int idx, vita2d_texture *t) {
    LFrame *f = &l->fr[idx];
    uLongf out = (uLongf)l->w * l->h;
    static uint8_t *tmp;
    static size_t tmp_cap;
    if (tmp_cap < out) {
        free(tmp);
        tmp = malloc(out);
        tmp_cap = tmp ? out : 0;
    }
    if (!tmp) return;
    if (uncompress(tmp, &out, f->data, f->size) != Z_OK) return;
    uint8_t *dst = vita2d_texture_get_datap(t);
    unsigned stride = vita2d_texture_get_stride(t);
    for (int y = 0; y < l->h; y++) memcpy(dst + y * stride, tmp + y * l->w, l->w);
}

static Lofi *lofi_load(const char *id) {
    char path[128];
    snprintf(path, sizeof(path), ASSET("lofi/%s.nla"), id);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    Lofi *l = calloc(1, sizeof(Lofi));
    if (!l) { fclose(fp); return NULL; }
    l->file = malloc(n);
    if (!l->file || fread(l->file, 1, n, fp) != (size_t)n || n < 12 + 1024 || memcmp(l->file, "NLA1", 4)) {
        fclose(fp);
        lofi_free(l);
        return NULL;
    }
    fclose(fp);
    snprintf(l->id, sizeof(l->id), "%s", id);
    const uint8_t *p = l->file + 4;
    l->w = p[0] | p[1] << 8;
    l->h = p[2] | p[3] << 8;
    l->n = p[4] | p[5] << 8;
    l->pixel = (p[6] | p[7] << 8) & 1;
    const uint8_t *pal = l->file + 12;
    p = pal + 1024;
    l->fr = calloc(l->n, sizeof(LFrame));
    for (int i = 0; i < l->n; i++) {
        if (p + 6 > l->file + n) { l->n = i; break; }
        l->fr[i].delay = p[0] | p[1] << 8;
        l->fr[i].size = p[2] | p[3] << 8 | p[4] << 16 | (uint32_t)p[5] << 24;
        l->fr[i].data = p + 6;
        p += 6 + l->fr[i].size;
        if (l->fr[i].delay < 20) l->fr[i].delay = 100;
    }
    for (int k = 0; k < 2; k++) {
        l->tex[k] = vita2d_create_empty_texture_format(l->w, l->h, SCE_GXM_TEXTURE_FORMAT_P8_ABGR);
        if (!l->tex[k]) { lofi_free(l); return NULL; }
        memcpy(vita2d_texture_get_palette(l->tex[k]), pal, 1024);
        if (l->pixel)
            vita2d_texture_set_filters(l->tex[k], SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
        else
            vita2d_texture_set_filters(l->tex[k], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    if (l->n > 0) decode(l, 0, l->tex[0]);
    l->shown = 0;
    l->ok = l->n > 0;
    return l;
}

Lofi *lofi_get(const char *scene) {
    if (!scene) return NULL;
    for (int i = 0; i < MAXL; i++)
        if (cache[i] && !strcmp(cache[i]->id, scene)) return cache[i];
    int slot = -1;
    for (int i = 0; i < MAXL; i++)
        if (!cache[i]) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < MAXL; i++)
            if (cache[i]->last_draw < cache[slot]->last_draw) slot = i;
        if (cache[slot]->last_draw >= g_time) return NULL; /* all in use this frame */
        lofi_free(cache[slot]);
        cache[slot] = NULL;
    }
    cache[slot] = lofi_load(scene);
    if (cache[slot]) cache[slot]->last_draw = g_time;
    return cache[slot];
}

static int release_all;
void lofi_release_all(void) { release_all = 1; }

void lofi_tick(float dt) {
    if (release_all) { /* runs at frame start, before anything is drawn */
        release_all = 0;
        for (int i = 0; i < MAXL; i++) {
            lofi_free(cache[i]);
            cache[i] = NULL;
        }
        vita2d_wait_rendering_done();
        for (int i = 0; i < NSOFT; i++)
            if (soft[i].tex) {
                vita2d_free_texture(soft[i].tex);
                soft[i].tex = NULL;
                soft[i].id[0] = 0;
            }
    }
    for (int i = 0; i < MAXL; i++) {
        Lofi *l = cache[i];
        if (!l || !l->ok || l->n < 2) continue;
        if (g_time - l->last_draw > 0.25f) continue; /* not on screen: pause */
        float step = dt * 1000.0f * (S.reduce_motion ? 0.5f : 1.0f);
        l->acc += step;
        int advanced = 0;
        while (l->acc >= l->fr[l->cur].delay && advanced < 3) {
            l->acc -= l->fr[l->cur].delay;
            l->cur = (l->cur + 1) % l->n;
            advanced++;
        }
        if (l->acc > 1000) l->acc = 0;
        if (advanced) {
            int back = 1 - l->shown;
            decode(l, l->cur, l->tex[back]);
            l->shown = back;
        }
    }
}

void lofi_draw(Lofi *l, float x, float y, float w, float h, float r, col_t tint, float ax, float ay) {
    if (!l || !l->ok) return;
    l->last_draw = g_time;
    gfx_tex(l->tex[l->shown], x, y, w, h, r, tint, ax, ay);
}

vita2d_texture *lofi_soft(const char *scene) {
    if (!scene) return NULL;
    int oldest = 0;
    for (int i = 0; i < NSOFT; i++) {
        if (soft[i].tex && !strcmp(soft[i].id, scene)) {
            soft[i].last = g_time;
            return soft[i].tex;
        }
        if (soft[i].last < soft[oldest].last) oldest = i;
    }
    /* never free a texture already drawn this frame */
    if (soft[oldest].tex && soft[oldest].last >= g_time) return NULL;
    char path[128];
    snprintf(path, sizeof(path), ASSET("lofi/%s_soft.jpg"), scene);
    vita2d_texture *t = vita2d_load_JPEG_file(path);
    if (!t) return NULL;
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    if (soft[oldest].tex) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(soft[oldest].tex);
    }
    snprintf(soft[oldest].id, sizeof(soft[oldest].id), "%s", scene);
    soft[oldest].tex = t;
    soft[oldest].last = g_time;
    return t;
}

void lofi_trim(void) {
    for (int i = 0; i < MAXL; i++)
        if (cache[i] && g_time - cache[i]->last_draw > 8.0f) {
            lofi_free(cache[i]);
            cache[i] = NULL;
        }
}
