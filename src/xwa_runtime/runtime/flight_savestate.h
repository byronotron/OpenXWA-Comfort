#ifndef XWA_RUNTIME_FLIGHT_SAVESTATE_H
#define XWA_RUNTIME_FLIGHT_SAVESTATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-mission save states: a snapshot of the running single-player mission
 * held in memory, put back later in the same mission.
 *
 * The mission's object pools stay where Mission_Init put them for the whole
 * mission, so a snapshot copies them byte for byte (their pointers into each
 * other stay right), together with the mission's, the players', the AI's
 * and the flight loop's state held in globals. Restoring copies it all back
 * and then resets what only mirrors the world (sounds, particle effects,
 * HUD message panes, the music's current state) so it follows the world
 * restored. A snapshot only fits the mission instance it was taken in
 * (FlightSaveState_Fits). */
typedef struct FlightSaveState FlightSaveState;

/* Whether the running flight can be captured now: single player, flying
 * (not in the hangar, the map, a film, the proving grounds), the local
 * player in a craft. */
int FlightSaveState_CanCapture(void);
FlightSaveState* FlightSaveState_Capture(void);
/* Whether the snapshot belongs to the mission instance running now. */
int FlightSaveState_Fits(const FlightSaveState* state);
/* Puts the snapshot back (it stays valid, to be restored again). Returns 0
 * when it does not fit the running mission. Call between two frames of the
 * flight loop. */
int FlightSaveState_Restore(const FlightSaveState* state);
void FlightSaveState_Free(FlightSaveState* state);

/* Mission time when it was taken (the mission clock, in seconds). */
int FlightSaveState_MissionSeconds(const FlightSaveState* state);
size_t FlightSaveState_Bytes(const FlightSaveState* state);

/* Testing: the reset a restore ends with (sounds, effects, HUD messages,
 * music), on its own, to start a reference run from the same point. */
void FlightSaveState_ResetMirrorsForTesting(void);
/* Testing: a hash of everything a snapshot holds, as it is now; with names,
 * one hash per region (returns the region count, fills up to max). */
uint64_t FlightSaveState_HashLive(void);
/* Testing: writes each region of a snapshot to <dir>/<name>.bin. */
void FlightSaveState_DumpForTesting(const FlightSaveState* state, const char* dir);
void FlightSaveState_DumpLiveForTesting(const char* dir);
int FlightSaveState_HashRegions(uint64_t* hashes, const char** names, int max);

#ifdef __cplusplus
}
#endif

#endif
