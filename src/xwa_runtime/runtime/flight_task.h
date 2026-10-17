#ifndef XWA_RUNTIME_FLIGHT_TASK_H
#define XWA_RUNTIME_FLIGHT_TASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int XwaFlightTask_Init(char* missionCmdLine, const char* filmFilePath);
void XwaFlightTask_Tick(void);
int XwaFlightTask_Shutdown(void);
int XwaFlightTask_IsActive(void);
int XwaFlightTask_IsComplete(void);
int XwaFlightTask_GetResult(void);
uint64_t XwaFlightTask_NextWakeDelayUs(void);

#include "xwa/util/savestate_region.h"
/* Counts mission instances (a restart is a new one): save states taken in
 * one only fit it. */
unsigned XwaFlightTask_MissionSerial(void);
/* The flight loop's step-target smoothing, for in-mission save states. */
void XwaFlightTask_SaveStateRegions(XwaSaveStateRegionFn add, void* ctx);

#ifdef __cplusplus
}
#endif

#endif
