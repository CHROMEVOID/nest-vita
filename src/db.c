/* SQLite storage with a small Vita file-system layer (VFS). */
#include "nest.h"

#include <time.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>

#include "sqlite3.h"

static sqlite3 *db;

/* ================= VFS ================= */

typedef struct {
    sqlite3_file base;
    SceUID fd;
} VFile;

static int vf_close(sqlite3_file *f) {
    VFile *v = (VFile *)f;
    if (v->fd >= 0) sceIoClose(v->fd);
    v->fd = -1;
    return SQLITE_OK;
}
static int vf_read(sqlite3_file *f, void *buf, int amt, sqlite3_int64 off) {
    VFile *v = (VFile *)f;
    if (sceIoLseek(v->fd, off, SCE_SEEK_SET) != off) return SQLITE_IOERR_READ;
    int r = sceIoRead(v->fd, buf, amt);
    if (r < 0) return SQLITE_IOERR_READ;
    if (r < amt) {
        memset((char *)buf + r, 0, amt - r);
        return SQLITE_IOERR_SHORT_READ;
    }
    return SQLITE_OK;
}
static int vf_write(sqlite3_file *f, const void *buf, int amt, sqlite3_int64 off) {
    VFile *v = (VFile *)f;
    if (sceIoLseek(v->fd, off, SCE_SEEK_SET) != off) return SQLITE_IOERR_WRITE;
    int w = sceIoWrite(v->fd, buf, amt);
    return w == amt ? SQLITE_OK : SQLITE_IOERR_WRITE;
}
static int vf_truncate(sqlite3_file *f, sqlite3_int64 size) {
    VFile *v = (VFile *)f;
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    st.st_size = size;
    if (sceIoChstatByFd(v->fd, &st, SCE_CST_SIZE) < 0) return SQLITE_IOERR_TRUNCATE;
    return SQLITE_OK;
}
static int vf_sync(sqlite3_file *f, int flags) {
    (void)flags;
    VFile *v = (VFile *)f;
    sceIoSyncByFd(v->fd, 0);
    return SQLITE_OK;
}
static int vf_size(sqlite3_file *f, sqlite3_int64 *size) {
    VFile *v = (VFile *)f;
    SceOff cur = sceIoLseek(v->fd, 0, SCE_SEEK_CUR);
    SceOff end = sceIoLseek(v->fd, 0, SCE_SEEK_END);
    sceIoLseek(v->fd, cur, SCE_SEEK_SET);
    if (end < 0) return SQLITE_IOERR_FSTAT;
    *size = end;
    return SQLITE_OK;
}
static int vf_lock(sqlite3_file *f, int l) { (void)f; (void)l; return SQLITE_OK; }
static int vf_check_lock(sqlite3_file *f, int *out) { (void)f; *out = 0; return SQLITE_OK; }
static int vf_fcntl(sqlite3_file *f, int op, void *arg) { (void)f; (void)op; (void)arg; return SQLITE_NOTFOUND; }
static int vf_sector(sqlite3_file *f) { (void)f; return 512; }
static int vf_devchar(sqlite3_file *f) { (void)f; return 0; }

static const sqlite3_io_methods vio = {
    1, vf_close, vf_read, vf_write, vf_truncate, vf_sync, vf_size, vf_lock, vf_lock, vf_check_lock,
    vf_fcntl, vf_sector, vf_devchar,
};

