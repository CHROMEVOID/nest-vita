/* Buttons with gentle auto-repeat, analog stick as d-pad, and front touch
   gestures (tap, double tap, long press, drag, fling). */
#include "nest.h"

#include <psp2/apputil.h>
#include <psp2/system_param.h>

Input in;
int g_enter_cross = 1;

static uint32_t prev_held;
static float rep_timer[4];
static int was_touching;
static float touch_time, still_time;
static float last_tap_time = -10, last_tap_x, last_tap_y;
static float vel_x, vel_y;
static int long_fired, moved;
static float clock_s;

void input_init(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    SceAppUtilInitParam ip;
    SceAppUtilBootParam bp;
    memset(&ip, 0, sizeof(ip));
    memset(&bp, 0, sizeof(bp));
    sceAppUtilInit(&ip, &bp);
    int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
    g_enter_cross = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
}

void input_update(float dt) {
    clock_s += dt;
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t h = 0;
    if (pad.buttons & SCE_CTRL_UP) h |= B_UP;
    if (pad.buttons & SCE_CTRL_DOWN) h |= B_DOWN;
    if (pad.buttons & SCE_CTRL_LEFT) h |= B_LEFT;
    if (pad.buttons & SCE_CTRL_RIGHT) h |= B_RIGHT;
    if (pad.buttons & (g_enter_cross ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE)) h |= B_OK;
    if (pad.buttons & (g_enter_cross ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS)) h |= B_BACK;
    if (pad.buttons & SCE_CTRL_TRIANGLE) h |= B_TRI;
    if (pad.buttons & SCE_CTRL_SQUARE) h |= B_SQR;
    if (pad.buttons & SCE_CTRL_LTRIGGER) h |= B_L;
    if (pad.buttons & SCE_CTRL_RTRIGGER) h |= B_R;
    if (pad.buttons & SCE_CTRL_START) h |= B_START;
    if (pad.buttons & SCE_CTRL_SELECT) h |= B_SELECT;
    /* left stick as d-pad with a comfortable dead zone */
    int lx = pad.lx - 128, ly = pad.ly - 128;
    if (abs(lx) > 70 && abs(lx) > abs(ly)) h |= lx < 0 ? B_LEFT : B_RIGHT;
    if (abs(ly) > 70 && abs(ly) >= abs(lx)) h |= ly < 0 ? B_UP : B_DOWN;
    in.lstick_y = abs(ly) > 30 ? ly / 128.0f : 0;

    in.pressed = h & ~prev_held;
    in.repeat = in.pressed;
    const uint32_t dirs[4] = {B_UP, B_DOWN, B_LEFT, B_RIGHT};
    for (int i = 0; i < 4; i++) {
        if (h & dirs[i]) {
            if (in.pressed & dirs[i]) {
                rep_timer[i] = 0.34f;
            } else {
                rep_timer[i] -= dt;
                if (rep_timer[i] <= 0) {
                    in.repeat |= dirs[i];
                    rep_timer[i] = 0.085f;
                }
            }
        }
    }
    in.held = h;
    prev_held = h;

    /* touch */
    SceTouchData td;
    memset(&td, 0, sizeof(td));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
    in.tap = in.long_press = in.double_tap = 0;
    in.released = 0;
    in.ddx = in.ddy = 0;
    if (td.reportNum > 0) {
        float x = td.report[0].x / 2.0f, y = td.report[0].y / 2.0f;
        if (!was_touching) {
            in.sx = in.tx = x;
            in.sy = in.ty = y;
            touch_time = 0;
            still_time = 0;
            moved = 0;
            long_fired = 0;
            in.dragging = 0;
            vel_x = vel_y = 0;
        } else {
            float dx = x - in.tx, dy = y - in.ty;
            touch_time += dt;
            if (!moved && (fabsf(x - in.sx) > 14 || fabsf(y - in.sy) > 14)) {
                moved = 1;
                in.dragging = 1;
            }
            if (in.dragging) {
                in.ddx = dx;
                in.ddy = dy;
                if (dt > 0) {
                    vel_x = lerpf(vel_x, dx / dt, 0.4f);
                    vel_y = lerpf(vel_y, dy / dt, 0.4f);
                }
            }
            if (!moved && !long_fired && touch_time > 0.48f) {
                long_fired = 1;
                in.long_press = 1;
                in.tap_x = in.sx;
                in.tap_y = in.sy;
            }
            in.tx = x;
            in.ty = y;
        }
        in.touching = 1;
        was_touching = 1;
    } else {
        if (was_touching) {
            in.released = 1;
            if (!moved && !long_fired) {
                in.tap = 1;
                in.tap_x = in.sx;
                in.tap_y = in.sy;
                if (clock_s - last_tap_time < 0.3f && fabsf(in.sx - last_tap_x) < 60 && fabsf(in.sy - last_tap_y) < 60) {
                    in.double_tap = 1;
                    last_tap_time = -10;
                } else {
                    last_tap_time = clock_s;
                    last_tap_x = in.sx;
                    last_tap_y = in.sy;
                }
            }
            in.vx = in.dragging ? vel_x : 0;
            in.vy = in.dragging ? vel_y : 0;
        }
        in.touching = 0;
        in.dragging = 0;
        was_touching = 0;
    }
    in.any = in.held || in.touching || in.released;
}

int hit(float x, float y, float w, float h) {
    return in.tap && in.tap_x >= x && in.tap_x < x + w && in.tap_y >= y && in.tap_y < y + h;
}

int hit_long(float x, float y, float w, float h) {
    return in.long_press && in.tap_x >= x && in.tap_x < x + w && in.tap_y >= y && in.tap_y < y + h;
}

int touch_in(float x, float y, float w, float h) {
    return in.touching && in.tx >= x && in.tx < x + w && in.ty >= y && in.ty < y + h;
}

void input_consume_touch(void) {
    in.tap = in.long_press = in.double_tap = 0;
}
