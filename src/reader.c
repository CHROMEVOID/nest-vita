/* Reader for comics/manga (CBZ, CBR) and text books (EPUB, TXT).
   Pages turn with the d-pad, L/R or edge taps; manga mode reads right-to-left. */
#include "nest.h"

#include "book.h"

static int eidx = -1;
static int mode;              /* 0 images, 1 text */
static char **pages;
static int npages, page;
static float turn_anim, chrome_a, chrome_t, pan_y, pan_y_t, zoom_fit; /* 0 page 1 width 2 height 3 two pages 4 strip */
static float strip_off, strip_t;  /* long strip: pixels into the current page */
static int strip_end;              /* bottom of the last page is on screen */
static const char *VIEW_NAMES[5] = {"Whole page", "Fit width", "Fit height", "Two pages", "Long strip (webtoon)"};
#define NVIEWS 5
static int turn_dir;
static float read_secs, save_t;
static char err[128];

/* text books */
static char *textbuf;
static int *tpages;           /* byte offset where each page starts */
static int ntpages;
/* book layout, from Settings > Reading */
static float TX = 70, TW = SCR_W - 140, TLINE = 25;
static int TSIZE = 17, layout_size = -1;
static int adv_ready;

static void book_layout(void) {
    static const int sizes[4] = {15, 17, 20, 24};
    static const float spacing[3] = {1.25f, 1.48f, 1.75f};
    static const float margins[3] = {36, 70, 120};
    TSIZE = sizes[clampi(S.read_size, 0, 3)];
    TLINE = roundf(TSIZE * spacing[clampi(S.read_spacing, 0, 2)]);
    TX = margins[clampi(S.read_margin, 0, 2)];
    TW = SCR_W - TX * 2;
    if (layout_size != TSIZE) adv_ready = 0; /* glyph widths depend on size */
    layout_size = TSIZE;
}

static col_t page_bg(void) {
    static const unsigned bg[4] = {0x121214, 0xF3E7CF, 0xFAF8F2, 0x000000};
    return HEX(bg[clampi(S.read_light, 0, 3)]);
}
static col_t page_ink(void) {
    static const unsigned ink[4] = {0xE8E4DA, 0x3B2F22, 0x1E1D1A, 0xB9B6AE};
    return HEX(ink[clampi(S.read_light, 0, 3)]);
}

static char src_buf[1100];
static const char *page_src(int i) {
    if (i < 0 || i >= npages) return NULL;
    snprintf(src_buf, sizeof(src_buf), "arc:%s\x01%s", L.e[eidx].path, pages[i]);
    return src_buf;
}

