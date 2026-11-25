/* Nest on Android (and PC): the Vita system calls Nest uses, re-declared so the
   same source compiles everywhere. Each one is implemented in port/*.c on top of
   SDL2, OpenGL ES, POSIX and (on Android) the NDK media decoders. */
#ifndef NEST_PSP2_ALL_H
#define NEST_PSP2_ALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- basic types ---------------- */
typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef int32_t SceInt;
typedef uint64_t SceUInt64;
typedef int64_t SceInt64;
typedef int64_t SceOff;
typedef int32_t SceBool;
typedef uint32_t SceUInt32;
typedef int32_t SceInt32;
typedef uint8_t SceUInt8;
typedef uint16_t SceWChar16;
#define SCE_TRUE 1
#define SCE_FALSE 0

typedef struct SceDateTime {
    unsigned short year, month, day, hour, minute, second;
    unsigned int microsecond;
} SceDateTime;

/* ---------------- threads, time, memory ---------------- */
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int prio, SceSize stack, SceUInt attr, int cpu, const void *opt);
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp);
int sceKernelExitDeleteThread(int status);
int sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout);
int sceKernelDeleteThread(SceUID thid);
int sceKernelDelayThread(SceUInt delay);
SceUID sceKernelCreateMutex(const char *name, SceUInt attr, int init, void *opt);
int sceKernelLockMutex(SceUID id, int count, SceUInt *timeout);
int sceKernelUnlockMutex(SceUID id, int count);
SceUID sceKernelCreateSema(const char *name, SceUInt attr, int init, int max, void *opt);
int sceKernelWaitSema(SceUID id, int signal, SceUInt *timeout);
int sceKernelSignalSema(SceUID id, int signal);
SceUInt64 sceKernelGetProcessTimeWide(void);
int sceKernelExitProcess(int res);

#define SCE_KERNEL_POWER_TICK_DEFAULT 0
#define SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND 1
int sceKernelPowerTick(int type);

typedef enum SceKernelMemBlockType {
    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW = 0x0c20d060,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE = 0x0c208060,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW = 0x09408060
} SceKernelMemBlockType;
#define SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT 0x00000004U
typedef struct SceKernelAllocMemBlockOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 uidBaseBlock;
    const char *strBaseBlockName;
    int flags;
    int reserved[10];
} SceKernelAllocMemBlockOpt;
SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type, SceSize size, SceKernelAllocMemBlockOpt *opt);
int sceKernelFreeMemBlock(SceUID uid);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
SceUID sceKernelFindMemBlockByAddr(const void *addr, SceSize size);
typedef struct SceKernelFreeMemorySizeInfo {
    int size;
    int size_user;
    int size_cdram;
    int size_phycont;
} SceKernelFreeMemorySizeInfo;
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info);

/* ---------------- files ---------------- */
#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_RDWR 0x0003
#define SCE_O_APPEND 0x0100
#define SCE_O_CREAT 0x0200
#define SCE_O_TRUNC 0x0400
#define SCE_O_EXCL 0x0800
#define SCE_SEEK_SET 0
#define SCE_SEEK_CUR 1
#define SCE_SEEK_END 2
#define SCE_S_IFDIR 0x1000
#define SCE_S_IFREG 0x2000
#define SCE_S_ISDIR(m) (((m) & 0xF000) == SCE_S_IFDIR)
#define SCE_S_ISREG(m) (((m) & 0xF000) == SCE_S_IFREG)
#define SCE_CST_MODE 0x0001
#define SCE_CST_SIZE 0x0004
typedef struct SceIoStat {
    int st_mode;
    unsigned int st_attr;
    SceOff st_size;
    SceDateTime st_ctime;
    SceDateTime st_atime;
    SceDateTime st_mtime;
    unsigned int st_private[6];
} SceIoStat;
typedef struct SceIoDirent {
    SceIoStat d_stat;
    char d_name[256];
    void *d_private;
    int dummy;
} SceIoDirent;
SceUID sceIoOpen(const char *file, int flags, int mode);
int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void *data, SceSize size);
int sceIoWrite(SceUID fd, const void *data, SceSize size);
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence);
int sceIoSyncByFd(SceUID fd, int flag);
int sceIoChstatByFd(SceUID fd, const SceIoStat *stat, unsigned int bits);
SceUID sceIoDopen(const char *dirname);
int sceIoDread(SceUID fd, SceIoDirent *dir);
int sceIoDclose(SceUID fd);
int sceIoGetstat(const char *file, SceIoStat *stat);
int sceIoMkdir(const char *dir, int mode);
int sceIoRmdir(const char *path);
int sceIoRemove(const char *file);
int sceIoRename(const char *oldname, const char *newname);
int sceIoDevctl(const char *dev, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen);

