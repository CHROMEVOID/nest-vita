/* Watch history ("what did I watch, and when") and Your Year: a recap of
   this year's hours, active days, finished episodes and favourite titles. */
#include "nest.h"

#include <psp2/rtc.h>

#include "sqlite3.h"

sqlite3 *db_handle(void);

typedef struct {
    int e;          /* entry index */
    int64_t when;
    int pos, dur, finished;
} HRow;

static HRow rows[200];
static int nrows, sel;
static float fsel, sy, sy_t, anim;
static int y_video, y_read, y_days, y_finished;
static int top_tid[3], top_secs[3], ntop;
static int year_now;
static int64_t today_start; /* local midnight, as unix time */
static int tz_off;          /* local - UTC, seconds */

static int entry_by_path(const char *p) {
    for (int i = 0; i < L.ne; i++)
        if (!strcmp(L.e[i].path, p)) return i;
    return -1;
}

static void time_basics(void) {
    SceDateTime d;
    sceRtcGetCurrentClockLocalTime(&d);
    int64_t now = now_s();
    int local_secs = d.hour * 3600 + d.minute * 60 + d.second;
    today_start = now - local_secs;
    int utc_secs = (int)(now % 86400);
    tz_off = local_secs - utc_secs;
    if (tz_off > 14 * 3600) tz_off -= 86400;
    if (tz_off < -12 * 3600) tz_off += 86400;
    year_now = d.year;
}

/* days since 1970-01-01 -> y/m/d (Howard Hinnant's civil_from_days) */
static void civil(int64_t z, int *y, int *m, int *dd) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *dd = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static void when_label(int64_t t, char *out, int n) {
    static const char *mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int64_t lt = t + tz_off;
    int mins = (int)((lt % 86400 + 86400) % 86400 / 60);
    int h = mins / 60, mi = mins % 60;
    char clock[16];
    snprintf(clock, sizeof(clock), "%d:%02d %s", h % 12 ? h % 12 : 12, mi, h < 12 ? "AM" : "PM");
    if (t >= today_start) snprintf(out, n, "Today, %s", clock);
    else if (t >= today_start - 86400) snprintf(out, n, "Yesterday, %s", clock);
    else {
        int y, m, d;
        civil(lt / 86400, &y, &m, &d);
        if (y == year_now) snprintf(out, n, "%d %s, %s", d, mon[m - 1], clock);
        else snprintf(out, n, "%d %s %d", d, mon[m - 1], y);
    }
}

