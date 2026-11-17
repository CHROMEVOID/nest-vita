/* Six-digit PIN pad for the Secret tab. */
#include "nest.h"

int g_onboard_pin_done;

static int purpose, stage; /* stage: 0 enter (or old), 1 new, 2 confirm */
static char digits[8], first[8];
static int nd;
static int kfocus = 4;
static float shake, msg_t;
static char msg[96];
static float dot_pop[6];

static uint64_t fnv(const char *s, uint64_t h) {
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static void hash_pin(const char *pin, const char *salt, char *out, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 4000; i++) {
        h = fnv(salt, h);
        h = fnv(pin, h);
    }
    snprintf(out, n, "%s:%016llx", salt, (unsigned long long)h);
}

static int check_pin(const char *pin) {
    const char *stored = db_get("pin_hash", NULL);
    if (!stored) return 0;
    char salt[32];
    const char *c = strchr(stored, ':');
    if (!c || c - stored >= (int)sizeof(salt)) return 0;
    memcpy(salt, stored, c - stored);
    salt[c - stored] = 0;
    char h[96];
    hash_pin(pin, salt, h, sizeof(h));
    return !strcmp(h, stored);
}

static void save_pin(const char *pin) {
    char salt[24], h[96];
    snprintf(salt, sizeof(salt), "%08x", (unsigned)(sceKernelGetProcessTimeWide() * 2654435761u) ^ (unsigned)rand());
    hash_pin(pin, salt, h, sizeof(h));
    db_set("pin_hash", h);
}

void pin_open(int p) {
    purpose = p;
    stage = p == PIN_CREATE ? 1 : 0;
    nd = 0;
    memset(digits, 0, sizeof(digits));
    msg[0] = 0;
    kfocus = 4;
    nav_push(SCR_PIN);
}

static void say(const char *m) {
    snprintf(msg, sizeof(msg), "%s", m);
    msg_t = 0;
}

static void submit(void) {
    digits[6] = 0;
    if (stage == 0) {
        if (check_pin(digits)) {
            if (purpose == PIN_UNLOCK) {
                g_secret_unlocked = 1;
                sfx(1);
                nav_pop();
                return;
            }
            stage = 1;
            say("Now choose a new PIN");
        } else {
            shake = 1;
            say("That's not it. Try again.");
            sfx(2);
        }
    } else if (stage == 1) {
        snprintf(first, sizeof(first), "%s", digits);
        stage = 2;
        say("Once more to confirm");
    } else {
        if (!strcmp(first, digits)) {
            save_pin(digits);
            extern int g_pending_secret_tid;
            if (g_pending_secret_tid >= 0) {
                Title *t = lib_title_by_id(g_pending_secret_tid);
                g_pending_secret_tid = -1;
                if (t && !t->secret) {
                    lib_set_secret(t, 1);
                    toast("PIN set \xe2\x80\x94 added to the Secret tab");
                    sfx(1);
                    nav_pop();
                    return;
                }
            }
            g_onboard_pin_done = 1;
            g_secret_unlocked = purpose != PIN_CHANGE ? g_secret_unlocked : g_secret_unlocked;
            toast(purpose == PIN_CHANGE ? "PIN changed" : "PIN set");
            sfx(1);
            nav_pop();
            return;
        }
        shake = 1;
        stage = 1;
        say("Those didn't match. Choose a PIN again.");
        sfx(2);
    }
    nd = 0;
    memset(digits, 0, sizeof(digits));
}

static void press(int k) {
    /* keys: 0-8 = digits 1-9, 9 = back, 10 = 0, 11 = delete */
    if (k == 9) { nav_pop(); return; }
    if (k == 11) {
        if (nd > 0) digits[--nd] = 0;
        sfx(0);
        return;
    }
    if (nd >= 6) return;
    digits[nd] = (char)('0' + (k == 10 ? 0 : k + 1));
    dot_pop[nd] = 1;
    nd++;
    sfx(0);
    if (nd == 6) submit();
}

