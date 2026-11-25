/* Nest port: threads, time, files and the little system calls, on SDL2 + POSIX.
   Vita paths are mapped here:
     app0:...           the app's own files (fonts, scenes)
     ux0:data/Nest/...  Nest's private data (library database, caches)
     ux0:music          Android's Music folder
     ux0: / uma0: ...   internal storage / SD cards, for your videos, books and music */
#include "port.h"
#undef fopen
#undef main

#include <SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif

#include "psp2_all.h"

/* ================================================================ paths */

static char asset_root[512], data_root[512], music_root[512];
static char dev_root[6][512];
static const char *DEV_IDS[6] = {"ux0", "uma0", "imc0", "xmc0", "grw0", "ur0"};
static int paths_ready;

static void mkdirs(const char *p) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", p);
    for (char *c = tmp + 1; *c; c++) {
        if (*c == '/' || *c == '\\') {
            char s = *c;
            *c = 0;
#ifdef _WIN32
            mkdir(tmp);
#else
            mkdir(tmp, 0777);
#endif
            *c = s;
        }
    }
#ifdef _WIN32
    mkdir(tmp);
#else
    mkdir(tmp, 0777);
#endif
}

void port_paths_init(void) {
    if (paths_ready) return;
    paths_ready = 1;
#ifdef __ANDROID__
    const char *internal = SDL_AndroidGetInternalStoragePath(); /* private to Nest */
    snprintf(asset_root, sizeof(asset_root), "%s/app0", internal ? internal : "/data/local/tmp");
    snprintf(data_root, sizeof(data_root), "%s/data", internal ? internal : "/data/local/tmp");
    snprintf(dev_root[0], sizeof(dev_root[0]), "/storage/emulated/0");
    snprintf(music_root, sizeof(music_root), "/storage/emulated/0/Music");
    /* SD cards: /storage/XXXX-XXXX */
    DIR *d = opendir("/storage");
    int k = 1;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && k < 4) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "emulated") || !strcmp(e->d_name, "self")) continue;
            snprintf(dev_root[k++], sizeof(dev_root[0]), "/storage/%s", e->d_name);
        }
        closedir(d);
    }
#else
    /* a PC: the assets sit next to the program; ux0: is a folder you can point anywhere */
    char *base = SDL_GetBasePath();
    snprintf(asset_root, sizeof(asset_root), "%s", base ? base : "./");
    SDL_free(base);
    const char *nd = getenv("NEST_DATA"); /* testing: keep the library somewhere specific */
    if (nd) snprintf(data_root, sizeof(data_root), "%s", nd);
    else {
        char *pref = SDL_GetPrefPath("chromevoid", "Nest");
        snprintf(data_root, sizeof(data_root), "%sdata", pref ? pref : "./");
        SDL_free(pref);
    }
    const char *ux = getenv("NEST_UX0");
    snprintf(dev_root[0], sizeof(dev_root[0]), "%s", ux ? ux : asset_root);
    const char *um = getenv("NEST_UMA0");
    if (um) snprintf(dev_root[1], sizeof(dev_root[1]), "%s", um);
    snprintf(music_root, sizeof(music_root), "%s/music", dev_root[0]);
#endif
    /* no trailing slashes */
    char *roots[] = {asset_root, data_root, music_root, dev_root[0], dev_root[1], dev_root[2], dev_root[3]};
    for (int i = 0; i < 7; i++) {
        size_t l = strlen(roots[i]);
        while (l > 1 && (roots[i][l - 1] == '/' || roots[i][l - 1] == '\\')) roots[i][--l] = 0;
    }
    mkdirs(data_root);
}

const char *port_asset_root(void) {
    port_paths_init();
    return asset_root;
}

static int prefix_ci(const char *s, const char *p) {
    size_t l = strlen(p);
    return !strncasecmp(s, p, l);
}