static int vfs_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *f, int flags, int *out) {
    (void)vfs;
    VFile *v = (VFile *)f;
    v->fd = -1;
    if (!name) return SQLITE_IOERR;
    int mode = (flags & SQLITE_OPEN_READWRITE) ? SCE_O_RDWR : SCE_O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE) mode |= SCE_O_CREAT;
    v->fd = sceIoOpen(name, mode, 0777);
    if (v->fd < 0) return SQLITE_CANTOPEN;
    v->base.pMethods = &vio;
    if (out) *out = flags;
    return SQLITE_OK;
}
static int vfs_delete(sqlite3_vfs *vfs, const char *name, int sync) {
    (void)vfs; (void)sync;
    if (sceIoRemove(name) < 0) {
        SceIoStat st;
        if (sceIoGetstat(name, &st) >= 0) return SQLITE_IOERR_DELETE; /* still there: say so */
    }
    return SQLITE_OK;
}
static int vfs_access(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
    (void)vfs; (void)flags;
    SceIoStat st;
    *out = sceIoGetstat(name, &st) >= 0;
    return SQLITE_OK;
}
static int vfs_full(sqlite3_vfs *vfs, const char *name, int n, char *out) {
    (void)vfs;
    snprintf(out, n, "%s", name);
    return SQLITE_OK;
}
static int vfs_random(sqlite3_vfs *vfs, int n, char *out) {
    (void)vfs;
    for (int i = 0; i < n; i++) out[i] = (char)(rand() & 255);
    return n;
}
static int vfs_sleep(sqlite3_vfs *vfs, int us) {
    (void)vfs;
    sceKernelDelayThread(us);
    return us;
}
static int vfs_time(sqlite3_vfs *vfs, double *out) {
    (void)vfs;
    *out = 2440587.5 + (double)time(NULL) / 86400.0;
    return SQLITE_OK;
}
static int vfs_err(sqlite3_vfs *vfs, int n, char *out) { (void)vfs; (void)n; (void)out; return 0; }

static sqlite3_vfs vita_vfs = {
    1, sizeof(VFile), 512, NULL, "nestvita", NULL, vfs_open, vfs_delete, vfs_access, vfs_full,
    NULL, NULL, NULL, NULL, vfs_random, vfs_sleep, vfs_time, vfs_err,
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&vita_vfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }

/* ================= helpers ================= */

int64_t now_s(void) { return (int64_t)time(NULL); }

int today_ymd(void) {
    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    return t.year * 10000 + t.month * 100 + t.day;
}

int db_exec(const char *sql) {
    if (!db) return -1;
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static sqlite3_stmt *prep(const char *sql) {
    sqlite3_stmt *s = NULL;
    if (!db || sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    return s;
}

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS titles(id INTEGER PRIMARY KEY, key TEXT UNIQUE, kind INT, secret INT,"
    " title TEXT, year INT, overview TEXT, genres TEXT, poster TEXT, backdrop TEXT, logo TEXT,"
    " u_title TEXT, u_poster TEXT, u_backdrop TEXT, u_logo TEXT, favorite INT DEFAULT 0, rating INT DEFAULT 0,"
    " score REAL DEFAULT 0, added INT, folder TEXT, present INT DEFAULT 1, u_secret INT DEFAULT -1, meta TEXT);"
    "CREATE TABLE IF NOT EXISTS entries(id INTEGER PRIMARY KEY, tid INT, season INT, number INT, number_end INT,"
    " name TEXT, path TEXT UNIQUE, size INT, playable INT, media INT);"
    "CREATE INDEX IF NOT EXISTS entries_tid ON entries(tid);"
    "CREATE TABLE IF NOT EXISTS progress(path TEXT PRIMARY KEY, pos INT, dur INT, finished INT, updated INT);"
    "CREATE TABLE IF NOT EXISTS activity(day INT, tid INT, kind INT, secret INT, seconds INT, PRIMARY KEY(day, tid));"
    "CREATE TABLE IF NOT EXISTS collections(id INTEGER PRIMARY KEY, name TEXT, secret INT);"
    "CREATE TABLE IF NOT EXISTS collection_items(cid INT, tid INT, PRIMARY KEY(cid, tid));"
    "CREATE TABLE IF NOT EXISTS folders(path TEXT PRIMARY KEY, secret INT, mode INT);"
    "CREATE TABLE IF NOT EXISTS settings(k TEXT PRIMARY KEY, v TEXT);"
    "CREATE TABLE IF NOT EXISTS searches(q TEXT PRIMARY KEY, t INT);"
    "CREATE TABLE IF NOT EXISTS excludes(path TEXT PRIMARY KEY);"
    "CREATE TABLE IF NOT EXISTS views(id INTEGER PRIMARY KEY, name TEXT, kind INT, sort INT, filter INT);"
    "CREATE TABLE IF NOT EXISTS found(key TEXT PRIMARY KEY, title TEXT, year INT, overview TEXT, genres TEXT,"
    " score REAL, poster TEXT, backdrop TEXT, logo TEXT);"
    "CREATE TABLE IF NOT EXISTS found_ep(path TEXT PRIMARY KEY, name TEXT);";

int db_open(void) {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DATA_DIR, 0777);
    if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "nestvita") != SQLITE_OK) {
        db = NULL;
        return -1;
    }
    /* normal locking + DELETE journal: the journal is removed after every save,
       so a leftover journal only ever means a save was really interrupted */
    db_exec("PRAGMA locking_mode=NORMAL; PRAGMA journal_mode=DELETE; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");
    db_exec(SCHEMA);
    /* added later: picture shape per video (fails harmlessly once the column exists) */
    sqlite3_exec(db, "ALTER TABLE entries ADD COLUMN aspect REAL DEFAULT 0", NULL, NULL, NULL);
    return 0;
}

