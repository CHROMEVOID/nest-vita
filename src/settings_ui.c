/* Settings (with a live theme gallery), Library Health and the personal
   Stats dashboard with a watch/read heatmap. */
#include "nest.h"

#include <psp2/io/dirent.h>

extern char g_scan_path[512];
extern int g_scan_secret, g_scan_mode;

enum { CAT_LOOK, CAT_LIBRARY, CAT_PLAYBACK, CAT_READING, CAT_PRIVACY, CAT_ABOUT, CAT_N };
static const char *CAT_NAMES[CAT_N] = {"Appearance", "Library", "Playback", "Reading", "Privacy", "About"};
static const int CAT_ICONS[CAT_N] = {IC_DROP, IC_FOLDER, IC_PLAY, IC_BOOK, IC_LOCK, IC_INFO};

enum { R_THEME, R_CHOICE, R_TOGGLE, R_ACTION, R_INFO, R_FOLDER, R_EXCL, R_VIEW };
typedef struct {
    int type;
    const char *label, *sub;
    int *val;
    const char *opts[20];
    int nopts;
    int action;
    char dyn[600];
} SRow;

static SRow rows[40];
static int nrows;
static int cat, side = 1, ri;
static float rsy, rsy_t, rfsel, cfsel;
static int theme_focus;
static char folders[24][512];
static int fsecret[24], fmode[24], nfolders;
static char excls[40][512];
static int nexcls;
static int view_ids[8], nviews;
static char view_names[8][64];
static const char *SCENE_NAMES[20];

enum { A_ADD, A_MUSIC, A_ADD_SECRET, A_RESCAN, A_HEALTH, A_STATS, A_PIN, A_LOCK, A_CLEAR, A_WHOLE, A_EXCL_ADD, A_CLEARLIB,
       A_HISTORY, A_STORAGE, A_PC,
       A_FOLDER = 100, A_EXCL = 200, A_VIEW = 300 };

static SRow *add(int type, const char *label, const char *sub) {
    SRow *r = &rows[nrows++];
    memset(r, 0, sizeof(*r));
    r->type = type;
    r->label = label;
    r->sub = sub;
    return r;
}

