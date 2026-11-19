/* Folder / image picker that browses the Vita's storage devices. */
#include "nest.h"
#include "music.h"
#include "mtags.h"

#include <psp2/io/dirent.h>

extern int g_pick_tid;
char g_scan_path[512];
int g_scan_secret, g_scan_mode;

typedef struct {
    char name[256];
    int dir;
    int media;
} PEnt;

static int purpose;
static char path[512];       /* "" = device list */
static PEnt *ents;
static int nents, cap;
static int focus, nvid, nbook, nsong;
static float psy, psy_t, pfsel;

static const char *DEVICES[] = {"ux0:", "uma0:", "imc0:", "xmc0:", "grw0:", "ur0:"};
static const char *DEVICE_NAMES[] = {"Memory card", "USB / SD2Vita", "Internal storage", "External card", "Game card", "System (ur0)"};

static int image_mode(void) { return purpose == PICK_POSTER || purpose == PICK_LOGO || purpose == PICK_BACKDROP; }

static void cb(void *ud, const char *name, int is_dir, long long size) {
    (void)ud;
    (void)size;
    if (name[0] == '.') return;
    int m = is_dir ? 0 : media_type_of(name);
    if (!is_dir) {
        if (m == MEDIA_VIDEO) nvid++;
        if (m == MEDIA_BOOK) nbook++;
        if (mtags_is_audio(name)) nsong++;
        if (!image_mode() || m != MEDIA_IMAGE) return;
    }
    if (nents == cap) {
        cap = cap ? cap * 2 : 128;
        PEnt *n = realloc(ents, sizeof(PEnt) * cap);
        if (!n) return;
        ents = n;
    }
    snprintf(ents[nents].name, sizeof(ents[nents].name), "%s", name);
    ents[nents].dir = is_dir;
    ents[nents].media = m;
    nents++;
}

