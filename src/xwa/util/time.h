#ifndef XWA_UTIL_TIME_H
#define XWA_UTIL_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_gameTime;
extern uint32_t g_lastTickTime;
extern uint32_t g_simStepLastTickTime;

uint32_t timeGetTime(void);
uint32_t GetTickCount(void);
void Time_ResetFrameDeltaClocks(void);
uint32_t Time_GetFrameDelta(void);
uint32_t Time_GetSimStepDelta(void);
/* OpenXWA: the sim's speed through Time_GetFrameDelta (1 = classic; the VR
 * quick wheel's slow motion). */
void Time_SetFrameDeltaScale(float scale);

#ifdef __cplusplus
}
#endif

#endif
