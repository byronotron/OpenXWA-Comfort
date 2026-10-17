#ifndef XWA_RUNTIME_FLIGHT_CHECKPOINTS_H
#define XWA_RUNTIME_FLIGHT_CHECKPOINTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* In-mission checkpoints (single player), held in memory for the mission:
 *
 *  - automatic ones, about once a minute and after a mission goal changes,
 *    taken only while the player is in no immediate danger; the two newest
 *    are kept;
 *  - a quick save, made and loaded from the in-flight menu;
 *  - when the player's craft is destroyed, the mission's end waits while
 *    "Retry from checkpoint" is offered (A / Enter: retry, B / N: carry on
 *    to the mission's normal end).
 *
 * The flight loop calls XwaFlightCheckpoints_ServiceFrame at the start of
 * each single-player frame, between two simulation steps. */
void XwaFlightCheckpoints_ServiceFrame(void);
/* While the retry is being offered, the mission's end is held back. */
int XwaFlightCheckpoints_HoldingMissionEnd(void);
/* The VR controller's A / B while the retry is offered (1: answered, the
 * press is not passed on). */
int XwaFlightCheckpoints_OfferAnswer(int retry);
int XwaFlightCheckpoints_Offering(void);
/* Drops every checkpoint (the mission ended). */
void XwaFlightCheckpoints_Reset(void);

/* The in-flight menu. */
typedef enum XwaFlightCheckpointRequest {
	XWA_FLIGHT_CHECKPOINT_REQUEST_NONE = 0,
	XWA_FLIGHT_CHECKPOINT_REQUEST_QUICK_SAVE,
	XWA_FLIGHT_CHECKPOINT_REQUEST_QUICK_LOAD,
	XWA_FLIGHT_CHECKPOINT_REQUEST_RETRY,
} XwaFlightCheckpointRequest;
int XwaFlightCheckpoints_Available(void); /* single-player flight: the rows show */
int XwaFlightCheckpoints_HasQuickSave(void);
int XwaFlightCheckpoints_HasCheckpoint(void);
/* Carried out at the next frame (after the menu closes). */
void XwaFlightCheckpoints_Request(XwaFlightCheckpointRequest request);

#ifdef __cplusplus
}
#endif

#endif
