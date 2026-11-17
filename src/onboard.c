/* First launch: welcome, PIN, folders (looping), a short setup and a soft
   "your library is ready" moment. Everything works offline. */
#include "nest.h"

extern int g_onboard_pin_done, g_onboard_folders_done;

enum { OB_WELCOME, OB_PIN, OB_FOLDERS, OB_SETUP, OB_READY };
static int step;
static float t, step_t;
static int focus, setup_row;

void onboard_enter(void) {
    step = OB_WELCOME;
    t = step_t = 0;
    focus = 0;
}

static void go(int s) {
    step = s;
    step_t = 0;
    focus = 0;
    setup_row = 0;
    sfx(1);
}

static void words(const char *eyebrow, const char *title, const char *body, float a) {
    float x = 72, y = 168 + (1 - ease_out(a)) * 14;
    text(F_META, 12, x, y, col_a(T->accent, a), eyebrow);
    int size = T->display_font == 2 ? 26 : 34;
    int lines = text_wrap(F_DISPLAY, size, x, y + size + 14, 420, size + 6, 2, col_a(T->text, a), title);
    text_wrap(F_REG, 15, x, y + 34 + lines * (size + 6), 400, 23, 5, col_a(T->text2, a), body);
}

static int two_buttons(const char *a, int ic_a, const char *b, int ic_b, float y) {
    float x = 72, w0 = fmaxf(150, text_w(F_SEMI, 15, a) + 64), w1 = fmaxf(120, text_w(F_SEMI, 15, b) + 48);
    if (in.repeat & B_LEFT) focus = 0;
    if (in.repeat & B_RIGHT && b) focus = 1;
    draw_button(x, y, w0, 46, ic_a, a, 1, focus == 0);
    if (b) draw_button(x + w0 + 12, y, w1, 46, ic_b, b, 0, focus == 1);
    if (hit(x, y, w0, 46)) return 0;
    if (b && hit(x + w0 + 12, y, w1, 46)) return 1;
    if (in.pressed & B_OK) return focus;
    return -1;
}

static void scene_window(const char *scene, float a) {
    Lofi *l = lofi_get(scene);
    float wx = 540, wy = 100, ww = 360, wh = 330;
    float bob = S.reduce_motion ? 0 : sinf(t * 0.9f) * 2.5f;
    gfx_shadow(wx, wy + 12, ww, wh, T->radius + 8, 40, col_a(T->shadow, a));
    if (l) lofi_draw(l, wx, wy + bob, ww, wh, T->radius + 8, col_a(0xFFFFFFFF, a), 0.5f, 0.55f);
}