const char *port_path(const char *in, char *out, size_t n) {
    port_paths_init();
    if (!in) { out[0] = 0; return out; }
    if (!strncmp(in, "http://", 7) || !strncmp(in, "https://", 8)) { snprintf(out, n, "%s", in); return out; }
    const char *colon = strchr(in, ':');
    const char *slash = strchr(in, '/');
    if (!colon || (slash && slash < colon) || colon - in > 5) { snprintf(out, n, "%s", in); return out; } /* already a real path */
    const char *rest = colon + 1;
    while (*rest == '/') rest++;
    char dev[8];
    snprintf(dev, sizeof(dev), "%.*s", (int)(colon - in), in);
    if (!strcasecmp(dev, "app0")) {
        snprintf(out, n, "%s/%s", asset_root, rest);
        return out;
    }
    if (!strcasecmp(dev, "ux0") && prefix_ci(rest, "data/Nest") && (rest[9] == 0 || rest[9] == '/')) {
        snprintf(out, n, "%s%s", data_root, rest + 9);
        return out;
    }
    if (!strcasecmp(dev, "ux0") && prefix_ci(rest, "music") && (rest[5] == 0 || rest[5] == '/')) {
        snprintf(out, n, "%s%s", music_root, rest + 5);
        return out;
    }
    for (int i = 0; i < 6; i++)
        if (!strcasecmp(dev, DEV_IDS[i])) {
            if (!dev_root[i][0]) break;
            if (*rest) snprintf(out, n, "%s/%s", dev_root[i], rest);
            else snprintf(out, n, "%s", dev_root[i]);
            return out;
        }
    snprintf(out, n, "/nonexistent/%s", rest); /* a device this one doesn't have */
    return out;
}

FILE *port_fopen(const char *path, const char *mode) {
    char p[1024];
    port_path(path, p, sizeof(p));
    return fopen(p, mode);
}

int port_devices(const char **ids, const char **names, int max) {
    port_paths_init();
    static const char *nm[6] = {"Internal storage", "SD card", "SD card 2", "Storage 3", "Storage 4", "Storage 5"};
#ifndef __ANDROID__
    nm[0] = "This PC (NEST_UX0)";
#endif
    int n = 0;
    for (int i = 0; i < 6 && n < max; i++)
        if (dev_root[i][0]) {
            static char idbuf[6][8];
            snprintf(idbuf[i], sizeof(idbuf[i]), "%s:", DEV_IDS[i]);
            ids[n] = idbuf[i];
            names[n] = nm[i];
            n++;
        }
    return n;
}

/* ================================================================ threads */

typedef struct {
    int used;
    SceKernelThreadEntry entry;
    SDL_Thread *th;
    volatile int done;
    SceSize arglen;
    void *argp;
    char name[32];
} PThread;
#define MAXTH 64
static PThread threads[MAXTH];
static SDL_mutex *th_lock;
static SDL_TLSID tls_self;

static void th_init(void) {
    if (!th_lock) {
        th_lock = SDL_CreateMutex();
        tls_self = SDL_TLSCreate();
    }
}

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int prio, SceSize stack, SceUInt attr, int cpu, const void *opt) {
    (void)prio; (void)stack; (void)attr; (void)cpu; (void)opt;
    th_init();
    SDL_LockMutex(th_lock);
    for (int i = 1; i < MAXTH; i++)
        if (!threads[i].used) {
            memset(&threads[i], 0, sizeof(PThread));
            threads[i].used = 1;
            threads[i].entry = entry;
            snprintf(threads[i].name, sizeof(threads[i].name), "%s", name ? name : "nest");
            SDL_UnlockMutex(th_lock);
            return i;
        }
    SDL_UnlockMutex(th_lock);
    return -1;
}

static int th_main(void *p) {
    PThread *t = p;
    SDL_TLSSet(tls_self, t, NULL);
    int r = t->entry(t->arglen, t->argp);
    if (t->arglen && t->argp) free(t->argp);
    t->done = 1;
    t->used = 0;
    return r;
}

int sceKernelStartThread(SceUID id, SceSize arglen, void *argp) {
    if (id <= 0 || id >= MAXTH || !threads[id].used) return -1;
    PThread *t = &threads[id];
    /* the Vita copies the arguments: so do we */
    if (arglen && argp) {
        t->argp = malloc(arglen);
        memcpy(t->argp, argp, arglen);
    } else t->argp = argp;
    t->arglen = arglen;
    t->th = SDL_CreateThreadWithStackSize(th_main, t->name, 1024 * 1024, t);
    if (!t->th) return -1;
    SDL_DetachThread(t->th);
    return 0;
}

int sceKernelExitDeleteThread(int status) {
    PThread *t = SDL_TLSGet(tls_self);
    if (t) {
        t->done = 1;
        t->used = 0; /* the slot is free again */
    }
    return status;
}

int sceKernelWaitThreadEnd(SceUID id, int *stat, SceUInt *timeout) {
    if (id <= 0 || id >= MAXTH) return -1;
    Uint64 start = SDL_GetTicks64();
    while (threads[id].used && !threads[id].done) {
        if (timeout && (SDL_GetTicks64() - start) * 1000 >= *timeout) return 0x80028005; /* timed out */
        SDL_Delay(1);
    }
    if (stat) *stat = 0;
    return 0;
}