/* ---------------- clock ---------------- */
int sceRtcGetCurrentClockLocalTime(SceDateTime *time);

/* ---------------- controls ---------------- */
enum {
    SCE_CTRL_SELECT = 0x00000001,
    SCE_CTRL_L3 = 0x00000002,
    SCE_CTRL_R3 = 0x00000004,
    SCE_CTRL_START = 0x00000008,
    SCE_CTRL_UP = 0x00000010,
    SCE_CTRL_RIGHT = 0x00000020,
    SCE_CTRL_DOWN = 0x00000040,
    SCE_CTRL_LEFT = 0x00000080,
    SCE_CTRL_LTRIGGER = 0x00000100,
    SCE_CTRL_RTRIGGER = 0x00000200,
    SCE_CTRL_TRIANGLE = 0x00001000,
    SCE_CTRL_CIRCLE = 0x00002000,
    SCE_CTRL_CROSS = 0x00004000,
    SCE_CTRL_SQUARE = 0x00008000
};
#define SCE_CTRL_MODE_DIGITAL 0
#define SCE_CTRL_MODE_ANALOG 1
typedef struct SceCtrlData {
    uint64_t timeStamp;
    unsigned int buttons;
    unsigned char lx, ly, rx, ry;
    uint8_t up, right, down, left, lt, rt, l1, r1, triangle, circle, cross, square;
    uint8_t reserved[4];
} SceCtrlData;
int sceCtrlSetSamplingMode(int mode);
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count);

/* ---------------- touch ---------------- */
#define SCE_TOUCH_PORT_FRONT 0
#define SCE_TOUCH_SAMPLING_STATE_START 1
#define SCE_TOUCH_MAX_REPORT 8
typedef struct SceTouchReport {
    uint8_t id;
    uint8_t force;
    uint16_t x;
    uint16_t y;
    uint8_t reserved[8];
    uint16_t info;
} SceTouchReport;
typedef struct SceTouchData {
    uint64_t timeStamp;
    unsigned int status;
    unsigned int reportNum;
    SceTouchReport report[SCE_TOUCH_MAX_REPORT];
} SceTouchData;
int sceTouchSetSamplingState(int port, int state);
int sceTouchPeek(int port, SceTouchData *pData, int nBufs);

/* ---------------- audio ---------------- */
#define SCE_AUDIO_OUT_PORT_TYPE_MAIN 0
#define SCE_AUDIO_OUT_PORT_TYPE_BGM 1
#define SCE_AUDIO_OUT_PORT_TYPE_VOICE 2
#define SCE_AUDIO_OUT_MODE_MONO 0
#define SCE_AUDIO_OUT_MODE_STEREO 1
#define SCE_AUDIO_VOLUME_FLAG_L_CH 0x1
#define SCE_AUDIO_VOLUME_FLAG_R_CH 0x2
#define SCE_AUDIO_VOLUME_0DB 32768
int sceAudioOutOpenPort(int type, int len, int freq, int mode);
int sceAudioOutReleasePort(int port);
int sceAudioOutOutput(int port, const void *buf);
int sceAudioOutSetVolume(int port, int flag, int *vol);
int sceAudioOutSetConfig(int port, int len, int freq, int mode);