void db_close(void) {
    if (db) sqlite3_close(db);
    db = NULL;
}

static char setting_buf[16][256];
static int setting_i;

const char *db_get(const char *k, const char *def) {
    sqlite3_stmt *s = prep("SELECT v FROM settings WHERE k=?");
    if (!s) return def;
    sqlite3_bind_text(s, 1, k, -1, SQLITE_STATIC);
    const char *r = def;
    if (sqlite3_step(s) == SQLITE_ROW) {
        char *b = setting_buf[setting_i++ & 15];
        snprintf(b, 256, "%s", (const char *)sqlite3_column_text(s, 0));
        r = b;
    }
    sqlite3_finalize(s);
    return r;
}

int db_geti(const char *k, int def) {
    const char *v = db_get(k, NULL);
    return v ? atoi(v) : def;
}

void db_set(const char *k, const char *v) {
    sqlite3_stmt *s = prep("INSERT OR REPLACE INTO settings(k, v) VALUES(?, ?)");
    if (!s) return;
    sqlite3_bind_text(s, 1, k, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, v, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_seti(const char *k, int v) {
    char b[24];
    snprintf(b, sizeof(b), "%d", v);
    db_set(k, b);
}

/* ================= import a scan ================= */

/* tiny reader for the flat nest.json written by tools/nest_meta.py */
static const char *json_key(const char *j, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* parse a JSON string at p (pointing at the opening quote) into out */
static const char *json_str(const char *p, char *out, size_t n) {
    if (!p || *p != '"') return NULL;
    p++;
    size_t k = 0;
    while (*p && *p != '"') {
        unsigned c = (unsigned char)*p++;
        if (c == 92 /* backslash */ && *p) {
            char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = ' ';
            else if (e == 'u' && p[0] && p[1] && p[2] && p[3]) {
                char hx[5] = {p[0], p[1], p[2], p[3], 0};
                unsigned u = (unsigned)strtoul(hx, NULL, 16);
                p += 4;
                if (u < 0x80) c = u;
                else if (u < 0x800) {
                    if (k + 2 < n) { out[k++] = (char)(0xC0 | u >> 6); out[k++] = (char)(0x80 | (u & 63)); }
                    continue;
                } else {
                    if (k + 3 < n) {
                        out[k++] = (char)(0xE0 | u >> 12);
                        out[k++] = (char)(0x80 | (u >> 6 & 63));
                        out[k++] = (char)(0x80 | (u & 63));
                    }
                    continue;
                }
            } else c = (unsigned char)e;
        }
        if (k + 1 < n) out[k++] = (char)c;
    }
    out[k] = 0;
    return *p == '"' ? p + 1 : p;
}

void db_read_meta(const char *path, char **overview, char **genres, float *score, char **title, int *year) {
    char *j = NULL;
    if (net_is_url(path)) {
        size_t nn = 0;
        j = net_read_all(path, &nn);
        if (!j) return;
    } else {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 256 * 1024) { fclose(f); return; }
    j = malloc(n + 1);
    if (!j) { fclose(f); return; }
    if (fread(j, 1, n, f) != (size_t)n) { free(j); fclose(f); return; }
    j[n] = 0;
    fclose(f);
    }
    static char buf[8192];
    const char *p;
    if ((p = json_key(j, "overview")) && json_str(p, buf, sizeof(buf)) && buf[0]) *overview = strdup(buf);
    if ((p = json_key(j, "title")) && json_str(p, buf, sizeof(buf)) && buf[0]) *title = strdup(buf);
    if ((p = json_key(j, "rating")) && (isdigit((unsigned char)*p) || *p == '-')) *score = strtof(p, NULL);
    if ((p = json_key(j, "year")) && isdigit((unsigned char)*p)) *year = atoi(p);
    if ((p = json_key(j, "genres")) && *p == '[') {
        char out[256] = "", one[64];
        p++;
        while (*p && *p != ']') {
            while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
            if (*p != '"') break;
            p = json_str(p, one, sizeof(one));
            if (!p) break;
            if (out[0]) strncat(out, ", ", sizeof(out) - strlen(out) - 1);
            strncat(out, one, sizeof(out) - strlen(out) - 1);
        }
        if (out[0]) *genres = strdup(out);
    }
    free(j);
}

int db_import(ScanResult *r, const char *folder) {
    if (!db) return -1;
    db_exec("BEGIN");
    /* titles from this folder that are not found again become "missing" */
    sqlite3_stmt *mark = prep("UPDATE titles SET present=0 WHERE folder=?");
    if (mark) {
        sqlite3_bind_text(mark, 1, folder, -1, SQLITE_STATIC);
        sqlite3_step(mark);
        sqlite3_finalize(mark);
    }
    sqlite3_stmt *up = prep(
        "INSERT INTO titles(key, kind, secret, title, year, overview, genres, poster, backdrop, logo, score, added,"
        " folder, present, meta) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,1,?14)"
        " ON CONFLICT(key) DO UPDATE SET title=?4, year=CASE WHEN ?5>0 THEN ?5 ELSE year END,"
        " overview=COALESCE(?6, overview), genres=COALESCE(?7, genres), poster=COALESCE(?8, poster),"
        " backdrop=COALESCE(?9, backdrop), logo=COALESCE(?10, logo), score=CASE WHEN ?11>0 THEN ?11 ELSE score END,"
        " folder=?13, present=1, meta=?14");
    sqlite3_stmt *getid = prep("SELECT id FROM titles WHERE key=?");
    sqlite3_stmt *ins = prep(
        "INSERT INTO entries(tid, season, number, number_end, name, path, size, playable, media, aspect)"
        " VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET tid=excluded.tid, season=excluded.season,"
        " number=excluded.number, number_end=excluded.number_end, name=excluded.name, size=excluded.size,"
        " playable=excluded.playable, media=excluded.media,"
        " aspect=CASE WHEN excluded.aspect > 0 THEN excluded.aspect ELSE entries.aspect END");
    sqlite3_stmt *known = prep("SELECT aspect FROM entries WHERE path=? AND size=? AND aspect > 0");
    if (!up || !getid || !ins) {
        db_exec("ROLLBACK");
        return -1;
    }
    int64_t now = now_s();
    for (int i = 0; i < r->nt; i++) {
        ScanTitle *t = &r->t[i];
        char *ov = NULL, *ge = NULL, *mt = NULL;
        float score = 0;
        int myear = 0;
        if (t->meta) db_read_meta(t->meta, &ov, &ge, &score, &mt, &myear);
        sqlite3_reset(up);
        sqlite3_bind_text(up, 1, t->key, -1, SQLITE_STATIC);
        sqlite3_bind_int(up, 2, t->kind);
        sqlite3_bind_int(up, 3, t->secret);
        sqlite3_bind_text(up, 4, mt ? mt : t->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(up, 5, myear ? myear : t->year);
        if (ov) sqlite3_bind_text(up, 6, ov, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(up, 6);
        if (ge) sqlite3_bind_text(up, 7, ge, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(up, 7);
        if (t->poster) sqlite3_bind_text(up, 8, t->poster, -1, SQLITE_STATIC); else sqlite3_bind_null(up, 8);
        if (t->backdrop) sqlite3_bind_text(up, 9, t->backdrop, -1, SQLITE_STATIC); else sqlite3_bind_null(up, 9);
        if (t->logo) sqlite3_bind_text(up, 10, t->logo, -1, SQLITE_STATIC); else sqlite3_bind_null(up, 10);
        sqlite3_bind_double(up, 11, score);
        sqlite3_bind_int64(up, 12, now);
        sqlite3_bind_text(up, 13, folder, -1, SQLITE_STATIC);
        if (t->meta) sqlite3_bind_text(up, 14, t->meta, -1, SQLITE_STATIC); else sqlite3_bind_null(up, 14);
        sqlite3_step(up);
        free(ov);
        free(ge);
        free(mt);
        sqlite3_reset(getid);
        sqlite3_bind_text(getid, 1, t->key, -1, SQLITE_STATIC);
        if (sqlite3_step(getid) != SQLITE_ROW) continue;
        int tid = sqlite3_column_int(getid, 0);
        for (int k = 0; k < t->ne; k++) {
            ScanEntry *e = &t->e[k];
            sqlite3_reset(ins);
            sqlite3_bind_int(ins, 1, tid);
            sqlite3_bind_int(ins, 2, e->season);
            sqlite3_bind_int(ins, 3, e->number);
            sqlite3_bind_int(ins, 4, e->number_end);
            sqlite3_bind_text(ins, 5, e->name, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 6, e->path, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ins, 7, e->size);
            sqlite3_bind_int(ins, 8, e->playable);
            sqlite3_bind_int(ins, 9, e->media);
            /* the picture shape, read once per file (files on the card only: the PC is probed at play) */
            double asp = 0;
            if (e->media == MEDIA_VIDEO && e->playable && !net_is_url(e->path)) {
                if (known) {
                    sqlite3_reset(known);
                    sqlite3_bind_text(known, 1, e->path, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(known, 2, e->size);
                    if (sqlite3_step(known) == SQLITE_ROW) asp = sqlite3_column_double(known, 0);
                }
                if (asp <= 0) asp = media_aspect(e->path);
            }
            sqlite3_bind_double(ins, 10, asp);
            sqlite3_step(ins);
        }
    }
    sqlite3_finalize(up);
    sqlite3_finalize(getid);
    sqlite3_finalize(ins);
    if (known) sqlite3_finalize(known);
    /* drop entries whose files vanished from this folder, then empty titles */
    sqlite3_stmt *gone = prep("DELETE FROM entries WHERE tid IN (SELECT id FROM titles WHERE folder=? AND present=0)");
    if (gone) {
        sqlite3_bind_text(gone, 1, folder, -1, SQLITE_STATIC);
        sqlite3_step(gone);
        sqlite3_finalize(gone);
    }
    db_exec("DELETE FROM titles WHERE present=0 AND id NOT IN (SELECT DISTINCT tid FROM entries)");
    db_found_apply(); /* info found with "Find info in the folder" outlives rescans */
    db_exec("COMMIT");
    return 0;
}

/* ================= folders ================= */

void db_add_folder(const char *path, int secret, int mode) {
    sqlite3_stmt *s = prep("INSERT OR REPLACE INTO folders(path, secret, mode) VALUES(?,?,?)");
    if (!s) return;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, secret);
    sqlite3_bind_int(s, 3, mode);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_delete_entry(const char *path) {
    const char *q[2] = {"DELETE FROM entries WHERE path=?", "DELETE FROM progress WHERE path=?"};
    for (int i = 0; i < 2; i++) {
        sqlite3_stmt *s = prep(q[i]);
        if (!s) continue;
        sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

void db_delete_title(int id) {
    const char *q[3] = {"DELETE FROM entries WHERE tid=?", "DELETE FROM collection_items WHERE tid=?",
                        "DELETE FROM titles WHERE id=?"};
    for (int i = 0; i < 3; i++) {
        sqlite3_stmt *s = prep(q[i]);
        if (!s) continue;
        sqlite3_bind_int(s, 1, id);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

void db_title_folder(int id, char *out, int n) {
    out[0] = 0;
    sqlite3_stmt *s = prep("SELECT folder FROM titles WHERE id=?");
    if (!s) return;
    sqlite3_bind_int(s, 1, id);
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_text(s, 0))
        snprintf(out, n, "%s", (const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
}

void db_remove_folder(const char *path) {
    sqlite3_stmt *s = prep("DELETE FROM folders WHERE path=?");
    if (s) {
        sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    s = prep("DELETE FROM entries WHERE tid IN (SELECT id FROM titles WHERE folder=?)");
    if (s) {
        sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    s = prep("DELETE FROM titles WHERE folder=?");
    if (s) {
        sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

int db_folders(char paths[][512], int *secret, int *mode, int max) {
    sqlite3_stmt *s = prep("SELECT path, secret, mode FROM folders ORDER BY secret, path");
    if (!s) return 0;
    int n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        snprintf(paths[n], 512, "%s", (const char *)sqlite3_column_text(s, 0));
        if (secret) secret[n] = sqlite3_column_int(s, 1);
        if (mode) mode[n] = sqlite3_column_int(s, 2);
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

/* ================= progress / activity ================= */

void db_save_progress(const char *path, int pos, int dur, int finished) {
    sqlite3_stmt *s = prep("INSERT OR REPLACE INTO progress(path, pos, dur, finished, updated) VALUES(?,?,?,?,?)");
    if (!s) return;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, pos);
    sqlite3_bind_int(s, 3, dur);
    sqlite3_bind_int(s, 4, finished);
    sqlite3_bind_int64(s, 5, now_s());
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_add_activity(int tid, int kind, int secret, int seconds) {
    if (seconds <= 0) return;
    sqlite3_stmt *s = prep(
        "INSERT INTO activity(day, tid, kind, secret, seconds) VALUES(?,?,?,?,?)"
        " ON CONFLICT(day, tid) DO UPDATE SET seconds = seconds + excluded.seconds");
    if (!s) return;
    sqlite3_bind_int(s, 1, today_ymd());
    sqlite3_bind_int(s, 2, tid);
    sqlite3_bind_int(s, 3, kind);
    sqlite3_bind_int(s, 4, secret);
    sqlite3_bind_int(s, 5, seconds);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

int db_activity_days(int secret, int *days, int *secs, int max) {
    sqlite3_stmt *s = prep("SELECT day, SUM(seconds) FROM activity WHERE secret<=? GROUP BY day ORDER BY day DESC");
    if (!s) return 0;
    sqlite3_bind_int(s, 1, secret);
    int n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        days[n] = sqlite3_column_int(s, 0);
        secs[n] = sqlite3_column_int(s, 1);
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

static int ymd_to_days(int ymd) {
    int y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    /* days from civil (Howard Hinnant) */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void db_stats(int secret, int *video_secs, int *read_secs, int *streak, int *best_streak, int *finished) {
    *video_secs = *read_secs = *streak = *best_streak = *finished = 0;
    sqlite3_stmt *s = prep("SELECT kind, SUM(seconds) FROM activity WHERE secret<=? GROUP BY kind");
    if (s) {
        sqlite3_bind_int(s, 1, secret);
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (sqlite3_column_int(s, 0) == KIND_BOOK) *read_secs += sqlite3_column_int(s, 1);
            else *video_secs += sqlite3_column_int(s, 1);
        }
        sqlite3_finalize(s);
    }
    int days[400], secs[400];
    int n = db_activity_days(secret, days, secs, 400);
    int today = ymd_to_days(today_ymd());
    int run = 0, best = 0, prev = 0, cur_streak = 0;
    for (int i = 0; i < n; i++) {
        int d = ymd_to_days(days[i]);
        if (i == 0 || prev - d != 1) run = 1;
        else run++;
        if (run > best) best = run;
        if (i == 0 && today - d <= 1) cur_streak = 1;
        else if (cur_streak == run - 1 && cur_streak > 0 && prev - d == 1) cur_streak = run;
        prev = d;
    }
    *streak = cur_streak;
    *best_streak = best;
    s = prep("SELECT COUNT(*) FROM progress WHERE finished=1");
    if (s) {
        if (sqlite3_step(s) == SQLITE_ROW) *finished = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
}

int db_views(int *ids, char names[][64], int *kinds, int *sorts, int *filters, int max) {
    sqlite3_stmt *s = prep("SELECT id, name, kind, sort, filter FROM views ORDER BY id");
    if (!s) return 0;
    int n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        ids[n] = sqlite3_column_int(s, 0);
        snprintf(names[n], 64, "%s", (const char *)sqlite3_column_text(s, 1));
        kinds[n] = sqlite3_column_int(s, 2);
        sorts[n] = sqlite3_column_int(s, 3);
        filters[n] = sqlite3_column_int(s, 4);
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

void db_view_create(const char *name, int kind, int sort, int filter) {
    sqlite3_stmt *s = prep("INSERT INTO views(name, kind, sort, filter) VALUES(?,?,?,?)");
    if (!s) return;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, kind);
    sqlite3_bind_int(s, 3, sort);
    sqlite3_bind_int(s, 4, filter);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_view_delete(int id) {
    sqlite3_stmt *s = prep("DELETE FROM views WHERE id=?");
    if (!s) return;
    sqlite3_bind_int(s, 1, id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_clear_library(void) {
    db_exec("BEGIN; DELETE FROM entries; DELETE FROM titles; DELETE FROM collection_items; DELETE FROM folders;"
            " DELETE FROM settings WHERE k='last_pick'; COMMIT;");
}

int db_excludes(char out[][512], int max) {
    sqlite3_stmt *s = prep("SELECT path FROM excludes ORDER BY path");
    if (!s) return 0;
    int n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) snprintf(out[n++], 512, "%s", (const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    return n;
}

void db_add_exclude(const char *path) {
    sqlite3_stmt *s = prep("INSERT OR IGNORE INTO excludes(path) VALUES(?)");
    if (!s) return;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_remove_exclude(const char *path) {
    sqlite3_stmt *s = prep("DELETE FROM excludes WHERE path=?");
    if (!s) return;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void scan_load_excludes(void) {
    static char list[64][512];
    int n = db_excludes(list, 64);
    scan_set_excludes(list, n);
}

void db_clear_history(void) { db_exec("DELETE FROM activity; DELETE FROM progress; DELETE FROM searches;"); }

/* ================= title edits ================= */

void db_title_set_int(int id, const char *field, int v) {
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE titles SET %s=? WHERE id=?", field);
    sqlite3_stmt *s = prep(sql);
    if (!s) return;
    sqlite3_bind_int(s, 1, v);
    sqlite3_bind_int(s, 2, id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_title_set_str(int id, const char *field, const char *v) {
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE titles SET %s=? WHERE id=?", field);
    sqlite3_stmt *s = prep(sql);
    if (!s) return;
    if (v) sqlite3_bind_text(s, 1, v, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 1);
    sqlite3_bind_int(s, 2, id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* ================= collections ================= */

int db_collections(int secret, int *ids, char names[][64], int max) {
    sqlite3_stmt *s = prep("SELECT id, name FROM collections WHERE secret=? ORDER BY name");
    if (!s) return 0;
    sqlite3_bind_int(s, 1, secret);
    int n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        ids[n] = sqlite3_column_int(s, 0);
        snprintf(names[n], 64, "%s", (const char *)sqlite3_column_text(s, 1));
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

int db_collection_items(int cid, int *tids, int max) {
    sqlite3_stmt *s = prep("SELECT tid FROM collection_items WHERE cid=?");
    if (!s) return 0;
    sqlite3_bind_int(s, 1, cid);
    int n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) tids[n++] = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

int db_collection_create(const char *name, int secret) {
    sqlite3_stmt *s = prep("INSERT INTO collections(name, secret) VALUES(?,?)");
    if (!s) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, secret);
    sqlite3_step(s);
    sqlite3_finalize(s);
    return (int)sqlite3_last_insert_rowid(db);
}

int db_collection_has(int cid, int tid) {
    sqlite3_stmt *s = prep("SELECT 1 FROM collection_items WHERE cid=? AND tid=?");
    if (!s) return 0;
    sqlite3_bind_int(s, 1, cid);
    sqlite3_bind_int(s, 2, tid);
    int r = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return r;
}

void db_collection_toggle(int cid, int tid) {
    int has = db_collection_has(cid, tid);
    sqlite3_stmt *s = prep(has ? "DELETE FROM collection_items WHERE cid=? AND tid=?"
                               : "INSERT INTO collection_items(cid, tid) VALUES(?,?)");
    if (!s) return;
    sqlite3_bind_int(s, 1, cid);
    sqlite3_bind_int(s, 2, tid);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_collection_delete(int cid) {
    sqlite3_stmt *s = prep("DELETE FROM collection_items WHERE cid=?");
    if (s) { sqlite3_bind_int(s, 1, cid); sqlite3_step(s); sqlite3_finalize(s); }
    s = prep("DELETE FROM collections WHERE id=?");
    if (s) { sqlite3_bind_int(s, 1, cid); sqlite3_step(s); sqlite3_finalize(s); }
}

/* ================= searches ================= */

void db_search_add(const char *q) {
    if (S.private_session || !q || !*q) return;
    sqlite3_stmt *s = prep("INSERT OR REPLACE INTO searches(q, t) VALUES(?,?)");
    if (!s) return;
    sqlite3_bind_text(s, 1, q, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, now_s());
    sqlite3_step(s);
    sqlite3_finalize(s);
    db_exec("DELETE FROM searches WHERE q NOT IN (SELECT q FROM searches ORDER BY t DESC LIMIT 12)");
}

int db_search_recent(char out[][64], int max) {
    sqlite3_stmt *s = prep("SELECT q FROM searches ORDER BY t DESC LIMIT 12");
    if (!s) return 0;
    int n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        snprintf(out[n], 64, "%s", (const char *)sqlite3_column_text(s, 0));
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

/* ================= library loading (used by lib.c) ================= */

sqlite3 *db_handle(void) { return db; }

/* ================= info found in the folder ================= */

static void bind_opt(sqlite3_stmt *s, int i, const char *v) {
    if (v) sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(s, i);
}

void db_found_save(const char *key, const char *title, int year, const char *overview, const char *genres,
                   float score, const char *poster, const char *backdrop, const char *logo) {
    if (!key || !*key) return;
    /* a new search only adds to (or replaces) what an earlier one found */
    sqlite3_stmt *s = prep(
        "INSERT INTO found(key, title, year, overview, genres, score, poster, backdrop, logo)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) ON CONFLICT(key) DO UPDATE SET"
        " title=COALESCE(?2, title), year=CASE WHEN ?3>0 THEN ?3 ELSE year END, overview=COALESCE(?4, overview),"
        " genres=COALESCE(?5, genres), score=CASE WHEN ?6>0 THEN ?6 ELSE score END,"
        " poster=COALESCE(?7, poster), backdrop=COALESCE(?8, backdrop), logo=COALESCE(?9, logo)");
    if (!s) return;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    bind_opt(s, 2, title);
    sqlite3_bind_int(s, 3, year);
    bind_opt(s, 4, overview);
    bind_opt(s, 5, genres);
    sqlite3_bind_double(s, 6, score);
    bind_opt(s, 7, poster);
    bind_opt(s, 8, backdrop);
    bind_opt(s, 9, logo);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_found_episode(const char *path, const char *name) {
    sqlite3_stmt *s = prep("INSERT OR REPLACE INTO found_ep(path, name) VALUES(?,?)");
    if (!s) return;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void db_found_apply(void) {
    db_exec("UPDATE titles SET"
            " title=COALESCE((SELECT title FROM found f WHERE f.key=titles.key), title),"
            " year=COALESCE((SELECT year FROM found f WHERE f.key=titles.key AND f.year>0), year),"
            " overview=COALESCE((SELECT overview FROM found f WHERE f.key=titles.key), overview),"
            " genres=COALESCE((SELECT genres FROM found f WHERE f.key=titles.key), genres),"
            " score=COALESCE((SELECT score FROM found f WHERE f.key=titles.key AND f.score>0), score),"
            " poster=COALESCE((SELECT poster FROM found f WHERE f.key=titles.key), poster),"
            " backdrop=COALESCE((SELECT backdrop FROM found f WHERE f.key=titles.key), backdrop),"
            " logo=COALESCE((SELECT logo FROM found f WHERE f.key=titles.key), logo)"
            " WHERE key IN (SELECT key FROM found)");
    db_exec("UPDATE entries SET name=(SELECT name FROM found_ep f WHERE f.path=entries.path)"
            " WHERE path IN (SELECT path FROM found_ep)");
}