void onboard_frame(float dt) {
    t += dt;
    step_t += dt;
    float a = clampf(step_t / 0.9f, 0, 1);

    if (step == OB_WELCOME) {
        Lofi *l = lofi_get("window");
        if (l) lofi_draw(l, 0, 0, SCR_W, SCR_H, 0, col_a(0xFFFFFFFF, clampf(t / 1.6f, 0, 1)), 0.5f, 0.5f);
        gfx_grad_v(0, 0, SCR_W, SCR_H, col_a(T->bg, 0.2f), col_a(T->bg, 0.9f));
        float ta = clampf((t - 0.7f) / 1.2f, 0, 1);
        float y = 300 + (1 - ease_out(ta)) * 16;
        text_center(F_DISPLAY, T->display_font == 2 ? 50 : 64, SCR_W / 2, y, col_a(T->text, ta), "nest");
        text_center(F_REG, 16, SCR_W / 2, y + 36, col_a(T->text2, ta), "A calm home for your films, shows and books.");
        float ba = clampf((t - 1.6f) / 0.8f, 0, 1);
        if (ba > 0) {
            draw_button(SCR_W / 2 - 80, y + 70, 160, 46, IC_CHEVRON_R, "Begin", 1, ba);
            if ((in.pressed & B_OK) || hit(SCR_W / 2 - 80, y + 70, 160, 46)) go(OB_PIN);
        }
        return;
    }
    draw_backdrop_scene(0.1f);
    /* step dots */
    for (int i = 0; i < 4; i++) {
        float on = i == step - 1;
        gfx_rrect(72 + i * 22, 92, on ? 16 : 8, 4, 2, on ? T->accent : col_a(T->text, 0.2f));
    }

    if (step == OB_PIN) {
        scene_window("alley", a);
        if (g_onboard_pin_done) {
            g_onboard_pin_done = 0;
            go(OB_FOLDERS);
            return;
        }
        words("A PRIVATE CORNER", "First, a PIN for Secret",
              "Nest has a Secret tab for videos and books you'd rather keep to yourself. Choose a six-digit PIN to protect it. You'll need it each time you open Secret.",
              a);
        int c = two_buttons("Choose a PIN", IC_LOCK, "Later", -1, 420);
        if (c == 0) pin_open(PIN_CREATE);
        else if (c == 1) go(OB_FOLDERS);
        return;
    }
    if (step == OB_FOLDERS) {
        scene_window("study", a);
        if (g_onboard_folders_done) {
            g_onboard_folders_done = 0;
            go(OB_SETUP);
            return;
        }
        words("YOUR SHELVES", "Where do your stories live?",
              "Pick a folder and Nest looks through everything inside it, folders within folders too. It works out films from shows, joins seasons that live in separate folders, and tidies names like Shameless.S02E04.1080p.x265 into Shameless.",
              a);
        int c = two_buttons("Pick a folder", IC_FOLDER, "Skip", -1, 420);
        if (c == 0) picker_open(PICK_ONBOARD);
        else if (c == 1) go(OB_SETUP);
        return;
    }
    if (step == OB_SETUP) {
        words("MAKE IT YOURS", "A few small choices", "You can change all of this later in Settings.", a);
        /* theme cards */
        float cx0 = 72, cy = 300, cw = 100, ch = 74, cg = 10;
        if (setup_row == 0) {
            if (in.repeat & B_LEFT && S.theme > 0) { S.theme--; theme_apply(S.theme); sfx(0); }
            if (in.repeat & B_RIGHT && S.theme < THEME_COUNT - 1) { S.theme++; theme_apply(S.theme); sfx(0); }
        } else if (setup_row == 1) {
            if (in.repeat & B_LEFT && S.poster_size > 0) S.poster_size--, sfx(0);
            if (in.repeat & B_RIGHT && S.poster_size < 2) S.poster_size++, sfx(0);
        } else if (setup_row == 2) {
            if (in.pressed & B_OK || in.repeat & (B_LEFT | B_RIGHT)) S.ambient = !S.ambient, sfx(0);
        }
        if (in.repeat & B_UP && setup_row > 0) setup_row--, sfx(0);
        if (in.repeat & B_DOWN && setup_row < 3) setup_row++, sfx(0);
        for (int k = 0; k < THEME_COUNT; k++) {
            const Theme *th = &THEMES[k];
            float x = cx0 + k * (cw + cg);
            vita2d_texture *soft = lofi_soft(th->scene);
            gfx_rrect(x, cy, cw, ch, th->radius_sm, th->bg);
            if (soft) gfx_tex(soft, x, cy, cw, 44, th->radius_sm, 0xFFFFFFFF, 0.5f, 0.5f);
            gfx_rect(x, cy + 36, cw, 8, th->bg);
            text(F_SEMI, 12, x + 8, cy + 64, th->text, th->name);
            gfx_rrect(x + cw - 26, cy + 56, 16, 5, 2.5f, th->accent);
            if (k == S.theme) gfx_rrect_outline(x - 3, cy - 3, cw + 6, ch + 6, th->radius_sm + 3, 2.5f, setup_row == 0 ? T->focus : col_a(T->text, 0.5f));
            if (hit(x, cy, cw, ch)) { S.theme = k; theme_apply(k); setup_row = 0; }
        }
        text(F_REG, 13, cx0, cy + ch + 22, T->text3, THEMES[S.theme].tagline);
        /* poster size + ambient */
        float ry = cy + ch + 40;
        static const char *sizes[] = {"Small posters", "Medium posters", "Large posters"};
        float cw2;
        for (int i = 0; i < 3; i++) {
            float x = cx0 + i * 140;
            draw_chip(x, ry, sizes[i], S.poster_size == i, setup_row == 1 && S.poster_size == i ? 1 : 0, &cw2);
            if (hit(x, ry, cw2, 30)) { S.poster_size = i; setup_row = 1; }
        }
        draw_chip(cx0 + 440, ry, S.ambient ? "Ambient mode: on" : "Ambient mode: off", S.ambient, setup_row == 2 ? 1 : 0, &cw2);
        if (hit(cx0 + 440, ry, cw2, 30)) { S.ambient = !S.ambient; setup_row = 2; }
        float by = ry + 48;
        draw_button(cx0, by, 160, 44, IC_CHEVRON_R, "Continue", 1, setup_row == 3);
        if ((setup_row == 3 && (in.pressed & B_OK)) || hit(cx0, by, 160, 44)) {
            settings_save();
            go(OB_READY);
        }
        return;
    }
    if (step == OB_READY) {
        Lofi *l = lofi_get(T->scene);
        if (l) lofi_draw(l, 0, 0, SCR_W, SCR_H, 0, col_a(0xFFFFFFFF, 0.9f * a), 0.5f, 0.5f);
        gfx_grad_v(0, 0, SCR_W, SCR_H, col_a(T->bg, 0.3f), col_a(T->bg, 0.92f));
        text_center(F_DISPLAY, T->display_font == 2 ? 30 : 40, SCR_W / 2, 300 + (1 - ease_out(a)) * 12, col_a(T->text, a),
                    "Your library is ready");
        char sub[160];
        if (L.nt) snprintf(sub, sizeof(sub), "%d films  \xc2\xb7  %d shows  \xc2\xb7  %d books", L.nmovies, L.nshows, L.nbooks);
        else snprintf(sub, sizeof(sub), "Add folders any time from Settings \xe2\x80\xba Library.");
        text_center(F_REG, 15, SCR_W / 2, 334, col_a(T->text2, a), sub);
        if (step_t > 2.6f || ((in.pressed & B_OK) && step_t > 0.6f) || (in.tap && step_t > 0.6f)) {
            S.onboarded = 1;
            settings_save();
            nav_replace(SCR_MAIN);
        }
    }
}
