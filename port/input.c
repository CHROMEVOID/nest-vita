/* Nest port: buttons, sticks and touch.
   Retroid Pocket and other Android handhelds: the bottom face button confirms (Vita's X),
   the right one goes back (O), left = square, top = triangle, L1/L2 = L, R1/R2 = R,
   Start and Select as on the Vita, and the Android back key also goes back.
   On a PC: arrows, X C V Z, Q E, Enter (Start), Right Shift (Select); the mouse is a finger. */
#include "port.h"
#undef fopen
#undef main

#include <SDL.h>
#include "psp2_all.h"

int port_ime_event(const SDL_Event *e);
void port_screen_scale_xy(float *sx, float *sy, float *ox, float *oy);
SDL_Window *port_window(void);

static SDL_GameController *pads[4];
static unsigned int key_buttons;   /* from the keyboard / back key */
static int touch_down;
static float touch_x, touch_y;      /* logical 960x544 */
static SDL_FingerID touch_id;
static int release_next; /* a quick tap: let Nest see the touch for one frame first */
static void test_tick(void);

static void open_pads(void) {
    if (getenv("NEST_NO_PADS")) return; /* testing on a PC with other controllers plugged in */
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID jid = SDL_JoystickGetDeviceInstanceID(i);
        int have = 0;
        for (int k = 0; k < 4; k++)
            if (pads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[k])) == jid) have = 1;
        if (have) continue;
        for (int k = 0; k < 4; k++)
            if (!pads[k]) {
                pads[k] = SDL_GameControllerOpen(i);
                break;
            }
    }
}

static unsigned int key_bit(SDL_Keycode k, SDL_Scancode sc) {
    if (sc == SDL_SCANCODE_AC_BACK) return SCE_CTRL_CIRCLE; /* Android back */
    switch (k) {
    case SDLK_UP: return SCE_CTRL_UP;
    case SDLK_DOWN: return SCE_CTRL_DOWN;
    case SDLK_LEFT: return SCE_CTRL_LEFT;
    case SDLK_RIGHT: return SCE_CTRL_RIGHT;
    case SDLK_x: return SCE_CTRL_CROSS;
    case SDLK_c: case SDLK_ESCAPE: return SCE_CTRL_CIRCLE;
    case SDLK_v: return SCE_CTRL_TRIANGLE;
    case SDLK_z: return SCE_CTRL_SQUARE;
    case SDLK_q: return SCE_CTRL_LTRIGGER;
    case SDLK_e: return SCE_CTRL_RTRIGGER;
    case SDLK_RETURN: return SCE_CTRL_START;
    case SDLK_RSHIFT: case SDLK_BACKSPACE: return SCE_CTRL_SELECT;
    /* Android gamepads that arrive as keys */
    case SDLK_AC_BACK: return SCE_CTRL_CIRCLE;
    default: return 0;
    }
}

static void to_logical(float nx, float ny, float *lx, float *ly) {
    int w, h;
    SDL_GL_GetDrawableSize(port_window(), &w, &h);
    float sx, sy, ox, oy;
    port_screen_scale_xy(&sx, &sy, &ox, &oy);
    *lx = (nx * w - ox) / sx;
    *ly = (ny * h - oy) / sy;
}

void port_pump_events(void) {
    SDL_Event e;
    if (release_next) {
        touch_down = 0;
        release_next = 0;
    }
    test_tick();
    while (SDL_PollEvent(&e)) {
        if (port_ime_event(&e)) continue;
        switch (e.type) {
        case SDL_QUIT:
        case SDL_APP_TERMINATING:
            SDL_Quit();
            exit(0);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            open_pads();
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (int k = 0; k < 4; k++)
                if (pads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[k])) == e.cdevice.which) {
                    SDL_GameControllerClose(pads[k]);
                    pads[k] = NULL;
                }
            break;
        case SDL_KEYDOWN:
            key_buttons |= key_bit(e.key.keysym.sym, e.key.keysym.scancode);
            break;
        case SDL_KEYUP:
            key_buttons &= ~key_bit(e.key.keysym.sym, e.key.keysym.scancode);
            break;
        case SDL_FINGERDOWN:
            if (!touch_down || release_next) {
                release_next = 0;
                touch_down = 1;
                touch_id = e.tfinger.fingerId;
                to_logical(e.tfinger.x, e.tfinger.y, &touch_x, &touch_y);
            }
            break;
        case SDL_FINGERMOTION:
            if (touch_down && e.tfinger.fingerId == touch_id) to_logical(e.tfinger.x, e.tfinger.y, &touch_x, &touch_y);
            break;
        case SDL_FINGERUP:
            if (touch_down && e.tfinger.fingerId == touch_id) {
                to_logical(e.tfinger.x, e.tfinger.y, &touch_x, &touch_y);
                release_next = 1;
            }
            break;
        default:
            break;
        }
    }
}

/* ---------------- test hook (PC builds, only when NEST_TEST_DIR is set) ----------------
   Commands in <dir>/cmd.txt, one per line: "key x", "tap 480 300", "drag 600 470 600 200 14",
   "wait 30" (frames), "shot name". Screenshots land in <dir>/name.png. */