/* ---- EPUB / TXT to plain text ---- */

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 65536;
        while (nc < *len + n + 1) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void html_to_text(const char *h, char **buf, size_t *len, size_t *cap) {
    const char *p = h;
    /* skip <head> */
    const char *body = strstr(h, "<body");
    if (body) p = body;
    int space = 0;
    while (*p) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            char tag[16] = {0};
            int k = 0;
            const char *q = p + 1;
            if (*q == '/') q++;
            while (k < 15 && isalpha((unsigned char)*q)) tag[k++] = (char)tolower((unsigned char)*q++);
            if (!strcmp(tag, "p") || !strcmp(tag, "br") || !strcmp(tag, "div") || !strcmp(tag, "h1") ||
                !strcmp(tag, "h2") || !strcmp(tag, "h3") || !strcmp(tag, "li") || !strcmp(tag, "tr")) {
                if (*len && (*buf)[*len - 1] != '\n') append(buf, len, cap, "\n", 1);
                if (p[1] != '/' && (!strcmp(tag, "h1") || !strcmp(tag, "h2"))) append(buf, len, cap, "\n", 1);
                space = 0;
            }
            if (!strcmp(tag, "script") || !strcmp(tag, "style")) {
                char close[32];
                snprintf(close, sizeof(close), "</%s", tag);
                const char *c = strstr(e, close);
                if (c) e = strchr(c, '>');
                if (!e) break;
            }
            p = e + 1;
            continue;
        }
        if (*p == '&') {
            const char *semi = strchr(p, ';');
            if (semi && semi - p < 10) {
                const char *rep = " ";
                if (!strncmp(p, "&amp;", 5)) rep = "&";
                else if (!strncmp(p, "&lt;", 4)) rep = "<";
                else if (!strncmp(p, "&gt;", 4)) rep = ">";
                else if (!strncmp(p, "&quot;", 6)) rep = "\"";
                else if (!strncmp(p, "&#8217;", 7) || !strncmp(p, "&rsquo;", 7)) rep = "\xe2\x80\x99";
                else if (!strncmp(p, "&#8220;", 7) || !strncmp(p, "&ldquo;", 7)) rep = "\xe2\x80\x9c";
                else if (!strncmp(p, "&#8221;", 7) || !strncmp(p, "&rdquo;", 7)) rep = "\xe2\x80\x9d";
                else if (!strncmp(p, "&mdash;", 7) || !strncmp(p, "&#8212;", 7)) rep = "\xe2\x80\x94";
                append(buf, len, cap, rep, strlen(rep));
                p = semi + 1;
                continue;
            }
        }
        if (*p == '\n' || *p == '\r' || *p == '\t' || *p == ' ') {
            if (!space && *len && (*buf)[*len - 1] != '\n') append(buf, len, cap, " ", 1);
            space = 1;
            p++;
            continue;
        }
        space = 0;
        append(buf, len, cap, p, 1);
        p++;
    }
    if (*len && (*buf)[*len - 1] != '\n') append(buf, len, cap, "\n", 1);
}

static int load_epub(const char *path) {
    Zip *z = zip_open(path);
    if (!z) return 0;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    /* container.xml -> OPF -> spine order */
    char opf_path[512] = "";
    int ci = zip_find(z, "META-INF/container.xml");
    if (ci >= 0) {
        char *c = zip_read(z, ci, NULL);
        if (c) {
            char *fp = strstr(c, "full-path=\"");
            if (fp) {
                fp += 11;
                char *q = strchr(fp, '"');
                if (q && q - fp < 500) {
                    memcpy(opf_path, fp, q - fp);
                    opf_path[q - fp] = 0;
                }
            }
            free(c);
        }
    }
    int done = 0;
    if (opf_path[0]) {
        char *opf = zip_read(z, zip_find(z, opf_path), NULL);
        if (opf) {
            char base[512];
            snprintf(base, sizeof(base), "%s", opf_path);
            char *sl = strrchr(base, '/');
            if (sl) sl[1] = 0;
            else base[0] = 0;
            char *spine = strstr(opf, "<spine");
            for (char *it = spine; it && (it = strstr(it, "idref=\"")); it += 7) {
                char id[128];
                char *q = strchr(it + 7, '"');
                if (!q || q - (it + 7) > 120) break;
                memcpy(id, it + 7, q - (it + 7));
                id[q - (it + 7)] = 0;
                /* find manifest item with this id */
                char needle[160];
                snprintf(needle, sizeof(needle), "id=\"%s\"", id);
                char *item = strstr(opf, needle);
                if (!item) continue;
                char *start = item;
                while (start > opf && *start != '<') start--;
                char *href = strstr(start, "href=\"");
                char *end = strchr(item, '>');
                if (!href || (end && href > end)) continue;
                href += 6;
                char *hq = strchr(href, '"');
                if (!hq) continue;
                char full[700];
                snprintf(full, sizeof(full), "%s%.*s", base, (int)(hq - href), href);
                char *html = zip_read(z, zip_find(z, full), NULL);
                if (html) {
                    html_to_text(html, &buf, &len, &cap);
                    free(html);
                    done++;
                }
            }
            free(opf);
        }
    }
    if (!done) {
        /* fall back: every html file in archive order */
        for (int i = 0; i < zip_count(z); i++) {
            const char *n = zip_name(z, i);
            const char *d = strrchr(n, '.');
            if (d && (!strcasecmp(d, ".html") || !strcasecmp(d, ".xhtml") || !strcasecmp(d, ".htm"))) {
                char *html = zip_read(z, i, NULL);
                if (html) {
                    html_to_text(html, &buf, &len, &cap);
                    free(html);
                }
            }
        }
    }
    zip_close(z);
    textbuf = buf;
    return buf != NULL && len > 0;
}

