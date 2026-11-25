/* Calm scanning screen: walks folders on a worker thread, then saves. */
#include "nest.h"

extern char g_scan_path[512];
extern int g_scan_secret, g_scan_mode;
int g_onboard_folders_done;

static int purpose;
static ScanResult res;
static volatile int phase;   /* 0 scanning, 1 scanned (awaiting save), 2 saving, 3 done */
static volatile int saved_ok;
static float t0, shown_files;
static int focus;
static char folder_list[24][512];
static int f_secret[24], f_mode[24], nfold;
static volatile int cur_folder;
static int ask_another;

static void do_scan(void) {
    scan_load_excludes();
    if (purpose == SCAN_RESCAN) {
        for (int i = 0; i < nfold && !res.cancel; i++) {
            cur_folder = i;
            ScanResult r = {0};
            scan_folder(folder_list[i], f_secret[i], f_mode[i], &r);
            res.files_seen += r.files_seen;
            snprintf(res.current, sizeof(res.current), "%s", r.current);
            scan_finish(&r);
            res.movies += r.movies;
            res.shows += r.shows;
            res.episodes += r.episodes;
            res.books += r.books;
            res.unplayable += r.unplayable;
            if (!res.cancel) db_import(&r, folder_list[i]);
            scan_free(&r);
        }
        phase = 3;
    } else {
        scan_folder(g_scan_path, g_scan_secret, g_scan_mode, &res);
        scan_finish(&res);
        if (purpose == SCAN_ONBOARD) {
            phase = 1;
        } else {
            phase = 2;
            db_add_folder(g_scan_path, g_scan_secret, g_scan_mode);
            db_import(&res, g_scan_path);
            phase = 3;
        }
    }
}

static int worker(SceSize a, void *p) {
    (void)a;
    (void)p;
    do_scan();
    return sceKernelExitDeleteThread(0);
}

static void do_save(void) {
    db_add_folder(g_scan_path, g_scan_secret, g_scan_mode);
    saved_ok = db_import(&res, g_scan_path) == 0;
    phase = 3;
}

static int save_worker(SceSize a, void *p) {
    (void)a;
    (void)p;
    do_save();
    return sceKernelExitDeleteThread(0);
}

void scan_open(int p) {
    purpose = p;
    scan_free(&res);
    memset(&res, 0, sizeof(res));
    phase = 0;
    t0 = 0;
    shown_files = 0;
    focus = 0;
    ask_another = 0;
    if (p == SCAN_RESCAN) nfold = db_folders(folder_list, f_secret, f_mode, 24);
    SceUID th = sceKernelCreateThread("nest_scan", worker, 0x10000110, 256 * 1024, 0, 0x40000, NULL);
    if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) do_scan(); /* fall back to scanning inline */
    if (nav_top() == SCR_PICKER) nav_replace(SCR_SCAN);
    else nav_push(SCR_SCAN);
}

static void finish_and_leave(void) {
    lib_load();
    main_enter();
    scan_free(&res);
    if (purpose == SCAN_ONBOARD) {
        g_onboard_folders_done = 1;
        nav_pop();
    } else {
        toast("Library updated");
        nav_pop();
    }
}

static void count_line(char *b, int n) {
    char part[64];
    b[0] = 0;
    if (res.movies) { snprintf(part, sizeof(part), "%d film%s", res.movies, res.movies == 1 ? "" : "s"); strcat(b, part); }
    if (res.shows) {
        snprintf(part, sizeof(part), "%s%d show%s", b[0] ? "  \xc2\xb7  " : "", res.shows, res.shows == 1 ? "" : "s");
        strncat(b, part, n - strlen(b) - 1);
    }
    if (res.books) {
        snprintf(part, sizeof(part), "%s%d book%s", b[0] ? "  \xc2\xb7  " : "", res.books, res.books == 1 ? "" : "s");
        strncat(b, part, n - strlen(b) - 1);
    }
    if (!b[0]) snprintf(b, n, "Nothing to add here yet");
}