#ifndef __ANDROID__
static char test_dir[512];
static char tcmds[64][96];
static int tn, ti, twait;
static unsigned int test_keys;
static int test_key_frames;
static int drag_on, drag_i, drag_n;
static float dx0, dy0, dx1, dy1;
char port_test_shot[600];
static void test_tick(void) {
    static int init;
    if (!init) {
        init = 1;
        const char *d = getenv("NEST_TEST_DIR");
        if (d) snprintf(test_dir, sizeof(test_dir), "%s", d);
    }
    if (!test_dir[0]) return;
    if (test_key_frames > 0 && --test_key_frames == 0) key_buttons &= ~test_keys;
    if (drag_on) {
        drag_i++;
        float k = drag_i / (float)drag_n;
        touch_x = dx0 + (dx1 - dx0) * k;
        touch_y = dy0 + (dy1 - dy0) * k;
        if (drag_i >= drag_n) { drag_on = 0; release_next = 1; }
        return;
    }
    if (twait > 0) { twait--; return; }
    if (ti >= tn) {
        char p[600];
        snprintf(p, sizeof(p), "%s/cmd.txt", test_dir);
        FILE *f = fopen(p, "rb");
        if (!f) return;
        tn = ti = 0;
        while (tn < 64 && fgets(tcmds[tn], sizeof(tcmds[0]), f)) tn++;
        fclose(f);
        remove(p);
    }
    if (ti >= tn) return;
    char *c = tcmds[ti++];
    char a[32];
    float x, y, x2, y2;
    int n;
    if (sscanf(c, "key %31s", a) == 1) {
        SDL_Keycode k = SDL_GetKeyFromName(a);
        if (!strcmp(a, "x")) k = SDLK_x;
        test_keys = key_bit(k, SDL_SCANCODE_UNKNOWN);
        key_buttons |= test_keys;
        test_key_frames = 3;
        twait = 6;
    } else if (sscanf(c, "tap %f %f", &x, &y) == 2) {
        touch_down = 1;
        touch_x = x;
        touch_y = y;
        release_next = 1;
        twait = 4;
    } else if (sscanf(c, "drag %f %f %f %f %d", &x, &y, &x2, &y2, &n) >= 4) {
        drag_on = 1;
        drag_i = 0;
        drag_n = n > 1 ? n : 14;
        dx0 = x; dy0 = y; dx1 = x2; dy1 = y2;
        touch_down = 1;
        touch_x = x;
        touch_y = y;
    } else if (sscanf(c, "wait %d", &n) == 1) {
        twait = n;
    } else if (sscanf(c, "shot %63s", a) == 1) {
        snprintf(port_test_shot, sizeof(port_test_shot), "%s/%s.png", test_dir, a);
        twait = 1;
    }
}
#else
static void test_tick(void) {}
char port_test_shot[4];
#endif

int sceCtrlSetSamplingMode(int mode) {
    (void)mode;
    open_pads();
    return 0;
}

int sceCtrlPeekBufferPositive(int port, SceCtrlData *d, int count) {
    (void)port;
    (void)count;
    static int pumped_once;
    if (!pumped_once) { port_pump_events(); pumped_once = 1; }
    memset(d, 0, sizeof(*d));
    unsigned int b = key_buttons;
    int lx = 128, ly = 128;
    for (int k = 0; k < 4; k++) {
        SDL_GameController *c = pads[k];
        if (!c) continue;
#define B(sdl, sce) if (SDL_GameControllerGetButton(c, sdl)) b |= sce
        B(SDL_CONTROLLER_BUTTON_A, SCE_CTRL_CROSS);
        B(SDL_CONTROLLER_BUTTON_B, SCE_CTRL_CIRCLE);
        B(SDL_CONTROLLER_BUTTON_X, SCE_CTRL_SQUARE);
        B(SDL_CONTROLLER_BUTTON_Y, SCE_CTRL_TRIANGLE);
        B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SCE_CTRL_LTRIGGER);
        B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SCE_CTRL_RTRIGGER);
        B(SDL_CONTROLLER_BUTTON_START, SCE_CTRL_START);
        B(SDL_CONTROLLER_BUTTON_BACK, SCE_CTRL_SELECT);
        B(SDL_CONTROLLER_BUTTON_DPAD_UP, SCE_CTRL_UP);
        B(SDL_CONTROLLER_BUTTON_DPAD_DOWN, SCE_CTRL_DOWN);
        B(SDL_CONTROLLER_BUTTON_DPAD_LEFT, SCE_CTRL_LEFT);
        B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SCE_CTRL_RIGHT);
        B(SDL_CONTROLLER_BUTTON_LEFTSTICK, SCE_CTRL_L3);
        B(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SCE_CTRL_R3);
#undef B
        /* the analog triggers work as L / R too */
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= SCE_CTRL_LTRIGGER;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= SCE_CTRL_RTRIGGER;
        int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(ax - 0) > abs(lx - 128) * 256) lx = 128 + ax / 256;
        if (abs(ay - 0) > abs(ly - 128) * 256) ly = 128 + ay / 256;
    }
    d->buttons = b;
    d->lx = (unsigned char)SDL_clamp(lx, 0, 255);
    d->ly = (unsigned char)SDL_clamp(ly, 0, 255);
    d->rx = d->ry = 128;
    return 1;
}

int sceTouchSetSamplingState(int port, int state) { (void)port; (void)state; return 0; }

int sceTouchPeek(int port, SceTouchData *td, int n) {
    (void)port;
    (void)n;
    memset(td, 0, sizeof(*td));
    if (touch_down) {
        td->reportNum = 1;
        /* the Vita's front panel reports at twice the screen resolution */
        float x = SDL_clamp(touch_x, 0, 959.5f), y = SDL_clamp(touch_y, 0, 543.5f);
        td->report[0].x = (uint16_t)(x * 2);
        td->report[0].y = (uint16_t)(y * 2);
        td->report[0].id = 0;
    }
    return 1;
}