static int pcmp(const void *a, const void *b) {
    const PEnt *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void list(void) {
    nents = nvid = nbook = nsong = 0;
    if (!path[0]) {
        for (int i = 0; i < 6; i++) {
            char p[16];
            snprintf(p, sizeof(p), "%s", DEVICES[i]);
            if (nents == cap) {
                cap = cap ? cap * 2 : 128;
                PEnt *n = realloc(ents, sizeof(PEnt) * cap);
                if (!n) return;
                ents = n;
            }
            /* only show devices that can be opened */
            int ok = 0;
            {
                SceUID d = sceIoDopen(p);
                if (d >= 0) { ok = 1; sceIoDclose(d); }
            }
            if (!ok) continue;
            snprintf(ents[nents].name, sizeof(ents[nents].name), "%s", DEVICES[i]);
            ents[nents].dir = 1;
            ents[nents].media = i; /* device index */
            nents++;
        }
    } else {
        fs_list(path, cb, NULL);
        qsort(ents, nents, sizeof(PEnt), pcmp);
    }
    focus = 0;
    psy = psy_t = 0;
    pfsel = 0;
}

void picker_open(int p) {
    purpose = p;
    const char *last = db_get(p == PICK_MUSIC ? "last_music_pick" : "last_pick", NULL);
    if (last && !image_mode() && fs_exists(last)) snprintf(path, sizeof(path), "%s", last);
    else path[0] = 0;
    list();
    if (nav_top() == SCR_SCAN) nav_replace(SCR_PICKER);
    else nav_push(SCR_PICKER);
}

static void go_up(void) {
    if (!path[0]) { nav_pop(); return; }
    size_t n = strlen(path);
    if (n > 1 && path[n - 1] == '/') path[n - 1] = 0;
    char *colon = strchr(path, ':');
    char *s = strrchr(path, '/');
    if (!colon || !s) path[0] = 0;          /* "ux0:" -> device list */
    else if (s == colon + 1) s[0] = 0;      /* "ux0:/Movies" -> "ux0:" */
    else *s = 0;                            /* deeper -> parent */
    list();
    sfx(2);
}

static void enter(int i) {
    if (i < 0 || i >= nents) return;
    PEnt *e = &ents[i];
    if (!path[0]) {
        snprintf(path, sizeof(path), "%s", e->name);
        list();
        sfx(1);
        return;
    }
    char np[512];
    path_join(path, e->name, np, sizeof(np));
    if (e->dir) {
        snprintf(path, sizeof(path), "%s", np);
        list();
        sfx(1);
    } else if (image_mode()) {
        Title *t = lib_title_by_id(g_pick_tid);
        if (t) {
            int which = purpose == PICK_POSTER ? 0 : purpose == PICK_BACKDROP ? 1 : 2;
            lib_set_art(t, which, np);
            toast(which == 0 ? "Poster updated" : which == 1 ? "Backdrop updated" : "Logo updated");
        }
        nav_pop();
    }
}

static void secret_mode_cb(int choice, void *ud) {
    (void)ud;
    if (choice < 0) return;
    g_scan_secret = 1;
    g_scan_mode = choice == 1;
    scan_open(SCAN_ADD);
}

static void lib_mode_cb(int choice, void *ud) {
    (void)ud;
    if (choice < 0) return;
    g_scan_secret = 0;
    g_scan_mode = choice == 1;
    scan_open(purpose == PICK_ONBOARD ? SCAN_ONBOARD : SCAN_ADD);
}

static char target[512];

/* the folder that "Use" applies to: the highlighted one when there is one,
   otherwise the folder you are inside */
static const char *target_path(int current) {
    if (!path[0]) {
        if (nents && focus < nents) snprintf(target, sizeof(target), "%s", ents[focus].name);
        else target[0] = 0;
    } else if (!current && nents && focus < nents && ents[focus].dir) {
        path_join(path, ents[focus].name, target, sizeof(target));
    } else {
        snprintf(target, sizeof(target), "%s", path);
    }
    return target;
}

static int is_device_root(const char *p) {
    size_t n = strlen(p);
    return n && (p[n - 1] == ':' || (n >= 2 && p[n - 1] == '/' && p[n - 2] == ':'));
}

static void really_choose(void);
static void whole_cb(int choice, void *ud) {
    (void)ud;
    if (choice == 1) really_choose();
}

static void choose_target(const char *t) {
    if (!t || !t[0] || image_mode()) return;
    snprintf(g_scan_path, sizeof(g_scan_path), "%s", t);
    if (purpose == PICK_MUSIC) {
        db_set("last_music_pick", t);
        music_add_root(t);
        sfx(1);
        nav_pop();
        return;
    }
    if (purpose == PICK_EXCLUDE) {
        db_add_exclude(t);
        toast("Nest will skip that folder from now on");
        sfx(1);
        nav_pop();
        return;
    }
    if (is_device_root(t)) {
        modal_confirm("Scan this whole card?",
                      "Nest will look through everything on it, skipping system, game and app folders and anything you've excluded in Settings.",
                      "Scan it all", "Cancel", whole_cb, NULL);
        return;
    }
    really_choose();
}

static void choose(void) { choose_target(target_path(0)); }

static void really_choose(void) {
    db_set("last_pick", g_scan_path);
    sfx(1);
    if (purpose == PICK_SECRET) {
        static const char *items[] = {"Sort by guess \xe2\x80\x94 films, shows and books", "Sort by folder \xe2\x80\x94 keep it exactly as it is"};
        modal_menu("How should Nest sort it?", items, 2, secret_mode_cb, NULL);
        return;
    }
    static const char *items[] = {"Sort by guess \xe2\x80\x94 Nest works out films, shows and books",
                                  "Sort by folder \xe2\x80\x94 a shelf for each folder, videos inside it"};
    modal_menu("How should Nest sort it?", items, 2, lib_mode_cb, NULL);
}

void picker_frame(float dt) {
    if (in.pressed & B_BACK) go_up();
    if (in.repeat & B_UP && focus > 0) focus--, sfx(0);
    if (in.repeat & B_DOWN && focus < nents - 1) focus++, sfx(0);
    if (in.repeat & B_LEFT) go_up();
    if (in.pressed & B_OK) enter(focus);
    if (in.repeat & B_RIGHT && nents && ents[focus].dir) enter(focus);
    if (in.pressed & B_TRI) {
        if (!path[0] && !image_mode() && nents) choose_target(target_path(0)); /* whole device */
        else choose();
    }

    pfsel = approach(pfsel, (float)focus, 20, dt);
    draw_scene_bg(0.8f);
    /* header */
    icon(IC_CHEVRON_L, 40, 40, 18, T->text2);
    if (hit(16, 14, 50, 50)) go_up();
    const char *title = image_mode() ? "Choose an image"
                        : purpose == PICK_SECRET ? "Choose a secret folder"
                        : purpose == PICK_EXCLUDE ? "Choose a folder to skip"
                        : purpose == PICK_MUSIC ? "Choose your music folder"
                                                  : "Choose a folder";
    text(F_DISPLAY, T->display_font == 2 ? 20 : 25, 62, 47, T->text, title);
    text_ellipsis(F_META, 13, 62, 70, 560, T->text3, path[0] ? path : "Storage");
    if (!image_mode() && path[0]) {
        /* primary: the highlighted folder. secondary: the folder you're in */
        char lab[96], nm[40];
        const char *tp = target_path(0);
        snprintf(nm, sizeof(nm), "%s", path_basename(tp));
        if (strlen(nm) > 22) strcpy(nm + 19, "...");
        snprintf(lab, sizeof(lab), "%s \xe2\x80\x9c%s\xe2\x80\x9d", purpose == PICK_EXCLUDE ? "Exclude" : "Use", nm);
        float bw = fminf(330, text_w(F_SEMI, 15, lab) + 60);
        draw_button(SCR_W - 40 - bw, 22, bw, 44, purpose == PICK_EXCLUDE ? IC_EYE_OFF : IC_CHECK, lab, 1, 0);
        if (hit(SCR_W - 40 - bw, 22, bw, 44)) choose();
        if (nents && ents[focus].dir) {
            const char *cur_lab = purpose == PICK_EXCLUDE ? "or exclude the folder you're in" : "or use the folder you're in";
            float cw = text_w(F_SEMI, 12, cur_lab);
            text(F_SEMI, 12, SCR_W - 40 - cw, 84, T->accent, cur_lab);
            if (hit(SCR_W - 40 - cw - 10, 70, cw + 20, 26) || (in.pressed & B_START)) choose_target(target_path(1));
        }
        char found[64];
        if (purpose == PICK_MUSIC) {
            snprintf(found, sizeof(found), nsong ? "%d song%s directly in here" : "Folders inside are searched too", nsong, nsong == 1 ? "" : "s");
            if (!(nents && ents[focus].dir) || nsong) text_right(F_REG, 12, SCR_W - 40, 84, nsong ? T->accent : T->text3, found);
        } else if ((nvid || nbook) && !(nents && ents[focus].dir)) {
            snprintf(found, sizeof(found), "%d video%s, %d book%s directly in here", nvid, nvid == 1 ? "" : "s", nbook, nbook == 1 ? "" : "s");
            text_right(F_REG, 12, SCR_W - 40, 84, T->text3, found);
        }
    }
    /* list */
    float top = 100, rh = 52;
    float fy = focus * rh;
    if ((in.repeat & (B_UP | B_DOWN | B_LEFT | B_RIGHT))) {
        if (fy - psy_t > SCR_H - top - rh * 2) psy_t = fy - (SCR_H - top - rh * 2);
        if (fy - psy_t < rh) psy_t = fmaxf(0, fy - rh);
    }
    static KScroll pks;
    kscroll(&psy, &psy_t, &pks, nents * rh - (SCR_H - top - 40), in.dragging, in.ddy, in.vy, dt);
    gfx_clip(0, top, SCR_W, SCR_H - top);
    for (int i = 0; i < nents; i++) {
        float y = top + i * rh - psy;
        if (y > SCR_H || y + rh < top) continue;
        float f = clampf(1 - fabsf(pfsel - i), 0, 1);
        if (f > 0) gfx_rrect(28, y + 2, SCR_W - 56, rh - 4, T->radius_sm, col_a(T->text, 0.08f * f));
        if (i == focus && T->focus_style == 1) gfx_rrect_outline(28, y + 2, SCR_W - 56, rh - 4, T->radius_sm, 1.5f, col_a(T->focus, 0.7f));
        PEnt *e = &ents[i];
        if (!path[0]) {
            icon(IC_FOLDER, 60, y + rh / 2, 20, T->accent);
            text(F_SEMI, 15, 86, y + 24, T->text, DEVICE_NAMES[e->media]);
            text(F_META, 12, 86, y + 42, T->text3, e->name);
        } else {
            icon(e->dir ? IC_FOLDER : IC_IMAGE, 60, y + rh / 2, 19, e->dir ? T->text2 : T->accent);
            text_ellipsis(F_SEMI, 15, 86, y + 31, SCR_W - 180, T->text, e->name);
            if (e->dir) icon(IC_CHEVRON_R, SCR_W - 60, y + rh / 2, 13, T->text3);
        }
        if (hit(28, y, SCR_W - 56, rh) && !in.dragging) {
            focus = i;
            enter(i);
            break;
        }
    }
    gfx_noclip();
    if (!nents) text_center(F_REG, 14, SCR_W / 2, 260, T->text3, image_mode() ? "No folders or images here" : "This folder is empty");
    static const char *h1[] = {"OK", "Open", "TRI", "Use highlighted", "START", "Use this one", "BACK", "Up"};
    static const char *h2[] = {"OK", "Open", "BACK", "Up"};
    gfx_grad_v(0, SCR_H - 44, SCR_W, 44, col_a(T->bg, 0), col_a(T->bg, 0.95f));
    if (!image_mode() && path[0]) draw_hint_bar(h1, 8);
    else draw_hint_bar(h2, 4);
}