void history_enter(void) {
    time_basics();
    sqlite3 *db = db_handle();
    nrows = 0;
    sel = 0;
    fsel = sy = sy_t = 0;
    anim = 0;
    int show_secret = g_secret_unlocked;
    sqlite3_stmt *s;
    if (db && sqlite3_prepare_v2(db, "SELECT path, pos, dur, finished, updated FROM progress WHERE updated > 0"
                                     " ORDER BY updated DESC LIMIT 400", -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW && nrows < 200) {
            const char *p = (const char *)sqlite3_column_text(s, 0);
            int e = p ? entry_by_path(p) : -1;
            if (e < 0) continue;
            if (L.t[L.e[e].tidx].secret && !show_secret) continue;
            HRow *r = &rows[nrows++];
            r->e = e;
            r->pos = sqlite3_column_int(s, 1);
            r->dur = sqlite3_column_int(s, 2);
            r->finished = sqlite3_column_int(s, 3);
            r->when = sqlite3_column_int64(s, 4);
        }
        sqlite3_finalize(s);
    }
    /* this year */
    int y0 = year_now * 10000 + 101;
    y_video = y_read = y_days = y_finished = 0;
    ntop = 0;
    if (db && sqlite3_prepare_v2(db, "SELECT kind, SUM(seconds) FROM activity WHERE day >= ? AND secret <= ? GROUP BY kind",
                                 -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, y0);
        sqlite3_bind_int(s, 2, show_secret);
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (sqlite3_column_int(s, 0) == KIND_BOOK) y_read += sqlite3_column_int(s, 1);
            else y_video += sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }
    if (db && sqlite3_prepare_v2(db, "SELECT COUNT(DISTINCT day) FROM activity WHERE day >= ? AND secret <= ?", -1, &s,
                                 NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, y0);
        sqlite3_bind_int(s, 2, show_secret);
        if (sqlite3_step(s) == SQLITE_ROW) y_days = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    {
        /* local Jan 1 midnight, roughly */
        int y, m, d;
        civil((now_s() + tz_off) / 86400, &y, &m, &d);
        static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        int doy = cum[m - 1] + d - 1 + (m > 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
        int64_t jan1 = today_start - (int64_t)doy * 86400;
        if (db && sqlite3_prepare_v2(db, "SELECT path FROM progress WHERE finished = 1 AND updated >= ?", -1, &s, NULL) ==
                      SQLITE_OK) {
            sqlite3_bind_int64(s, 1, jan1);
            while (sqlite3_step(s) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(s, 0);
                int e = p ? entry_by_path(p) : -1;
                if (e >= 0 && (show_secret || !L.t[L.e[e].tidx].secret)) y_finished++;
            }
            sqlite3_finalize(s);
        }
    }
    if (db && sqlite3_prepare_v2(db, "SELECT tid, SUM(seconds) AS t FROM activity WHERE day >= ? AND secret <= ?"
                                     " GROUP BY tid ORDER BY t DESC LIMIT 6", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, y0);
        sqlite3_bind_int(s, 2, show_secret);
        while (sqlite3_step(s) == SQLITE_ROW && ntop < 3) {
            int tid = sqlite3_column_int(s, 0);
            if (!lib_title_by_id(tid)) continue;
            top_tid[ntop] = tid;
            top_secs[ntop++] = sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }
    nav_push(SCR_HISTORY);
}

static void hours(int secs, char *o, int n) {
    if (secs < 3600) snprintf(o, n, "%d min", secs / 60);
    else snprintf(o, n, "%.1f h", secs / 3600.0f);
}

void history_frame(float dt) {
    if (in.pressed & B_BACK) { sfx(2); nav_pop(); return; }
    if (nrows) {
        if (in.repeat & B_UP && sel > 0) { sel--; sfx(0); }
        if (in.repeat & B_DOWN && sel < nrows - 1) { sel++; sfx(0); }
        if (in.pressed & B_OK) {
            sfx(1);
            detail_open(L.t[L.e[rows[sel].e].tidx].id);
            return;
        }
    }
    anim = approach(anim, 1, 5, dt);
    fsel = approach(fsel, (float)sel, 16, dt);
    draw_scene_bg(1.0f);
    icon(IC_CHEVRON_L, 40, 40, 18, T->text2);
    if (hit(16, 14, 50, 50)) { nav_pop(); return; }
    text(F_DISPLAY, T->display_font == 2 ? 22 : 28, 62, 48, T->text, "History & your year");

    /* ---- Your year card ---- */
    float cx = 40, cy = 76, cw = 330, ch = 420;
    float a = ease_out(anim);
    gfx_shadow(cx, cy + 6, cw, ch, T->radius, 18, col_a(T->shadow, 0.6f * a));
    gfx_rrect_grad(cx, cy, cw, ch, T->radius, col_mix(T->accent, T->surface, 0.55f), T->surface);
    char yl[32];
    snprintf(yl, sizeof(yl), "Your %d", year_now);
    text(F_DISPLAY, T->display_font == 2 ? 22 : 26, cx + 20, cy + 42, T->text, yl);
    char v[4][24];
    hours(y_video, v[0], 24);
    hours(y_read, v[1], 24);
    snprintf(v[2], 24, "%d", y_days);
    snprintf(v[3], 24, "%d", y_finished);
    const char *lab[4] = {"watched", "read", "days with something on", "episodes & films finished"};
    for (int i = 0; i < 4; i++) {
        float tx = cx + 20 + (i % 2) * 150, ty = cy + 64 + (i / 2) * 76;
        text(F_DISPLAY, T->display_font == 2 ? 20 : 26, tx, ty + 30, i == 0 ? T->accent : T->text, v[i]);
        text_wrap(F_META, 11, tx, ty + 50, 140, 14, 2, T->text3, lab[i]);
    }
    text(F_SEMI, 13, cx + 20, cy + 252, T->text2, "Most time with");
    for (int i = 0; i < ntop; i++) {
        Title *t = lib_title_by_id(top_tid[i]);
        if (!t) continue;
        float ty = cy + 266 + i * 50;
        draw_poster(t, cx + 20, ty, 28, 42, 0, 1);
        text_ellipsis(F_SEMI, 14, cx + 58, ty + 18, cw - 80, T->text, t->title);
        char hl[32];
        hours(top_secs[i], hl, sizeof(hl));
        text(F_META, 11, cx + 58, ty + 36, T->text3, hl);
    }
    if (!ntop) text_wrap(F_REG, 13, cx + 20, cy + 284, cw - 40, 18, 4, T->text3,
                         "Watch or read something this year and your favourites will show up here.");

    /* ---- history list ---- */
    float lx = 400, lw = SCR_W - lx - 36, rh = 64;
    text(F_SEMI, 15, lx, 92, T->text2, "Recently watched and read");
    if (!nrows) {
        text_wrap(F_REG, 14, lx, 130, lw, 20, 3, T->text3, "Nothing yet. Everything you play or read appears here with the time you were on it.");
        return;
    }
    if ((in.repeat & (B_UP | B_DOWN | B_LEFT | B_RIGHT))) sy_t = fmaxf(0, sel * (rh + 6) - 150);
    static KScroll hks;
    kscroll(&sy, &sy_t, &hks, nrows * (rh + 6) - (SCR_H - 108) + 30, in.dragging && in.sx > lx, in.ddy, in.vy, dt);
    gfx_clip(lx - 8, 104, lw + 16, SCR_H - 104);
    for (int i = 0; i < nrows; i++) {
        float y = 108 + i * (rh + 6) - sy;
        if (y > SCR_H || y + rh < 100) continue;
        HRow *r = &rows[i];
        Entry *e = &L.e[r->e];
        Title *t = &L.t[e->tidx];
        float f = clampf(1 - fabsf(fsel - i), 0, 1);
        gfx_rrect(lx, y, lw, rh, T->radius_sm, col_a(T->text, 0.045f + 0.05f * f));
        if (i == sel) draw_focus(lx, y, lw, rh, T->radius_sm, f);
        draw_poster(t, lx + 10, y + 8, 32, 48, 0, 1);
        text_ellipsis(F_SEMI, 15, lx + 54, y + 25, lw - 230, T->text, t->title);
        char el[48], line[200];
        if (t->kind != KIND_MOVIE) {
            lib_entry_label(e, el, sizeof(el));
            snprintf(line, sizeof(line), "%s%s%s", el, e->name[0] ? "  \xc2\xb7  " : "", e->name);
        } else {
            snprintf(line, sizeof(line), "%s", t->year ? "Film" : "");
            if (t->year) snprintf(line, sizeof(line), "Film \xc2\xb7 %d", t->year);
        }
        text_ellipsis(F_META, 12, lx + 54, y + 45, lw - 230, T->text3, line);
        char wl[48];
        when_label(r->when, wl, sizeof(wl));
        text_right(F_META, 12, lx + lw - 14, y + 25, T->text2, wl);
        if (r->finished) text_right(F_META, 11, lx + lw - 14, y + 45, T->accent, "Finished");
        else if (r->dur > 0) draw_progress(lx + lw - 114, y + 41, 100, 4, clampf((float)r->pos / r->dur, 0, 1));
        if (hit(lx, y, lw, rh)) {
            sel = i;
            detail_open(t->id);
            gfx_noclip();
            return;
        }
    }
    gfx_noclip();
    (void)a;
}