static void build(void) {
    nrows = 0;
    SRow *r;
    switch (cat) {
    case CAT_LOOK:
        add(R_THEME, "Theme", NULL);
        r = add(R_CHOICE, "Poster size", "How many titles fit on a shelf");
        r->val = &S.poster_size; r->opts[0] = "Small"; r->opts[1] = "Medium"; r->opts[2] = "Large"; r->nopts = 3;
        r = add(R_CHOICE, "Scene in the background", "Your theme's animated scene behind the shelves");
        r->val = &S.bg_level; r->opts[0] = "Off"; r->opts[1] = "Soft"; r->opts[2] = "Vivid"; r->nopts = 3;
        r = add(R_CHOICE, "Which scene", "Pick any of the lofi scenes, or follow the theme");
        r->val = &S.bg_scene;
        {
            static const char *pretty[] = {"Theme's own", "Cat window", "Rainy caf\xc3\xa9", "Night train", "Plant room",
                                           "Mountain lake", "Lantern alley", "Rooftop", "Witch's study", "Ramen stall",
                                           "Dreamer", "Forest light", "Garden", "Castle", "Koi pond", "Rainy city", "Ruins"};
            int n = 1 + LOFI_SCENE_COUNT;
            if (n > 17) n = 17;
            for (int k = 0; k < n; k++) r->opts[k] = pretty[k];
            r->nopts = n;
        }
        r = add(R_TOGGLE, "Ambient library", "Bigger posters, more room, slower scrolling and the scene behind it all");
        r->val = &S.ambient;
        r = add(R_TOGGLE, "Lofi touches", "Little animated scenes in quiet places");
        r->val = &S.show_lofi;
        r = add(R_TOGGLE, "Rotate the featured banner", NULL);
        r->val = &S.hero_rotate;
        r = add(R_TOGGLE, "Reduce motion", "Calmer transitions, slower scenes");
        r->val = &S.reduce_motion;
        r = add(R_TOGGLE, "Interface sounds", "Soft ticks when moving around");
        r->val = &S.sounds;
        r = add(R_CHOICE, "Ambient sound", "Quiet background while browsing and reading");
        r->val = &S.ambient_sound; r->opts[0] = "Off"; r->opts[1] = "Rain"; r->opts[2] = "Vinyl"; r->nopts = 3;
        break;
    case CAT_LIBRARY:
        nfolders = db_folders(folders, fsecret, fmode, 24);
        for (int i = 0; i < nfolders; i++) {
            r = add(R_FOLDER, NULL, NULL);
            snprintf(r->dyn, sizeof(r->dyn), "%s", folders[i]);
            r->action = A_FOLDER + i;
        }
        add(R_ACTION, "Add a folder", "Films, shows, books and manga")->action = A_ADD;
        add(R_ACTION, "Add a music folder", "Where your songs are: Nest sorts them into albums and artists")->action = A_MUSIC;
        add(R_ACTION, "Add a secret folder", "Kept behind your PIN")->action = A_ADD_SECRET;
        add(R_ACTION, "Scan the whole Vita", "Every memory card, minus system, game and app folders")->action = A_WHOLE;
        add(R_ACTION, "Watch from your PC (Wi-Fi)", "Run Nest PC Server on your computer, then type the address it shows")->action = A_PC;
        add(R_ACTION, "Rescan everything", "Pick up new and removed files")->action = A_RESCAN;
        nexcls = db_excludes(excls, 40);
        for (int i = 0; i < nexcls; i++) {
            r = add(R_EXCL, NULL, NULL);
            snprintf(r->dyn, sizeof(r->dyn), "%s", excls[i]);
            r->action = A_EXCL + i;
        }
        add(R_ACTION, "Skip a folder\xe2\x80\xa6", "Excluded folders are never scanned")->action = A_EXCL_ADD;
        r = add(R_TOGGLE, "Thumbnails from videos", "When there's no poster, Nest grabs a frame from the video");
        r->val = &S.thumbs;
        {
            int vk[8], vs[8], vf[8];
            nviews = db_views(view_ids, view_names, vk, vs, vf, 8);
            for (int i = 0; i < nviews; i++) {
                r = add(R_VIEW, NULL, NULL);
                snprintf(r->dyn, sizeof(r->dyn), "%s", view_names[i]);
                r->action = A_VIEW + i;
            }
        }
        add(R_ACTION, "Library health", "Files that won't play, missing art, duplicates")->action = A_HEALTH;
        add(R_ACTION, "Your stats", "Hours, streaks and your year in a calendar")->action = A_STATS;
        add(R_ACTION, "History & your year", "What you watched and when, and this year's recap")->action = A_HISTORY;
        add(R_ACTION, "Storage", "What's using your memory card, and quick clean-ups")->action = A_STORAGE;
        add(R_ACTION, "Clear library", "Forget every title and folder, then add folders again")->action = A_CLEARLIB;
        break;
    case CAT_PLAYBACK:
        r = add(R_TOGGLE, "Play the next episode automatically", "With a 10 second countdown you can cancel");
        r->val = &S.autoplay;
        r = add(R_TOGGLE, "Ask before resuming", "Choose between resuming and starting over");
        r->val = &S.resume_ask;
        r = add(R_CHOICE, "Subtitle size", "Uses .srt or .vtt files next to the video");
        r->val = &S.sub_size; r->opts[0] = "Small"; r->opts[1] = "Medium"; r->opts[2] = "Large"; r->nopts = 3;
        r = add(R_CHOICE, "Picture size", "For new shows. SELECT changes it per show while watching");
        r->val = &S.video_fit; r->opts[0] = "Fit"; r->opts[1] = "Fill"; r->opts[2] = "Stretch"; r->nopts = 3;
        r = add(R_TOGGLE, "Show clock and end time", NULL);
        r->val = &S.clock;
        add(R_INFO, "Skip intro & credits", "While watching, press START (or tap ⋯) to mark a show's intro and credits once. Then R skips the intro on every episode.");
        r = add(R_TOGGLE, "Focus mode", "Keep the controls hidden while watching (\xe2\x96\xb3 toggles)");
        r->val = &S.focus_mode;
        break;
    case CAT_READING:
        r = add(R_TOGGLE, "Manga mode", "Read right to left");
        r->val = &S.manga_rtl;
        r = add(R_CHOICE, "Page fit (comics)", "\xe2\x96\xa1 switches while reading");
        r->val = &S.reader_fit; r->opts[0] = "Whole page"; r->opts[1] = "Fit width"; r->opts[2] = "Fit height"; r->nopts = 3;
        r = add(R_TOGGLE, "Page turn animation", NULL);
        r->val = &S.read_anim;
        r = add(R_TOGGLE, "Tap the edges to turn pages", "Left and right sides of the screen");
        r->val = &S.read_tap;
        r = add(R_CHOICE, "Text size (books)", NULL);
        r->val = &S.read_size; r->opts[0] = "Small"; r->opts[1] = "Medium"; r->opts[2] = "Large"; r->opts[3] = "Huge"; r->nopts = 4;
        r = add(R_CHOICE, "Line spacing", NULL);
        r->val = &S.read_spacing; r->opts[0] = "Tight"; r->opts[1] = "Comfortable"; r->opts[2] = "Airy"; r->nopts = 3;
        r = add(R_CHOICE, "Margins", NULL);
        r->val = &S.read_margin; r->opts[0] = "Narrow"; r->opts[1] = "Normal"; r->opts[2] = "Wide"; r->nopts = 3;
        r = add(R_CHOICE, "Reading light", "Page colour for books");
        r->val = &S.read_light; r->opts[0] = "Night"; r->opts[1] = "Sepia"; r->opts[2] = "Paper"; r->opts[3] = "Black"; r->nopts = 4;
        add(R_INFO, "Formats", "CBZ, CBR and EPUB. Plain TXT works too. PDF isn't supported yet.");
        break;
    case CAT_PRIVACY:
        r = add(R_TOGGLE, "Private session", "Nothing you watch or read is remembered until Nest closes");
        r->val = &S.private_session;
        add(R_ACTION, db_get("pin_hash", NULL) ? "Change PIN" : "Set a PIN", "Protects the Secret tab")->action = A_PIN;
        add(R_ACTION, "Lock Secret now", NULL)->action = A_LOCK;
        add(R_ACTION, "Clear watch and reading history", "Progress, activity and searches")->action = A_CLEAR;
        add(R_INFO, "About the PIN", "It keeps Secret out of sight inside Nest. Files stay where they are on your memory card.");
        break;
    case CAT_ABOUT:
        add(R_INFO, "Nest " NEST_VERSION, "A calm home for your films, shows and books.");
        add(R_INFO, "Made by chromevoid", "Follow on TikTok: @chr0m3void  \xc2\xb7  github.com/CHROMEVOID/nest-vita");
        add(R_INFO, "Video", "Plays MP4 with H.264 video and AAC audio (the Vita's hardware decoder). MKV and HEVC/x265 need converting first.");
        add(R_INFO, "Posters and descriptions", "Put poster.jpg, fanart.jpg or .nfo files next to your files, use \xe2\x8b\xaf \xe2\x80\xba Find info in the folder, or run Nest Metadata on a PC.");
        add(R_INFO, "Credits", "Inter, DM Serif Display and Space Mono fonts (OFL). SQLite, libjpeg-turbo, libwebp, zlib, unrar.");
        break;
    }
    if (ri >= nrows) ri = nrows ? nrows - 1 : 0;
}

