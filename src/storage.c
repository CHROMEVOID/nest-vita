/* Storage: what's taking the space on your memory card, biggest first,
   with one-tap clean-ups (delete what you've already watched). */
#include "nest.h"

#include <psp2/io/devctl.h>

typedef struct {
    int tidx;
    long long bytes, done_bytes;
    int done_n;
} SItem;

static SItem items[512];
static int nitems, sel;
static float fsel, sy, sy_t;
static long long card_total, card_free, lib_bytes;
static int pending_tid = -1;

typedef struct {
    uint64_t max_size;
    uint64_t free_size;
    uint32_t cluster_size;
    void *unk;
} DevInfo;

static void card_space(void) {
    DevInfo info;
    memset(&info, 0, sizeof(info));
    card_total = card_free = 0;
    if (sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info)) >= 0) {
        card_total = (long long)info.max_size;
        card_free = (long long)info.free_size;
    }
}

static void build(void) {
    nitems = 0;
    lib_bytes = 0;
    for (int i = 0; i < L.nt && nitems < 512; i++) {
        Title *t = &L.t[i];
        if (t->secret && !g_secret_unlocked) continue;
        if (t->ne <= 0 || net_is_url(L.e[t->e0].path)) continue; /* PC titles take no card space */
        SItem *s = &items[nitems];
        memset(s, 0, sizeof(*s));
        s->tidx = i;
        for (int k = t->e0; k < t->e0 + t->ne; k++) {
            s->bytes += L.e[k].size;
            if (L.e[k].finished) {
                s->done_bytes += L.e[k].size;
                s->done_n++;
            }
        }
        lib_bytes += s->bytes;
        nitems++;
    }
    for (int a = 0; a < nitems; a++)
        for (int b = a + 1; b < nitems; b++)
            if (items[b].bytes > items[a].bytes) {
                SItem tmp = items[a];
                items[a] = items[b];
                items[b] = tmp;
            }
    if (sel >= nitems) sel = nitems ? nitems - 1 : 0;
    card_space();
}

void storage_enter(void) {
    sel = 0;
    fsel = sy = sy_t = 0;
    build();
    nav_push(SCR_STORAGE);
}

static void gb(long long b, char *o, int n) {
    if (b >= 1024LL * 1024 * 1024) snprintf(o, n, "%.1f GB", b / 1073741824.0);
    else snprintf(o, n, "%d MB", (int)(b / 1048576));
}

static void del_done_cb(int choice, void *ud) {
    (void)ud;
    Title *t = lib_title_by_id(pending_tid);
    if (choice != 1 || !t) return;
    char folder[512];
    db_title_folder(t->id, folder, sizeof(folder));
    /* collect paths first: the library reloads afterwards */
    int n = 0, bad = 0;
    char **paths = calloc(t->ne + 1, sizeof(char *));
    int np = 0;
    for (int k = t->e0; k < t->e0 + t->ne; k++)
        if (L.e[k].finished && paths) paths[np++] = strdup(L.e[k].path);
    for (int i = 0; i < np; i++) {
        int e = -1;
        for (int k = 0; k < L.ne; k++)
            if (!strcmp(L.e[k].path, paths[i])) { e = k; break; }
        if (e >= 0 && remove_entry_files(e, folder)) n++;
        else bad++;
        free(paths[i]);
    }
    free(paths);
    lib_load();
    build();
    char m[96];
    snprintf(m, sizeof(m), bad ? "Deleted %d watched files; %d couldn't be removed" : "Deleted %d watched files", n, bad);
    toast(m);
}

static void del_all_cb(int choice, void *ud) {
    (void)ud;
    Title *t = lib_title_by_id(pending_tid);
    if (choice != 1 || !t) return;
    int failed = 0;
    remove_title_files(t, &failed);
    lib_load();
    build();
    toast(failed ? "Some files couldn't be removed" : "Deleted from your Vita");
}

static void menu_cb(int choice, void *ud) {
    (void)ud;
    Title *t = lib_title_by_id(pending_tid);
    if (!t || choice < 0) return;
    SItem *s = NULL;
    for (int i = 0; i < nitems; i++)
        if (L.t[items[i].tidx].id == t->id) s = &items[i];
    if (!s) return;
    char body[300], sz[32];
    if (choice == 0) {
        detail_open(t->id);
    } else if (choice == 1 && s->done_n) {
        gb(s->done_bytes, sz, sizeof(sz));
        snprintf(body, sizeof(body),
                 "Permanently deletes the %d file%s you've finished (%s) from the memory card. Unwatched ones stay.",
                 s->done_n, s->done_n == 1 ? "" : "s", sz);
        modal_confirm("Delete what you've watched?", body, "Delete", "Keep", del_done_cb, NULL);
    } else if (choice == (s->done_n ? 2 : 1)) {
        gb(s->bytes, sz, sizeof(sz));
        snprintf(body, sizeof(body), "Permanently deletes all %d file%s (%s) and the artwork. It can't be undone.",
                 t->ne, t->ne == 1 ? "" : "s", sz);
        modal_confirm("Delete from Vita?", body, "Delete", "Keep", del_all_cb, NULL);
    }
}