/* ---------------- graphics (only what Nest touches) ---------------- */
typedef enum SceGxmPrimitiveType {
    SCE_GXM_PRIMITIVE_TRIANGLES = 0x00000000,
    SCE_GXM_PRIMITIVE_LINES = 0x04000000,
    SCE_GXM_PRIMITIVE_POINTS = 0x08000000,
    SCE_GXM_PRIMITIVE_TRIANGLE_STRIP = 0x0c000000,
    SCE_GXM_PRIMITIVE_TRIANGLE_FAN = 0x10000000,
    SCE_GXM_PRIMITIVE_TRIANGLE_EDGES = 0x14000000
} SceGxmPrimitiveType;
typedef enum SceGxmTextureFormat {
    SCE_GXM_TEXTURE_FORMAT_A8B8G8R8 = 0x0C001000,
    SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR = 0x0C000000,
    SCE_GXM_TEXTURE_FORMAT_P8_ABGR = 0x95000000,
    SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1 = 0x91000000,
    SCE_GXM_TEXTURE_FORMAT_U8_R111 = 0x00001000
} SceGxmTextureFormat;
typedef enum SceGxmTextureFilter {
    SCE_GXM_TEXTURE_FILTER_POINT = 0x00000000,
    SCE_GXM_TEXTURE_FILTER_LINEAR = 0x00000001
} SceGxmTextureFilter;
typedef enum SceGxmMultisampleMode {
    SCE_GXM_MULTISAMPLE_NONE = 0,
    SCE_GXM_MULTISAMPLE_2X = 1,
    SCE_GXM_MULTISAMPLE_4X = 2
} SceGxmMultisampleMode;
#define SCE_GXM_MEMORY_ATTRIB_READ 1
#define SCE_GXM_MEMORY_ATTRIB_WRITE 2
/* a texture: either pixels Nest owns (RGBA / palette) or a video frame (YVU 4:2:0) */
typedef struct SceGxmTexture {
    void *data;            /* video frames: Y plane then interleaved VU */
    unsigned int format;
    unsigned int width, height;
    unsigned int serial;   /* goes up with every new frame */
} SceGxmTexture;
int sceGxmTextureInitLinear(SceGxmTexture *texture, const void *data, SceGxmTextureFormat texFormat, unsigned int width, unsigned int height, unsigned int mipCount);
int sceGxmMapMemory(void *base, SceSize size, int attr);
int sceGxmUnmapMemory(void *base);

