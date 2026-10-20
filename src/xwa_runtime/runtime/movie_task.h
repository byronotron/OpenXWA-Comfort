#ifndef XWA_RUNTIME_MOVIE_TASK_H
#define XWA_RUNTIME_MOVIE_TASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int XwaMovieTask_Begin(const char* name, int noFade);
void XwaMovieTask_ReapFinished(void);
void XwaMovieTask_Tick(void);
void XwaMovieTask_PausedFrame(void);
void XwaMovieTask_SuppressClassicSubtitles(void);
void XwaMovieTask_Stop(int skipped);
int XwaMovieTask_IsActive(void);
int XwaMovieTask_IsComplete(void);
int XwaMovieTask_GetResult(void);
/* The film playing (its name as Movie_Play got it, e.g. "intro_final") and
 * its playback clock in microseconds (the player's media position: audio
 * synced, advancing between its frames); "" / 0 when none plays. */
const char* XwaMovieTask_Name(void);
int64_t XwaMovieTask_PositionUs(void);
uint64_t XwaMovieTask_NextWakeDelayUs(void);
void XwaMovieTask_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
