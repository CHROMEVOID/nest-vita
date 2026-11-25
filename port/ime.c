/* Nest port: the Vita's system keyboard dialog, done with Android's own keyboard
   (or a PC keyboard). Nest keeps drawing its frame; we draw a text box on top. */
#include "port.h"
#undef fopen
#undef main

#include <SDL.h>
#include "vita2d.h"

void port_draw_text_raw(vita2d_font *f, int x, int y, unsigned int color, unsigned int size, const char *text);
/* the keyboard box's own font (Nest's fonts are private to gfx.c) */
static vita2d_font *port_ui_font(void) {
    static vita2d_font *f;
    static int tried;
    if (!f && !tried) {
        tried = 1;
        FILE *fp = port_fopen("app0:assets/fonts/Inter-Regular.ttf", "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long n = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            void *b = malloc(n);
            if (b && fread(b, 1, n, fp) == (size_t)n) f = vita2d_load_font_mem(b, (unsigned)n);
            free(b);
            fclose(fp);
        }
    }
    return f;
}

static int active, status = SCE_COMMON_DIALOG_STATUS_NONE, result_button;
static char title[256], text[1024];
static int maxlen = 500;
static SceWChar16 *out16;
static float blink;

static void u16_to_u8(const SceWChar16 *s, char *o, int n) {
    int k = 0;
    for (int i = 0; s && s[i] && k < n - 4; i++) {
        unsigned c = s[i];
        if (c < 0x80) o[k++] = (char)c;
        else if (c < 0x800) { o[k++] = (char)(0xC0 | c >> 6); o[k++] = (char)(0x80 | (c & 63)); }
        else { o[k++] = (char)(0xE0 | c >> 12); o[k++] = (char)(0x80 | (c >> 6 & 63)); o[k++] = (char)(0x80 | (c & 63)); }
    }
    o[k] = 0;
}

static int u8_len_chars(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

void sceImeDialogParamInit(SceImeDialogParam *p) { memset(p, 0, sizeof(*p)); }

int sceImeDialogInit(const SceImeDialogParam *p) {
    if (active) return -1;
    u16_to_u8(p->title, title, sizeof(title));
    u16_to_u8(p->initialText, text, sizeof(text));
    maxlen = p->maxTextLength ? (int)p->maxTextLength : 500;
    out16 = p->inputTextBuffer;
    active = 1;
    status = SCE_COMMON_DIALOG_STATUS_RUNNING;
    result_button = SCE_IME_DIALOG_BUTTON_NONE;
    SDL_StartTextInput();
    return 0;
}

static void finish(int button) {
    SDL_StopTextInput();
    /* utf8 -> utf16 into Nest's buffer */
    int k = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (out16 && *p && k < maxlen) {
        unsigned c = *p++;
        if (c >= 0xF0) { p += 3; c = '?'; }
        else if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        out16[k++] = (SceWChar16)c;
    }
    if (out16) out16[k] = 0;
    result_button = button;
    status = SCE_COMMON_DIALOG_STATUS_FINISHED;
}

SceCommonDialogStatus sceImeDialogGetStatus(void) { return status; }
int sceImeDialogGetResult(SceImeDialogResult *r) {
    r->result = 0;
    r->button = result_button;
    return 0;
}
int sceImeDialogTerm(void) {
    active = 0;
    status = SCE_COMMON_DIALOG_STATUS_NONE;
    return 0;
}

/* events go to the text box first while it's open */
int port_ime_event(const SDL_Event *e) {
    if (!active || status != SCE_COMMON_DIALOG_STATUS_RUNNING) return 0;
    switch (e->type) {
    case SDL_TEXTINPUT:
        if (u8_len_chars(text) < maxlen && strlen(text) + strlen(e->text.text) < sizeof(text) - 1) strcat(text, e->text.text);
        return 1;
    case SDL_KEYDOWN:
        if (e->key.keysym.sym == SDLK_BACKSPACE) {
            size_t n = strlen(text);
            while (n && ((unsigned char)text[n - 1] & 0xC0) == 0x80) n--; /* whole characters */
            if (n) n--;
            text[n] = 0;
        } else if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER) finish(SCE_IME_DIALOG_BUTTON_ENTER);
        else if (e->key.keysym.sym == SDLK_ESCAPE || e->key.keysym.scancode == SDL_SCANCODE_AC_BACK) finish(SCE_IME_DIALOG_BUTTON_CLOSE);
        return 1;
    case SDL_KEYUP:
        return 1;
    case SDL_CONTROLLERBUTTONDOWN:
        /* on a handheld: Start or the confirm button finishes, the back button cancels */
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_START || e->cbutton.button == SDL_CONTROLLER_BUTTON_A) finish(SCE_IME_DIALOG_BUTTON_ENTER);
        else if (e->cbutton.button == SDL_CONTROLLER_BUTTON_B) finish(SCE_IME_DIALOG_BUTTON_CLOSE);
        else if (e->cbutton.button == SDL_CONTROLLER_BUTTON_X) {
            size_t n = strlen(text);
            while (n && ((unsigned char)text[n - 1] & 0xC0) == 0x80) n--;
            if (n) n--;
            text[n] = 0;
        } else if (e->cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
            SDL_StopTextInput(); /* bring the on-screen keyboard back */
            SDL_StartTextInput();
        }
        return 1;
    case SDL_FINGERDOWN:
        if (!SDL_IsTextInputShown()) SDL_StartTextInput();
        return 1;
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        return 1;
    default:
        return 0;
    }
}

/* the text box, over whatever Nest drew this frame */
void port_ime_draw(void) {
    if (!active || status != SCE_COMMON_DIALOG_STATUS_RUNNING) return;
    vita2d_font *f = port_ui_font();
    blink += 1 / 60.0f;
    float x = 120, y = 60, w = 720, h = 128;
    vita2d_draw_rectangle(0, 0, 960, 544, 0x99000000);
    vita2d_draw_rectangle(x, y, w, h, 0xF2201C24);
    vita2d_draw_rectangle(x, y, w, 2, 0x40FFFFFF);
    if (f) {
        port_draw_text_raw(f, (int)x + 24, (int)y + 36, 0xB3FFFFFF, 15, title);
        vita2d_draw_rectangle(x + 20, y + 52, w - 40, 44, 0x22FFFFFF);
        /* show the end of long text */
        const char *show = text;
        while (vita2d_font_text_width(f, 20, show) > w - 70 && *show) {
            show++;
            while (((unsigned char)*show & 0xC0) == 0x80) show++;
        }
        port_draw_text_raw(f, (int)x + 34, (int)y + 81, 0xFFFFFFFF, 20, show);
        float cx = x + 34 + vita2d_font_text_width(f, 20, show) + 2;
        if (((int)(blink * 2)) % 2 == 0) vita2d_draw_rectangle(cx, y + 62, 2, 24, 0xFFFFFFFF);
        port_draw_text_raw(f, (int)x + 24, (int)y + h - 12, 0x80FFFFFF, 12, "Enter / Start to finish  -  Back to cancel");
    }
}