/* ---------------- video decoding ---------------- */
typedef int32_t SceAvPlayerHandle;
typedef void *(*SceAvPlayerAlloc)(void *arg, uint32_t alignment, uint32_t size);
typedef void (*SceAvPlayerFree)(void *arg, void *ptr);
typedef void *(*SceAvPlayerAllocFrame)(void *arg, uint32_t alignment, uint32_t size);
typedef void (*SceAvPlayerFreeFrame)(void *arg, void *ptr);
typedef int (*SceAvPlayerOpenFile)(void *p, const char *filename);
typedef int (*SceAvPlayerCloseFile)(void *p);
typedef int (*SceAvPlayerReadOffsetFile)(void *p, uint8_t *buffer, uint64_t position, uint32_t length);
typedef uint64_t (*SceAvPlayerSizeFile)(void *p);
typedef struct SceAvPlayerMemReplacement {
    void *objectPointer;
    SceAvPlayerAlloc allocate;
    SceAvPlayerFree deallocate;
    SceAvPlayerAllocFrame allocateTexture;
    SceAvPlayerFreeFrame deallocateTexture;
} SceAvPlayerMemReplacement;
typedef struct SceAvPlayerFileReplacement {
    void *objectPointer;
    SceAvPlayerOpenFile open;
    SceAvPlayerCloseFile close;
    SceAvPlayerReadOffsetFile readOffset;
    SceAvPlayerSizeFile size;
} SceAvPlayerFileReplacement;
typedef void (*SceAvPlayerEventCallback)(void *p, int32_t argEventId, int32_t argSourceId, void *argEventData);
typedef struct SceAvPlayerEventReplacement {
    void *objectPointer;
    SceAvPlayerEventCallback eventCallback;
} SceAvPlayerEventReplacement;
typedef struct SceAvPlayerInitData {
    SceAvPlayerMemReplacement memoryReplacement;
    SceAvPlayerFileReplacement fileReplacement;
    SceAvPlayerEventReplacement eventReplacement;
    int32_t debugLevel;
    uint32_t basePriority;
    int32_t numOutputVideoFrameBuffers;
    SceBool autoStart;
    uint8_t reserved[3];
    const char *defaultLanguage;
} SceAvPlayerInitData;
typedef struct SceAvPlayerAudio {
    uint16_t channelCount;
    uint8_t reserved[2];
    uint32_t sampleRate;
    uint32_t size;
    char languageCode[4];
} SceAvPlayerAudio;
typedef struct SceAvPlayerVideo {
    uint32_t width;
    uint32_t height;
    float aspectRatio;
    char languageCode[4];
} SceAvPlayerVideo;
typedef union SceAvPlayerStreamDetails {
    SceAvPlayerAudio audio;
    SceAvPlayerVideo video;
} SceAvPlayerStreamDetails;
typedef struct SceAvPlayerFrameInfo {
    uint8_t *pData;
    uint8_t reserved[4];
    uint64_t timeStamp;
    SceAvPlayerStreamDetails details;
} SceAvPlayerFrameInfo;
typedef struct SceAvPlayerStreamInfo {
    uint32_t type;
    uint8_t reserved[4];
    SceAvPlayerStreamDetails details;
    uint64_t duration;
    uint64_t startTime;
} SceAvPlayerStreamInfo;
SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData *pInit);
int sceAvPlayerClose(SceAvPlayerHandle h);
int sceAvPlayerAddSource(SceAvPlayerHandle h, const char *filename);
int sceAvPlayerIsActive(SceAvPlayerHandle h);
SceBool sceAvPlayerGetVideoData(SceAvPlayerHandle h, SceAvPlayerFrameInfo *info);
SceBool sceAvPlayerGetAudioData(SceAvPlayerHandle h, SceAvPlayerFrameInfo *info);
int sceAvPlayerGetStreamInfo(SceAvPlayerHandle h, uint32_t argStreamID, SceAvPlayerStreamInfo *argInfo);
uint64_t sceAvPlayerCurrentTime(SceAvPlayerHandle h);
int sceAvPlayerJumpToTime(SceAvPlayerHandle h, uint64_t argJumpTimeMsec);
int sceAvPlayerPause(SceAvPlayerHandle h);
int sceAvPlayerResume(SceAvPlayerHandle h);
int sceAvPlayerStop(SceAvPlayerHandle h);
int sceAvPlayerSetTrickSpeed(SceAvPlayerHandle h, int32_t trickSpeed);

/* ---------------- system bits ---------------- */
#define SCE_SYSMODULE_NET 0x0001
#define SCE_SYSMODULE_HTTP 0x0002
#define SCE_SYSMODULE_IME 0x0003
#define SCE_SYSMODULE_AVPLAYER 0x0004
int sceSysmoduleLoadModule(int id);

typedef struct SceAppUtilInitParam { SceSize workBufSize; uint8_t reserved[60]; } SceAppUtilInitParam;
typedef struct SceAppUtilBootParam { SceUInt attr; SceUInt appVersion; uint8_t reserved[32]; } SceAppUtilBootParam;
int sceAppUtilInit(SceAppUtilInitParam *initParam, SceAppUtilBootParam *bootParam);
#define SCE_SYSTEM_PARAM_ID_ENTER_BUTTON 1000
#define SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE 0
#define SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS 1
int sceAppUtilSystemParamGetInt(int paramId, int *value);
int sceAppMgrAcquireBgmPort(void);
int sceAppMgrReleaseBgmPort(void);