void scan_frame(float dt) {
    t0 += dt;
    shown_files = approach(shown_files, (float)res.files_seen, 6, dt);
    Lofi *l = lofi_get(purpose == SCAN_ONBOARD ? "study" : T->scene);
    float ww = 360, wh = 202, wx = (SCR_W - ww) / 2, wy = 64;
    float breathe = S.reduce_motion ? 0 : sinf(t0 * 1.2f) * 2;
    gfx_shadow(wx, wy + 10, ww, wh, T->radius + 6, 34, col_a(T->shadow, 1));
    if (l) lofi_draw(l, wx, wy + breathe, ww, wh, T->radius + 6, 0xFFFFFFFF, 0.5f, 0.5f);
    else gfx_rrect(wx, wy, ww, wh, T->radius + 6, T->surface);

    float cy = 312;
    if (phase == 0 || phase == 2) {
        const char *head = phase == 2 ? "Tucking everything in\xe2\x80\xa6" : "Looking through your folders\xe2\x80\xa6";
        text_center(F_DISPLAY, T->display_font == 2 ? 20 : 26, SCR_W / 2, cy, T->text, head);
        char c[64];
        snprintf(c, sizeof(c), "%d files looked at", (int)(shown_files + 0.5f));
        text_center(F_META, 14, SCR_W / 2, cy + 30, T->text2, c);
        text_ellipsis(F_REG, 12, SCR_W / 2 - 280, cy + 56, 560, T->text3, (const char *)res.current);
        /* gentle indeterminate bar */
        float bw = 240, bx = (SCR_W - bw) / 2, by = cy + 80;
        gfx_rrect(bx, by, bw, 4, 2, col_a(T->text, 0.1f));
        float k = fmodf(t0 * 0.6f, 1.0f);
        float seg = 70;
        float sx = bx + (bw + seg) * ease_inout(k) - seg;
        gfx_clip(bx, by, bw, 4);
        gfx_rrect(sx, by, seg, 4, 2, T->accent);
        gfx_noclip();
        if (in.pressed & B_BACK) res.cancel = 1;
        return;
    }
    char counts[160];
    count_line(counts, sizeof(counts));
    if (phase == 1 && !ask_another) {
        text_center(F_DISPLAY, T->display_font == 2 ? 20 : 26, SCR_W / 2, cy, T->text, "Here's what Nest found");
        text_center(F_SEMI, 17, SCR_W / 2, cy + 34, T->accent, counts);
        char sub[160] = "";
        if (res.episodes) snprintf(sub, sizeof(sub), "%d episodes", res.episodes);
        if (res.unplayable) {
            char u[96];
            snprintf(u, sizeof(u), "%s%d need converting to play", sub[0] ? "  \xc2\xb7  " : "", res.unplayable);
            strncat(sub, u, sizeof(sub) - strlen(sub) - 1);
        }
        text_center(F_REG, 13, SCR_W / 2, cy + 58, T->text3, sub);
        float bw = 170, gap = 14, bx = SCR_W / 2 - bw - gap / 2, by = cy + 84;
        if (in.repeat & B_LEFT) focus = 0;
        if (in.repeat & B_RIGHT) focus = 1;
        draw_button(bx, by, bw, 46, IC_CHECK, "Save", 1, focus == 0);
        draw_button(bx + bw + gap, by, bw, 46, IC_FOLDER, "Different folder", 0, focus == 1);
        int choose = -1;
        if (in.pressed & B_OK) choose = focus;
        if (hit(bx, by, bw, 46)) choose = 0;
        if (hit(bx + bw + gap, by, bw, 46)) choose = 1;
        if (in.pressed & B_BACK) choose = 1;
        if (choose == 0) {
            phase = 2;
            SceUID th = sceKernelCreateThread("nest_save", save_worker, 0x10000110, 128 * 1024, 0, 0, NULL);
            if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0) do_save();
        } else if (choose == 1) {
            scan_free(&res);
            picker_open(PICK_ONBOARD);
        }
        return;
    }
    if (phase == 3 && purpose == SCAN_ONBOARD && !ask_another) {
        ask_another = 1;
        focus = 1;
        lib_load();
    }
    if (ask_another) {
        text_center(F_DISPLAY, T->display_font == 2 ? 20 : 26, SCR_W / 2, cy, T->text, "Saved. Would you like to pick another folder?");
        text_center(F_REG, 14, SCR_W / 2, cy + 30, T->text3, "Maybe your books live somewhere else, or a second memory card.");
        float bw = 190, gap = 14, bx = SCR_W / 2 - bw - gap / 2, by = cy + 60;
        if (in.repeat & B_LEFT) focus = 0;
        if (in.repeat & B_RIGHT) focus = 1;
        draw_button(bx, by, bw, 46, IC_FOLDER, "Pick another", 0, focus == 0);
        draw_button(bx + bw + gap, by, bw, 46, IC_CHECK, "I'm done", 1, focus == 1);
        int choose = -1;
        if (in.pressed & B_OK) choose = focus;
        if (hit(bx, by, bw, 46)) choose = 0;
        if (hit(bx + bw + gap, by, bw, 46)) choose = 1;
        if (choose == 0) {
            scan_free(&res);
            picker_open(PICK_ONBOARD);
        } else if (choose == 1) {
            finish_and_leave();
        }
        return;
    }
    /* phase 3 for library adds / rescans */
    text_center(F_DISPLAY, T->display_font == 2 ? 20 : 26, SCR_W / 2, cy, T->text, res.cancel ? "Stopped" : "All tucked in");
    text_center(F_SEMI, 16, SCR_W / 2, cy + 32, T->accent, counts);
    draw_button(SCR_W / 2 - 80, cy + 64, 160, 46, IC_CHECK, "Done", 1, 1);
    if ((in.pressed & (B_OK | B_BACK)) || hit(SCR_W / 2 - 80, cy + 64, 160, 46)) finish_and_leave();
}