void settings_enter(void) {
    cat = 0;
    side = 1;
    ri = 0;
    theme_focus = S.theme;
    build();
    nav_push(SCR_SETTINGS);
}

static void folder_cb(int choice, void *ud) {
    int i = (int)(intptr_t)ud;
    if (choice < 0 || i < 0 || i >= nfolders) return;
    if (choice == 0) {
        snprintf(g_scan_path, sizeof(g_scan_path), "%s", folders[i]);
        g_scan_secret = fsecret[i];
        g_scan_mode = fmode[i];
        scan_open(SCAN_ADD);
    } else if (choice == 1) {
        db_remove_folder(folders[i]);
        lib_load();
        build();
        toast("Folder removed from Nest (your files are untouched)");
    }
}

static void excl_cb(int choice, void *ud) {
    int i = (int)(intptr_t)ud;
    if (choice == 0 && i >= 0 && i < nexcls) {
        db_remove_exclude(excls[i]);
        build();
        toast("Nest will scan that folder again");
    }
}

static void view_cb(int choice, void *ud) {
    int i = (int)(intptr_t)ud;
    if (choice == 0 && i >= 0 && i < nviews) {
        db_view_delete(view_ids[i]);
        build();
        main_enter();
        toast("Shelf removed");
    }
}

static void clearlib_cb(int choice, void *ud) {
    (void)ud;
    if (choice == 1) {
        db_clear_library();
        lib_load();
        main_enter();
        build();
        toast("Library cleared \xe2\x80\x94 add a folder to start again");
    }
}

static void whole_vita(void) {
    static const char *devs[] = {"ux0:", "uma0:", "imc0:", "xmc0:", "grw0:"};
    int added = 0;
    for (int i = 0; i < 5; i++) {
        SceUID d = sceIoDopen(devs[i]);
        if (d < 0) continue;
        sceIoDclose(d);
        db_add_folder(devs[i], 0, 0);
        added++;
    }
    if (!added) { toast("No memory cards found"); return; }
    g_scan_path[0] = 0;
    scan_open(SCAN_RESCAN);
}