static int load_txt(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 32 * 1024 * 1024) { fclose(f); return 0; }
    textbuf = malloc(n + 1);
    if (!textbuf) { fclose(f); return 0; }
    if (fread(textbuf, 1, n, f) != (size_t)n) { fclose(f); free(textbuf); textbuf = NULL; return 0; }
    textbuf[n] = 0;
    fclose(f);
    for (char *p = textbuf; *p; p++)
        if (*p == '\r') *p = ' ';
    return 1;
}

/* cached glyph advances: measuring every line through FreeType would make
   long books take far too long to paginate */
static float adv[256];
static float fast_w(const char *s) {
    if (!adv_ready) {
        char b[2] = {0, 0};
        for (int c = 32; c < 127; c++) {
            b[0] = (char)c;
            adv[c] = (float)text_w(F_REG, TSIZE, b);
        }
        for (int c = 128; c < 256; c++) adv[c] = (c & 0xC0) == 0x80 ? 0 : adv['n'];
        adv_ready = 1;
    }
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) w += *p < 32 ? 0 : adv[*p];
    return w * 1.02f;
}

/* split text into screen pages by measuring words */
static void paginate(void) {
    free(tpages);
    int cap = 256;
    tpages = malloc(sizeof(int) * cap);
    ntpages = 0;
    const char *p = textbuf;
    int lines_per_page = (int)((SCR_H - 100) / TLINE);
    while (*p) {
        if (ntpages == cap) {
            cap *= 2;
            int *np2 = realloc(tpages, sizeof(int) * cap);
            if (!np2) break;
            tpages = np2;
        }
        tpages[ntpages++] = (int)(p - textbuf);
        int lines = 0;
        while (*p && lines < lines_per_page) {
            /* one line */
            char line[400];
            int ll = 0;
            const char *last_break = NULL;
            const char *q = p;
            while (*q && *q != '\n') {
                const char *w = q;
                while (*w && *w != ' ' && *w != '\n') w++;
                int add = (int)(w - q) + 1;
                if (ll + add >= 399) break;
                memcpy(line + ll, q, w - q);
                line[ll + (w - q)] = 0;
                if (fast_w(line) > TW && last_break) break;
                ll += (int)(w - q);
                line[ll++] = ' ';
                last_break = w;
                q = *w == ' ' ? w + 1 : w;
            }
            if (!last_break) {
                /* single giant word or empty line */
                if (*q == '\n') q++;
                p = q;
            } else {
                p = q;
                if (*p == '\n') p++;
            }
            lines++;
        }
    }
}

static int text_page_lines(int pg, char out[][400], int max) {
    if (pg < 0 || pg >= ntpages) return 0;
    const char *p = textbuf + tpages[pg];
    const char *end = pg + 1 < ntpages ? textbuf + tpages[pg + 1] : textbuf + strlen(textbuf);
    int n = 0;
    while (p < end && n < max) {
        int ll = 0;
        const char *last = NULL;
        const char *q = p;
        out[n][0] = 0;
        while (q < end && *q != '\n') {
            const char *w = q;
            while (w < end && *w != ' ' && *w != '\n') w++;
            if (ll + (w - q) + 1 >= 399) break;
            memcpy(out[n] + ll, q, w - q);
            out[n][ll + (w - q)] = 0;
            if (fast_w(out[n]) > TW && last) {
                out[n][ll > 0 ? ll - 1 : 0] = 0;
                break;
            }
            ll += (int)(w - q);
            out[n][ll++] = ' ';
            out[n][ll] = 0;
            last = w;
            q = *w == ' ' ? w + 1 : w;
        }
        if (!last) {
            out[n][0] = 0;
            if (q < end && *q == '\n') q++;
        } else if (q < end && *q == '\n') q++;
        p = q;
        n++;
    }
    return n;
}

