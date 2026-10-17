#ifndef XWA_FLIGHT_FILM_H
#define XWA_FLIGHT_FILM_H

#include "xwa/util/memory.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int Film_ReadBytes(void* dst, int size);
int Film_WriteBytesBuffered(const void* src, size_t size);
int Film_SeekPastHeaderAndMissionName(void);
int Film_SeekToEmbeddedMissionData(void);
int Film_SeekPastEmbeddedMissionData(void);
int Film_FlushWriteBuffer(void);
int FilmNamePrompt_Update(int frameCounter);
char* FilmNamePrompt_Run(void);

#ifdef XWA_MODERN
/* Memory mode (in-mission save states): while active, Film_WriteBytesBuffered
 * appends to a growing heap buffer and Film_ReadBytes reads from one,
 * instead of the film file, so the film state blocks (Death Star, hangar)
 * can be captured and restored in memory. End returns the written buffer
 * (malloc'd, the caller frees it) and its size. */
void Film_BeginMemoryWrite(void);
uint8_t* Film_EndMemoryWrite(size_t* size);
void Film_BeginMemoryRead(const uint8_t* data, size_t size);
/* Returns 1 when everything was read back exactly. */
int Film_EndMemoryRead(void);
#endif

extern uint8_t* g_filmWriteBuffer;
extern MemoryHandle g_filmWriteBufferHandle;
extern int      g_filmWriteBufferSize;
extern int      g_filmWriteBufferedBytes;
extern int      g_filmNamePromptOverwriteConfirm;
extern char     g_filmNamePromptName[128];

#ifdef __cplusplus
}
#endif

#endif
