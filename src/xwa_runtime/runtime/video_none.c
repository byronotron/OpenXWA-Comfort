/* Builds without FFmpeg (XWA_ENABLE_VIDEO=OFF, e.g. headless test hosts):
 * no films. Every open fails, so the game skips them. */
#include "aeron/video.h"
#include <stddef.h>
AeronVideoPlayer* Aeron_VideoOpen(const AeronVideoOpenDesc* desc) { (void)desc; return NULL; }
void Aeron_VideoClose(AeronVideoPlayer* p) { (void)p; }
void Aeron_VideoPlay(AeronVideoPlayer* p) { (void)p; }
void Aeron_VideoPause(AeronVideoPlayer* p) { (void)p; }
void Aeron_VideoStop(AeronVideoPlayer* p) { (void)p; }
int Aeron_VideoSeek(AeronVideoPlayer* p, int64_t t) { (void)p; (void)t; return 0; }
void Aeron_VideoSetGain(AeronVideoPlayer* p, float g) { (void)p; (void)g; }
void Aeron_VideoUpdate(AeronVideoPlayer* p) { (void)p; }
int Aeron_VideoSubmit(AeronVideoPlayer* p, const AeronVideoPresentDesc* d) { (void)p; (void)d; return 0; }
AeronVideoState Aeron_VideoGetState(const AeronVideoPlayer* p) { (void)p; return (AeronVideoState)0; }
int64_t Aeron_VideoGetPositionUs(const AeronVideoPlayer* p) { (void)p; return 0; }
uint64_t Aeron_VideoGetPresentedFrameIndex(const AeronVideoPlayer* p) { (void)p; return 0; }
int Aeron_VideoGetNextWakeDelayUs(const AeronVideoPlayer* p, uint64_t* d) { (void)p; if (d) *d = 0; return 0; }
int Aeron_VideoGetInfo(const AeronVideoPlayer* p, AeronVideoInfo* i) { (void)p; (void)i; return 0; }
int Aeron_VideoGetStats(const AeronVideoPlayer* p, AeronVideoStats* s) { (void)p; (void)s; return 0; }
const char* Aeron_VideoGetError(const AeronVideoPlayer* p) { (void)p; return "no video in this build"; }