int sceKernelDeleteThread(SceUID id) {
    if (id > 0 && id < MAXTH) threads[id].used = 0;
    return 0;
}

int sceKernelDelayThread(SceUInt us) {
    if (us >= 1000) SDL_Delay(us / 1000);
    else SDL_Delay(us ? 1 : 0);
    return 0;
}

/* mutexes and semaphores live in small tables, so their ids look like the Vita's */
static SDL_mutex *mutexes[128];
static SDL_sem *semas[64];

SceUID sceKernelCreateMutex(const char *name, SceUInt attr, int init, void *opt) {
    (void)name; (void)attr; (void)opt;
    th_init();
    SDL_LockMutex(th_lock);
    for (int i = 1; i < 128; i++)
        if (!mutexes[i]) {
            mutexes[i] = SDL_CreateMutex();
            SDL_UnlockMutex(th_lock);
            if (init) SDL_LockMutex(mutexes[i]);
            return i;
        }
    SDL_UnlockMutex(th_lock);
    return -1;
}
int sceKernelLockMutex(SceUID id, int count, SceUInt *timeout) {
    (void)count; (void)timeout;
    if (id <= 0 || id >= 128 || !mutexes[id]) return -1;
    return SDL_LockMutex(mutexes[id]);
}
int sceKernelUnlockMutex(SceUID id, int count) {
    (void)count;
    if (id <= 0 || id >= 128 || !mutexes[id]) return -1;
    return SDL_UnlockMutex(mutexes[id]);
}
SceUID sceKernelCreateSema(const char *name, SceUInt attr, int init, int max, void *opt) {
    (void)name; (void)attr; (void)max; (void)opt;
    th_init();
    SDL_LockMutex(th_lock);
    for (int i = 1; i < 64; i++)
        if (!semas[i]) {
            semas[i] = SDL_CreateSemaphore(init);
            SDL_UnlockMutex(th_lock);
            return i;
        }
    SDL_UnlockMutex(th_lock);
    return -1;
}
int sceKernelWaitSema(SceUID id, int signal, SceUInt *timeout) {
    if (id <= 0 || id >= 64 || !semas[id]) return -1;
    for (int k = 0; k < signal; k++) {
        int r = timeout ? SDL_SemWaitTimeout(semas[id], *timeout / 1000 + 1) : SDL_SemWait(semas[id]);
        if (r != 0) return 0x80028005;
    }
    return 0;
}
int sceKernelSignalSema(SceUID id, int signal) {
    if (id <= 0 || id >= 64 || !semas[id]) return -1;
    for (int k = 0; k < signal; k++) SDL_SemPost(semas[id]);
    return 0;
}

SceUInt64 sceKernelGetProcessTimeWide(void) {
    static Uint64 freq, start;
    if (!freq) {
        freq = SDL_GetPerformanceFrequency();
        start = SDL_GetPerformanceCounter();
    }
    Uint64 c = SDL_GetPerformanceCounter() - start;
    return (SceUInt64)((double)c * 1000000.0 / (double)freq);
}

int sceKernelExitProcess(int res) {
    SDL_Quit();
    exit(res);
    return 0;
}

/* keep the screen awake while something asks for it (watching, reading); let it sleep otherwise */
static Uint64 last_awake;
int sceKernelPowerTick(int type) {
    if (type == SCE_KERNEL_POWER_TICK_DEFAULT) {
        if (!last_awake) SDL_DisableScreenSaver();
        last_awake = SDL_GetTicks64();
    }
    return 0;
}
void port_power_frame(void) {
    if (last_awake && SDL_GetTicks64() - last_awake > 30000) {
        SDL_EnableScreenSaver();
        last_awake = 0;
    }
}

/* ================================================================ memory blocks */

typedef struct { void *p; SceSize size; } Blk;
static Blk blocks[256];
SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt *opt) {
    (void)name; (void)type; (void)opt;
    for (int i = 1; i < 256; i++)
        if (!blocks[i].p) {
            blocks[i].p = calloc(1, size);
            if (!blocks[i].p) return -1;
            blocks[i].size = size;
            return i;
        }
    return -1;
}
int sceKernelFreeMemBlock(SceUID uid) {
    if (uid <= 0 || uid >= 256) return -1;
    free(blocks[uid].p);
    blocks[uid].p = NULL;
    return 0;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base) {
    if (uid <= 0 || uid >= 256 || !blocks[uid].p) return -1;
    *base = blocks[uid].p;
    return 0;
}
SceUID sceKernelFindMemBlockByAddr(const void *addr, SceSize size) {
    (void)size;
    for (int i = 1; i < 256; i++)
        if (blocks[i].p && (const char *)addr >= (char *)blocks[i].p && (const char *)addr < (char *)blocks[i].p + blocks[i].size) return i;
    return -1;
}
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info) {
    info->size_user = 512 * 1024 * 1024;
    info->size_cdram = 256 * 1024 * 1024;
    info->size_phycont = 64 * 1024 * 1024;
    return 0;
}
int sceGxmMapMemory(void *base, SceSize size, int attr) { (void)base; (void)size; (void)attr; return 0; }
int sceGxmUnmapMemory(void *base) { (void)base; return 0; }

