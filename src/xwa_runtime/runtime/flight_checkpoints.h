#ifndef XWA_RUNTIME_FLIGHT_CHECKPOINTS_H
#define XWA_RUNTIME_FLIGHT_CHECKPOINTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* In-mission quick save (single player), held in memory for the mission
 * only (it is gone once the mission is left):
 *
 *  - made and loaded from the in-flight menu;
 *  - when the player's craft is destroyed, the death menu below offers it
 *    (with a restart) while the mission's end waits. (The older offer in
 *    the message pane, A / Enter: load, B / N: carry on, is no longer
 *    started: hidden with the HUD in the death camera, it left the pilot
 *    on a still picture.)
 *
 * The flight loop calls XwaFlightCheckpoints_ServiceFrame at the start of
 * each single-player frame, between two simulation steps. */
void XwaFlightCheckpoints_ServiceFrame(void);
/* While loading the quick save is offered or the death menu is due or open,
 * the mission's end is held back. */
int XwaFlightCheckpoints_HoldingMissionEnd(void);
/* The VR controller's A / B while loading is offered (1: answered, the
 * press is not passed on). */
int XwaFlightCheckpoints_OfferAnswer(int retry);
int XwaFlightCheckpoints_Offering(void);
/* Drops the quick save (the mission ended). */
void XwaFlightCheckpoints_Reset(void);
/* The death menu: once the player's craft is lost (single player, not the
 * combat simulator), after a moment of the death camera, the in-flight
 * menu opens as "Your craft was destroyed" with Restart Mission / Load
 * Quick Save / Show Mission Hints / Continue; the mission's end waits
 * until Continue (or a restart / the quick save) closes it.
 *
 * XwaFlightCheckpoints_DeathMenuDue: 1 when the port should open the menu
 * now (Flight_OpenLocalOptionsModal, retried each frame until it is up or
 * a moment has passed); it sets g_configDeathMenuActive. */
int XwaFlightCheckpoints_DeathMenuDue(void);
/* The craft is lost and the death menu not yet closed: the in-flight menu,
 * however it was opened (the pilot's own menu key too), is the death menu. */
int XwaFlightCheckpoints_DeathMenuWanted(void);
/* The death menu draws (each frame): it is open. Returns 0 for a moment
 * after it first shows, while presses are not taken as choices. */
int XwaFlightCheckpoints_DeathMenuShown(void);
/* The flight loop's mission-end check waits (the hold above, or a restart
 * chosen from the death menu on its way). */
int XwaFlightCheckpoints_SkipMissionEndCheck(void);

/* The in-flight menu. */
typedef enum XwaFlightCheckpointRequest {
	XWA_FLIGHT_CHECKPOINT_REQUEST_NONE = 0,
	XWA_FLIGHT_CHECKPOINT_REQUEST_QUICK_SAVE,
	XWA_FLIGHT_CHECKPOINT_REQUEST_QUICK_LOAD,
	XWA_FLIGHT_CHECKPOINT_REQUEST_RESTART, /* the mission from its start */
} XwaFlightCheckpointRequest;
int XwaFlightCheckpoints_Available(void); /* single-player flight: the rows show */
int XwaFlightCheckpoints_HasQuickSave(void);
/* Carried out at the next frame (after the menu closes). */
void XwaFlightCheckpoints_Request(XwaFlightCheckpointRequest request);

#ifdef __cplusplus
}
#endif

#endif