int scePowerSetArmClockFrequency(int freq);
int scePowerSetBusClockFrequency(int freq);
int scePowerSetGpuClockFrequency(int freq);
int scePowerSetGpuXbarClockFrequency(int freq);
int scePowerRequestDisplayOff(void);
int scePowerRequestDisplayOn(void);

/* ---------------- the system keyboard ---------------- */
typedef enum SceCommonDialogStatus {
    SCE_COMMON_DIALOG_STATUS_NONE = 0,
    SCE_COMMON_DIALOG_STATUS_RUNNING = 1,
    SCE_COMMON_DIALOG_STATUS_FINISHED = 2
} SceCommonDialogStatus;
#define SCE_IME_TYPE_DEFAULT 0
#define SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT 0
#define SCE_IME_DIALOG_BUTTON_NONE 0
#define SCE_IME_DIALOG_BUTTON_CLOSE 1
#define SCE_IME_DIALOG_BUTTON_ENTER 2
typedef struct SceImeDialogParam {
    SceUInt32 supportedLanguages;
    SceBool languagesForced;
    SceUInt32 type;
    SceUInt32 option;
    void *filterFunc;
    SceUInt32 dialogMode;
    SceUInt32 textBoxMode;
    const SceWChar16 *title;
    SceUInt32 maxTextLength;
    SceWChar16 *initialText;
    SceWChar16 *inputTextBuffer;
} SceImeDialogParam;
typedef struct SceImeDialogResult {
    SceInt32 result;
    SceInt32 button;
    char reserved[28];
} SceImeDialogResult;
void sceImeDialogParamInit(SceImeDialogParam *param);
int sceImeDialogInit(const SceImeDialogParam *param);
SceCommonDialogStatus sceImeDialogGetStatus(void);
int sceImeDialogGetResult(SceImeDialogResult *result);
int sceImeDialogTerm(void);

/* ---------------- network (Watch from your PC) ---------------- */
typedef struct SceNetInitParam { void *memory; int size; int flags; } SceNetInitParam;
int sceNetInit(SceNetInitParam *param);
int sceNetCtlInit(void);
#define SCE_NETCTL_STATE_DISCONNECTED 0
#define SCE_NETCTL_STATE_CONNECTED 3
int sceNetCtlInetGetState(int *state);
#define SCE_HTTP_VERSION_1_0 1
#define SCE_HTTP_VERSION_1_1 2
#define SCE_HTTP_METHOD_GET 0
#define SCE_HTTP_HEADER_OVERWRITE 0
#define SCE_HTTP_HEADER_ADD 1
int sceHttpInit(unsigned int poolSize);
int sceHttpCreateTemplate(const char *userAgent, int httpVer, int autoProxyConf);
int sceHttpSetResolveTimeOut(int id, unsigned int usec);
int sceHttpSetConnectTimeOut(int id, unsigned int usec);
int sceHttpSetSendTimeOut(int id, unsigned int usec);
int sceHttpSetRecvTimeOut(int id, unsigned int usec);
int sceHttpCreateConnectionWithURL(int tmplId, const char *url, int enableKeepalive);
int sceHttpDeleteConnection(int connId);
int sceHttpCreateRequestWithURL(int connId, int method, const char *url, unsigned long long contentLength);
int sceHttpDeleteRequest(int reqId);
int sceHttpAddRequestHeader(int id, const char *name, const char *value, unsigned int mode);
int sceHttpSendRequest(int reqId, const void *postData, unsigned int size);
int sceHttpGetStatusCode(int reqId, int *statusCode);
int sceHttpGetResponseContentLength(int reqId, unsigned long long *contentLength);
int sceHttpReadData(int reqId, void *data, unsigned int size);

#ifdef __cplusplus
}
#endif
#endif