/* ================================================================ files */

static void to_dt(time_t t, SceDateTime *d) {
    struct tm tmv, *tm;
#ifdef _WIN32
    tm = localtime(&t);
    if (tm) tmv = *tm;
    tm = tm ? &tmv : NULL;
#else
    tm = localtime_r(&t, &tmv);
#endif
    memset(d, 0, sizeof(*d));
    if (!tm) return;
    d->year = tm->tm_year + 1900;
    d->month = tm->tm_mon + 1;
    d->day = tm->tm_mday;
    d->hour = tm->tm_hour;
    d->minute = tm->tm_min;
    d->second = tm->tm_sec;
}

static void fill_stat(const struct stat *s, SceIoStat *o) {
    memset(o, 0, sizeof(*o));
    o->st_mode = S_ISDIR(s->st_mode) ? SCE_S_IFDIR | 0777 : SCE_S_IFREG | 0666;
    o->st_size = s->st_size;
#ifdef __ANDROID__
    to_dt(s->st_mtim.tv_sec, &o->st_mtime);
    to_dt(s->st_mtim.tv_sec, &o->st_ctime);
    to_dt(s->st_atim.tv_sec, &o->st_atime);
#else
    to_dt(s->st_mtime, &o->st_mtime);
    to_dt(s->st_mtime, &o->st_ctime);
    to_dt(s->st_atime, &o->st_atime);
#endif
}

static int oflags(int f) {
    int r = 0;
    int acc = f & 3;
    r = acc == SCE_O_RDONLY ? O_RDONLY : acc == SCE_O_WRONLY ? O_WRONLY : O_RDWR;
    if (f & SCE_O_CREAT) r |= O_CREAT;
    if (f & SCE_O_TRUNC) r |= O_TRUNC;
    if (f & SCE_O_APPEND) r |= O_APPEND;
    if (f & SCE_O_EXCL) r |= O_EXCL;
#ifdef O_BINARY
    r |= O_BINARY;
#endif
    return r;
}

SceUID sceIoOpen(const char *file, int flags, int mode) {
    char p[1024];
    port_path(file, p, sizeof(p));
    int fd = open(p, oflags(flags), mode ? mode : 0666);
    return fd >= 0 ? fd + 1 : (SceUID)0x80010002; /* the Vita's "not found" */
}
int sceIoClose(SceUID fd) { return close(fd - 1); }
int sceIoRead(SceUID fd, void *data, SceSize size) { return (int)read(fd - 1, data, size); }
int sceIoWrite(SceUID fd, const void *data, SceSize size) { return (int)write(fd - 1, data, size); }
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence) {
#ifdef _WIN32
    return _lseeki64(fd - 1, offset, whence);
#else
    return lseek(fd - 1, offset, whence);
#endif
}
int sceIoSyncByFd(SceUID fd, int flag) {
    (void)flag;
#ifdef _WIN32
    return _commit(fd - 1);
#else
    return fsync(fd - 1);
#endif
}
int sceIoChstatByFd(SceUID fd, const SceIoStat *st, unsigned int bits) {
    if (bits & SCE_CST_SIZE) {
#ifdef _WIN32
        return _chsize_s(fd - 1, st->st_size) == 0 ? 0 : -1;
#else
        return ftruncate(fd - 1, st->st_size);
#endif
    }
    return 0;
}

