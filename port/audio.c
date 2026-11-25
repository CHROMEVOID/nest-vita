/* Nest port: the Vita's audio ports as SDL audio devices.
   sceAudioOutOutput blocks until the device can take more, like on the Vita, so
   Nest's audio threads keep their own pace. Volume is applied here. */
#include "port.h"
#undef fopen
#undef main

#include <SDL.h>
#include "psp2_all.h"

typedef struct {
    int used;
    SDL_AudioDeviceID dev;
    int len, freq, ch;
    int vol; /* 0..32768 */
} APort;
static APort ports[8];
static SDL_mutex *alock;
static volatile Uint32 last_sound;

int port_audio_busy(void) { return last_sound && SDL_GetTicks() - last_sound < 4000; }
#ifdef __ANDROID__
#include <jni.h>
JNIEXPORT jboolean JNICALL Java_com_chromevoid_nest_NestActivity_nativeAudioBusy(JNIEnv *env, jclass cls) {
    (void)env;
    (void)cls;
    return port_audio_busy() ? JNI_TRUE : JNI_FALSE;
}
#endif

static SDL_AudioDeviceID open_dev(int freq, int ch, int len) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = freq;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)ch;
    want.samples = (Uint16)(len < 256 ? 256 : len);
    want.callback = NULL; /* we queue */
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (d) SDL_PauseAudioDevice(d, 0);
    return d;
}

int sceAudioOutOpenPort(int type, int len, int freq, int mode) {
    (void)type;
    if (!alock) alock = SDL_CreateMutex();
    SDL_LockMutex(alock);
    for (int i = 1; i < 8; i++)
        if (!ports[i].used) {
            APort *p = &ports[i];
            p->ch = mode == SCE_AUDIO_OUT_MODE_MONO ? 1 : 2;
            p->len = len;
            p->freq = freq;
            p->vol = 32768;
            p->dev = open_dev(freq, p->ch, len);
            if (!p->dev) {
                SDL_UnlockMutex(alock);
                return -1;
            }
            p->used = 1;
            SDL_UnlockMutex(alock);
            return i;
        }
    SDL_UnlockMutex(alock);
    return -1;
}

int sceAudioOutReleasePort(int port) {
    if (port <= 0 || port >= 8 || !ports[port].used) return -1;
    SDL_CloseAudioDevice(ports[port].dev);
    ports[port].used = 0;
    return 0;
}

int sceAudioOutSetConfig(int port, int len, int freq, int mode) {
    if (port <= 0 || port >= 8 || !ports[port].used) return -1;
    APort *p = &ports[port];
    int ch = mode < 0 ? p->ch : (mode == SCE_AUDIO_OUT_MODE_MONO ? 1 : 2);
    int nl = len > 0 ? len : p->len, nf = freq > 0 ? freq : p->freq;
    if (ch == p->ch && nl == p->len && nf == p->freq) return 0;
    SDL_CloseAudioDevice(p->dev);
    p->ch = ch;
    p->len = nl;
    p->freq = nf;
    p->dev = open_dev(nf, ch, nl);
    return p->dev ? 0 : -1;
}

int sceAudioOutSetVolume(int port, int flag, int *vol) {
    (void)flag;
    if (port <= 0 || port >= 8 || !ports[port].used || !vol) return -1;
    ports[port].vol = vol[0];
    return 0;
}

int sceAudioOutOutput(int port, const void *buf) {
    if (port <= 0 || port >= 8 || !ports[port].used) return -1;
    APort *p = &ports[port];
    int n = p->len * p->ch;
    Uint32 bytes = (Uint32)n * 2;
    /* wait while there's more than a couple of buffers queued: the Vita blocks the same way */
    Uint32 limit = bytes * 3;
    while (SDL_GetQueuedAudioSize(p->dev) > limit) SDL_Delay(1);
    if (!buf) return 0;
    { /* remember when something audible last played (Android keeps Nest alive in the background then) */
        const int16_t *s16 = buf;
        for (int i = 0; i < n; i += 61)
            if (s16[i] > 64 || s16[i] < -64) { last_sound = SDL_GetTicks(); break; }
    }
    if (p->vol >= 32768) {
        SDL_QueueAudio(p->dev, buf, bytes);
    } else {
        static int16_t tmp[8][4096 * 2];
        int16_t *o = tmp[port];
        const int16_t *s = buf;
        if (n > 4096 * 2) n = 4096 * 2;
        for (int i = 0; i < n; i++) o[i] = (int16_t)((s[i] * p->vol) >> 15);
        SDL_QueueAudio(p->dev, o, (Uint32)n * 2);
    }
    return 0;
}
