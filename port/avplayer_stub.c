/* Nest port: video decoding on a PC test build is not wired up; Android uses
   avplayer_android.c (the phone's hardware decoders). Nest shows its usual
   "couldn't play" message here. */
#include "port.h"
#ifndef __ANDROID__
#include "psp2_all.h"

SceAvPlayerHandle sceAvPlayerInit(SceAvPlayerInitData *p) { (void)p; return 0; }
int sceAvPlayerClose(SceAvPlayerHandle h) { (void)h; return 0; }
int sceAvPlayerAddSource(SceAvPlayerHandle h, const char *f) { (void)h; (void)f; return -1; }
int sceAvPlayerIsActive(SceAvPlayerHandle h) { (void)h; return 0; }
SceBool sceAvPlayerGetVideoData(SceAvPlayerHandle h, SceAvPlayerFrameInfo *i) { (void)h; (void)i; return 0; }
SceBool sceAvPlayerGetAudioData(SceAvPlayerHandle h, SceAvPlayerFrameInfo *i) { (void)h; (void)i; return 0; }
int sceAvPlayerGetStreamInfo(SceAvPlayerHandle h, uint32_t id, SceAvPlayerStreamInfo *i) { (void)h; (void)id; (void)i; return -1; }
uint64_t sceAvPlayerCurrentTime(SceAvPlayerHandle h) { (void)h; return 0; }
int sceAvPlayerJumpToTime(SceAvPlayerHandle h, uint64_t t) { (void)h; (void)t; return 0; }
int sceAvPlayerPause(SceAvPlayerHandle h) { (void)h; return 0; }
int sceAvPlayerResume(SceAvPlayerHandle h) { (void)h; return 0; }
int sceAvPlayerStop(SceAvPlayerHandle h) { (void)h; return 0; }
int sceAvPlayerSetTrickSpeed(SceAvPlayerHandle h, int32_t s) { (void)h; (void)s; return 0; }
#endif