typedef struct { DIR *d; char path[1024]; } PDir;
static PDir dirs[64];
SceUID sceIoDopen(const char *dirname) {
    char p[1024];
    port_path(dirname, p, sizeof(p));
    DIR *d = opendir(p);
    if (!d) return (SceUID)0x80010002;
    for (int i = 1; i < 64; i++)
        if (!dirs[i].d) {
            dirs[i].d = d;
            snprintf(dirs[i].path, sizeof(dirs[i].path), "%s", p);
            return i;
        }
    closedir(d);
    return -1;
}
int sceIoDread(SceUID fd, SceIoDirent *ent) {
    if (fd <= 0 || fd >= 64 || !dirs[fd].d) return -1;
    struct dirent *e = readdir(dirs[fd].d);
    if (!e) return 0;
    memset(ent, 0, sizeof(*ent));
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    char full[1300];
    snprintf(full, sizeof(full), "%s/%s", dirs[fd].path, e->d_name);
    struct stat s;
    if (stat(full, &s) == 0) fill_stat(&s, &ent->d_stat);
    else ent->d_stat.st_mode = SCE_S_IFREG;
    return 1;
}
int sceIoDclose(SceUID fd) {
    if (fd <= 0 || fd >= 64 || !dirs[fd].d) return -1;
    closedir(dirs[fd].d);
    dirs[fd].d = NULL;
    return 0;
}

int sceIoGetstat(const char *file, SceIoStat *st) {
    /* Nest checks for the Vita's video module before making thumbnails: Android always has a decoder */
    if (!strncasecmp(file, "vs0:", 4)) {
#ifdef __ANDROID__
        memset(st, 0, sizeof(*st));
        st->st_mode = SCE_S_IFREG;
        return 0;
#else
        return (int)0x80010002;
#endif
    }
    char p[1024];
    port_path(file, p, sizeof(p));
    size_t l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
    struct stat s;
    if (stat(p, &s) != 0) return (int)0x80010002;
    fill_stat(&s, st);
    return 0;
}
int sceIoMkdir(const char *dir, int mode) {
    (void)mode;
    char p[1024];
    port_path(dir, p, sizeof(p));
#ifdef _WIN32
    return mkdir(p) == 0 ? 0 : -1;
#else
    return mkdir(p, 0777) == 0 ? 0 : -1;
#endif
}
int sceIoRmdir(const char *path) {
    char p[1024];
    port_path(path, p, sizeof(p));
    return rmdir(p) == 0 ? 0 : -1;
}
int sceIoRemove(const char *file) {
    char p[1024];
    port_path(file, p, sizeof(p));
    return remove(p) == 0 ? 0 : (int)0x80010002;
}
int sceIoRename(const char *a, const char *b) {
    char p[1024], q[1024];
    port_path(a, p, sizeof(p));
    port_path(b, q, sizeof(q));
#ifdef _WIN32
    remove(q);
#endif
    return rename(p, q) == 0 ? 0 : -1;
}
int sceIoDevctl(const char *dev, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen) {
    (void)indata; (void)inlen;
    if (cmd != 0x3001 || outlen < 16) return -1;
    char p[1024];
    port_path(dev, p, sizeof(p));
    uint64_t *o = outdata;
#ifdef _WIN32
    o[0] = 0;
    o[1] = 0;
    return -1;
#else
    struct statvfs v;
    if (statvfs(p, &v) != 0) return -1;
    o[0] = (uint64_t)v.f_blocks * v.f_frsize;
    o[1] = (uint64_t)v.f_bavail * v.f_frsize;
    return 0;
#endif
}

int sceRtcGetCurrentClockLocalTime(SceDateTime *t) {
    to_dt(time(NULL), t);
    return 0;
}

/* ================================================================ small things */

int sceSysmoduleLoadModule(int id) { (void)id; return 0; }
int sceAppUtilInit(SceAppUtilInitParam *a, SceAppUtilBootParam *b) { (void)a; (void)b; return 0; }
int sceAppUtilSystemParamGetInt(int id, int *v) {
    if (id == SCE_SYSTEM_PARAM_ID_ENTER_BUTTON) *v = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS; /* the bottom face button confirms */
    else *v = 0;
    return 0;
}
int sceAppMgrAcquireBgmPort(void) { return 0; }
int sceAppMgrReleaseBgmPort(void) { return 0; }
int scePowerSetArmClockFrequency(int f) { (void)f; return 0; }
int scePowerSetBusClockFrequency(int f) { (void)f; return 0; }
int scePowerSetGpuClockFrequency(int f) { (void)f; return 0; }
int scePowerSetGpuXbarClockFrequency(int f) { (void)f; return 0; }
int scePowerRequestDisplayOff(void) { return 0; }
int scePowerRequestDisplayOn(void) { return 0; }
int sceNetInit(SceNetInitParam *p) { (void)p; return 0; }
int sceNetCtlInit(void) { return 0; }
int sceNetCtlInetGetState(int *s) { *s = SCE_NETCTL_STATE_CONNECTED; return 0; }