static void whole_cb(int choice, void *ud) {
    (void)ud;
    if (choice == 1) whole_vita();
}

static void clear_cb(int choice, void *ud) {
    (void)ud;
    if (choice == 1) {
        db_clear_history();
        lib_load();
        toast("History cleared");
    }
}

static int pc_count;
static void pc_count_cb(void *ud, const char *name, int is_dir, long long size) {
    (void)ud; (void)name; (void)is_dir; (void)size;
    pc_count++;
}

/* "192.168.1.20", "192.168.1.20:8642" or "http://192.168.1.20:8642/" all work */
static void pc_cb(const char *text, void *ud) {
    (void)ud;
    if (!text || !*text) return;
    char addr[128];
    const char *s = text;
    while (*s == ' ') s++;
    if (!strncmp(s, "http://", 7)) s += 7;
    snprintf(addr, sizeof(addr), "%s", s);
    char *slash = strchr(addr, '/');
    if (slash) *slash = 0;
    for (char *p = addr + strlen(addr); p > addr && p[-1] == ' '; ) *--p = 0;
    if (!strchr(addr, ':')) strncat(addr, ":8642", sizeof(addr) - strlen(addr) - 1);
    if (!net_wifi_connected()) {
        toast("Connect your Vita to Wi-Fi first");
        return;
    }
    char root[200];
    snprintf(root, sizeof(root), "http://%s/f", addr);
    pc_count = 0;
    if (fs_list(root, pc_count_cb, NULL) < 0) {
        toast("Couldn't reach your PC \xe2\x80\x94 is Nest PC Server running on the same Wi-Fi?");
        return;
    }
    db_set("pc_addr", addr);
    snprintf(g_scan_path, sizeof(g_scan_path), "%s", root);
    g_scan_secret = 0;
    g_scan_mode = 0;
    scan_open(SCAN_ADD);
}

static void health(void) {
    int unplayable = 0, noart = 0, dups = 0, books_pdf = 0;
    for (int i = 0; i < L.ne; i++) {
        if (L.e[i].media == MEDIA_VIDEO && !L.e[i].playable) unplayable++;
        const char *d = strrchr(L.e[i].path, '.');
        if (d && !strcasecmp(d, ".pdf")) books_pdf++;
    }
    for (int i = 0; i < L.nt; i++) {
        Title *t = &L.t[i];
        if (!title_poster(t)) noart++;
        if (t->kind == KIND_MOVIE && t->ne > 1) dups += t->ne - 1;
        for (int k = t->e0 + 1; k < t->e0 + t->ne; k++)
            if (t->kind == KIND_SERIES && L.e[k].season == L.e[k - 1].season && L.e[k].number == L.e[k - 1].number) dups++;
    }
    static char body[512];
    snprintf(body, sizeof(body),
             "%d video file%s need converting to MP4 (H.264) before they can play.\n"
             "%d title%s have no poster yet (they use a soft placeholder).\n"
             "%d likely duplicate file%s.\n%d PDF file%s can't be opened yet.",
             unplayable, unplayable == 1 ? "" : "s", noart, noart == 1 ? "" : "s", dups, dups == 1 ? "" : "s",
             books_pdf, books_pdf == 1 ? "" : "s");
    modal_confirm("Library health", body, "Done", "Close", NULL, NULL);
}