/* ---------------- open / close ---------------- */

void reader_open(int e) {
    if (e < 0 || e >= L.ne) return;
    reader_close();
    eidx = e;
    Entry *en = &L.e[e];
    err[0] = 0;
    npages = 0;
    page = 0;
    mode = 0;
    const char *d = strrchr(en->path, '.');
    if (d && (!strcasecmp(d, ".epub") || !strcasecmp(d, ".txt"))) {
        mode = 1;
        int ok = !strcasecmp(d, ".epub") ? load_epub(en->path) : load_txt(en->path);
        book_layout();
        if (ok) paginate();
        else snprintf(err, sizeof(err), "This book couldn't be opened.");
        npages = ntpages;
        /* an epub made only of images (many manga) */
        if (!ok && !strcasecmp(d, ".epub")) {
            mode = 0;
            npages = book_list_images(en->path, &pages);
            if (npages) err[0] = 0;
        }
    } else if (d && !strcasecmp(d, ".pdf")) {
        snprintf(err, sizeof(err), "PDF isn't supported yet \xe2\x80\x94 convert it to CBZ or EPUB on a PC.");
    } else {
        npages = book_list_images(en->path, &pages);
        if (!npages) snprintf(err, sizeof(err), "No pages were found in this file.");
    }
    if (npages && en->pos > 0 && !en->finished) page = clampi(en->pos, 0, npages - 1);
    turn_anim = 1;
    chrome_a = 1;
    chrome_t = 0;
    pan_y = pan_y_t = 0;
    read_secs = 0;
    save_t = 0;
    {
        char k[32];
        snprintf(k, sizeof(k), "rz:%d", L.t[en->tidx].id);
        zoom_fit = clampi(db_geti(k, S.reader_fit), 0, NVIEWS - 1); /* remembered per book/series */
    }
    strip_off = strip_t = 0;
    strip_end = 0;
    if (nav_top() == SCR_READER) nav_replace(SCR_READER);
    else nav_push(SCR_READER);
}

static void save(int final) {
    if (eidx < 0 || npages <= 0) return;
    int fin = page >= npages - 1;
    int secs = (int)read_secs;
    read_secs -= secs;
    lib_progress(&L.e[eidx], page, npages, fin, secs);
    (void)final;
}

void reader_close(void) {
    if (eidx >= 0) save(1);
    if (pages) {
        for (int i = 0; i < npages; i++) img_drop(page_src(i));
        book_free_list(pages, npages);
    }
    pages = NULL;
    npages = 0;
    free(textbuf);
    textbuf = NULL;
    free(tpages);
    tpages = NULL;
    ntpages = 0;
    eidx = -1;
}

static void turn(int d) {
    if (mode == 0 && (int)zoom_fit == 4) { /* long strip scrolls instead of flipping */
        if (d > 0 && strip_end && page >= npages - 1) {
            int nx = lib_next_entry(eidx);
            save(1);
            if (nx >= 0) { toast("Next volume"); reader_open(nx); }
            else toast("You reached the end");
            return;
        }
        strip_t += d * SCR_H * 0.85f;
        return;
    }
    if (mode == 0 && (int)zoom_fit == 3) d *= 2; /* two pages at a time */
    int np2 = page + d;
    if (d < 0 && np2 < 0 && page > 0) np2 = 0;
    if (d > 0 && np2 >= npages && page < npages - 1 && (int)zoom_fit == 3 && mode == 0) np2 = npages - 1;
    if (np2 < 0) {
        toast("This is the first page");
        return;
    }
    if (np2 >= npages) {
        int nx = lib_next_entry(eidx);
        save(1);
        if (nx >= 0) {
            toast("Next volume");
            reader_open(nx);
        } else {
            toast("You reached the end");
        }
        return;
    }
    page = np2;
    turn_dir = d;
    turn_anim = 0;
    pan_y = pan_y_t = 0;
    sfx(0);
}

