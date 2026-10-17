#ifndef XWA_REMASTER_VR_LAP_CONSOLE_H
#define XWA_REMASTER_VR_LAP_CONSOLE_H

/*
 * VR lap console: a holographic touch panel in front of the player's lap
 * with the commands that are not on a controller button (energy, targeting,
 * wingmen, ship). It sleeps (dim, not pressable) while the player looks
 * forward and wakes when they look down. Buttons are pressed by poking
 * them with the controller tip, or by pointing the controller at them and
 * pulling the trigger (the trigger does not fire while it points at the
 * console). Squeezing the grip at either end picks it up: it can be put
 * anywhere in the cockpit, turned any way, and stays there.
 *
 * Per host frame: BeginFrame, SetState (game state), then the flight
 * renderer calls Frame while it renders in stereo, then EndFrame. Pressed
 * buttons come out of PollAction.
 */

#include "aeron/render.h"
#include "aeron/xr.h"
#include "xwa_remaster/vr_console_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void XwaVrConsole_BeginFrame(float dt_seconds);
void XwaVrConsole_SetState(const XwaVrConsoleState* state);

/* Handles the hands and, when the console changed, redraws its panel layer
 * (AERON_XR_PANEL_CONSOLE; width x height = one panel). Fills `out` with
 * its quads (one, or one per eye while a hand is over it) and returns their
 * count for Aeron_XrStereoCommitPanelLayer: 0 when it is not shown or out
 * of view. `views` are the stereo frame's eye views. */
int XwaVrConsole_Frame(AeronCommandBuffer* cmd, int width, int height, const AeronXrView views[2],
					   AeronXrPanel out[2]);

/* Quick wheel (XWA_XR_WHEEL=0 turns it off): held open with the grip of
 * the hand that has the game's left controls (the right one when
 * left-handed), away from a panel's handle or the flight stick. It floats
 * in front of that controller; pointing picks a slice, the trigger takes
 * it (the wheel stays open), letting go of the grip closes it. Four
 * categories (ship & weapons, energy, targeting, wingmen) round CONFIRM
 * (Space); each opens its commands round BACK. Drawn on the voice card's
 * layer by CardFrame(1); the console and both cards are hidden while it
 * is open, and its picks come out of PollAction like the console's. */

/* Reference cards (`card` 0: the controller layout, on the player's right;
 * 1: the voice commands, on the left), each a panel layer of its own
 * (AERON_XR_PANEL_CARD / _CARD2) handled like the console. They appear at
 * every mission start just out of view and can be picked up by their ends.
 * XWA_XR_BUTTON_CARD=0 / XWA_XR_VOICE_CARD=0 turn them off (the voice card
 * also goes with XWA_XR_VOICE=0). Call after Frame. */
int XwaVrConsole_CardFrame(AeronCommandBuffer* cmd, int card, int width, int height, const AeronXrView views[2],
						   AeronXrPanel out[2]);

/* Another VR panel owns hand 0/1's grip this frame (applied with the
 * console's own in EndFrame). */
void XwaVrConsole_CaptureGrip(int hand);

/* While hand 0/1 points its ray at the console: the beam from the hand to
 * where the ray lands (LOCAL space), for a faint pointer beam (returns 1);
 * the quick wheel's pointer, a bright one (returns 2). 0: no beam. */
int XwaVrConsole_RayBeam(int hand, float start[3], float end[3]);

/* Releases the trigger when the console was not shown this frame. */
void XwaVrConsole_EndFrame(void);

/* After the player moved the console by its handles (grip at either end):
 * copies the new XWA_XR_CONSOLE_POSE value to `out` once, to be saved. */
int XwaVrConsole_TakePlacement(char* out, int size);

/* Pops a pressed console action. Returns 0 when none is queued. */
int XwaVrConsole_PollAction(XwaVrConsoleAction* action, int* arg);

void XwaVrConsole_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