static void act(int a) {
    sfx(1);
    if (a >= A_VIEW) {
        int i = a - A_VIEW;
        static const char *items[] = {"Remove this shelf"};
        modal_menu(view_names[i], items, 1, view_cb, (void *)(intptr_t)i);
        return;
    }
    if (a >= A_EXCL) {
        int i = a - A_EXCL;
        static const char *items[] = {"Stop skipping this folder"};
        modal_menu(path_basename(excls[i])[0] ? path_basename(excls[i]) : excls[i], items, 1, excl_cb, (void *)(intptr_t)i);
        return;
    }
    if (a >= A_FOLDER) {
        int i = a - A_FOLDER;
        static const char *items[] = {"Rescan this folder", "Remove from Nest"};
        modal_menu(path_basename(folders[i])[0] ? path_basename(folders[i]) : folders[i], items, 2, folder_cb, (void *)(intptr_t)i);
        return;
    }
    switch (a) {
    case A_ADD: picker_open(PICK_LIBRARY); break;
    case A_MUSIC: picker_open(PICK_MUSIC); break;
    case A_ADD_SECRET:
        if (!db_get("pin_hash", NULL)) { pin_open(PIN_CREATE); toast("Set a PIN first"); }
        else picker_open(PICK_SECRET);
        break;
    case A_RESCAN: g_scan_path[0] = 0; scan_open(SCAN_RESCAN); break;
    case A_HEALTH: health(); break;
    case A_STATS: stats_enter(); break;
    case A_HISTORY: history_enter(); break;
    case A_STORAGE: storage_enter(); break;
    case A_PC: ime_request("Your PC's address (shown in Nest PC Server)", db_get("pc_addr", "192.168."), 60, pc_cb, NULL); break;
    case A_PIN: pin_open(db_get("pin_hash", NULL) ? PIN_CHANGE : PIN_CREATE); break;
    case A_LOCK: g_secret_unlocked = 0; toast("Secret is locked"); break;
    case A_WHOLE:
        modal_confirm("Scan the whole Vita?",
                      "Nest will look through every memory card. System, game and app folders are always skipped, and so is anything you exclude below.",
                      "Scan everything", "Cancel", whole_cb, NULL);
        break;
    case A_EXCL_ADD: picker_open(PICK_EXCLUDE); break;
    case A_CLEARLIB:
        modal_confirm("Clear the library?",
                      "Nest forgets every scanned title and every folder you added. Your files, PIN, settings and watch progress stay. Add your folders again to bring things back.",
                      "Clear", "Keep", clearlib_cb, NULL);
        break;
    case A_CLEAR:
        modal_confirm("Clear history?", "Resume points, the activity calendar and recent searches will be forgotten.",
                      "Clear", "Keep", clear_cb, NULL);
        break;
    }
}

static void change(SRow *r, int d) {
    if (!r->val) return;
    if (r->type == R_TOGGLE) *r->val = !*r->val;
    else if (r->type == R_CHOICE) *r->val = (*r->val + d + r->nopts) % r->nopts;
    sfx(0);
    if (r->val == &S.private_session) toast(S.private_session ? "Private session on" : "Private session off");
    settings_save();
}

static float row_h(SRow *r) { return r->type == R_THEME ? 170 : r->type == R_INFO ? (r->sub && strlen(r->sub) > 70 ? 74 : 60) : 60; }