static void open_menu(int i) {
    SItem *s = &items[i];
    Title *t = &L.t[s->tidx];
    pending_tid = t->id;
    static char done_label[64];
    const char *opts[3];
    int n = 0;
    opts[n++] = "Open";
    if (s->done_n) {
        char sz[32];
        gb(s->done_bytes, sz, sizeof(sz));
        snprintf(done_label, sizeof(done_label), "Delete watched (%d, %s)", s->done_n, sz);
        opts[n++] = done_label;
    }
    opts[n++] = "Delete from Vita\xe2\x80\xa6";
    modal_menu(t->title, opts, n, menu_cb, NULL);
}

void storage_frame(float dt) {
    if (in.pressed & B_BACK) { sfx(2); nav_pop(); return; }
    if (nitems) {
        if (in.repeat & B_UP && sel > 0) { sel--; sfx(0); }
        if (in.repeat & B_DOWN && sel < nitems - 1) { sel++; sfx(0); }
        if (in.pressed & B_OK) { sfx(1); open_menu(sel); }
    }
    fsel = approach(fsel, (float)sel, 16, dt);
    draw_scene_bg(1.0f);
    icon(IC_CHEVRON_L, 40, 40, 18, T->text2);
    if (hit(16, 14, 50, 50)) { nav_pop(); return; }
    text(F_DISPLAY, T->display_font == 2 ? 22 : 28, 62, 48, T->text, "Storage");

    /* card bar: your library vs everything else vs free */
    float bx = 40, by = 76, bw = SCR_W - 80;
    char a[32], b[32], c[64];
    if (card_total > 0) {
        long long used = card_total - card_free;
        long long other = used - lib_bytes > 0 ? used - lib_bytes : 0;
        gfx_rrect(bx, by, bw, 14, 7, col_a(T->text, 0.08f));
        float wl = bw * (float)lib_bytes / card_total, wo = bw * (float)other / card_total;
        gfx_rrect(bx, by, fmaxf(14, wl + wo), 14, 7, col_a(T->text, 0.28f));
        gfx_rrect(bx, by, fmaxf(14, wl), 14, 7, T->accent);
        gb(lib_bytes, a, sizeof(a));
        gb(other, b, sizeof(b));
        gb(card_free, c, sizeof(c));
        char line[200];
        snprintf(line, sizeof(line), "Nest library %s   \xc2\xb7   games, apps & other %s   \xc2\xb7   free %s", a, b, c);
        text(F_META, 12, bx, by + 34, T->text2, line);
    }
    if (!nitems) {
        text(F_REG, 14, bx, 150, T->text3, "Nothing on the memory card yet.");
        return;
    }
    float rh = 58, ly = 128;
    long long maxb = items[0].bytes > 0 ? items[0].bytes : 1;
    if ((in.repeat & (B_UP | B_DOWN | B_LEFT | B_RIGHT))) sy_t = fmaxf(0, sel * (rh + 6) - 160);
    static KScroll sks;
    kscroll(&sy, &sy_t, &sks, nitems * (rh + 6) - (SCR_H - ly) + 30, in.dragging, in.ddy, in.vy, dt);
    gfx_clip(0, ly - 4, SCR_W, SCR_H - ly + 4);
    for (int i = 0; i < nitems; i++) {
        float y = ly + i * (rh + 6) - sy;
        if (y > SCR_H || y + rh < ly - 10) continue;
        SItem *s = &items[i];
        Title *t = &L.t[s->tidx];
        float f = clampf(1 - fabsf(fsel - i), 0, 1);
        gfx_rrect(bx, y, bw, rh, T->radius_sm, col_a(T->text, 0.045f + 0.05f * f));
        if (i == sel) draw_focus(bx, y, bw, rh, T->radius_sm, f);
        draw_poster(t, bx + 10, y + 7, 30, 44, 0, 1);
        text_ellipsis(F_SEMI, 15, bx + 52, y + 24, 420, T->text, t->title);
        char sub[96], sz[32];
        if (s->done_n) {
            gb(s->done_bytes, sz, sizeof(sz));
            snprintf(sub, sizeof(sub), "%d file%s \xc2\xb7 %d watched (%s)", t->ne, t->ne == 1 ? "" : "s", s->done_n, sz);
        } else {
            snprintf(sub, sizeof(sub), "%d file%s", t->ne, t->ne == 1 ? "" : "s");
        }
        text(F_META, 12, bx + 52, y + 44, T->text3, sub);
        float barx = bx + 500, barw = bw - 500 - 110;
        gfx_rrect(barx, y + rh / 2 - 4, barw, 8, 4, col_a(T->text, 0.08f));
        gfx_rrect(barx, y + rh / 2 - 4, fmaxf(8, barw * (float)s->bytes / maxb), 8, 4, T->accent);
        if (s->done_bytes)
            gfx_rrect(barx, y + rh / 2 - 4, fmaxf(8, barw * (float)s->done_bytes / maxb), 8, 4, T->accent2);
        gb(s->bytes, sz, sizeof(sz));
        text_right(F_SEMI, 14, bx + bw - 16, y + rh / 2 + 5, T->text, sz);
        if (hit(bx, y, bw, rh)) {
            sel = i;
            open_menu(i);
        }
    }
    gfx_noclip();
    draw_hint_bar((const char *const[]){"\xe2\x9c\x95", "Options", "\xe2\x97\x8b", "Back"}, 4);
}