void reader_frame(float dt) {
    if (eidx < 0) { nav_pop(); return; }
    Entry *e = &L.e[eidx];
    Title *t = &L.t[e->tidx];
    int rtl = mode == 0 && S.manga_rtl;
    gfx_rect(0, 0, SCR_W, SCR_H, mode == 1 ? page_bg() : 0xFF000000);
    read_secs += dt;
    save_t += dt;
    if (save_t > 15) { save_t = 0; save(0); }

    /* input */
    if (in.pressed & B_BACK) { sfx(2); nav_pop(); return; }
    int fwd = rtl ? B_LEFT : B_RIGHT, back = rtl ? B_RIGHT : B_LEFT;
    if (in.repeat & (fwd | B_R)) turn(1);
    if (in.repeat & (back | B_L)) turn(-1);
    if (mode == 0) {
        if (in.held & B_DOWN) pan_y_t += 900 * dt;
        if (in.held & B_UP) pan_y_t -= 900 * dt;
        if (in.lstick_y != 0) pan_y_t += in.lstick_y * 900 * dt;
        if (in.pressed & B_SQR) {
            zoom_fit = (int)(zoom_fit + 1) % NVIEWS;
            pan_y = pan_y_t = 0;
            strip_off = strip_t = 0;
            char k[32];
            snprintf(k, sizeof(k), "rz:%d", t->id);
            db_seti(k, (int)zoom_fit);
            char m[80];
            snprintf(m, sizeof(m), "%s \xc2\xb7 remembered for this book", VIEW_NAMES[(int)zoom_fit]);
            toast(m);
        }
        if ((int)zoom_fit == 4) { /* strip: d-pad and stick scroll */
            if (in.held & B_DOWN) strip_t += 900 * dt;
            if (in.held & B_UP) strip_t -= 900 * dt;
            if (in.lstick_y != 0) strip_t += in.lstick_y * 900 * dt;
            if (in.dragging) { strip_off -= in.ddy; strip_t = strip_off; }
        }
    }
    if (in.pressed & B_TRI) {
        S.manga_rtl = !S.manga_rtl;
        settings_save();
        toast(S.manga_rtl ? "Manga mode: right to left" : "Left to right");
    }
    if (in.pressed & (B_OK | B_START)) chrome_t = chrome_a > 0.5f ? 10 : 0;
    if (in.tap) {
        float zone = S.read_tap ? in.tap_x / SCR_W : 0.5f;
        if (zone < 0.28f) turn(rtl ? 1 : -1);
        else if (zone > 0.72f) turn(rtl ? -1 : 1);
        else chrome_t = chrome_a > 0.5f ? 10 : 0;
    }
    if (in.dragging && mode == 0 && zoom_fit == 1) {
        pan_y -= in.ddy;
        pan_y_t = pan_y;
    }
    if (in.released && fabsf(in.vx) > 900 && fabsf(in.vx) > fabsf(in.vy) * 1.5f) turn((in.vx < 0) != rtl ? 1 : -1);
    chrome_t += dt;
    chrome_a = approach(chrome_a, chrome_t < 2.5f ? 1 : 0, 6, dt);
    turn_anim = approach(turn_anim, 1, 14, dt);

    if (!S.read_anim) turn_anim = 1;
    float slide = (1 - ease_out(turn_anim)) * 40 * turn_dir * (rtl ? -1 : 1);
    float alpha = 0.4f + 0.6f * ease_out(turn_anim);
    if (err[0]) {
        icon(IC_BOOK, SCR_W / 2, 230, 40, T->text3);
        text_center(F_SEMI, 17, SCR_W / 2, 290, T->text, t->title);
        text_center(F_REG, 14, SCR_W / 2, 318, T->text2, err);
    } else if (mode == 0 && (int)zoom_fit == 4) {
        /* long strip: pages stacked at full width, scrolled smoothly */
        if (!in.touching) strip_off = approach(strip_off, strip_t, 14, dt);
        float hcur = 0;
        for (int guard = 0; guard < 8; guard++) { /* move the anchor page as you scroll */
            float fd;
            vita2d_texture *tx = img_get(page_src(page), SCR_W, 0, &fd);
            hcur = tx ? vita2d_texture_get_height(tx) * (SCR_W / (float)vita2d_texture_get_width(tx)) : SCR_W * 1.4f;
            if (strip_off >= hcur && page < npages - 1) { strip_off -= hcur; strip_t -= hcur; page++; continue; }
            if (strip_off < 0 && page > 0) {
                vita2d_texture *pv = img_get(page_src(page - 1), SCR_W, 0, &fd);
                float hp = pv ? vita2d_texture_get_height(pv) * (SCR_W / (float)vita2d_texture_get_width(pv)) : SCR_W * 1.4f;
                page--; strip_off += hp; strip_t += hp;
                continue;
            }
            break;
        }
        if (page == 0 && strip_off < 0) strip_off = strip_t = 0;
        float maxoff = page == npages - 1 ? fmaxf(0, hcur - SCR_H) : 1e9f;
        if (strip_off > maxoff) { strip_off = maxoff; if (strip_t > maxoff) strip_t = maxoff; }
        strip_end = page == npages - 1 && strip_off >= maxoff - 1;
        float y = -roundf(strip_off);
        for (int pg = page; pg < npages && y < SCR_H; pg++) {
            float fd = 1;
            vita2d_texture *tx = img_get(page_src(pg), SCR_W, 0, &fd);
            if (tx) {
                float s2 = SCR_W / (float)vita2d_texture_get_width(tx);
                float h = vita2d_texture_get_height(tx) * s2;
                vita2d_draw_texture_tint_scale(tx, 0, y, s2, s2, col_a(0xFFFFFFFF, fd));
                y += h;
            } else {
                draw_spinner(SCR_W / 2, y + 120, 16, col_a(0xFFFFFFFF, 0.7f));
                y += SCR_W * 1.4f;
            }
        }
        float fx;
        img_get(page_src(page + 2), SCR_W, 0, &fx); /* read ahead */
    } else if (mode == 0 && (int)zoom_fit == 3) {
        /* two pages side by side; manga mode puts the first page on the right */
        float half = SCR_W / 2.0f;
        for (int k = 0; k < 2; k++) {
            int pg = page + k;
            if (pg >= npages) break;
            float fd = 0;
            vita2d_texture *tx = img_get(page_src(pg), 0, SCR_H, &fd);
            int right = rtl ? k == 0 : k == 1;
            float x0 = right ? half : 0;
            if (tx) {
                float tw = vita2d_texture_get_width(tx), th = vita2d_texture_get_height(tx);
                float s2 = fminf(half / tw, SCR_H / th);
                float dw = tw * s2, dh = th * s2;
                float x = right ? x0 : x0 + half - dw; /* pages meet at the spine */
                vita2d_draw_texture_tint_scale(tx, x + slide, (SCR_H - dh) / 2, s2, s2, col_a(0xFFFFFFFF, fd * alpha));
            } else {
                draw_spinner(x0 + half / 2, SCR_H / 2, 14, col_a(0xFFFFFFFF, 0.6f));
            }
        }
        float fx;
        img_get(page_src(page + 2), 0, SCR_H, &fx);
        img_get(page_src(page + 3), 0, SCR_H, &fx);
    } else if (mode == 0) {
        float fade = 0;
        int fit = (int)zoom_fit;
        int ww = fit == 1 ? SCR_W : 0, hh = fit == 1 ? 0 : SCR_H;
        vita2d_texture *tex = img_get(page_src(page), ww, hh, &fade);
        /* preload neighbours */
        float fx;
        img_get(page_src(page + 1), ww, hh, &fx);
        img_get(page_src(page - 1), ww, hh, &fx);
        if (tex) {
            float tw = vita2d_texture_get_width(tex), th = vita2d_texture_get_height(tex);
            float s = fit == 1 ? SCR_W / tw : fit == 2 ? SCR_H / th : fminf(SCR_W / tw, SCR_H / th);
            float dw = tw * s, dh = th * s;
            float maxp = fmaxf(0, dh - SCR_H);
            pan_y_t = clampf(pan_y_t, 0, maxp);
            if (!in.touching) pan_y = approach(pan_y, pan_y_t, 14, dt);
            pan_y = clampf(pan_y, 0, maxp);
            float x = (SCR_W - dw) / 2 + slide, y = dh > SCR_H ? -pan_y : (SCR_H - dh) / 2;
            vita2d_draw_texture_tint_scale(tex, x, y, s, s, col_a(0xFFFFFFFF, fade * alpha));
        } else {
            draw_spinner(SCR_W / 2, SCR_H / 2, 16, col_a(0xFFFFFFFF, 0.7f));
        }
    } else {
        static char lines[40][400];
        int n = text_page_lines(page, lines, 40);
        col_t c = col_a(page_ink(), alpha);
        for (int i = 0; i < n; i++) text(F_REG, TSIZE, TX + slide, 56 + TSIZE + i * TLINE, c, lines[i]);
    }

    /* chrome */
    if (chrome_a > 0.01f) {
        float a = chrome_a;
        gfx_grad_v(0, 0, SCR_W, 80, col_a(0xFF000000, 0.7f * a), 0);
        gfx_grad_v(0, SCR_H - 70, SCR_W, 70, 0, col_a(0xFF000000, 0.75f * a));
        icon(IC_CHEVRON_L, 40, 34, 20, col_a(0xFFFFFFFF, a));
        if (hit(16, 10, 56, 50)) { nav_pop(); return; }
        char el[32], hd[256];
        lib_entry_label(e, el, sizeof(el));
        snprintf(hd, sizeof(hd), t->ne > 1 ? "%s  \xc2\xb7  %s" : "%s", t->title, el);
        text_ellipsis(F_SEMI, 15, 66, 39, 560, col_a(0xFFFFFFFF, a), hd);
        if (npages > 0) {
            char pg[48];
            snprintf(pg, sizeof(pg), "%d / %d", page + 1, npages);
            text_right(F_META, 13, SCR_W - 36, 39, col_a(0xFFFFFFFF, 0.8f * a), pg);
            float p = npages > 1 ? (float)page / (npages - 1) : 1;
            float bx = 40, bw = SCR_W - 80, by = SCR_H - 30;
            gfx_rrect(bx, by, bw, 4, 2, col_a(0xFFFFFFFF, 0.25f * a));
            float fill = rtl ? bw * (1 - p) : 0;
            gfx_rrect(bx + fill, by, fmaxf(4, bw * p), 4, 2, col_a(T->accent, a));
            /* rough reading time left from your own speed */
            if (read_secs > 0 && page > 0) {
                /* simple estimate: 6s per comic page, 50s per text page if no history */
            }
            if (touch_in(bx, by - 18, bw, 40) && in.dragging) {
                float k = clampf((in.tx - bx) / bw, 0, 1);
                if (rtl) k = 1 - k;
                page = clampi((int)(k * (npages - 1) + 0.5f), 0, npages - 1);
                chrome_t = 0;
            }
        }
    }
}