void settings_frame(float dt) {
    /* input */
    if (in.pressed & B_BACK) {
        if (side == 1) side = 0;
        else { sfx(2); nav_pop(); return; }
        sfx(2);
    }
    if (side == 0) {
        if (in.repeat & B_UP && cat > 0) { cat--; ri = 0; build(); sfx(0); }
        if (in.repeat & B_DOWN && cat < CAT_N - 1) { cat++; ri = 0; build(); sfx(0); }
        if (in.repeat & B_RIGHT || in.pressed & B_OK) { side = 1; sfx(0); }
    } else if (nrows) {
        SRow *r = &rows[ri];
        if (in.repeat & B_UP) { if (ri > 0) ri--; sfx(0); }
        if (in.repeat & B_DOWN && ri < nrows - 1) { ri++; sfx(0); }
        r = &rows[ri];
        if (r->type == R_THEME) {
            int old = theme_focus;
            if (in.repeat & B_LEFT) { if (theme_focus > 0) theme_focus--; else side = 0; }
            if (in.repeat & B_RIGHT && theme_focus < THEME_COUNT - 1) theme_focus++;
            if (old != theme_focus) {
                S.theme = theme_focus;
                theme_apply(S.theme);
                settings_save();
                sfx(0);
            }
        } else {
            if (in.repeat & B_LEFT) {
                if (r->type == R_CHOICE) change(r, -1);
                else side = 0;
            }
            if (in.repeat & B_RIGHT && r->type == R_CHOICE) change(r, 1);
            if (in.pressed & B_OK) {
                if (r->type == R_TOGGLE || r->type == R_CHOICE) change(r, 1);
                else if (r->type == R_ACTION || r->type == R_FOLDER || r->type == R_EXCL || r->type == R_VIEW) act(r->action);
            }
        }
    }
    cfsel = approach(cfsel, (float)cat, 18, dt);
    rfsel = approach(rfsel, (float)ri, 18, dt);

    /* side panel */
    draw_scene_bg(1.0f);
    gfx_rect(0, 0, 250, SCR_H, col_a(T->bg2, 0.92f));
    gfx_rect(250, 0, 1, SCR_H, col_a(T->text, 0.06f));
    icon(IC_CHEVRON_L, 34, 40, 18, T->text2);
    if (hit(10, 14, 50, 50)) { nav_pop(); return; }
    text(F_DISPLAY, T->display_font == 2 ? 22 : 26, 56, 47, T->text, "Settings");
    for (int i = 0; i < CAT_N; i++) {
        float y = 96 + i * 50;
        float f = clampf(1 - fabsf(cfsel - i), 0, 1);
        if (i == cat) gfx_rrect(18, y, 214, 42, T->radius_sm, col_a(T->text, side == 0 ? 0.14f : 0.07f));
        if (side == 0 && i == cat) draw_focus(18, y, 214, 42, T->radius_sm, f);
        icon(CAT_ICONS[i], 44, y + 21, 17, i == cat ? T->accent : T->text3);
        text(F_SEMI, 15, 66, y + 26, i == cat ? T->text : T->text2, CAT_NAMES[i]);
        if (hit(18, y, 214, 42)) { cat = i; ri = 0; side = 1; build(); }
    }
    /* lofi corner in the side panel: a small framed window */
    if (S.show_lofi) {
        Lofi *l = lofi_get(T->scene);
        if (l) {
            gfx_shadow(26, 410, 198, 108, T->radius_sm, 12, col_a(T->shadow, 0.8f));
            lofi_draw(l, 26, 410, 198, 108, T->radius_sm, 0xFFFFFFFF, 0.5f, 0.5f);
        }
    }

    /* panel rows */
    float px = 280, pw2 = SCR_W - px - 36;
    float y = 34, fy = 0;
    for (int i = 0; i < nrows; i++) {
        if (i == ri) fy = y;
        y += row_h(&rows[i]) + 6;
    }
    rsy_t = fmaxf(0, fy - 200);
    rsy_t = fminf(rsy_t, fmaxf(0, y - SCR_H + 30));
    if (in.dragging && in.sx > 250) { rsy -= in.ddy; rsy_t = rsy; }
    if (!in.touching) rsy = approach(rsy, rsy_t, 12, dt);
    text(F_META, 12, px, 30 - rsy, T->text3, CAT_NAMES[cat]);
    y = 44 - rsy;
    for (int i = 0; i < nrows; i++) {
        SRow *r = &rows[i];
        float h = row_h(r);
        float f = side == 1 ? clampf(1 - fabsf(rfsel - i), 0, 1) : 0;
        if (y > SCR_H || y + h < 0) { y += h + 6; continue; }
        if (r->type != R_INFO) {
            gfx_rrect(px, y, pw2, h, T->radius_sm, col_a(T->text, (T->light ? 0.05f : 0.045f) + 0.05f * f));
            if (f > 0.5f && i == ri) draw_focus(px, y, pw2, h, T->radius_sm, f * (T->focus_style == 2 ? 0.0f : 1.0f));
            if (f > 0.5f && i == ri && T->focus_style == 2) gfx_rrect(px, y + 10, 3, h - 20, 1.5f, T->accent);
        }
        int tapped = hit(px, y, pw2, h);
        if (r->type == R_THEME) {
            text(F_SEMI, 15, px + 18, y + 26, T->text, "Theme");
            text(F_REG, 13, px + 80, y + 26, T->text3, THEMES[S.theme].tagline);
            float cw = 96, ch = 104, cg = 8;
            for (int k = 0; k < THEME_COUNT; k++) {
                float cx = px + 14 + k * (cw + cg), cy = y + 42;
                const Theme *th = &THEMES[k];
                vita2d_texture *soft = lofi_soft(th->scene);
                gfx_rrect(cx, cy, cw, ch, th->radius_sm, th->bg);
                if (soft) gfx_tex(soft, cx, cy, cw, 58, th->radius_sm, 0xFFFFFFFF, 0.5f, 0.5f);
                gfx_rect(cx, cy + 50, cw, 8, th->bg);
                gfx_rrect(cx + 8, cy + 64, 30, 6, 3, th->accent);
                gfx_rrect(cx + 42, cy + 64, 16, 6, 3, th->accent2);
                text(F_SEMI, 12, cx + 8, cy + 92, th->text, th->name);
                int sel = k == S.theme;
                if (sel) gfx_rrect_outline(cx - 3, cy - 3, cw + 6, ch + 6, th->radius_sm + 3, 2.5f, T->focus);
                if (tapped && in.tap_x >= cx && in.tap_x < cx + cw && in.tap_y >= cy) {
                    theme_focus = S.theme = k;
                    theme_apply(k);
                    settings_save();
                }
            }
        } else if (r->type == R_VIEW) {
            icon(IC_GRID, px + 30, y + h / 2, 18, T->accent);
            text_ellipsis(F_SEMI, 15, px + 52, y + 27, pw2 - 80, T->text, r->dyn);
            text(F_REG, 12, px + 52, y + 46, T->text3, "Smart view \xc2\xb7 a shelf on Home");
            icon(IC_DOTS, px + pw2 - 26, y + h / 2, 16, T->text3);
            if (tapped) { ri = i; side = 1; act(r->action); }
        } else if (r->type == R_EXCL) {
            icon(IC_EYE_OFF, px + 30, y + h / 2, 18, T->text3);
            text_ellipsis(F_SEMI, 15, px + 52, y + 27, pw2 - 80, T->text2, r->dyn);
            text(F_REG, 12, px + 52, y + 46, T->text3, "Skipped when scanning");
            icon(IC_DOTS, px + pw2 - 26, y + h / 2, 16, T->text3);
            if (tapped) { ri = i; side = 1; act(r->action); }
        } else if (r->type == R_FOLDER) {
            int idx = r->action - A_FOLDER;
            icon(fsecret[idx] ? IC_LOCK : IC_FOLDER, px + 30, y + h / 2, 18, fsecret[idx] ? T->accent2 : T->text2);
            text_ellipsis(F_SEMI, 15, px + 52, y + 27, pw2 - 80, T->text, r->dyn);
            text(F_REG, 12, px + 52, y + 46, T->text3,
                 fsecret[idx] ? (fmode[idx] ? "Secret \xc2\xb7 sorted by folder" : "Secret \xc2\xb7 sorted by guess") : "Library");
            icon(IC_DOTS, px + pw2 - 26, y + h / 2, 16, T->text3);
            if (tapped) { ri = i; side = 1; act(r->action); }
        } else {
            text(F_SEMI, 15, px + 18, y + (r->sub ? 26 : 35), r->type == R_INFO ? T->text2 : T->text, r->label);
            if (r->sub) {
                if (r->type == R_INFO) text_wrap(F_REG, 13, px + 18, y + 46, pw2 - 36, 18, 2, T->text3, r->sub);
                else text_ellipsis(F_REG, 13, px + 18, y + 46, pw2 - 200, T->text3, r->sub);
            }
            if (r->type == R_TOGGLE) {
                float sw = 46, sh = 26, sx = px + pw2 - sw - 18, sy2 = y + (h - sh) / 2;
                int on = *r->val;
                gfx_rrect(sx, sy2, sw, sh, sh / 2, on ? T->accent : col_a(T->text, 0.18f));
                gfx_circle(on ? sx + sw - sh / 2 : sx + sh / 2, sy2 + sh / 2, sh / 2 - 3, on ? T->on_accent : T->text);
                if (tapped) { ri = i; change(r, 1); }
            } else if (r->type == R_CHOICE) {
                const char *v = r->opts[clampi(*r->val, 0, r->nopts - 1)];
                float vw = text_w(F_SEMI, 14, v);
                float vx = px + pw2 - 18 - vw - 22;
                icon(IC_CHEVRON_L, vx - 16, y + h / 2, 12, T->text3);
                text(F_SEMI, 14, vx, y + h / 2 + 5, T->accent, v);
                icon(IC_CHEVRON_R, vx + vw + 14, y + h / 2, 12, T->text3);
                if (tapped) { ri = i; change(r, 1); }
            } else if (r->type == R_ACTION) {
                icon(IC_CHEVRON_R, px + pw2 - 24, y + h / 2, 14, T->text3);
                if (tapped) { ri = i; side = 1; act(r->action); }
            }
        }
        y += h + 6;
    }
}