void pin_frame(float dt) {
    if (in.pressed & B_BACK) {
        if (nd > 0) press(11);
        else { nav_pop(); return; }
    }
    if (in.repeat & B_LEFT && kfocus % 3 > 0) kfocus--, sfx(0);
    if (in.repeat & B_RIGHT && kfocus % 3 < 2) kfocus++, sfx(0);
    if (in.repeat & B_UP && kfocus >= 3) kfocus -= 3, sfx(0);
    if (in.repeat & B_DOWN && kfocus < 9) kfocus += 3, sfx(0);
    if (in.pressed & B_OK) press(kfocus);
    if (in.pressed & B_SQR) press(11);

    shake = approach(shake, 0, 5, dt);
    msg_t += dt;
    vita2d_texture *soft = lofi_soft("alley");
    if (soft) gfx_tex(soft, 0, 0, SCR_W, SCR_H, 0, 0xFFFFFFFF, 0.5f, 0.5f);
    gfx_rect(0, 0, SCR_W, SCR_H, col_a(T->bg, 0.62f));

    /* left: words + dots */
    float lx = 80;
    const char *title = stage == 0 ? (purpose == PIN_CHANGE ? "Enter your current PIN" : "Enter your PIN")
                        : stage == 1 ? (purpose == PIN_CHANGE ? "Choose a new PIN" : "Choose a PIN")
                                     : "Confirm your PIN";
    text(F_DISPLAY, T->display_font == 2 ? 24 : 32, lx, 200, T->text, title);
    text_wrap(F_REG, 15, lx, 236, 360, 22, 3, T->text2,
              stage == 0 ? "Secret keeps private videos and books out of sight."
                         : "Six digits. It protects the Secret tab, where private videos and books live.");
    float sx = sinf(shake * 30) * 10 * shake;
    for (int i = 0; i < 6; i++) {
        dot_pop[i] = approach(dot_pop[i], 0, 8, dt);
        float cx = lx + 14 + i * 34 + sx, cy = 330;
        float r = 9 + 3 * dot_pop[i];
        if (i < nd) gfx_circle(cx, cy, r, T->accent);
        else gfx_ring(cx, cy, 9, 2, col_a(T->text, 0.4f));
    }
    if (msg[0]) text(F_SEMI, 14, lx, 378, col_a(stage == 0 && shake > 0.01f ? T->accent2 : T->text2, clampf(msg_t * 4, 0, 1)), msg);

    /* right: keypad */
    float kw = 84, kh = 64, kg = 12;
    float kx = SCR_W - 80 - 3 * kw - 2 * kg, ky = 104;
    for (int k = 0; k < 12; k++) {
        float x = kx + (k % 3) * (kw + kg), y = ky + (k / 3) * (kh + kg);
        int f = kfocus == k;
        const char *lab = NULL;
        char d[4];
        if (k < 9) { snprintf(d, sizeof(d), "%d", k + 1); lab = d; }
        else if (k == 10) lab = "0";
        int is_digit = lab != NULL;
        gfx_rrect(x, y, kw, kh, T->nav_style == 0 ? kh / 2 : T->radius, col_a(T->text, is_digit ? (f ? 0.2f : 0.08f) : (f ? 0.14f : 0.0f)));
        if (f) draw_focus(x, y, kw, kh, T->nav_style == 0 ? kh / 2 : T->radius, 1);
        if (lab) text_center(F_SEMI, 24, x + kw / 2, y + kh / 2 + 9, T->text, lab);
        else if (k == 9) icon(IC_CLOSE, x + kw / 2, y + kh / 2, 18, T->text2);
        else icon(IC_BACK, x + kw / 2, y + kh / 2, 20, T->text2);
        if (hit(x, y, kw, kh)) { kfocus = k; press(k); }
    }
}