/* ---------------- stats ---------------- */

static int s_video, s_read, s_streak, s_best, s_fin;
static int days[400], secs[400], ndays;
static float stats_anim;

void stats_enter(void) {
    int scope = g_secret_unlocked ? 1 : 0;
    db_stats(scope, &s_video, &s_read, &s_streak, &s_best, &s_fin);
    ndays = db_activity_days(scope, days, secs, 400);
    stats_anim = 0;
    nav_push(SCR_STATS);
}

static int ymd_wday(int ymd) {
    int y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7; /* 0 = Sunday */
}

static int ymd_add(int ymd, int delta) {
    static const int ml[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    while (delta < 0) {
        d--;
        if (d < 1) {
            m--;
            if (m < 1) { m = 12; y--; }
            d = ml[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
        }
        delta++;
    }
    return y * 10000 + m * 100 + d;
}

static int ymd_next(int ymd) {
    static const int ml[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    int len = ml[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
    if (++d > len) {
        d = 1;
        if (++m > 12) { m = 1; y++; }
    }
    return y * 10000 + m * 100 + d;
}

void stats_frame(float dt) {
    if (in.pressed & (B_BACK | B_OK)) { sfx(2); nav_pop(); return; }
    stats_anim = approach(stats_anim, 1, 5, dt);
    draw_scene_bg(1.0f);
    icon(IC_CHEVRON_L, 40, 40, 18, T->text2);
    if (hit(16, 14, 50, 50)) { nav_pop(); return; }
    text(F_DISPLAY, T->display_font == 2 ? 22 : 28, 62, 48, T->text, "Your nest, in numbers");
    /* tiles */
    char v[5][32];
    snprintf(v[0], 32, "%.1f", s_video / 3600.0f);
    snprintf(v[1], 32, "%.1f", s_read / 3600.0f);
    snprintf(v[2], 32, "%d", s_streak);
    snprintf(v[3], 32, "%d", s_best);
    snprintf(v[4], 32, "%d", s_fin);
    const char *lab[5] = {"hours watched", "hours read", "day streak", "best streak", "finished"};
    float tw = (SCR_W - 80 - 4 * 12) / 5.0f;
    for (int i = 0; i < 5; i++) {
        float x = 40 + i * (tw + 12), y = 80 + (1 - ease_out(clampf(stats_anim * 1.4f - i * 0.1f, 0, 1))) * 20;
        gfx_rrect(x, y, tw, 88, T->radius, col_a(T->text, 0.06f));
        text(F_DISPLAY, T->display_font == 2 ? 26 : 32, x + 16, y + 48, i == 2 && s_streak ? T->accent : T->text, v[i]);
        text(F_META, 12, x + 16, y + 72, T->text3, lab[i]);
    }
    /* heatmap: last 26 weeks */
    float hx = 40, hy = 214, cell = 13, cg = 3;
    text(F_SEMI, 15, hx, hy - 8, T->text2, "Last six months");
    int today = today_ymd();
    int wd = ymd_wday(today);
    int start = ymd_add(today, -(25 * 7 + wd));
    int maxs = 1;
    for (int i = 0; i < ndays; i++)
        if (secs[i] > maxs) maxs = secs[i];
    int d = start;
    for (int w = 0; w < 26; w++)
        for (int k = 0; k < 7; k++) {
            int s = 0;
            for (int i = 0; i < ndays; i++)
                if (days[i] == d) { s = secs[i]; break; }
            float x = hx + 30 + w * (cell + cg), y = hy + 6 + k * (cell + cg);
            float lvl = s ? 0.25f + 0.75f * sqrtf((float)s / maxs) : 0;
            col_t c = s ? col_a(T->accent, lvl * ease_out(stats_anim)) : col_a(T->text, 0.06f);
            if (d <= today) gfx_rrect(x, y, cell, cell, 3, c);
            if (d == today) gfx_rrect_outline(x - 1.5f, y - 1.5f, cell + 3, cell + 3, 4, 1.5f, T->text2);
            d = ymd_next(d);
        }
    text(F_META, 10, hx, hy + 20, T->text3, "Mon");
    text(F_META, 10, hx, hy + 52, T->text3, "Wed");
    text(F_META, 10, hx, hy + 84, T->text3, "Fri");
    /* most watched */
    float lx = 560, ly = 206;
    text(F_SEMI, 15, lx, ly, T->text2, "Most time spent with");
    int idx[64], n = 0;
    for (int i = 0; i < L.nt && n < 64; i++)
        if (L.t[i].last_used && (g_secret_unlocked || !L.t[i].secret)) idx[n++] = i;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (L.t[idx[b]].watched + L.t[idx[b]].progress > L.t[idx[a]].watched + L.t[idx[a]].progress) {
                int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
            }
    for (int i = 0; i < n && i < 5; i++) {
        Title *t = &L.t[idx[i]];
        float y = ly + 18 + i * 52;
        draw_poster(t, lx, y, 30, 45, 0, 1);
        text_ellipsis(F_SEMI, 14, lx + 42, y + 20, 320, T->text, t->title);
        char sub[64];
        if (t->kind == KIND_SERIES) snprintf(sub, sizeof(sub), "%d of %d episodes", t->watched, t->ne);
        else if (t->kind == KIND_BOOK) snprintf(sub, sizeof(sub), "%d%% read", (int)(t->progress * 100));
        else snprintf(sub, sizeof(sub), t->progress >= 0.995f ? "Watched" : "%d%% watched", (int)(t->progress * 100));
        text(F_META, 12, lx + 42, y + 38, T->text3, sub);
    }
    if (!n) text(F_REG, 13, lx, ly + 30, T->text3, "Watch or read something and it will show up here.");
    int empty = s_video + s_read == 0;
    if (empty) text(F_REG, 14, 40, 380, T->text3, "Your calendar fills in as you watch and read. Private sessions are never counted.");
}
